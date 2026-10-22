#!/usr/bin/env pwsh
# Alchemy: Skooma workflow helper.  Invoke as:  git skooma <command> [args]
# Design + rationale: doc/skooma/WORKFLOW.md
#
# Commands: status | new | wt-setup | sync | check | integrate | undo | retire | bootstrap
#
# Never edit the copy in .git/skooma/ — it is overwritten by `git skooma bootstrap`.
# The source of truth is scripts/skooma/skooma.ps1 on the develop branch.

[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Command = 'status',
    [Parameter(Position = 1, ValueFromRemainingArguments = $true)][string[]]$Rest = @()
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# A git alias exports GIT_DIR / GIT_WORK_TREE / GIT_INDEX_FILE for the worktree it
# was invoked from, and `git -C <other>` does NOT override them — every query about
# another worktree would silently be answered against this one.  Clear them; the
# alias has already cd'd us to a worktree toplevel, so normal discovery works.
foreach ($v in 'GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_PREFIX', 'GIT_COMMON_DIR', 'GIT_OBJECT_DIRECTORY') {
    if (Test-Path "Env:$v") { Remove-Item "Env:$v" }
}

# ---------------------------------------------------------------- primitives

# In porcelain mode (wt-new, called by tooling) stdout must carry the worktree path
# and nothing else, so chatter goes to stderr.
$script:Porcelain = $false
function Say  { param([string]$m) if ($script:Porcelain) { [Console]::Error.WriteLine($m) } else { Write-Host $m } }
function Info { param([string]$m) if ($script:Porcelain) { [Console]::Error.WriteLine("  $m") } else { Write-Host "  $m" -ForegroundColor DarkGray } }
function Good { param([string]$m) if ($script:Porcelain) { [Console]::Error.WriteLine("  $m") } else { Write-Host "  $m" -ForegroundColor Green } }
function Warn { param([string]$m) if ($script:Porcelain) { [Console]::Error.WriteLine("  ! $m") } else { Write-Host "  ! $m" -ForegroundColor Yellow } }
function Fail { param([string]$m) [Console]::Error.WriteLine("  x $m"); exit 1 }

function Show-Help {
    Say ''
    Say 'git skooma <command>'
    Say ''
    Say '  status              where am I, what is ahead/behind, what is dirty'
    Say '  new <name> [desc]   create <class>-<name> + worktree in .worktrees/<name>'
    Say '                      --class fix|workaround|<own>  (asked for when run with no name)'
    Say '                      --base <branch>  --tests (configure a local tests tree)'
    Say '  sync                fetch upstream -> next -> develop + every worktree'
    Say '  check [name|.]      validate a branch before it lands'
    Say '  integrate [name|.]  land worktree branches into their trunk or develop (no arg = all)'
    Say '                      <name> --resolve  on a conflict with develop, leave the merge to resolve'
    Say '                      --continue  commit a resolved --resolve merge   --abort  drop it'
    Say '  trunk [prefix branch]  list or set the trunk an area lands into (atmo -> feature-atmo-magic)'
    Say '                      --join [name|.]  make an existing branch land into its trunk'
    Say '                      --unset <prefix>'
    Say '  build [target]      build this worktree (shared Ninja tree) or the main checkout (VS)'
    Say '                      --obj <file>  compile one translation unit'
    Say '  test [--match re] [--label l] [--bench]  build + run CTest here (needs new --tests)'
    Say '  clean [name|--all]  delete a build tree to reclaim disk (never the main one)'
    Say '  undo                reset develop to the last-known-good commit'
    Say '  retire [name|.]     remove a worktree + branch whose work is already in its base'
    Say '                      --force  discard one that still carries work (prints the tip sha)'
    Say '  wt-setup [path]     (re)create agent files in a worktree'
    Say '  bootstrap           install scripts, hooks and config'
    Say ''
    Say '  -h, --help          show this help (safe anywhere: never treated as a name/arg)'
    Say ''
    Say '  docs: doc/skooma/WORKFLOW.md'
    Say ''
}

# Checked before any command touches $Rest: ConvertTo-Slug trims leading/trailing
# dashes off each path segment, so a stray `--help` silently becomes the valid slug
# 'help' instead of erroring — `git skooma new --help` created a real worktree named
# for it. Intercepted globally, before git/context setup, so it works even outside a repo.
if ($Command -in '-h', '--help', 'help' -or $Rest -contains '-h' -or $Rest -contains '--help') {
    Show-Help
    exit 0
}

$script:GitExit = 0

# Run git, return trimmed combined output, set $script:GitExit.
# These deliberately use $args rather than a param block: a declared parameter
# would swallow git's own short flags (-C, -G, ...) during binding.
function G {
    $out = & git @args 2>&1
    $script:GitExit = $LASTEXITCODE
    return (($out | Out-String) -replace "`r", '').TrimEnd()
}

# Same, but the ref-guard hook lets it through (used only where the script has
# already validated the ref move itself).
function GG {
    $old = $env:SKOOMA_SCRIPT
    $env:SKOOMA_SCRIPT = '1'
    try { return (G @args) } finally { $env:SKOOMA_SCRIPT = $old }
}

function GitOk {
    $null = G @args; return ($script:GitExit -eq 0)
}

# ------------------------------------------------------------------- context

$CommonDir = G rev-parse --path-format=absolute --git-common-dir
if ($script:GitExit -ne 0) { Fail 'not inside a git repository' }
$CommonDir = $CommonDir -replace '/', '\'

function Get-Worktrees {
    $list = @(); $cur = $null
    foreach ($line in (G worktree list --porcelain) -split "`n") {
        if ($line -like 'worktree *') {
            if ($cur) { $list += [pscustomobject]$cur }
            $cur = @{ Path = ($line.Substring(9) -replace '/', '\').TrimEnd('\'); Branch = ''; Detached = $false; Locked = $false }
        }
        elseif ($line -like 'branch *')  { $cur.Branch = $line.Substring(7) -replace '^refs/heads/', '' }
        elseif ($line -eq 'detached')    { $cur.Detached = $true }
        elseif ($line -like 'locked*')   { $cur.Locked = $true }
    }
    if ($cur) { $list += [pscustomobject]$cur }
    return $list
}

$Worktrees = Get-Worktrees
$MainWt    = $Worktrees[0].Path                     # first entry is always the main worktree
$WtRoot    = Join-Path $MainWt '.worktrees'
$Here      = (Get-Location).Path.TrimEnd('\')
$HereWt    = $Worktrees | Where-Object { $_.Path -ieq $Here } | Select-Object -First 1

function Cfg { param([string]$key, [string]$default = '')
    $v = G config --get $key
    if ($script:GitExit -ne 0 -or -not $v) { return $default }
    return $v.Trim()
}

$UpstreamBranch = Cfg 'skooma.upstreambranch' 'next'

# Fork-only paths that must never appear in a feature-* commit.
$ForbiddenPaths = @(
    'AGENTS.md', 'CLAUDE.local.md', 'doc/skooma/', 'scripts/skooma/',
    '.githooks/', '.worktrees/', '.claude/', '.vcpkg-shared/',
    'vs2026-os/', 'ninja-os/', 'build-Windows-', 'build-Darwin-', 'build-Linux-'
)

function Test-Forbidden { param([string]$path)
    foreach ($p in $ForbiddenPaths) { if ($path -eq $p -or $path.StartsWith($p)) { return $true } }
    return $false
}

# Branch class: the prefix a working branch carries ('feature', 'fix', 'workaround',
# 'spike', or anything the human typed at `git skooma new`). Classes are cosmetic with
# one exception — 'spike' carries a role of its own, because it is the one kind of
# branch that is never meant to reach next. The rest just say what a branch is for.
# Custom classes are remembered in skooma.class so they appear in the list next time.
$BuiltinClasses = @('feature', 'fix', 'workaround', 'spike')

function Get-Classes {
    $extra = @((G config --get-all skooma.class) -split "`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    return @(@($BuiltinClasses) + $extra | Select-Object -Unique)
}
$script:Classes = Get-Classes

function Test-ClassOk { param([string]$c) return ($c -match '^[a-z][a-z0-9-]*$' -and $c -notin 'next', 'develop', 'publish', 'skooma') }

function Add-Class { param([string]$c)
    if ($script:Classes -contains $c) { return }
    $null = G config --add skooma.class $c
    $script:Classes = @($script:Classes) + $c
}

# Class of a branch: the config marker if it has one, else its prefix.
# 'glue' and 'sidecar' are the two non-working classes.
function Get-Class { param([string]$branch)
    if (-not $branch) { return '' }
    $m = Cfg "branch.$branch.skooma"
    if ($m) { return $m }
    if ($branch -eq 'skooma-glue') { return 'glue' }
    foreach ($c in $script:Classes) { if ($branch -like "$c-*") { return $c } }
    if ($branch -like 'skooma-*')   { return 'sidecar' }
    return ''
}

# Branch role: 'feature' | 'spike' | 'glue' | 'sidecar' | '' (not ours).
# Every working class (feature, fix, workaround, custom) has the 'feature' role:
# the same rules apply to all of them, because all of them go upstream.
# 'spike' is the exception. A spike may be integrated into develop so its
# instrumentation runs in the F5 build, but it never goes upstream, so the export
# rules do not bind it and a rule of its own does: see DO NOT LAND in check/retire.
# Dispatch on the config marker first so tool-created names (claude/*, t3code/*) work once adopted.
function Get-Role { param([string]$branch)
    $c = Get-Class $branch
    if (-not $c) { return '' }
    if ($c -in 'glue', 'sidecar') { return $c }
    if ($c -eq 'spike') { return 'spike' }
    return 'feature'
}

# Every prefix a working branch can carry, for ref globs and name resolution.
function Get-ClassPrefixes { return @($script:Classes | Where-Object { $_ -notin 'glue', 'sidecar' }) }

# A branch's base is what it was cut from and what it merges to stay current:
# `next` for a top-level feature, another feature branch for a sub-feature.
# Chains can be any depth (keyframes <- weather <- tornado).
function Get-Base { param([string]$branch)
    $b = Cfg "branch.$branch.skooma-base"
    if (-not $b) { return 'next' }
    if (-not (GitOk show-ref --verify --quiet "refs/heads/$b")) { return 'next' }  # parent retired/merged upstream
    return $b
}

# A trunk is the feature branch one area ships as (feature-navmesh, feature-atmo-magic).
# Worktrees of the area land into the trunk, and the trunk lands into develop, so the
# trunk holds everything the PR will. The map is name prefix -> trunk, set with
# `git skooma trunk <prefix> <branch>` and stored as skooma-trunk.<prefix>.branch.
function Get-TrunkMap {
    $map = @{}
    foreach ($l in (G config --get-regexp '^skooma-trunk\..*\.branch$') -split "`n" | Where-Object { $_ }) {
        if ($l -match '^skooma-trunk\.(.+)\.branch\s+(\S+)$') { $map[$Matches[1]] = $Matches[2] }
    }
    return $map
}

# The trunk a new name joins: longest prefix that matches at a / or - boundary.
function Find-TrunkFor { param([string]$name)
    $map = Get-TrunkMap
    $best = ''
    foreach ($p in $map.Keys) {
        if (($name -eq $p -or $name.StartsWith("$p/") -or $name.StartsWith("$p-")) -and $p.Length -gt $best.Length) { $best = $p }
    }
    if ($best) { return $map[$best] }
    return ''
}

function Test-IsTrunk { param([string]$branch) return ((Get-TrunkMap).Values -contains $branch) }

# Where a branch lands: its trunk if it has a live one, else develop.
function Get-LandTarget { param([string]$branch)
    $t = Cfg "branch.$branch.skooma-trunk"
    if ($t -and $t -ne $branch -and (GitOk show-ref --verify --quiet "refs/heads/$t")) { return $t }
    return 'develop'
}

function Get-Depth { param([string]$branch)
    $d = 0; $x = $branch
    while ($x -and $x -ne 'next' -and $d -lt 16) { $x = Get-Base $x; $d++ }
    return [Math]::Max(0, $d - 1)
}

# Parents before children. Anything in a cycle is appended in its original order.
function Sort-ByDependency { param([string[]]$branches)
    $remaining = [System.Collections.ArrayList]@($branches)
    $ordered = @()
    while ($remaining.Count -gt 0) {
        $progress = $false
        foreach ($b in @($remaining)) {
            if ($remaining -notcontains (Get-Base $b)) {
                $ordered += $b; $remaining.Remove($b); $progress = $true
            }
        }
        if (-not $progress) { $ordered += @($remaining); break }
    }
    return , $ordered
}

function Get-Counts { param([string]$from, [string]$to)
    $v = G rev-list --left-right --count "$from...$to"
    if ($script:GitExit -ne 0) { return @{ Behind = 0; Ahead = 0 } }
    $p = $v -split '\s+'
    return @{ Behind = [int]$p[0]; Ahead = [int]$p[1] }
}

function Test-Dirty { param([string]$path)
    $s = G -C $path status --porcelain --untracked-files=no --ignore-submodules=all
    return [bool]$s
}

function Test-MidOperation { param([string]$path)
    $d = (G -C $path rev-parse --path-format=absolute --git-dir) -replace '/', '\'
    foreach ($f in 'MERGE_HEAD', 'CHERRY_PICK_HEAD', 'REVERT_HEAD', 'rebase-merge', 'rebase-apply') {
        if (Test-Path (Join-Path $d $f)) { return $f }
    }
    return ''
}

# Only a compile of the main checkout conflicts with a land: a running viewer, or a
# build in a worktree or elsewhere, does not.  MSBuild is deliberately not checked:
# its worker nodes linger idle for reuse long after a build finishes.  cl/link/ninja
# only exist while compiling, and one is the main checkout's when it or an ancestor
# (F5's `cmake --build <main>/vs2026-os`) names the main checkout, or $Tree if given.
function Test-BuildRunning { param([string]$Tree = $MainWt)
    $procs = @{}
    foreach ($p in Get-CimInstance Win32_Process -ErrorAction SilentlyContinue) { $procs[[int]$p.ProcessId] = $p }
    $main = $Tree.TrimEnd('\').ToLowerInvariant() + '\'
    $wts  = $main + '.worktrees\'
    foreach ($p in $procs.Values) {
        if ($p.Name -notin 'cl.exe', 'link.exe', 'ninja.exe') { continue }
        $cur = $p
        for ($depth = 0; $cur -and $depth -lt 12; $depth++) {
            $text = ("$($cur.ExecutablePath) $($cur.CommandLine)" -replace '/', '\').ToLowerInvariant()
            if ($text.Contains($main) -and -not $text.Contains($wts)) { return $true }
            if ($cur.ParentProcessId -eq $cur.ProcessId) { break }
            $cur = $procs[[int]$cur.ParentProcessId]
        }
    }
    return $false
}

function Backup-Docs {
    $src = Join-Path $MainWt 'doc\skooma'
    $dst = 'V:\Scratch\skooma-docs'
    if (-not (Test-Path $src)) { return }
    if (-not (Test-Path 'V:\Scratch')) { return }
    & robocopy $src $dst /E /PURGE /NJH /NJS /NDL /NFL /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { Warn "doc backup to $dst failed (robocopy $LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
}

# ------------------------------------------------------------------ wt-setup

function Invoke-WtSetup { param([string]$Path = $Here)
    $Path = $Path.TrimEnd('\')
    if ($Path -ieq $MainWt) { Info 'wt-setup: main checkout, nothing to do'; return }

    $branch = G -C $Path rev-parse --abbrev-ref HEAD
    if ($branch -in 'develop', 'next', 'publish') {
        Fail "worktree $Path is on protected branch '$branch'; refusing to adopt it as a feature"
    }
    $role = Get-Role $branch

    # Adopt a worktree created by a tool under its own naming (claude/x, t3code/x).
    if (-not $role) {
        $leaf = Split-Path $Path -Leaf
        $new  = "feature-$($leaf.ToLowerInvariant() -replace '[^a-z0-9._-]','-')"
        if (GitOk show-ref --verify --quiet "refs/heads/$new") {
            Warn "branch '$branch' is not a Skooma branch and '$new' already exists; marking in place"
            $null = G config "branch.$branch.skooma" 'feature'
        }
        else {
            $null = G -C $Path branch -m $branch $new
            if ($script:GitExit -eq 0) {
                Good "renamed branch '$branch' -> '$new'"
                $branch = $new
            }
            else { Warn "could not rename '$branch'; marking in place" }
            $null = G config "branch.$branch.skooma" 'feature'
        }
        $role = 'feature'
    }

    # AGENTS.md: canonical copy lives in the main checkout; worktree gets an untracked copy.
    $agents = Join-Path $MainWt 'AGENTS.md'
    if (Test-Path $agents) { Copy-Item $agents (Join-Path $Path 'AGENTS.md') -Force }

    # CLAUDE.local.md: import the canonical AGENTS.md by absolute path (no junction, see WORKFLOW.md).
    $stub = @(
        "# Skooma worktree — branch ``$branch`` (role: $role)",
        '',
        "@$($MainWt -replace '\\','/')/AGENTS.md",
        '',
        "Design docs (read-only, absolute path): ``$MainWt\doc\skooma\``",
        "Main checkout (develop, F5 build): ``$MainWt``"
    ) -join "`n"
    Set-Content -Path (Join-Path $Path 'CLAUDE.local.md') -Value $stub -Encoding UTF8

    # .vscode is untracked and per-checkout; copy so F5/build tasks work here too.
    $vs = Join-Path $MainWt '.vscode'
    if (Test-Path $vs) {
        $dst = Join-Path $Path '.vscode'
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        Copy-Item (Join-Path $vs '*') $dst -Recurse -Force
    }

    Good "wt-setup complete: $Path"
}

# ----------------------------------------------------------------- submodules

function Initialize-Submodules { param([string]$Path)
    $refArgs = @()
    foreach ($sm in 'vcpkg', 'indra/dullahan') {
        $local = Join-Path $MainWt ($sm -replace '/', '\')
        if (Test-Path (Join-Path $local '.git')) { $refArgs += @('--reference', $local) }
    }
    Info 'initialising submodules (sharing objects with the main checkout)...'
    $null = G -C $Path submodule update --init --recursive @refArgs
    if ($script:GitExit -ne 0) {
        Warn 'submodule update with --reference failed; retrying without'
        $null = G -C $Path submodule update --init --recursive
        if ($script:GitExit -ne 0) { Warn 'submodule init failed — cmake configure will fail until fixed' }
    }
}

# --------------------------------------------------------------------- build

# Ninja needs cl.exe and ninja.exe on PATH; the VS generator sets that up itself,
# Ninja does not. VS BuildTools ships ninja, so entering the dev shell provides both.
function Enter-DevShell {
    if (Get-Command cl -ErrorAction SilentlyContinue) { return $true }
    $vs = ''
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vs = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null | Select-Object -First 1)
    }
    if (-not $vs) {
        $vs = @(Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\*\*' -Directory -EA SilentlyContinue |
                Where-Object { Test-Path (Join-Path $_.FullName 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll') } |
                Select-Object -First 1 -ExpandProperty FullName)
    }
    if (-not $vs) { return $false }
    $dll = Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    if (-not (Test-Path $dll)) { return $false }
    Import-Module $dll -ErrorAction Stop
    # VS's own dev-shell script shells out to vswhere and complains when it is not
    # on PATH, even though it succeeds. Swallow that noise.
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' *>$null
    return $true
}

# Worktrees build with Ninja into ninja-os/ (fast edit-compile loop, 0.4s null build).
# The main checkout keeps the Visual Studio generator in vs2026-os/ for F5 debugging.
function Get-BuildTree { param([string]$Path)
    if ($Path -ieq $MainWt) { return @{ Dir = 'vs2026-os'; Ninja = $false } }
    return @{ Dir = 'ninja-os'; Ninja = $true }
}

# All worktrees share one build worktree: a checkout plus a configured Ninja tree is
# ~19 GB and takes minutes to create, the build lock already serializes builds across
# worktrees, and nothing is lost by having one.  It lives on skooma-build, based on
# next, so `git skooma sync` keeps its content — and therefore the baseline its build
# tree was last built against — close to whatever the next branch is cut from.
function Get-BuildWorktreePath { return (Join-Path $WtRoot 'build') }

# Create the shared build worktree on first use.
function Initialize-BuildWorktree {
    $path = Get-BuildWorktreePath
    if ($Worktrees | Where-Object { $_.Path -ieq $path }) { return $path }
    if (Test-Path $path) { Fail "$path exists but is not a registered worktree" }
    if (GitOk show-ref --verify --quiet 'refs/heads/skooma-build') {
        $out = G worktree add $path skooma-build
    }
    else {
        $out = G worktree add --no-track -b skooma-build $path next
    }
    if ($script:GitExit -ne 0) { Fail "could not create the build worktree at ${path}`n$out" }
    $null = G config 'branch.skooma-build.skooma' 'sidecar'
    $null = G config 'branch.skooma-build.skooma-base' 'next'
    $null = G config 'branch.skooma-build.description' 'Shared build worktree for git skooma build'
    Initialize-Submodules $path
    Good "created build worktree: $path"
    return $path
}

# Materialize the calling worktree's tracked state in the build worktree.  `git stash
# create` captures staged and unstaged changes to tracked files as a commit without
# touching the source worktree, so `git skooma build --obj` compiles what was just
# written rather than the last commit.  Stash does not carry untracked files, and a
# build that silently ignores them would be worse than a slow one, so warn.
function Set-BuildWorktreeState { param([string]$BuildPath, [string]$SourcePath)
    if ($SourcePath -ieq $BuildPath) { return }
    # git writes warnings (line-ending normalisation, for one) to stderr, and G
    # combines both streams, so pick the hash out of the output instead of trusting it.
    $rev = ''
    $out = G -C $SourcePath stash create
    if ($script:GitExit -eq 0) {
        $rev = @($out -split "`n" | Where-Object { $_ -match '^[0-9a-f]{40}$' } | Select-Object -Last 1) -join ''
    }
    else { Warn 'could not snapshot the working tree; building HEAD instead' }
    if (-not $rev) {
        $rev = G -C $SourcePath rev-parse HEAD
        if ($script:GitExit -ne 0 -or -not $rev) { Fail "could not resolve the working state of $SourcePath" }
    }
    $out = G -C $BuildPath checkout --quiet --force --detach $rev
    if ($script:GitExit -ne 0) { Fail "could not check out the calling state in $BuildPath`n$out" }
    # Only source matters; the agent files every worktree carries are untracked noise.
    $untracked = @(G -C $SourcePath ls-files --others --exclude-standard | Where-Object { $_ -like 'indra/*' })
    if ($untracked.Count) {
        Warn "$($untracked.Count) untracked source file(s) are not in the build (git add them to include them)"
    }
}

# vcpkg_installed is ~4.5 GB and identical for every worktree using the same
# manifest, so share it. Keyed by a hash of indra/vcpkg.json: a branch that changes
# dependencies automatically gets its own directory instead of corrupting everyone's.
function Get-SharedVcpkgDir { param([string]$Path)
    $manifest = Join-Path $Path 'indra\vcpkg.json'
    if (-not (Test-Path $manifest)) { return '' }
    $h = (Get-FileHash $manifest -Algorithm SHA256).Hash.Substring(0, 12).ToLowerInvariant()
    return (Join-Path $MainWt ".vcpkg-shared\$h")
}

# Two worktrees configuring at once would race inside the shared directory.
function Enter-VcpkgLock { param([string]$dir, [int]$timeoutSec = 1800)
    $lock = "$dir.lock"
    New-Item -ItemType Directory -Force -Path (Split-Path $lock -Parent) | Out-Null
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ($true) {
        try { return [IO.File]::Open($lock, 'OpenOrCreate', 'ReadWrite', 'None') }
        catch {
            if ((Get-Date) -gt $deadline) { Warn 'timed out waiting for the shared vcpkg lock; configuring unshared'; return $null }
            Info 'another worktree is populating the shared vcpkg directory; waiting...'
            Start-Sleep -Seconds 5
        }
    }
}

# Compilers take every core by default; several agents building at once would each do
# that, so each build is capped. Override with `git config skooma.parallel <n>`.
function Get-Parallel { param([int]$Default = 12)
    $n = 0
    if ([int]::TryParse((Cfg 'skooma.parallel' ''), [ref]$n) -and $n -gt 0) { return $n }
    return $Default
}

# One build at a time, across every worktree: a build already saturates the machine,
# and two at once each take longer than running them back to back. The lock lives in
# the shared git dir; the OS releases the handle when the holder exits, so a killed
# build (Ctrl+C, agent timeout, task manager) cannot leave it stuck. Waits forever:
# the process holding it is the build the caller asked to wait for.
function Enter-BuildLock {
    $lock = Join-Path $CommonDir 'skooma-build.lock'
    $waiting = $false
    while ($true) {
        try { return [IO.File]::Open($lock, 'OpenOrCreate', 'ReadWrite', 'None') }
        catch {
            if (-not $waiting) { Info 'another build is running; waiting for it to finish...'; $waiting = $true }
            Start-Sleep -Seconds 5
        }
    }
}

function Initialize-BuildTree { param([string]$Path, [string]$Branch = '')
    $bt = Get-BuildTree $Path
    $cache = Join-Path $Path "$($bt.Dir)\CMakeCache.txt"

    # A branch created with --tests gets the unit and GL tests configured in; everyone else
    # keeps the faster tree. Read per branch so a re-configure after `clean` remembers it.
    # `new` passes the branch because $Worktrees was listed before the worktree existed.
    # The shared build worktree always carries them: it is where every worktree without a
    # tree of its own both builds and tests.
    $wtBranch = if ($Branch) { $Branch } else { ($Worktrees | Where-Object { $_.Path -ieq $Path } | Select-Object -First 1).Branch }
    $wantTests = ($Path -ieq (Get-BuildWorktreePath)) -or
                 ($wtBranch -and (Cfg "branch.$wtBranch.skooma-tests" 'false') -eq 'true')

    # An existing tree that was configured without the tests is re-configured rather than
    # rejected: CMake keeps the cache and the objects, so it costs a configure, not a build.
    if (Test-Path $cache) {
        $hasTests = [bool](Select-String -Path $cache -Pattern '^AL_BUILD_TESTS:BOOL=ON$' -Quiet)
        if (-not $wantTests -or $hasTests) { Info "build tree $($bt.Dir) already configured"; return $true }
        Info "build tree $($bt.Dir) has no tests; re-configuring with them"
    }
    if (-not (Enter-DevShell)) { Warn 'could not find the Visual Studio build environment; configure manually'; return $false }

    $args = @('-S', 'indra', '-B', $bt.Dir, '--preset', 'ninja-os', '-DAL_BUILD_PACKAGE=OFF')

    if ($wantTests) {
        $args += @('-DAL_BUILD_TESTS=ON', '-DAL_ENABLE_GL_TESTS=ON')
    }

    $shared = Get-SharedVcpkgDir $Path
    if ($shared) { $args += "-DVCPKG_INSTALLED_DIR=$($shared -replace '\\','/')" }
    # Built ports come back from the binary cache, so their buildtrees and packages are dead weight.
    $args += '-DVCPKG_INSTALL_OPTIONS=--clean-after-build'

    # Worktrees are for compile checks; debugging happens in the main checkout. Dropping
    # debug info shrinks objects, removes PDBs entirely, and compiles a little faster.
    if ((Cfg 'skooma.worktreedebuginfo' 'false') -ne 'true') {
        $args += '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT='
        $args += '-DCMAKE_POLICY_DEFAULT_CMP0141=NEW'
    }

    # Tracy is on by default because AL_CHANNEL is "Alchemy Test", but nobody profiles from a
    # worktree and its instrumentation macros break compiles. Profile in the main checkout; a
    # branch touching Tracy itself opts back in with branch.<name>.skooma-tracy.
    $tracyDefault = Cfg 'skooma.worktreetracy' 'false'
    $tracy = if ($wtBranch) { Cfg "branch.$wtBranch.skooma-tracy" $tracyDefault } else { $tracyDefault }
    if ($tracy -ne 'true') { $args += '-DAL_USE_TRACY=OFF' }

    # Downloads (source tarballs, msys2 packages, tool binaries) are the other thing every
    # worktree would otherwise fetch again: vcpkg honours VCPKG_DOWNLOADS, so point it at the
    # main checkout's cache, which the first configure filled.
    $downloads = Join-Path $MainWt 'vcpkg\downloads'
    if (Test-Path $downloads) { $env:VCPKG_DOWNLOADS = $downloads }

    # vcpkg compiles ports with every core by default; honour the same cap as builds.
    $env:VCPKG_MAX_CONCURRENCY = "$(Get-Parallel)"

    $tracyNote = if ($tracy -eq 'true') { 'Tracy' } else { 'no Tracy' }
    Info "configuring $($bt.Dir) (Ninja, no debug info, $tracyNote, shared vcpkg) — about a minute..."
    $lockHandle = if ($shared) { Enter-VcpkgLock $shared } else { $null }
    Push-Location $Path
    try {
        & cmake @args 2>&1 | Out-Null
        $ok = ($LASTEXITCODE -eq 0)
    } finally {
        Pop-Location
        if ($lockHandle) { $lockHandle.Dispose() }
    }
    if ($ok) { Good "build tree ready: $Path\$($bt.Dir)" } else { Warn 'configure failed; run it by hand to see why' }
    return $ok
}

# Reclaim disk from a worktree that is not being worked on right now. The worktree,
# its branch and its commits all survive; reconfiguring later takes about a minute
# because the vcpkg binary cache and the shared installed dir are still warm.
function Invoke-Clean {
    $all = $Rest -contains '--all'
    $targets = @()
    foreach ($w in $Worktrees) {
        if ($w.Path -ieq $MainWt) { continue }          # never touch the F5 build tree
        if (-not $w.Branch) {
            # The build worktree sits detached between builds and still has a tree.
            if ($w.Path -ieq (Get-BuildWorktreePath) -and ($all -or ($Rest -and 'build' -match [regex]::Escape($Rest[0])))) { $targets += $w }
            continue
        }
        if (-not (Get-Role $w.Branch)) { continue }
        if ($all) { $targets += $w; continue }
        if ($HereWt -and $w.Path -ieq $HereWt.Path) { $targets += $w }
        elseif ($Rest -and $w.Branch -match [regex]::Escape($Rest[0])) { $targets += $w }
    }
    if (-not $targets) {
        Say ''
        Info 'nothing to clean (pass --all, a branch name, or run inside a worktree)'
        Info 'the main checkout build tree is never cleaned by this command'
        Say ''
        return
    }
    Say ''
    foreach ($w in $targets) {
        $label = if ($w.Branch) { $w.Branch } else { 'build' }
        $dir = Join-Path $w.Path (Get-BuildTree $w.Path).Dir
        if (-not (Test-Path $dir)) { Info "${label}: no build tree"; continue }
        $gb = [math]::Round((Get-ChildItem $dir -Recurse -File -EA SilentlyContinue |
            Measure-Object Length -Sum).Sum / 1GB, 2)
        Remove-Item -Recurse -Force $dir -EA SilentlyContinue
        if (Test-Path $dir) { Warn "${label}: could not remove $dir (something holds a handle)" }
        else { Good "${label}: freed ${gb} GB" }
    }
    Say ''
    Info 'rebuild with: git skooma build   (reconfigures automatically)'
    Say ''
}

# Map a source file to its ninja object target by asking ninja, rather than guessing
# CMake's directory layout. Compiling one object is ~0.3s versus ~3s for its library.
function Resolve-ObjTarget { param([string]$dir, [string]$file, [string]$config)
    $leaf = Split-Path $file -Leaf
    # Ninja Multi-Config writes one build file per configuration; the default
    # build.ninja is whatever CMAKE_DEFAULT_BUILD_TYPE says (RelWithDebInfo here),
    # so querying it would resolve — and then build — the wrong configuration.
    $all = @(& ninja -C $dir -f "build-$config.ninja" -t targets all 2>$null)
    $cands = @($all | ForEach-Object { ($_ -split ':')[0] } |
        Where-Object { $_ -match "[\\/]$([regex]::Escape($leaf))\.obj$" })
    if (-not $cands) { return '' }
    $inConfig = @($cands | Where-Object { $_ -match "[\\/]$([regex]::Escape($config))[\\/]" })
    if ($inConfig) { $cands = $inConfig }
    if ($cands.Count -gt 1) {
        # Disambiguate by the source's own directory (a file can be compiled into
        # several targets, e.g. a library and its test harness).
        $parent = Split-Path (Split-Path $file -Parent) -Leaf
        $narrow = @($cands | Where-Object { $_ -match "^$([regex]::Escape($parent))[\\/]" })
        if ($narrow) { $cands = $narrow }
    }
    return $cands[0]
}

function Invoke-Build {
    $target = ''; $config = 'Release'; $obj = ''
    for ($i = 0; $i -lt $Rest.Count; $i++) {
        switch ($Rest[$i]) {
            '--config' { $config = $Rest[++$i] }
            '--target' { $target = $Rest[++$i] }
            '--obj'    { $obj = $Rest[++$i] }
            default    { if (-not $target -and -not $obj) { $target = $Rest[$i] } }
        }
    }

    $source = if ($HereWt) { $HereWt.Path } else { $MainWt }
    $path = $source
    $jobs = Get-Parallel
    $lock = Enter-BuildLock
    try {
        $bt = Get-BuildTree $path
        $dir = Join-Path $path $bt.Dir

        # A worktree without a tree of its own (the default now; --tests still creates
        # one) builds in the shared build worktree, which is checked out to this
        # worktree's tracked state below so uncommitted edits compile too.
        if ($source -ine $MainWt -and -not (Test-Path (Join-Path $dir 'CMakeCache.txt'))) {
            $path = Initialize-BuildWorktree
            $bt = Get-BuildTree $path
            $dir = Join-Path $path $bt.Dir
            Info "building $(Split-Path $source -Leaf) in the shared build worktree"
        }

        # Called even when the tree is already there: Initialize-BuildTree returns straight
        # away unless the tree was configured without the tests it now needs.
        if ($path -ine $MainWt -or -not (Test-Path (Join-Path $dir 'CMakeCache.txt'))) {
            $branchArg = if ($path -ieq $MainWt) { '' } elseif ($path -ieq (Get-BuildWorktreePath)) { 'skooma-build' } else { $HereWt.Branch }
            if (-not (Initialize-BuildTree $path -Branch $branchArg)) { Fail "no build tree at $dir" }
        }
        if ($bt.Ninja -and -not (Enter-DevShell)) { Fail 'could not enter the Visual Studio build environment' }
        if ($source -ine $path) { Set-BuildWorktreeState -BuildPath $path -SourcePath $source }

        # --obj: compile a single translation unit, no archive, no link.
        if ($obj) {
            if (-not $bt.Ninja) { Fail '--obj needs a Ninja build tree (worktrees only; the main checkout uses Visual Studio)' }
            Push-Location $path
            try {
                $t = Resolve-ObjTarget $bt.Dir $obj $config
                if (-not $t) { Fail "no object target found for '$obj' (is it in the build, and spelled right?)" }
                Say ''
                Info "$path  [$($bt.Dir), $config]"
                Info "compiling $t"
                $sw = [Diagnostics.Stopwatch]::StartNew()
                & ninja -C $bt.Dir -j $jobs -f "build-$config.ninja" $t
                $code = $LASTEXITCODE
                $sw.Stop()
            } finally { Pop-Location }
            Say ''
            if ($code -eq 0) { Good ("compiled in {0:N1}s" -f $sw.Elapsed.TotalSeconds) }
            else { Fail ("compile failed (exit $code) after {0:N1}s" -f $sw.Elapsed.TotalSeconds) }
            Say ''
            return
        }

        $cmakeArgs = @('--build', $bt.Dir, '--config', $config, '--parallel', $jobs)
        if ($target) { $cmakeArgs += @('--target', $target) }

        Say ''
        Info "$path  [$($bt.Dir), $config$(if ($target) { ", target $target" })]"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        Push-Location $path
        try { & cmake @cmakeArgs; $code = $LASTEXITCODE } finally { Pop-Location }
        $sw.Stop()
        Say ''
        if ($code -eq 0) { Good ("build ok in {0:N1}s" -f $sw.Elapsed.TotalSeconds) }
        else { Fail ("build failed (exit $code) after {0:N1}s" -f $sw.Elapsed.TotalSeconds) }
        Say ''
    } finally { $lock.Dispose() }
}

# -------------------------------------------------------------------- test

# git skooma test [--match <regex>] [--label <label>] [--config <cfg>] [--bench] [--no-build]
# Runs CTest in this worktree's build tree. Tests exist only in a tree configured with
# --tests (git skooma new --tests); GL tests carry the label gl, benchmarks the label
# benchmark (excluded unless --bench or --label benchmark). The test executables are not
# part of a library build, so the selected tests are built first (--no-build skips that).
# Output goes to the console and, when V:\Scratch exists, to V:\Scratch\<name>\ctest-<config>.log.
# (Long option names on purpose: a single-letter -R binds to the script's -Rest parameter.)
function Invoke-Test {
    $config = 'Release'; $regex = ''; $label = ''; $bench = $false; $build = $true
    for ($i = 0; $i -lt $Rest.Count; $i++) {
        switch ($Rest[$i]) {
            '--config'   { $config = $Rest[++$i] }
            '--match'    { $regex = $Rest[++$i] }
            '--label'    { $label = $Rest[++$i] }
            '--bench'    { $bench = $true }
            '--no-build' { $build = $false }
        }
    }
    $source = if ($HereWt) { $HereWt.Path } else { $MainWt }
    $path = $source
    $bt = Get-BuildTree $path
    $dir = Join-Path $path $bt.Dir

    # A worktree without a tree of its own tests in the shared build worktree, checked out
    # to this worktree's tracked state first so the tests that run are its own. The lock is
    # held from that checkout until CTest exits: tests read shaders and data from the source
    # tree, and another branch's build re-checking it out mid-run would test the wrong branch.
    $held = $null
    if ($source -ine $MainWt -and -not (Test-Path (Join-Path $dir 'CTestTestfile.cmake'))) {
        $path = Initialize-BuildWorktree
        $bt = Get-BuildTree $path
        $dir = Join-Path $path $bt.Dir
        Info "testing $(Split-Path $source -Leaf) in the shared build worktree"
        if (-not (Initialize-BuildTree $path -Branch 'skooma-build')) { Fail "no build tree at $dir" }
        $held = Enter-BuildLock
        Set-BuildWorktreeState -BuildPath $path -SourcePath $source
    }
    if (-not (Test-Path (Join-Path $dir 'CTestTestfile.cmake'))) {
        Fail "no tests in $dir — re-run 'git skooma build' to configure them in"
    }
    if ($bt.Ninja -and -not (Enter-DevShell)) { Fail 'could not enter the Visual Studio build environment' }

    # CTestTestfile.cmake describes the tree CMake last generated for. After a
    # source switch in the shared worktree, or a CMakeLists edit anywhere, it is
    # stale until the next build makes Ninja re-run CMake -- and the listing
    # below would then say no tests match. Ask Ninja for build.ninja first: it
    # regenerates only when the inputs changed and costs nothing otherwise.
    if ($bt.Ninja) {
        $lock = if ($held) { $null } else { Enter-BuildLock }
        Push-Location $path
        try { & ninja -C $bt.Dir build.ninja | Out-Null } finally { Pop-Location; if ($lock) { $lock.Dispose() } }
    }

    $ctestArgs = @('--test-dir', $dir, '--output-on-failure', '-C', $config)
    if ($regex) { $ctestArgs += @('-R', $regex) }
    if ($label) { $ctestArgs += @('-L', $label) }
    elseif ($bench) { $ctestArgs += @('-L', 'benchmark', '-V') }
    else { $ctestArgs += @('-LE', 'benchmark') }

    # Build what is about to run. `ctest -N` with the same selection lists the test names;
    # a unit test's target is its test name, an integration test drops the _RUNNER_, and a
    # benchmark's target is its name (BENCH_<x>). Skipped with --no-build.
    if ($build) {
        $listArgs = @('--test-dir', $dir, '-N', '-C', $config)
        if ($regex) { $listArgs += @('-R', $regex) }
        if ($label) { $listArgs += @('-L', $label) }
        elseif ($bench) { $listArgs += @('-L', 'benchmark') }
        else { $listArgs += @('-LE', 'benchmark') }
        Push-Location $path
        try { $listed = & ctest @listArgs 2>$null } finally { Pop-Location }
        $targets = @($listed | ForEach-Object {
            if ($_ -match '^\s*Test\s+#\d+:\s+(\S+)') { $Matches[1] -replace '^INTEGRATION_TEST_RUNNER_', 'INTEGRATION_TEST_' }
        } | Where-Object { $_ } | Sort-Object -Unique)
        if (-not $targets) { Fail "no tests match$(if ($regex) { " '$regex'" })$(if ($label) { " label '$label'" })" }
        Info "building $($targets.Count) test target(s)..."
        $buildArgs = @('--build', $dir, '--config', $config, '--parallel', (Get-Parallel), '--target') + $targets
        $lock = if ($held) { $null } else { Enter-BuildLock }
        Push-Location $path
        try { & cmake @buildArgs | Out-Null; $bcode = $LASTEXITCODE } finally { Pop-Location; if ($lock) { $lock.Dispose() } }
        if ($bcode -ne 0) { Fail "test build failed (exit $bcode); run: git skooma build $($targets[0])" }
    }

    $scratch = ''
    if ($HereWt -and $HereWt.Branch) {
        $class = Get-Class $HereWt.Branch
        $name = $HereWt.Branch
        if ($class -and $name.StartsWith("$class-")) { $name = $name.Substring($class.Length + 1) }
        $s = "V:\Scratch\$($name -replace '/', '-')"
        if (Test-Path 'V:\Scratch') {
            New-Item -ItemType Directory -Force -Path $s | Out-Null
            $scratch = Join-Path $s "ctest-$config.log"
        }
    }

    Say ''
    Info "$path  [$($bt.Dir), $config$(if ($regex) { ", -R $regex" })$(if ($label) { ", -L $label" })$(if ($bench) { ', benchmarks' })]"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Push-Location $path
    try {
        if ($scratch) { & ctest @ctestArgs 2>&1 | Tee-Object -FilePath $scratch; $code = $LASTEXITCODE }
        else { & ctest @ctestArgs; $code = $LASTEXITCODE }
    } finally { Pop-Location; if ($held) { $held.Dispose() } }
    $sw.Stop()
    Say ''
    if ($scratch) { Info "log: $scratch" }
    if ($code -eq 0) { Good ("tests ok in {0:N1}s" -f $sw.Elapsed.TotalSeconds) }
    else { Fail ("tests failed (exit $code) after {0:N1}s" -f $sw.Elapsed.TotalSeconds) }
    Say ''
}

# -------------------------------------------------------------------- status

function Invoke-Status {
    $lastGood = Cfg 'skooma.lastgood'
    Say ''
    Say "Repo   $MainWt"
    if ($HereWt) {
        $class = Get-Class $HereWt.Branch
        Say "Here   $($HereWt.Path)  [$($HereWt.Branch)$(if ($class) { " / $class" })]"
    } else { Say "Here   $Here  (not a registered worktree)" }

    $up = Get-Counts 'develop' 'next'
    Say ''
    Say "next     $(G rev-parse --short next)   upstream/$UpstreamBranch = $(G rev-parse --short "upstream/$UpstreamBranch")"
    Say "develop  $(G rev-parse --short develop)   $($up.Ahead) behind next, $($up.Behind) own commits"
    if ($lastGood) { Say "         last-known-good $($lastGood.Substring(0,[Math]::Min(9,$lastGood.Length)))  (git skooma undo)" }

    Say ''
    Say 'Worktrees                     (indented = built on the branch above)'
    $byBranch = @{}
    $featBranches = @()
    $rows = @()
    foreach ($w in $Worktrees) {
        if ($w.Path -ieq $MainWt) { $rows += $w; continue }
        if ($w.Branch -and (Get-Role $w.Branch)) { $byBranch[$w.Branch] = $w; $featBranches += $w.Branch }
    }
    foreach ($b in (Sort-ByDependency $featBranches)) { if ($byBranch.ContainsKey($b)) { $rows += $byBranch[$b] } }
    foreach ($w in $Worktrees) {
        if ($w.Path -ieq $MainWt) { continue }
        if (-not ($w.Branch -and (Get-Role $w.Branch))) { $rows += $w }
    }

    foreach ($w in $rows) {
        $role = Get-Class $w.Branch
        if ($w.Path -ieq $MainWt) { $role = 'main' }
        elseif ($w.Path -ieq (Get-BuildWorktreePath)) { $role = 'build' }
        $flags = @()
        if (Test-Dirty $w.Path) { $flags += 'dirty' }
        $op = Test-MidOperation $w.Path
        if ($op) { $flags += "mid-$op" }
        $ab = ''
        $indent = ''
        if ($role -and $role -ne 'main' -and $w.Branch) {
            $base = Get-Base $w.Branch
            $indent = '  ' * (Get-Depth $w.Branch)
            $into = Get-LandTarget $w.Branch
            $d = Get-Counts $into $w.Branch
            $b = Get-Counts $w.Branch $base
            $ab = "+$($d.Ahead) vs $into, $($b.Ahead) behind $base"
        }
        $name = $w.Path.Replace($MainWt, '.')
        $cell = if ($w.Branch) { "$indent$($w.Branch)" } else { '(detached)' }
        Say ("  {0,-34} {1,-30} {2,-9} {3} {4}" -f $name, $cell, $role, $ab, ($flags -join ','))
    }
    Say ''
}

# ----------------------------------------------------------------------- new

# 'my Cool Feature!' -> 'my-cool-feature';  'Atmo/Key Frames' -> 'atmo/key-frames'
function ConvertTo-Slug { param([string]$s)
    $s = $s.Trim().ToLowerInvariant() -replace '[^a-z0-9._/-]+', '-'
    $s = ($s -split '/' | Where-Object { $_ } | ForEach-Object { $_.Trim('-') }) -join '/'
    return $s
}

# A namespaced name (atmo/weather) means "part of the atmo feature", and parts build
# on the family's core. The default base is therefore the group's root: the one
# member whose own base is next. Deeper chains stay explicit via --base.
function Get-GroupRoot { param([string]$name)
    if ($name -notmatch '/') { return '' }
    $group = ($name -split '/')[0]
    $globs = @(Get-ClassPrefixes | ForEach-Object { "refs/heads/$_-$group/*" })
    $members = @(G for-each-ref --format='%(refname:short)' @globs) -split "`n" |
        Where-Object { $_ }
    if (-not $members) { return '' }                      # first in the group: it is the root
    $roots = @($members | Where-Object { (Get-Base $_) -eq 'next' })
    if ($roots) { return @($roots | Sort-Object)[0] }
    return @($members | Sort-Object)[0]                   # no clear root; oldest-named member
}

# Every existing working branch, whatever class prefix it carries.
# Filtered here rather than by ref glob: for-each-ref's * stops at /, so feature-* misses feature-nav/x.
function Get-WorkingBranches {
    $prefixes = @(Get-ClassPrefixes)
    return @((G for-each-ref --format='%(refname:short)' 'refs/heads/') -split "`n" |
        Where-Object { $b = $_; $b -and ($prefixes | Where-Object { $b -like "$_-*" }) })
}

# A name may only be used once across all classes: the worktree path is the name alone.
function Find-BranchByName { param([string]$name)
    foreach ($c in Get-ClassPrefixes) {
        if (GitOk show-ref --verify --quiet "refs/heads/$c-$name") { return "$c-$name" }
    }
    return ''
}

# Ask what kind of branch this is. Numbers pick from the list; anything else typed
# is taken as a new class, which Add-Class then remembers for future runs.
function Read-ClassChoice {
    $known = @(Get-ClassPrefixes)
    Say ''
    Info 'classification (Enter for feature):'
    for ($i = 0; $i -lt $known.Count; $i++) { Info ("    {0}) {1}" -f ($i + 1), $known[$i]) }
    Info ("    {0}) other..." -f ($known.Count + 1))
    for ($try = 0; $try -lt 3; $try++) {
        $pick = (Read-Host '  class [feature]').Trim()
        if (-not $pick) { return 'feature' }
        if ($pick -match '^[0-9]+$') {
            $n = [int]$pick
            if ($n -ge 1 -and $n -le $known.Count) { return $known[$n - 1] }
            if ($n -eq $known.Count + 1) {
                $custom = ConvertTo-Slug (Read-Host '  new class (e.g. refactor)')
                if (Test-ClassOk $custom) { return $custom }
                Warn 'not a usable class name'
                continue
            }
            Warn "no such choice: $pick"
            continue
        }
        $custom = ConvertTo-Slug $pick
        if (Test-ClassOk $custom) { return $custom }
        Warn "not a usable class name: $pick"
    }
    Fail 'no usable classification given'
}

function Test-SlugOk { param([string]$s)
    if ($s -notmatch '^[a-z0-9][a-z0-9._/-]*$') { return $false }
    if ($s -match '\.\.|//|\.lock(/|$)|/$') { return $false }
    return $true
}

function Invoke-New {
    # --base <branch> and --class <kind> may appear anywhere; everything else is positional.
    # --tests configures the worktree with the unit and GL tests on (git skooma test runs them).
    $argv = @(); $base = ''; $class = ''; $tests = $false
    for ($i = 0; $i -lt $Rest.Count; $i++) {
        if ($Rest[$i] -in '--base', '-b')      { $base = $Rest[$i + 1]; $i++ }
        elseif ($Rest[$i] -in '--class', '-c') { $class = $Rest[$i + 1]; $i++ }
        elseif ($Rest[$i] -eq '--tests')       { $tests = $true }
        elseif ($Rest[$i] -eq '--no-configure') { }
        else { $argv += $Rest[$i] }
    }
    $name = if ($argv.Count -ge 1) { $argv[0] } else { '' }
    $desc = if ($argv.Count -ge 2) { $argv[1] } else { '' }

    # No name on the command line: ask, but only if someone is actually there to
    # answer — a redirected stdin means a script or button runner, so fail loudly
    # rather than hang waiting on a prompt.
    if (-not $name) {
        if ($script:Porcelain -or [Console]::IsInputRedirected) {
            Fail 'usage: git skooma new <name> ["one-line description"] [--class fix] [--base <branch>]'
        }
        Say ''
        Say 'New branch'
        Say ''
        for ($i = 0; $i -lt 3 -and -not $name; $i++) {
            $raw = Read-Host '  name (e.g. atmo/keyframes)'
            if (-not $raw) { continue }
            $slug = ConvertTo-Slug $raw
            if (-not (Test-SlugOk $slug)) { Warn "cannot use '$raw' as a branch name"; continue }
            if ($slug -ne $raw.Trim().ToLowerInvariant()) { Info "using '$slug'" }
            $taken = Find-BranchByName $slug
            if ($taken) { Warn "$taken already exists"; continue }
            $name = $slug
        }
        if (-not $name) { Fail 'no usable name given' }
        $desc = Read-Host '  one-line description (optional)'

        # Classification picks the branch prefix. Anything typed that is not on the
        # list becomes a new class and is remembered for next time.
        if (-not $class) { $class = Read-ClassChoice }

        # Offer the inferred base (group root, or next) and let it be overridden.
        $guess = Find-TrunkFor $name
        if (-not $guess -or $guess -eq "$class-$name") { $guess = Get-GroupRoot $name }
        if (-not $guess) { $guess = 'next' }
        $existing = Get-WorkingBranches
        if ($existing) {
            Say ''
            Info 'builds on (Enter to accept):'
            foreach ($e in @('next') + $existing) { Info "    $e" }
            $pick = Read-Host "  base [$guess]"
            $base = if ($pick) { $pick } else { $guess }
        }
        Say ''
    }

    if (-not $class) { $class = 'feature' }
    $class = ConvertTo-Slug $class
    if (-not (Test-ClassOk $class)) { Fail "invalid class '$class' (lowercase letters, digits and -)" }
    Add-Class $class

    $name = ConvertTo-Slug $name
    if (-not (Test-SlugOk $name)) { Fail 'invalid name (lowercase letters, digits, and . _ - / )' }

    # An area with a trunk: the new branch lands into it and, unless --base says
    # otherwise, is cut from it, so it starts with everything the area already has.
    $trunk = Find-TrunkFor $name
    if ($trunk -eq "$class-$name") { $trunk = '' }
    if ($trunk) {
        if (-not (GitOk show-ref --verify --quiet "refs/heads/$trunk")) {
            Fail "'$name' belongs to trunk $trunk, which does not exist yet; create it first (git skooma new $($trunk -replace '^[a-z0-9-]+?-', ''))"
        }
        if (-not $base) { $base = $trunk; Info "'$name' lands into trunk $trunk; basing on it (use --base to stack on a sibling)" }
    }

    # Resolve and validate the base branch.
    if (-not $base) {
        $base = Get-GroupRoot $name
        if ($base) { Info "'$name' joins an existing feature group; basing on $base (use --base next to override)" }
        else { $base = 'next' }
    }
    if ($base -ne 'next') {
        $resolved = ''
        foreach ($cand in @($base) + @(Get-ClassPrefixes | ForEach-Object { "$_-$base" })) {
            if (GitOk show-ref --verify --quiet "refs/heads/$cand") { $resolved = $cand; break }
        }
        if (-not $resolved) { Fail "base branch '$base' does not exist" }
        if (-not (Get-Role $resolved)) { Fail "base '$resolved' is not a Skooma branch" }
        $base = $resolved
    }
    $branch = "$class-$name"
    $path   = Join-Path $WtRoot ($name -replace '/', '\')

    if (GitOk show-ref --verify --quiet "refs/heads/$branch") { Fail "branch $branch already exists" }
    if (Test-Path $path) { Fail "$path already exists" }
    # refs/heads/feature-atmo and refs/heads/feature-atmo/x cannot coexist (one is a file, the other a directory).
    $conflict = @(G for-each-ref --format='%(refname:short)' 'refs/heads/*') -split "`n" |
        Where-Object { $_ -and ($branch.StartsWith("$_/") -or $_.StartsWith("$branch/")) }
    if ($conflict) { Fail "branch name clashes with existing branch '$($conflict[0])' (git cannot have both)" }

    if ($base -eq 'next') { Invoke-Sync -Quiet -NextOnly }
    Say ''
    Info "creating worktree for $branch from $base"
    $null = G worktree add --no-track -b $branch $path $base
    if ($script:GitExit -ne 0) { Fail "git worktree add failed: $(G worktree add --no-track -b $branch $path $base)" }

    $null = G config "branch.$branch.skooma" $class
    $null = G config "branch.$branch.skooma-base" $base
    if ($trunk) { $null = G config "branch.$branch.skooma-trunk" $trunk }
    $null = G config "branch.$branch.remote" 'origin'
    $null = G config "branch.$branch.merge" "refs/heads/$base"  # a reflexive `git pull` takes the base, not develop
    if ($desc) { $null = G config "branch.$branch.description" $desc }
    if ($tests) { $null = G config "branch.$branch.skooma-tests" 'true' }

    Initialize-Submodules $path
    Invoke-WtSetup $path

    # Scratch space for benchmarks, captures and graphs — keeps artifacts out of the
    # branch, which has to stay clean enough to send upstream.  Created by whoever first writes to it.
    $scratch = "V:\Scratch\$($name -replace '/', '-')"

    # Feature worktrees are source checkouts: `git skooma build` runs in the shared
    # build worktree, which is already warm at next.  Only --tests needs a local tree,
    # because CTest must run against this worktree's own configuration.
    if (-not ($Rest -contains '--no-configure')) {
        if ($tests) { $null = Initialize-BuildTree $path -Branch $branch }
        else { Info "no local build tree; git skooma build runs in $(Get-BuildWorktreePath)" }
    }

    if ($script:Porcelain) { Write-Output $path; return }

    Say ''
    Good "worktree ready: $path"
    Say ''
    Say '  build (shared Ninja tree, warm at next):'
    Say "    cd $path"
    Say '    git skooma build                 # everything'
    Say '    git skooma build llrender        # one library, much faster'
    Say ''
    Say "  benchmarks / captures / graphs:  $scratch"
    Say ''
    Say '  launch an agent there:'
    Say "    claude          --add-dir $MainWt\doc\skooma"
    Say "    codex --cd $path"
    Say "    opencode        # run from $path"
    Say ''
    Say '  land the work:  git skooma integrate'
    Say ''
}

# ---------------------------------------------------------------------- sync

function Invoke-Sync { param([switch]$Quiet, [switch]$NextOnly)
    if (-not $Quiet) { Say '' ; Say 'Fetching upstream...' }

    $null = G fetch upstream --prune
    if ($script:GitExit -ne 0) { Fail 'git fetch upstream failed' }

    # Warn if upstream moved its main branch (D15).
    $remoteHead = G rev-parse --abbrev-ref "upstream/HEAD"
    if ($script:GitExit -eq 0 -and $remoteHead -and $remoteHead -ne "upstream/$UpstreamBranch") {
        Warn "upstream default branch looks like '$remoteHead', configured skooma.upstreamBranch=$UpstreamBranch"
    }

    # next := upstream/<branch>, but only ever forward. Two-step because a
    # `fetch upstream next:next` updates refs/heads/next *before* the tracking
    # ref, which the guard hook cannot validate. See .review-verdict.md.
    $old = G rev-parse next
    $new = G rev-parse "upstream/$UpstreamBranch"
    if ($script:GitExit -ne 0) { Fail "upstream/$UpstreamBranch does not exist" }

    if ($old -ne $new) {
        if (-not (GitOk merge-base --is-ancestor $old $new)) {
            Fail @"
next would move non-fast-forward ($($old.Substring(0,9)) -> $($new.Substring(0,9))).
Either upstream rewrote history, or a commit was made on next.
  inspect:  git log --oneline upstream/$UpstreamBranch..next
  recover:  git skooma retarget   (not implemented; do it by hand, see WORKFLOW.md)
"@
        }
        $null = GG update-ref refs/heads/next $new $old
        if ($script:GitExit -ne 0) { Fail 'could not fast-forward next' }
        Good "next $($old.Substring(0,9)) -> $($new.Substring(0,9))"
    }
    elseif (-not $Quiet) { Info 'next already up to date' }
    if ($NextOnly) { return }

    # Merge each branch's own base into it: next for develop and top-level features,
    # the parent branch for sub-features. Parents are processed before children so a
    # single sync carries upstream all the way down a chain.
    # The shared build worktree usually sits detached at whatever branch was built
    # last.  Attach it to skooma-build so the normal merge below keeps it current
    # with next; skip while a build is running, or the checkout would fight it.
    $buildWt = $Worktrees | Where-Object { $_.Path -ieq (Get-BuildWorktreePath) } | Select-Object -First 1
    if ($buildWt -and -not $buildWt.Branch -and -not (Test-BuildRunning $buildWt.Path)) {
        $out = G -C $buildWt.Path checkout --quiet 'skooma-build'
        if ($script:GitExit -eq 0) { $buildWt.Branch = 'skooma-build' }
        else { Warn "could not attach the build worktree: $out" }
    }

    $skipped = @()
    $byBranch = @{}
    foreach ($w in $Worktrees) {
        $b = if ($w.Path -ieq $MainWt) { G -C $w.Path rev-parse --abbrev-ref HEAD } else { $w.Branch }
        if ($b) { $byBranch[$b] = $w }
    }
    $ordered = @()
    if ($byBranch.ContainsKey('develop')) { $ordered += 'develop' }
    $ordered += Sort-ByDependency @($byBranch.Keys | Where-Object { $_ -ne 'develop' -and (Get-Role $_) })

    foreach ($branch in $ordered) {
        $w = $byBranch[$branch]
        $isMain = $w.Path -ieq $MainWt
        if ($isMain -and $branch -ne 'develop') { $skipped += "$($w.Path) (on $branch, not develop)"; continue }

        $mergeFrom = if ($branch -eq 'develop') { 'next' } else { Get-Base $branch }
        $behind = (Get-Counts $branch $mergeFrom).Ahead
        if ($behind -eq 0) { continue }

        $op = Test-MidOperation $w.Path
        if ($op) { $skipped += "$branch (mid-$op)"; continue }
        if (Test-Dirty $w.Path) { $skipped += "$branch (uncommitted changes)"; continue }

        Info "merging $mergeFrom into $branch ($behind commits)"
        $out = GG -C $w.Path merge --no-edit $mergeFrom
        if ($script:GitExit -ne 0) {
            Say ''
            Warn "CONFLICT merging $mergeFrom into $branch"
            Info "  resolve in: $($w.Path)"
            Info '  then: git commit  and re-run  git skooma sync'
            Say $out
            return
        }
        $null = G -C $w.Path submodule update --init --recursive
        Good "$branch updated"
    }

    foreach ($s in $skipped) { Warn "skipped $s" }

    # Worktrees hold untracked *copies* of AGENTS.md, so edits to the canonical file
    # never reach them on their own. Refresh every worktree here.
    $agents = Join-Path $MainWt 'AGENTS.md'
    if (Test-Path $agents) {
        $refreshed = 0
        foreach ($w in $Worktrees) {
            if ($w.Path -ieq $MainWt) { continue }
            $dst = Join-Path $w.Path 'AGENTS.md'
            if (-not (Test-Path $dst) -or (Get-FileHash $dst).Hash -ne (Get-FileHash $agents).Hash) {
                Copy-Item $agents $dst -Force; $refreshed++
            }
        }
        if ($refreshed) { Info "refreshed AGENTS.md in $refreshed worktree(s)" }
    }

    Backup-Docs
    if (-not $Quiet) { Good 'sync complete'; Say '' }
}

# --------------------------------------------------------------------- check

function Resolve-Target { param([string]$arg)
    if (-not $arg -or $arg -eq '.') {
        if (-not $HereWt -or -not $HereWt.Branch) { Fail 'not in a worktree with a branch; pass a name' }
        return $HereWt.Branch
    }
    $cands = @($arg) + @(Get-ClassPrefixes | ForEach-Object { "$_-$arg" }) + @("skooma-$arg")
    foreach ($cand in $cands) {
        if (GitOk show-ref --verify --quiet "refs/heads/$cand") { return $cand }
    }
    Fail "no such branch: $arg"
}

function Invoke-Check { param([string]$branch, [switch]$Quiet)
    if (-not $branch) { $branch = Resolve-Target ($(if ($Rest) { $Rest[0] } else { '' })) }
    $role = Get-Role $branch
    if (-not $role) { Fail "$branch is not a Skooma branch (no branch.$branch.skooma marker)" }

    # Everything is judged against this branch's own base: next for a top-level
    # feature, the parent branch for a sub-feature.
    $base = Get-Base $branch

    $problems = @()
    $warnings = @()

    # 1. Merge parents must all be ancestors of the base (no develop/unrelated features merged in).
    #    A sibling branch already integrated into develop is allowed with a warning: related
    #    worktrees (nav/*) ship together as one feature, so taking a landed sibling is not a leak.
    #    develop itself (its first-parent line) is never allowed.
    #    A trunk takes its worktrees' branches by merge, so for a trunk any parent is fine
    #    as long as it carries nothing that exists only on develop.
    $developSpine = $null
    $isTrunk = Test-IsTrunk $branch
    $developOnly = $null
    foreach ($sha in (G rev-list --merges "$base..$branch") -split "`n" | Where-Object { $_ }) {
        $parents = (G rev-list --parents -n 1 $sha) -split '\s+'
        foreach ($p in $parents[2..($parents.Count - 1)]) {
            if ($isTrunk -and -not (GitOk merge-base --is-ancestor $p $base)) {
                if ($null -eq $developOnly) {
                    # Reachable from develop but from no working branch: glue, setup commits, develop's own merges.
                    $developOnly = @{}
                    $not = @(Get-WorkingBranches | ForEach-Object { "^$_" })
                    foreach ($d in (G rev-list develop "^$base" @not) -split "`n") { if ($d) { $developOnly[$d] = $true } }
                }
                $leak = (G rev-list $p "^$base") -split "`n" | Where-Object { $_ -and $developOnly.ContainsKey($_) } | Select-Object -First 1
                if ($leak) { $problems += "merge $($sha.Substring(0,9)) brings in develop commit $($leak.Substring(0,9))" }
                continue
            }
            if (-not (GitOk merge-base --is-ancestor $p $base)) {
                if (GitOk merge-base --is-ancestor $p develop) {
                    if ($null -eq $developSpine) { $developSpine = @{}; foreach ($d in (G rev-list --first-parent develop) -split "`n") { if ($d) { $developSpine[$d] = $true } } }
                    if (-not $developSpine.ContainsKey($p)) {
                        $warnings += "merge $($sha.Substring(0,9)) brings in $($p.Substring(0,9)), a sibling already in develop but not from $base; it ships with this branch"
                        continue
                    }
                }
                $problems += "merge $($sha.Substring(0,9)) brings in $($p.Substring(0,9)) which is not from $base"
            }
        }
        # 2. Evil merge: fixes made *inside* a merge commit are silently dropped by rebase (see .review-verdict.md).
        if ($parents.Count -ge 3) {
            $mt = G merge-tree --write-tree $parents[1] $parents[2]
            if ($script:GitExit -eq 0) {
                $clean  = ($mt -split "`n")[0].Trim()
                $actual = G rev-parse "$sha^{tree}"
                if ($clean -match '^[0-9a-f]{40}$' -and $clean -ne $actual) {
                    $warnings += "merge $($sha.Substring(0,9)) was hand-edited; move those fixes into a follow-up commit (rebase drops them at export)"
                }
            }
        }
    }

    # 3. Fork-only paths never belong on feature branches. <SK:Nexii> markers do:
    # upstream already carries Firestorm's, and they survive the merge that a diff does not.
    if ($role -eq 'feature') {
        foreach ($f in (G diff --name-only "$base...$branch") -split "`n" | Where-Object { $_ }) {
            if (Test-Forbidden $f) { $problems += "fork-only path in feature branch: $f" }
        }
        $gen = G log --format='%H %s%n%b' "$base..$branch"
        if ($gen -match 'Generated with \[Claude Code\]') { $warnings += 'commit messages contain "Generated with" lines; strip them at export (Co-Authored-By is fine)' }
    }

    # 4. DO NOT LAND marks throwaway instrumentation. It may ride a spike into develop
    # so it runs in the F5 build, but it must never reach next — so on a branch that
    # does go upstream it is fatal, and on a spike it is only worth saying out loud.
    $dnl = @((G log --format='%h %s' --grep='DO NOT LAND' "$base..$branch") -split "`n" | Where-Object { $_ })
    if ($dnl) {
        if ($role -eq 'spike') {
            $warnings += "carries $($dnl.Count) DO NOT LAND commit(s): this branch may go to develop, never to next"
        }
        else {
            foreach ($d in $dnl) { $problems += "DO NOT LAND commit on a $role branch: $d" }
        }
    }

    if (-not $Quiet) {
        Say ''
        Say "check $branch ($role, base $base)"
        foreach ($w in $warnings) { Warn $w }
        if ($problems) { foreach ($p in $problems) { Write-Host "  x $p" -ForegroundColor Red } }
        else { Good 'ok' }
        Say ''
    }
    return , $problems
}

# ----------------------------------------------------------------- integrate

# A `--resolve` land left in the main checkout for the user to finish. Its presence is
# what lets `--continue` commit on develop; without it there is nothing to continue.
$ResolveState = Join-Path $CommonDir 'skooma-resolve.json'

function Get-Unmerged { param([string]$path)
    return @((G -C $path diff --name-only --diff-filter=U) -split "`n" | Where-Object { $_ })
}

function Invoke-IntegrateContinue {
    if (-not (Test-Path $ResolveState)) { Fail 'no integrate --resolve in progress' }
    $st = Get-Content $ResolveState -Raw | ConvertFrom-Json
    if (-not (Test-Path (Join-Path $CommonDir 'MERGE_HEAD'))) { Fail "no merge in progress in $MainWt; finish with git skooma integrate --abort" }
    $unmerged = @(Get-Unmerged $MainWt)
    if ($unmerged) { Fail "still unmerged (resolve, then git add):`n    $($unmerged -join "`n    ")" }
    $marked = @()
    foreach ($f in @($st.Files)) {
        $p = Join-Path $MainWt $f
        if ((Test-Path $p -PathType Leaf) -and (Select-String -Path $p -Pattern '^(<{7}|>{7})( |$)' -Quiet)) { $marked += $f }
    }
    if ($marked) { Fail "conflict markers left in:`n    $($marked -join "`n    ")" }

    $msg = "Merge branch '$($st.Branch)' into develop`n`nResolved conflicts with develop-only code in:`n" + ((@($st.Files) | ForEach-Object { "  $_" }) -join "`n") + "`n"
    $tmp = Join-Path $CommonDir 'skooma-resolve-msg.txt'
    [IO.File]::WriteAllText($tmp, $msg)
    $out = GG -C $MainWt commit --no-edit -F $tmp
    Remove-Item $tmp -ErrorAction SilentlyContinue
    if ($script:GitExit -ne 0) { Say $out; Fail 'commit failed; the merge is still in progress' }
    Remove-Item $ResolveState
    $null = G -C $MainWt submodule update --init --recursive

    Say ''
    Good "$($st.Branch) -> develop (merge commit, $(@($st.Files).Count) resolved file(s))"
    Info "develop is now $(G rev-parse --short develop)"
    Info "if the build breaks:  git skooma undo   (back to $($st.LastGood.Substring(0,9)))"
    Say ''
    Backup-Docs
}

function Invoke-IntegrateAbort {
    if (-not (Test-Path $ResolveState)) { Fail 'no integrate --resolve in progress' }
    $st = Get-Content $ResolveState -Raw | ConvertFrom-Json
    if (Test-Path (Join-Path $CommonDir 'MERGE_HEAD')) {
        $out = GG -C $MainWt merge --abort
        if ($script:GitExit -ne 0) { Say $out; Fail 'merge --abort failed' }
    }
    if ($st.PrevLastGood) { $null = G config 'skooma.lastgood' $st.PrevLastGood } else { $null = G config --unset 'skooma.lastgood' }
    Remove-Item $ResolveState
    Good "aborted; develop stays at $(G rev-parse --short develop)"
}

function Invoke-Integrate {
    if ($Rest -contains '--continue') { Invoke-IntegrateContinue; return }
    if ($Rest -contains '--abort') { Invoke-IntegrateAbort; return }
    if (Test-Path $ResolveState) { Fail 'an integrate --resolve is in progress; finish it with --continue or --abort' }

    $arg = if ($Rest -and $Rest[0] -notlike '--*') { $Rest[0] } else { '' }
    $force = $Rest -contains '--force'
    $resolve = $Rest -contains '--resolve'
    if ($resolve -and -not $arg) { Fail 'usage: git skooma integrate <name> --resolve' }

    $mainBranch = G -C $MainWt rev-parse --abbrev-ref HEAD
    if ($mainBranch -ne 'develop') { Fail "main checkout is on '$mainBranch', expected develop" }
    $op = Test-MidOperation $MainWt
    if ($op) { Fail "main checkout is mid-$op; finish or abort that first" }
    if (Test-Dirty $MainWt) { Fail "main checkout has uncommitted changes; commit or stash them first`n  (git -C $MainWt status)" }
    if ((Test-BuildRunning) -and -not $force) { Fail 'a build is running; wait for it or pass --force' }

    # Which branches? Each lands into its target: its trunk if it has one, else develop.
    # A trunk that receives work then lands into develop in the same run.
    $targets = @()
    if ($arg) {
        $targets = @(Resolve-Target $arg)
        if ($resolve -and (Get-LandTarget $targets[0]) -ne 'develop') { Fail "--resolve only lands into develop; $($targets[0]) lands into $(Get-LandTarget $targets[0])" }
    }
    else {
        foreach ($w in $Worktrees) {
            if ($w.Path -ieq $MainWt -or -not $w.Branch) { continue }
            $role = Get-Role $w.Branch
            if (-not $role) { continue }
            if ((Get-Counts (Get-LandTarget $w.Branch) $w.Branch).Ahead -gt 0) { $targets += $w.Branch }
        }
    }
    # Trunks go after everything that lands into them, wherever they came from.
    foreach ($b in @($targets)) {
        $t = Get-LandTarget $b
        while ($t -ne 'develop' -and $targets -notcontains $t) { $targets += $t; $t = Get-LandTarget $t }
    }
    # Glue first (it carries the scripts and hooks), then parents before children:
    # landing a sub-feature before its parent would drag the parent in unannounced.
    # Branches bound for a trunk precede the develop-bound ones, so the trunk is complete when it lands.
    $glue = @($targets | Where-Object { (Get-Role $_) -eq 'glue' } | Sort-Object)
    $toTrunk = Sort-ByDependency @($targets | Where-Object { (Get-Role $_) -ne 'glue' -and (Get-LandTarget $_) -ne 'develop' } | Sort-Object)
    $toDevelop = Sort-ByDependency @($targets | Where-Object { (Get-Role $_) -ne 'glue' -and (Get-LandTarget $_) -eq 'develop' } | Sort-Object)
    $targets = @($glue) + @($toTrunk) + @($toDevelop)

    if (-not $targets) { Say ''; Info 'nothing to integrate'; Say ''; return }

    Say ''
    Say "Integrating: $(($targets | ForEach-Object { "$_ -> $(Get-LandTarget $_)" }) -join ', ')"
    $lastGood = G rev-parse develop
    $results = @()

    foreach ($b in $targets) {
        $into = Get-LandTarget $b
        $ahead = (Get-Counts $into $b).Ahead
        if ($ahead -eq 0) { continue }   # a trunk whose work is already in develop

        $problems = Invoke-Check $b -Quiet
        if ($problems.Count -gt 0) {
            $results += [pscustomobject]@{ Branch = $b; Result = 'SKIPPED (check)'; Detail = $problems[0] }
            foreach ($p in $problems) { Warn "$b : $p" }
            continue
        }

        # Dry run first so a conflict never dirties the F5 tree or the trunk's worktree.
        $null = G merge-tree --write-tree --name-only $into $b
        if ($script:GitExit -ne 0) {
            # A conflict with develop-only code cannot be synced away (features never merge
            # develop), so --resolve leaves it in the main checkout for the user to finish.
            if ($resolve -and $into -eq 'develop') {
                $prev = Cfg 'skooma.lastgood'
                $null = GG -C $MainWt merge --no-edit $b
                $files = @(Get-Unmerged $MainWt)
                if (-not $files) {
                    $null = G -C $MainWt merge --abort
                    Fail "merging $b failed without conflicted files; aborted"
                }
                $null = G config 'skooma.lastgood' $lastGood
                [IO.File]::WriteAllText($ResolveState, (@{ Branch = $b; LastGood = $lastGood; PrevLastGood = $prev; Files = @($files) } | ConvertTo-Json))
                Say ''
                Warn "$b conflicts with develop; the merge is left in $MainWt"
                foreach ($f in $files) { Say "    $f" }
                Say ''
                Info 'resolve each file, git add it, then:  git skooma integrate --continue'
                Info 'to give up and leave develop as it was:  git skooma integrate --abort'
                Say ''
                return
            }
            $results += [pscustomobject]@{ Branch = $b; Result = 'SKIPPED (conflict)'; Detail = "conflicts with $into" }
            Warn "$b would conflict with $into; merge $into's base into it first (git skooma sync), or land it with integrate $b --resolve if the conflict is with develop-only code"
            continue
        }

        # develop merges in the main checkout; a trunk in its own worktree.
        $at = $MainWt
        if ($into -ne 'develop') {
            $tw = $Worktrees | Where-Object { $_.Branch -eq $into } | Select-Object -First 1
            if (-not $tw) {
                $results += [pscustomobject]@{ Branch = $b; Result = 'SKIPPED (trunk)'; Detail = "$into has no worktree" }
                Warn "trunk $into has no worktree to merge in"
                continue
            }
            $op = Test-MidOperation $tw.Path
            if ($op -or (Test-Dirty $tw.Path)) {
                $results += [pscustomobject]@{ Branch = $b; Result = 'SKIPPED (trunk)'; Detail = "$into worktree is $(if ($op) { "mid-$op" } else { 'dirty' })" }
                Warn "trunk $into worktree ($($tw.Path)) is not clean"
                continue
            }
            $at = $tw.Path
        }

        $out = GG -C $at merge --no-edit $b
        if ($script:GitExit -ne 0) {
            Say $out
            $null = G -C $at merge --abort
            $results += [pscustomobject]@{ Branch = $b; Result = 'FAILED'; Detail = 'merge failed, aborted' }
            continue
        }
        if ($at -ne $MainWt) { $null = G -C $at submodule update --init --recursive }
        $ff = if ($out -match 'Fast-forward') { 'fast-forward' } else { 'merge commit' }
        $results += [pscustomobject]@{ Branch = $b; Result = "landed in $into ($ff)"; Detail = "$ahead commits" }
        Good "$b -> $into ($ff, $ahead commits)"
    }

    if ($results | Where-Object { $_.Result -like 'landed*' }) {
        $null = G config 'skooma.lastgood' $lastGood
        $null = G -C $MainWt submodule update --init --recursive
    }

    Say ''
    $results | Format-Table -AutoSize | Out-String | Write-Host
    Info "develop is now $(G rev-parse --short develop)"
    Info "if the build breaks:  git skooma undo   (back to $($lastGood.Substring(0,9)))"
    Say ''
    Backup-Docs
}

# --------------------------------------------------------------------- trunk

function Invoke-Trunk {
    if ($Rest.Count -ge 1 -and $Rest[0] -eq '--unset') {
        if ($Rest.Count -lt 2) { Fail 'usage: git skooma trunk --unset <prefix>' }
        $null = G config --remove-section "skooma-trunk.$($Rest[1])"
        Good "no trunk for $($Rest[1])"
        return
    }
    if ($Rest.Count -ge 1 -and $Rest[0] -eq '--join') {
        $b = Resolve-Target ($(if ($Rest.Count -ge 2) { $Rest[1] } else { '.' }))
        $name = $b -replace '^[a-z0-9-]+?-', ''
        $t = Find-TrunkFor $name
        if (-not $t) { Fail "no trunk matches '$name' (git skooma trunk <prefix> <branch>)" }
        if ($t -eq $b) { Fail "$b is the trunk itself" }
        $null = G config "branch.$b.skooma-trunk" $t
        Good "$b now lands into $t (its base stays $(Get-Base $b))"
        return
    }
    if ($Rest.Count -ge 2) {
        $prefix = ConvertTo-Slug $Rest[0]
        $t = $Rest[1]
        if (-not (Test-SlugOk $prefix)) { Fail "invalid prefix '$($Rest[0])'" }
        $null = G config "skooma-trunk.$prefix.branch" $t
        Good "$prefix/* lands into $t"
        if (-not (GitOk show-ref --verify --quiet "refs/heads/$t")) { Info "create it first: git skooma new $($t -replace '^[a-z0-9-]+?-', '')" }
        return
    }
    $map = Get-TrunkMap
    Say ''
    if (-not $map.Count) { Info 'no trunks; set one with git skooma trunk <prefix> <branch>'; Say ''; return }
    foreach ($p in ($map.Keys | Sort-Object)) {
        $t = $map[$p]
        $members = @(Get-WorkingBranches | Where-Object { (Cfg "branch.$_.skooma-trunk") -eq $t })
        $state = if (GitOk show-ref --verify --quiet "refs/heads/$t") { "$($members.Count) branch(es)" } else { 'missing' }
        Say ("  {0,-20} -> {1,-28} {2}" -f $p, $t, $state)
    }
    Say ''
}

# ---------------------------------------------------------------------- undo

function Invoke-Undo {
    $lastGood = Cfg 'skooma.lastgood'
    if (-not $lastGood) { Fail 'no recorded last-known-good commit (nothing integrated yet)' }
    $cur = G rev-parse develop
    if ($cur -eq $lastGood) { Info 'develop is already at the last-known-good commit'; return }
    $mainBranch = G -C $MainWt rev-parse --abbrev-ref HEAD
    if ($mainBranch -ne 'develop') { Fail "main checkout is on '$mainBranch', expected develop" }
    if (Test-Dirty $MainWt) { Fail 'main checkout has uncommitted changes' }

    Say ''
    Say "develop  $($cur.Substring(0,9))  ->  $($lastGood.Substring(0,9))"
    G -C $MainWt log --oneline "$lastGood..$cur" | Write-Host
    Say ''
    $null = GG -C $MainWt reset --hard $lastGood
    if ($script:GitExit -ne 0) { Fail 'reset failed' }
    $null = G -C $MainWt submodule update --init --recursive
    Good 'develop reset. The feature branches still have their commits; re-integrate when fixed.'
    Say ''
}

# --------------------------------------------------------------------- retire

# DO NOT LAND commits from $branch that are still sitting in develop. Scoped to
# next..develop because anything already in next is upstream's problem, not a stranded
# spike, and filtered by ancestry so retiring one spike never reverts another's work.
# Commits develop has already reverted are dropped: this answers "is the instrumentation
# still here", and the commit being in the history does not mean that it is. A cleanup
# done by hand is not detected, but then the revert below just refuses and nothing breaks.
function Get-StrandedSpikes { param([string]$branch)
    $reverted = @{}
    foreach ($m in [regex]::Matches((G log --format='%b' 'next..develop'), 'This reverts commit ([0-9a-f]{40})')) {
        $reverted[$m.Groups[1].Value] = $true
    }
    $found = @()
    foreach ($line in ((G log --format='%H %h %s' --grep='DO NOT LAND' 'next..develop') -split "`n" | Where-Object { $_ })) {
        $sha = ($line -split ' ', 2)[0]
        if ($reverted.ContainsKey($sha)) { continue }
        if (GitOk merge-base --is-ancestor $sha $branch) { $found += $line }
    }
    return , $found
}

# A branch is retirable once its content is already in its base - upstream merged the
# PR, or the work arrived some other way. Everything it introduced is reachable without
# it, so dropping the worktree and the branch loses nothing and needs no confirmation.
# --force discards a branch that still carries work: a different decision with a
# different risk, so the flag is the confirmation and the tip sha is printed afterwards
# so the branch can be put back. --force overrides the content check ONLY; the other
# refusals are about correctness, not loss, and still apply.
function Invoke-Retire {
    $force = $Rest -contains '--force'
    $named = @($Rest | Where-Object { $_ -notlike '--*' })
    $branch = Resolve-Target ($(if ($named) { $named[0] } else { '' }))
    if (-not (Get-Role $branch)) { Fail "$branch is not a Skooma branch (no branch.$branch.skooma marker)" }

    $base  = Get-Base $branch
    $class = Cfg "branch.$branch.skooma"
    $wt    = $Worktrees | Where-Object { $_.Branch -eq $branch } | Select-Object -First 1

    if ($wt -and $wt.Path -ieq $MainWt)                   { Fail "$branch is checked out in the main checkout" }
    if ($wt -and $HereWt -and $HereWt.Path -ieq $wt.Path) { Fail "you are standing in $($wt.Path); run retire from the main checkout" }
    if ($wt -and (Test-Dirty $wt.Path))                   { Fail "$($wt.Path) has uncommitted changes" }
    $op = if ($wt) { Test-MidOperation $wt.Path } else { '' }
    if ($op)                                              { Fail "$($wt.Path) is mid-$op; finish or abort that first" }

    # Get-Base silently answers 'next' once a parent ref is gone, so retiring out of
    # order would quietly change what `check` measures a sub-feature against.
    $children = @(Get-WorkingBranches | Where-Object { $_ -ne $branch -and (Get-Base $_) -eq $branch })
    if ($children) { Fail "these are based on ${branch}: $($children -join ', ')`n    retire those first, or re-base them onto $base" }

    # A spike is allowed to merge its DO NOT LAND instrumentation into develop. Deleting
    # the branch does not take it back out again — and worse, the $carries check below is
    # *satisfied* by it, because the content really is in the base. That is how a whole
    # probe spike once stayed in develop after its branch was retired without complaint.
    # So look for the commits themselves, not for a diff.
    $stranded = Get-StrandedSpikes $branch
    if ($stranded) {
        $mainBranch = G -C $MainWt rev-parse --abbrev-ref HEAD
        if ($mainBranch -ne 'develop') { Fail "$branch left DO NOT LAND commits in develop, and the main checkout is on '$mainBranch'`n    put it back on develop so retire can revert them" }
        $mop = Test-MidOperation $MainWt
        if ($mop)                      { Fail "main checkout is mid-$mop; finish or abort that first" }
        if (Test-Dirty $MainWt)        { Fail "main checkout has uncommitted changes; commit or stash them first`n  (git -C $MainWt status)" }
        if (-not $force) {
            Say ''
            Warn "$branch merged $($stranded.Count) DO NOT LAND commit(s) into develop:"
            Say ''
            foreach ($s in $stranded) { Info (($s -split ' ', 2)[1]) }
            Say ''
            Info 'deleting the branch does not remove those. to retire and revert them:'
            Info "  git skooma retire $($named[0]) --force"
            Say ''
            exit 1
        }
    }

    $carries = -not (GitOk diff --quiet "$base...$branch")
    if ($carries -and -not $force) {
        Say ''
        Warn "$branch still carries work that is not in $base"
        Say ''
        G log --oneline "$base..$branch" | Write-Host
        Say ''
        Info "retire only removes a branch whose work already landed in $base."
        Info "to discard it anyway:  git skooma retire $($named[0]) --force"
        Say ''
        exit 1
    }

    $tip = G rev-parse $branch

    Say ''
    Say "retire $branch (base $base)"
    if ($carries) {
        Warn "--force: discarding work that is not in $base"
        G log --oneline "$base..$branch" | Write-Host
        Say ''
    }

    # Revert before anything is deleted, so a conflict leaves the branch intact to retry
    # from. git log is newest-first, which is already the right order to revert in.
    if ($stranded) {
        Info "reverting $($stranded.Count) DO NOT LAND commit(s) from develop"
        $before = G -C $MainWt rev-parse HEAD
        foreach ($s in $stranded) {
            $parts = $s -split ' ', 3
            $out = GG -C $MainWt revert --no-edit $parts[0]
            if ($script:GitExit -ne 0) {
                # A commit whose changes are already gone reverts to nothing, and git
                # reports that as a failure. It is not one: there is nothing left to undo,
                # which is the state we were trying to reach. --quit clears the sequencer
                # without touching the reverts already made.
                if ($out -match 'nothing to commit') {
                    $null = G -C $MainWt revert --quit
                    Info "skipped $($parts[1]) (nothing left to revert)"
                    continue
                }
                $null = G -C $MainWt revert --abort
                $null = GG -C $MainWt reset --hard $before
                Fail "revert of $($parts[1]) failed; develop is back at $($before.Substring(0,9)) and $branch is untouched`n$out"
            }
            Good "reverted $($parts[1]) $($parts[2])"
        }
        $null = G config 'skooma.lastgood' $before
        Say ''
    }

    if ($wt) {
        $out = G worktree remove $wt.Path
        if ($script:GitExit -ne 0) {
            # Every Skooma worktree has submodules, and git refuses those outright;
            # -f is what clears that guard. Match on the message so any other refusal -
            # locked, in use, untracked files - still stops us, because -f waives those
            # too and the checks above only cover tracked changes.
            if ($out -notmatch 'submodule') { Fail "worktree remove failed: $out" }
            Info 'worktree holds submodules; removing with --force'
            $out = G worktree remove --force $wt.Path
            if ($script:GitExit -ne 0) { Fail "worktree remove --force failed: $out" }
        }
        Good "removed worktree $($wt.Path)"
    }

    # -d refuses unless the branch merged into HEAD; the diff above already proved the
    # stronger property (base contains its content), so -D is the correct fallback.
    $null = GG branch -d $branch
    if ($script:GitExit -ne 0) {
        $null = GG branch -D $branch
        if ($script:GitExit -ne 0) { Fail "branch delete failed: $(GG branch -D $branch)" }
    }
    Good "deleted branch $branch"

    foreach ($k in 'skooma', 'skooma-base', 'remote', 'merge', 'description') {
        $null = G config --unset "branch.$branch.$k"
    }

    $name = if ($class) { $branch -replace "^$([regex]::Escape($class))-", '' } else { $branch }
    $scratch = "V:\Scratch\$($name -replace '/', '-')"
    Say ''
    if ($carries) { Info "discarded work is still reachable: git branch $branch $($tip.Substring(0,9))" }
    if (Test-Path $scratch) { Info "$scratch is left in place; delete it yourself if you want the space" }
    Info "the remote branch, if any, is untouched: git push origin --delete $branch"
    Say ''
}

# ------------------------------------------------------------------ bootstrap

function Invoke-Bootstrap {
    Say ''
    Say 'Bootstrapping Skooma workflow...'

    # 1. Copy scripts out of the working tree so a dirty/mid-merge develop cannot brick the tooling.
    $dst = Join-Path $CommonDir 'skooma'
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    $src = Join-Path $MainWt 'scripts\skooma'
    Copy-Item (Join-Path $src 'skooma.ps1') $dst -Force
    Good "scripts -> $dst"

    # 2. Hooks (sh, shared by every worktree via .git/hooks).
    $hookDst = Join-Path $CommonDir 'hooks'
    New-Item -ItemType Directory -Force -Path $hookDst | Out-Null
    foreach ($h in 'reference-transaction', 'pre-commit') {
        $s = Join-Path $src "hooks\$h"
        if (Test-Path $s) {
            $text = (Get-Content $s -Raw) -replace "`r`n", "`n"
            [IO.File]::WriteAllText((Join-Path $hookDst $h), $text)
            Good "hook $h installed"
        }
    }

    # 3. Config.
    $alias = '!f() { d=$(git rev-parse --path-format=absolute --git-common-dir); pwsh -NoProfile -File "$d/skooma/skooma.ps1" "$@"; }; f'
    $cfg = @(
        @('alias.skooma', $alias),
        @('remote.pushDefault', 'origin'),
        @('push.default', 'current'),
        @('merge.conflictStyle', 'zdiff3'),
        @('core.longpaths', 'true'),
        @('rerere.enabled', 'true'),
        @('rerere.autoUpdate', 'true'),
        @('branch.next.remote', 'upstream'),
        @('branch.next.merge', "refs/heads/$UpstreamBranch"),
        @('branch.develop.remote', 'origin'),
        @('skooma.upstreamBranch', $UpstreamBranch),
        @('skooma.prBase', (Cfg 'skooma.prbase' 'next'))
    )
    foreach ($kv in $cfg) { $null = G config $kv[0] $kv[1] }
    Good 'git config set'

    # 4. Shared exclude entries (untracked fork files must stay invisible in feature worktrees).
    $ex = Join-Path $CommonDir 'info\exclude'
    $have = if (Test-Path $ex) { (Get-Content $ex) -join "`n" } else { '' }
    # Build dirs: upstream's .gitignore only covers build-*/, but we configure with
    # explicit -B names, so those need excluding here.
    $want = @('CLAUDE.local.md', 'doc/skooma/.*', 'doc/skooma/*/*', 'AGENTS.md',
              'vs2026-os/', 'ninja-os/', '.worktrees/', '.vcpkg-shared/')
    $add = @($want | Where-Object { $have -notmatch [regex]::Escape($_) })
    if ($add) {
        Add-Content -Path $ex -Value ($add -join "`n")
        Good "exclude += $($add -join ', ')"
    }

    Backup-Docs
    Say ''
    Good 'bootstrap complete.  Try:  git skooma status'
    Say ''
}

# ------------------------------------------------------------------ dispatch

# Any command run inside a worktree refreshes that worktree's untracked AGENTS.md
# copy, so an edit to the canonical file can never go unnoticed for long.
if ($HereWt -and $HereWt.Path -ine $MainWt) {
    $srcAgents = Join-Path $MainWt 'AGENTS.md'
    $dstAgents = Join-Path $HereWt.Path 'AGENTS.md'
    if ((Test-Path $srcAgents) -and (-not (Test-Path $dstAgents) -or
        (Get-FileHash $srcAgents).Hash -ne (Get-FileHash $dstAgents).Hash)) {
        Copy-Item $srcAgents $dstAgents -Force
    }
}

switch ($Command.ToLowerInvariant()) {
    'status'    { Invoke-Status }
    'new'       { Invoke-New }
    'wt-new'    { $script:Porcelain = $true; Invoke-New }
    'wt-setup'  { $p = if ($Rest) { $Rest[0] } else { $Here }; Invoke-WtSetup $p }
    'sync'      { Invoke-Sync }
    'check' {
        # `check` with no argument from a feature worktree checks that branch; from the
        # main checkout (or anywhere not on a Skooma branch) it checks every worktree,
        # so the button is useful wherever it is pressed.
        $bad = 0
        if (-not $Rest -and -not (Get-Role $(if ($HereWt) { $HereWt.Branch } else { '' }))) {
            $any = $false
            foreach ($w in $Worktrees) {
                if (-not $w.Branch -or -not (Get-Role $w.Branch)) { continue }
                $any = $true
                $p = Invoke-Check $w.Branch
                $bad += $p.Count
            }
            if (-not $any) { Say ''; Info 'no feature worktrees to check'; Say '' }
        }
        else {
            $p = Invoke-Check
            $bad = $p.Count
        }
        if ($bad) { exit 1 }
    }
    'integrate' { Invoke-Integrate }
    'trunk'     { Invoke-Trunk }
    'undo'      { Invoke-Undo }
    'retire'    { Invoke-Retire }
    'build'     { Invoke-Build }
    'test'      { Invoke-Test }
    'clean'     { Invoke-Clean }
    'bootstrap' { Invoke-Bootstrap }
    default     { Show-Help }
}
