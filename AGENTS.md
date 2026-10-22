# Alchemy: Skooma — rules for agents

Fork of the Alchemy Second Life viewer. Full model: `D:\viewers\alchemy-skooma\doc\skooma\WORKFLOW.md`.

## Where you are

Run `git skooma status` first if unsure. Branch roles:

| Branch | Meaning |
|---|---|
| `develop` | Read-only mirror of upstream. Never commit, merge, checkout or reset onto it. |
| `main` | Integration branch + the human's F5 build. Do not do feature work here. |
| `feature-<name>` | Your workspace, in `.worktrees/<name>`. Branched from `develop`, or from its area's trunk. |
| trunk | The feature branch an area ships as: `feature-navmesh` (`nav/*`), `feature-atmo-magic` (`atmo/*`), `feature-combat/minimap` (`combat/minimap*`), `camera` (`camera/*`). Worktrees of the area land into it, and it lands into `main`. `git skooma trunk` lists them. |
| `fix-<name>`, `workaround-<name>`, … | The same kind of branch under a different classification chosen at `git skooma new`. Treat exactly like `feature-*`. |
| `spike-<name>` | Throwaway instrumentation to answer one question. The only branch that never goes upstream. See below. |
| `glue` | Fork-only files (scripts, docs, CI, branding). |

### `DO NOT LAND` means never reaches `develop` — not never reaches `main`

A spike exists to measure something, so its instrumentation has to run in a real
build. Mark each such commit with a `DO NOT LAND:` subject prefix. Then:

- It **may** be integrated into `main`, so it runs in the human's F5 build.
- It **must never** reach `develop`. `git skooma check` makes a `DO NOT LAND` commit
  fatal on any branch that goes upstream, and only a warning on `spike-*`.
- Retiring the branch is **not** the same as discarding the spike. `git skooma retire`
  refuses while the branch's `DO NOT LAND` commits are still in `main`, and
  `--force` reverts them out before deleting anything.

Do not hand-delete spike code from `main` — let `retire --force` do it, so the
revert is recorded and `git skooma undo` still works.

## Feature branches are upstream pull requests in waiting

Every commit must be acceptable to the Alchemy project as-is.

- Write upstream-style code in upstream's layout. `SK`/`sk` prefixes on your own files and `<SK:Nexii>` wrappers on injection sites into non-Skooma files are expected — Alchemy already carries Firestorm's `<FS:ND>` / `<FS:AR>`. Keep product/channel branding out.
- Never touch `AGENTS.md`, `CLAUDE.local.md`, `doc/skooma/`, `scripts/skooma/`, `.claude/`, `.worktrees/`, `vs2026-os/`. If a change needs one of those, say so — it belongs on `glue`.
- Never merge `main` or another feature into your branch. Take upstream (and your trunk) by `git merge develop` (or `git merge <trunk>`) in your own worktree; `git skooma sync` touches every worktree, so leave it to the human.
- Small logical commits; imperative subject (`Fix crash when pasting into text editor`); body explains why. Squash WIP before landing.
- `Co-Authored-By:` trailers are fine (upstream uses them). No `Generated with` lines.
- **After `git merge develop`, put any build fixes in a separate commit** — fixes made inside a merge commit are silently dropped when the branch is rebased for a PR.

## Landing work

```
git skooma check .        # validate this branch
git skooma integrate      # land every ready worktree branch into its trunk, then trunks into main
```

In an area with a trunk, your branch lands into the trunk, not `main`. Follow-up work
goes on a new branch in the same area (it is cut from the trunk) — never a stack of
leftover branches.

Do not run `git merge`, `git rebase`, `git push`, or `git checkout <other branch>` to land work. Do not create worktrees by hand — `git skooma new <name>` (add `--class fix` / `--class workaround` when it is not a feature).

## Building

Always `git skooma build` — never bare `cmake`/`ninja`. Ninja needs the Visual Studio
environment, which the command sets up for you.

**Pick the cheapest check that answers your question.** Compile after every few edits
rather than batching changes up — at these speeds there is no reason to wait:

```
git skooma build --obj indra/newview/llenvironment.cpp   # 1-10s  does THIS FILE compile?
git skooma build llrender                                # ~3s    does the library still build?
git skooma build                                         # ~7 min whole viewer, before you land
git skooma test --match <regex>                          # build + run the matching tests
```

Use `--obj` while iterating on one file — it compiles that translation unit only, no
archive and no link, which saves about 10s per iteration on a `newview` file. Cost scales
with how many headers the file pulls in: a small `llrender` file is under a second, a
heavy `newview` one is closer to 9s. Move up to the library once it compiles, and do a
full build before `git skooma integrate`. A null build is 0.4s, so re-running a check
costs nothing.

Worktrees build with Ninja in the shared build worktree (`.worktrees/build/ninja-os/`),
which `git skooma build` checks your tracked state out into first; untracked files are
invisible until `git add`. Only a worktree made with `new --tests` has its own tree. The main checkout builds with Visual Studio
into `vs2026-os/` — that is the human's F5 build; never delete it and never build it from
a worktree session. Both build trees are excluded from git; never `git add` them.

A worktree that builds on its own is the upstream-compatibility test.

## Benchmarks, captures, graphs

Write them to `V:\Scratch\<feature-name>\`, never into the repo. Artifacts committed to a
feature branch will fail `git skooma check` and would have to be stripped before an
upstream PR. Create the folder when you first need it; it survives the worktree being deleted.

## Tracy capture — quick lookup

When asked to capture, start promptly while the viewer is alive; diagnose afterward.
Use the installed CLI directly (no MCP setup or executable search required):
`D:\viewers\libraries\tracy\official-0.14.1\tracy-capture.exe` (0.14.1).

PowerShell recipe: use the requested duration, otherwise 30 seconds; replace `tracy`
with the current feature name when known. Timestamped output avoids overwriting captures.

```powershell
$captureDir = 'V:\Scratch\tracy'
$captureSeconds = 30
$capturePort = 8086
New-Item -ItemType Directory -Force -Path $captureDir | Out-Null
$captureFile = Join-Path $captureDir ("capture-{0}-p{1}.tracy" -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'), $capturePort)
& 'D:\viewers\libraries\tracy\official-0.14.1\tracy-capture.exe' -a 127.0.0.1 -p $capturePort -s $captureSeconds -o $captureFile
if ($LASTEXITCODE -ne 0) { throw "Tracy capture failed: $LASTEXITCODE" }
Get-Item -LiteralPath $captureFile | Select-Object FullName, Length, LastWriteTime
```

- Default endpoint: `127.0.0.1:8086`. If unavailable or several viewers are running,
  inspect listeners with `Get-NetTCPConnection -State Listen | Where-Object { $_.LocalPort -ge 8086 -and $_.LocalPort -le 8095 } | Select-Object LocalAddress, LocalPort, OwningProcess`;
  identify the intended viewer by PID and pass its port with `-p`.
- Run through a tool session that can yield/poll during capture. `-s` limits recording;
  do not wait indefinitely on a connection that never succeeds. Check connection output.
- Requires a live Tracy-enabled viewer: `AL_USE_TRACY=ON` and Release or RelWithDebInfo.
  Debug/OptDebug do not enable Tracy in this repo. GPU zones additionally require
  `AL_ENABLE_TRACY_GPU=ON`; do not delay an urgent CPU capture to rebuild.
- Verify successful exit and a nonempty `.tracy` file, then report its full path.
  Open/analyze with `D:\viewers\libraries\tracy\official-0.14.1\tracy-profiler.exe`.
  Do not use `-f` (overwrite) for routine captures.
- If the viewer has already exited, a new live capture cannot recover the crash;
  inspect `%APPDATA%\AlchemyNext\logs\Alchemy.log` / `Alchemy.old` instead.
- Optional MCP workflow and troubleshooting: `D:\viewers\alchemy-skooma\doc\skooma\.tracy-capture.md`.
  A missing/broken MCP server must not delay this CLI capture path.

## Docs

Design notes live in `doc/skooma/` **in the main checkout** — they are not in your worktree.
The `CLAUDE.local.md` in your worktree gives the absolute path (it is generated, so it is
always current even if the repo moves). `*.md` there are tracked indexes, `.*.md` are
working notes.
