# Alchemy: Skooma — rules for agents

Fork of the Alchemy Second Life viewer, for experimental features (expect instability). Full model: `D:\viewers\alchemy-skooma\doc\skooma\WORKFLOW.md`. Owner: Nexii.

## Where you are

Run `git skooma status` first if unsure. Branch roles:

| Branch | Meaning |
|---|---|
| `develop` | Read-only mirror of upstream (`AlchemyViewer/Alchemy` `develop`, the PR base), moved only by `sync`. Never commit, merge, checkout or reset onto it. Older notes say `next` for it, and `develop` for what is now `main`. |
| `main` | Integration branch + the human's F5 build, in the main checkout. Do not do feature work here. |
| `feature-<name>` | Your workspace, in `.worktrees/<name>`. Branched from `develop` (or a foundation it builds on); never from a trunk or `main`, which hold many PRs at once. The branch is the PR and holds the whole feature. |
| trunk | Merge-only branch gathering an area's features before `main` (`combat`, `camera`, `render`, `ui`, …), at `.worktrees/trunk-<area>`. Never cut from or commit to one. Worktrees of the area land into it, and it lands into `main`. `git skooma trunk` lists them. A feature with follow-ups (`feature-combat/minimap`) is not a trunk. |
| `fix-<name>`, `workaround-<name>`, … | The same kind of branch under a different classification chosen at `git skooma new`. Treat exactly like `feature-*`. |
| `amend-<area>/<x>` | A follow-up to an existing feature. Lands into that feature, never a PR on its own. See Landing work. |
| `foundation-<name>` | A shared base other features build on (`foundation-warp`: SKWarp under the camera and scrolling). Cut from `develop`; branches of any trunk may `git merge` it, and `check` accepts that. Lands into `main` and ships as its own PR first. |
| `spike-<name>` | Throwaway instrumentation to answer one question. The only branch that never goes upstream. See below. |
| `backup/<branch>` | A snapshot or a parked branch (`git skooma park`/`unpark`). Sync, check, integrate and retire skip it. |
| `glue` | Fork-only files (scripts, hooks, CI, branding, this file). |

### The main checkout

`D:\viewers\alchemy-skooma` on `main` is shared by several sessions and is Nexii's F5.

- **Never edit source there**, not even a quick fix to move later: `git skooma new <area>/<name>` before the first edit. Reading and grepping are fine.
- The one exception is `doc/skooma/` docs (below): edited there and committed in the same step, without asking. Never leave `main` dirty; uncommitted work there breaks other sessions.
- **Check it is idle first, in a command of its own**: `ls .git/MERGE_HEAD .git/CHERRY_PICK_HEAD .git/REVERT_HEAD`, unfiltered `git status --porcelain`, `git reflog <branch>`. Anything in progress or not yours → stop and ask. Never chain a check with `&&` into the change (that has committed onto the wrong branch and fast-forwarded `glue` over another session's work). Same for `glue` and trunks.
- Fork-only files "vanishing" from the main checkout means a tool swapped a worktree's HEAD, not data loss: `git worktree list`, free the branch, reclaim it.

### Stay inside your branch's remit

A `foundation-*` holds only what its name and its SKOOMA.md row own; a `feature-*` holds only its feature and builds on foundations rather than re-implementing or changing what they own. No drive-by refactors, defaults or "while we're here" unifications in another branch's territory (`foundation-ui/drag-feedback` rewrote the scroll container's autoscroll ramp, which is `foundation-ui/scrolling`'s, and bought a conflict with nothing visible to show for it).

- Briefs and designs name what the branch owns and which neighbouring foundations it must not touch.
- Before landing, read `git diff --stat develop...<branch>` and ask of every file "whose job is this?". Anything belonging elsewhere is dropped or moved to the owning branch (or a new foundation), never kept for convenience.
- Reviews check scope leaks as a standing item.

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

Every commit must be acceptable to the Alchemy project as-is. Every feature and foundation is upstreamable by construction: never ask "upstream or fork-only?"; list it in `doc/skooma/UPSTREAM.md`.

- Write upstream-style code in upstream's layout. `SK`/`sk` prefixes on your own files and `<SK:Nexii>` wrappers on injection sites into non-Skooma files are expected on every branch — Alchemy already carries Firestorm's `<FS:ND>` / `<FS:AR>`. Keep product/channel branding out.
- Markers: `// <SK:Nexii> <one sentence naming the feature>` ... `// </SK:Nexii>`, or a trailing `// <SK:Nexii>` on a single line. Every edit to a non-Skooma file, marked or not, gets a row in `doc/skooma/INJECTIONS.md` once it lands.
- New settings go in `settings_alchemy.xml` with an `SK` prefix (`SKCensus*`).
- Comments are sparse and intentional, usually one sentence.
- Never touch `AGENTS.md`, `CLAUDE.local.md`, `doc/skooma/`, `scripts/skooma/`, `.githooks/`, `.claude/`, `.worktrees/`, `vs2026-os/`. If a change needs one of those, say so — it belongs on `glue`, except `doc/skooma/` docs, which are edited on `main` in the main checkout and committed at once, never left uncommitted.
- Each feature is its own upstreamable PR, so merge nothing into it recklessly. Take upstream by `git merge develop`, in your own worktree. Merge another feature or a foundation (`git merge foundation-<name>`) only when your feature depends on it, and say so in the merge commit's message. Never merge `main` or a trunk into a branch. Never rebase; only `pr/<name>` copies are rebased.
- Small logical commits; imperative subject (`Fix crash when pasting into text editor`); body explains why. Squash WIP before landing.
- `Co-Authored-By:` trailers are fine (upstream uses them). No `Generated with` lines.
- **Never append to the end of a shared list file** (`settings*.xml`, `CMakeLists.txt` source lists, floater registrations, strings): every branch appends there, so every pair of branches conflicts. Insert beside a related entry (`SkoomaLocalChat*` next to `AlchemyChatHistoryStyle`), or alphabetically where the file is sorted.
- **After `git merge develop`, put any build fixes in a separate commit** — fixes made inside a merge commit are silently dropped when the branch is rebased for a PR.

## Landing work

```
git skooma check .            # validate this branch
git skooma integrate <name>   # land it along its chain (amend → feature → trunk → main); `.` for this branch
git skooma sync .             # take develop into this branch's chain
```

The branch name comes first, flags after (`integrate <name> --force`). `integrate all`
and `sync all` touch every worktree and are the human's; never run them unasked. Read
the landed list, and `git skooma undo` at once if it names a branch you did not mean.

In an area with a trunk, your branch lands into the trunk, not `main`.

Follow-up work on an existing feature goes in an amend:
`git skooma new <area>/<x> --amend [--base <feature>]` makes `amend-<area>/<x>` in
`.worktrees/amend/<area>/<x>`, cut from its parent (`--base`, else `feature-<area>/<x>`;
never a trunk, `develop`, `main`, `glue`, a spike or another amend). `integrate <amend>`
merges it into the parent in the parent's worktree, then carries the parent on to its
trunk and `main`. An amend is never a PR on its own and is retired once its parent holds
its work. Not a new branch from `develop`, not a new `feature-*`, not commits made ad hoc
in another agent's feature worktree.

Prefer clean lands: check first with `git merge-tree --write-tree --name-only main <branch>`.
Where a main-bound land conflicts, resolving in the main checkout is allowed through
`git skooma integrate <name> --resolve`, then `git add` and `integrate --continue` (or
`--abort`); never by hand-merging.

Never remove, retire or prune worktrees by a computed rule: "merged into `main` or a
trunk" is not done, since `main` holds every live feature. Retire only amends whose work
is in their parent, branches merged upstream, or what Nexii names; to tidy, print a
table and let Nexii pick. `retire` deletes untracked files unread, so look at them first.

Do not run `git merge`, `git rebase`, `git push`, or `git checkout <other branch>` to land work. Do not create worktrees by hand — `git skooma new <name>` (add `--class fix` / `--class workaround` when it is not a feature).

## Building

**Agents never use the shared build trees** — no `git skooma build`, `git skooma test`
or `--obj`, and never `vs2026-os/`. Do local compile checks only: compile the objects
you touch with your own script, using the shared tree's flags (inside the VS
environment), writing the objects to `V:\Scratch\<feature>\`. Compile after every few
edits rather than batching changes up.

**Nexii does the full builds (F5)** in the main checkout's `vs2026-os/`; never delete it,
build it or give commands that build it. Say in your report that no full build ran;
`git skooma undo` reverts a land that breaks F5. Build trees are excluded from git;
never `git add` them, and `git add` new sources so the next build sees them.

Traps a local compile check does not catch:
- F5 compiles Tracy in; a check without it does not. Two `LL_PROFILE_ZONE_*` in one scope fail (C2374) only in F5: give every added zone its own `{ }` unless it is the only one at that scope.
- A changed enum or struct that sizes members (camera ids, render types, pool counts) leaves stale objects in an incremental F5, and login crashes in cull/octree code. When you land one, tell Nexii to Rebuild `alchemy-bin`; diagnose such a crash by comparing obj timestamps with the header first.

Tests never block a change: write the test anyway, compile what you can, land it.

## Design rules

Put the relevant ones in every brief.

- **No TAA** or jitter-accumulation; a history buffer only with explicit reprojection for camera motion. Flag a paper's TAA-dependent technique as not transferable.
- **Defaults now, budgets later**: generous defaults for Nexii's gaming PC as plain debug settings; no tiers, presets, budget UIs or auto-scaling.
- **Current APIs**: for each data source say which cap/service/system is used and whether a newer one replaces it; legacy UDP only where nothing does.
- **Existing controls**: alt-cam, flycam, the Camera Controls floater, key_bindings actions, on-screen buttons. Never invent hotkeys.
- **XUI layout, not code**: fix placement with XUI attributes (`right="-N"`, `follows`); compute rects in C++ only where XUI cannot, and say why.
- **Cache-friendly hot code**: per-item scratch in small stack/thread-local buffers, no allocation per item; flat contiguous data in walk order, indices not pointers, no `std::map`/linked lists; chunks sized to L2 per worker; benchmark compute and memory-walk phases separately (Tracy zones) and keep a change only if it is at least as fast. Moved code: move first, then a measured layout pass.
- **Never touch AgentUpdate's BodyRotation**: it answers `llGetRot` and steers movement. Aim features converge the camera and HeadRotation only.
- **Rye's upstream work**: don't send Rye questions. Read `doc/skooma/.llrender-compute.md` before shader/compute work; park anything that depends on his pending work and check `develop` after each sync.
- **Use exact data**, never 4 m blocks or point sampling when triangles, portals or cells exist; no invented methods or workarounds unless intentional or upstreamable.
- **A refuted idea is removed entirely** and the negative decision recorded; never a reduced or "parity" version.
- **Record owner ideas at once**: any approach or rule Nexii proposes goes into the owning plan's owner section immediately, and into every later brief.

## World foundations

Physics shapes → census → air cells → rooms (`indra/skcensus`, `indra/skair`, `indra/skrooms`; settings `SKCensus*`, `SKAir*`, `SKRooms*`). Foundations never call their consumers (minimap, overview, probes), and their work never goes on `combat/*`. The Recast navmesh was dropped (2026-10-22); build nothing on it. The physics-shape parser is **clean-room**: only public sources or corpus measurement, never Havok SDK trees or SDK-only names. Rulings and test regions: `doc/skooma/AIR.md` and `.foundations-plan.md`.

**Atmo Magic is paused** (parked as `backup/atmo-magic`): no Atmo work unless Nexii names it. Rulings: `doc/skooma/ATMO.md`.

## Evidence, benchmarks, captures, graphs

Write them to `V:\Scratch\<feature-name>\`, never into the repo. Artifacts committed to a
feature branch will fail `git skooma check` and would have to be stripped before an
upstream PR. Create the folder when you first need it; it survives the worktree being deleted.
Keep briefs on disk there too, so a killed agent is relaunched in one line; every brief carries the build rules above.

- **Oracle first** for geometry, timing, colour or signal work: an independent reference written from the spec by someone who hasn't seen the C++, then the C++, then sweep-and-diff (`V:\Scratch\<feature>\viz\`, `test_pins.py`, `golden.json`).
- **Visuals** per `V:\Scratch\VISUALS.md`, in `V:\Scratch\<feature>\out\`; view and describe your own images.
- **PDFs** that are mostly figures: render each page as an image and verify every figure-sourced number on it.
- **Fetching**: one fetcher per platform through a queue with backoff (`V:\Scratch\web\cb-shape\FETCH.md`); no cookies, logins or proxies to evade limits.
- Report files and images as full absolute Windows paths.
- WebGL scratch demos run on ANGLE D3D11: in big shaders use `textureLod(…,0.)` in loops, one call site for heavy functions, no `sign()`. Headless Edge for dGPU timings (Chrome/Edge default to the iGPU).

## Tracy capture — quick lookup

When asked to capture, start promptly while the viewer is alive; diagnose afterward.
Use the installed CLI directly (no MCP setup or executable search required):
`D:\viewers\libraries\tracy\official-0.14.1\tracy-capture.exe` (0.14.1).

PowerShell recipe: use the requested duration, otherwise 30 seconds. Captures are named
`V:\Scratch\tracy\<yyyy-MM-dd-HHmm>.tracy`, as the existing ones are.

```powershell
$captureDir = 'V:\Scratch\tracy'
$captureSeconds = 30
$capturePort = 8086
New-Item -ItemType Directory -Force -Path $captureDir | Out-Null
$captureFile = Join-Path $captureDir ("{0}.tracy" -f (Get-Date -Format 'yyyy-MM-dd-HHmm'))
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
- Keep the profiler GUI closed while capturing: F5 records every allocation, and an undrained
  stream balloons the viewer's commit (~50 GB), sags FPS and crashes in Tracy's queue. That is
  a profiling artefact, not a leak; check `PrivateUsage` in `Alchemy.log` before trusting a slow-down.
- Verify successful exit and a nonempty `.tracy` file, then report its full path.
  Open/analyze with `D:\viewers\libraries\tracy\official-0.14.1\tracy-profiler.exe`.
  Do not use `-f` (overwrite) for routine captures.
- Analyze with `tracy-csvexport.exe` beside it (not on PATH): `-u -f "<zone>"` every event of a
  zone (split from the right, names hold commas), plain for per-zone stats, `-e` self time,
  `-m` messages. An unfiltered `-u` is ~180 M lines: stream and filter. Main thread is id 2;
  scripts in `V:\Scratch\tracy`. Unzoned self time is often a coroutine at a yield.
- If the viewer has already exited, a new live capture cannot recover the crash;
  inspect `%APPDATA%\AlchemyNext\logs\Alchemy.log` / `Alchemy.old` instead.
- Optional MCP workflow and troubleshooting: `D:\viewers\alchemy-skooma\doc\skooma\.tracy-capture.md`.
  A missing/broken MCP server must not delay this CLI capture path.

## Crashes

A crash caught in Nexii's attached VS Code debugger: read it yourself rather than asking for values —
`& V:\Scratch\dbg\peek.ps1 [-ProcessId <id>] -Commands '~0s','kn 20','.frame 1','dx -r1 this->mMember'`
(PowerShell; cdb non-invasive `-pv`, read-only, detaches with `qd`). "Optimized away" locals are
often readable as `.frame N` + `dx this->member`.

## Machine

Core2 (this PC): RTX 3080 Ti + AMD iGPU. Writes outside worktrees, gitignored `doc/skooma` files and
the scratch roots are refused by a sandbox hook in Claude sessions, and are off limits to every agent;
never work around it, ask Nexii. Elevated steps go to Nexii as commands to run as Administrator.
Never `New-Item -Force` an existing registry key.

## LSL

Nexii's includes (`#include "NexiiLSL/..."`) resolve under `D:\Second Life\LSL`; unused include code
costs no memory. `while(i --> 0)` reverse loops are house style, not a bug.

## Docs

Design notes live in `doc/skooma/` **in the main checkout** — they are not in your worktree;
read them by absolute path, `D:\viewers\alchemy-skooma\doc\skooma\`. `*.md` there are compact
tracked indexes, kept current as work changes; `.*.md` are gitignored working notes (research,
plans, reviews, briefs). `SKOOMA.md` is the feature index.
