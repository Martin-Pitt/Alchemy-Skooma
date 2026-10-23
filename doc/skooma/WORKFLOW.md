# Skooma Branch Workflow

**Humans: read [DEVELOPING.md](DEVELOPING.md) instead** — this file is the terse reference.

Compact index. Full rationale, algorithms and open items: `doc/skooma/.workflow-design.md` (untracked).
Status (2026-10-18): **implemented and in daily use.** `git skooma status|new|wt-setup|sync|check|integrate|trunk|undo|retire|build|test|clean|bootstrap` works; `develop` is hook-protected; agent stubs and the Claude worktree redirect are installed. `main` is deliberately left **unlocked** for direct commits during fork setup. Deferred: `publish`, CI. Reviews: `.review-verdict.md`. Docs backed up to `V:\Scratch\skooma-docs`.

## Remotes

| Remote | URL | Role |
|---|---|---|
| `upstream` | `https://github.com/AlchemyViewer/Alchemy.git` | Alchemy (GitHub). Main branch `develop`. Read-only for us except PR branches |
| `origin` | `https://github.com/Martin-Pitt/Alchemy-Skooma.git` | Skooma fork (GitHub). All our branches live here |
| `forgejo` | `ssh://git@git.paranoid.dev:222/Nexii/alchemy-skooma.git` | Old fork, kept for reference |
| `paranoid` | `ssh://git@git.paranoid.dev:222/alchemy/alchemy.git` | Alchemy on Forgejo |

`github.com/AlchemyViewer/Alchemy` is a mirror, never a sync source.

## Branches

| Branch | Role | Checked out? | Moves by |
|---|---|---|---|
| `develop` | Mirror of `upstream/develop`. Zero Skooma commits, ever | Never | `git fetch upstream develop:develop` (inherently ff-only) |
| `main` | Permanent integration branch. F5/debug checkout. Direct commits **allowed for now** (fork setup: README, branding, glue); feature work still belongs in a worktree | Main checkout `D:\viewers\alchemy-skooma` | `git skooma sync` / `integrate`, plus setup commits |
| `glue` | Fork-only glue: `AGENTS.md`, `scripts/skooma`, hooks, skins, CI. Never upstreamed | `.worktrees/glue` | Commits; `git merge develop` |
| `feature-<name>` | One feature, branched from `develop`, upstreamable as-is | `.worktrees/<name>` | Commits; `git merge develop` |
| trunk (`feature-navmesh`, `feature-atmo-magic`, `feature-combat/minimap`, `camera`) | The branch an area ships as one PR. Area worktrees are cut from it and land into it; it lands into `main` | `.worktrees/<name>` | `integrate` merges area branches; `git merge develop` |
| `fix-<name>`, `workaround-<name>`, `<class>-<name>` | Same thing under a different *classification* — a label chosen at `git skooma new`, purely to say what the branch is for. Identical rules, identical handling | `.worktrees/<name>` | Commits; `git merge develop` |
| `amend-<area>/<x>` | A follow-up to an existing feature by another agent. Cut from, and landed into, that feature (`skooma-base`); never a PR on its own; retired routinely once its parent holds its work | `.worktrees/amend/<area>/<x>` | Commits; `git merge <parent>` |
| `spike-<name>` | Throwaway instrumentation answering one question. May be integrated into `main` so it runs in the F5 build; never upstreamable | `.worktrees/<name>` | Commits; `git merge develop` |
| `skooma-<name>` | Optional fork-side sidecar for one feature (settings defaults, skin placement) | `.worktrees/<name>-sk` | Commits; `git merge feature-<name>` |
| `publish` | Release pointer for CI (deferred: not created yet) | Never | `git merge --ff-only main` |

```
upstream/develop ──fetch──▶ develop ──merge──▶ main ──ff──▶ publish
                          │                ▲
                          └──▶ feature-* ──┴──▶ GitHub PR to AlchemyViewer/Alchemy:develop
                                 (merge)      (pushed as-is)
```

Nothing ever flows *into* `develop`. No branch is ever rewritten. Merge commits are fine; git
decides ff vs merge. A feature takes upstream by merging `develop`, never by rebasing onto it,
and that same branch is what an upstream PR is opened from.

## Invariants

1. `develop` == `upstream/develop` at all times. Never check it out, never commit/merge onto it.
2. `feature-*` branch from `develop`. Never merge `main` or another feature into a feature (declare a dependency instead: branch off the dependency, PR it first).
3. Fork-only content never enters `feature-*`: `AGENTS.md`, `CLAUDE.local.md`, `doc/skooma/`, `scripts/skooma/`, `.githooks/`, `.worktrees/`, Skooma skins/branding/CI. `<SK:Nexii>` markers are *not* fork-only — they belong on injection sites on every branch class.
4. Fork-only content goes on `glue`; feature-specific fork tweaks on a `skooma-<name>` sidecar. `check` allows fork paths/markers on `skooma-*`, forbids them on `feature-*`.
4b. A branch's **class** (`feature`, `fix`, `workaround`, or any custom one) is a label, not a rule: every class is handled identically and every class is upstream-bound. Known classes = the four built in plus whatever `skooma.class` has collected; `branch.<b>.skooma` records the class of each branch. `spike` is the one class that is *not* just a label — see 4c.
4c. A `spike-*` branch is throwaway and never upstream-bound. Commits carrying its instrumentation are marked with a `DO NOT LAND:` subject prefix, and **`DO NOT LAND` means "never reaches `develop`", not "never reaches `main`"** — a spike has to run in a real build to measure anything, so integrating it is expected. `check` makes such a commit fatal on any upstream-bound branch and a warning on `spike-*`. Because the spike's code is then in `main`, retiring the branch does not discard it: `retire` refuses while those commits are live in `main`, and `--force` reverts them out first. This trap has bitten once already — the `spike-atmo/probe` branch was retired cleanly while all 740 of its lines stayed in `main`, because `retire`'s "content is already in the base" check was *satisfied* by the merge that put it there.
5. Landing work = `git skooma integrate` (all worktree branches) or `integrate <name>` / `integrate .`. Nobody commits on `develop`/`publish`: `pre-commit` refuses both, and `reference-transaction` rejects any move of `develop` not made by `sync`. `main` is not hook-guarded yet (fork setup).
6. Upstream is taken by `git skooma sync`: `develop` first, then `main`, then every active feature worktree.
7. **Trunks.** An area (`nav`, `atmo`, `combat/minimap`, `camera`) maps to one trunk branch: `skooma-trunk.<prefix>.branch` in repo config, set with `git skooma trunk <prefix> <branch>`. `new <prefix>/<x>` (or `<prefix>-<x>`; longest prefix wins) records `branch.<b>.skooma-trunk` and bases on the trunk unless `--base` stacks it on a sibling. `integrate` lands such a branch into the trunk (merged in the trunk's worktree, which must be clean), then the trunk into `main` in the same run. `check` lets a trunk merge any branch that carries no main-only commit. `undo` only resets `main`; trunk merges stay. Existing branches join with `trunk --join <name>`. Current map: `nav` → `feature-navmesh`, `atmo` → `feature-atmo-magic`, `combat/minimap` → `feature-combat/minimap`.

8. **Amends.** Follow-up work on an existing feature goes on `new <area>/<x> --amend [--base <feature>]` → `amend-<area>/<x>`, base (and land target) `--base` or `feature-<area>/<x>`; refused if that resolves to a trunk, `develop`, `main`, `glue`, a spike or another amend. `integrate <amend>` merges it into its parent in the parent's worktree (clean; no worktree = skipped and said so) with the subject `Merge amend '<b>' into <parent>`, which `check` on the parent accepts like a trunk's merges, then carries the parent on to its trunk and `main`. No-arg `integrate` takes amends first. Nothing auto-retires. `retire <amend>` needs no `--force` once `git diff --quiet <parent>...<amend>`. A feature is never retired for being in `main` or a trunk: retire judges a trunk-based feature against `develop` and says plainly when its work is only in `main`/a trunk. Added 2026-10-23 after real features were treated as retirable amendments.

## Commands (`git skooma …`)

| Command | Does |
|---|---|
| `bootstrap` | Copies scripts to `.git/skooma/`, installs `sh` hooks into shared `.git/hooks`, git config, `.git/info/exclude` entries, `alias.skooma` |
| `status` | Which worktree/branch am I in, ahead/behind `main` and `develop`, dirty state, last-known-good `main`. Amends are listed indented directly under their parent, tagged `amend` |
| `new <name> ["desc"] [--class <kind>] [--base <branch>] [--tests]` | `sync` `develop` when the base is `develop`; `git worktree add .worktrees/<name> -b <class>-<name> <base>` (base: `--base`, else the area's trunk, else `develop`); submodules `--reference`; sets `branch.<b>.skooma=<class>`; `wt-setup`; prints agent launch lines. No local build tree: `git skooma build` uses the shared build worktree, except with `--tests`, which configures a local tree for CTest. Class defaults to `feature`; run `new` with no name to be asked (list of known classes + "other...", which remembers what you type in `skooma.class`) |
| `wt-setup` | In a worktree: `CLAUDE.local.md` import stub, `AGENTS.md` + `.vscode` copies, branch marker/rename for tool-created worktrees. **No junction** — read `D:\viewers\alchemy-skooma\doc\skooma\` by absolute path. Also the T3 Code "on worktree creation" action |
| `sync` | `git fetch upstream`, guarded `update-ref develop`; merge `develop` into `main`; merge `develop` into each clean active worktree (+`submodule update`); refresh copies; back up docs; stops at first conflict |
| `check [name \| .]` | No argument from a feature worktree checks that branch, from the main checkout every worktree branch. Merge parents ⊆ `develop`, no forbidden paths in `git diff develop...feature-<name>`, merge trees == `merge-tree` of parents, no `DO NOT LAND:` commit unless the branch is a `spike-*`. On an `amend-*` it checks against the parent and warns that it is never a PR on its own |
| `undo` | Reset `main` to the last-known-good sha recorded by `integrate` (requires clean checkout) |
| `build [target]` | Build in the shared build worktree (`.worktrees/build`) or the main checkout (VS), entering the VS environment first. The calling worktree's tracked working state is checked out there first (`git stash create`), so uncommitted edits compile; untracked files are invisible until `git add`. A worktree that has its own tree (`--tests`) builds locally. `git skooma build llrender` for a fast single-library check; `--obj <file>` compiles one translation unit |
| `test [--match re] [--label l] [--bench] [--no-build]` | Build the selected tests, then run CTest on this worktree's state: in its own tree if it was created with `new --tests`, otherwise in the shared build worktree, which is always configured with `AL_BUILD_TESTS` + `AL_ENABLE_GL_TESTS`. GL tests carry the label `gl`. Benchmarks (label `benchmark`) only with `--bench`. Log tees to `V:\Scratch\<name>\ctest-<config>.log`. Long option names only: a single-letter `-R` binds to the script's `-Rest` parameter |
| `clean [name \| --all]` | Delete a worktree's `ninja-os` build tree to reclaim disk; never the main checkout's. Rebuilding takes about a minute because the shared vcpkg install and downloads stay warm. `clean build` targets the shared build worktree |
| `trunk [prefix branch \| --join name \| --unset prefix]` | List, set or clear area → trunk mappings; `--join` makes an existing branch land into its trunk (base unchanged) |
| `integrate [name \| .]` | No arg = all `skooma-*`/`feature-*` worktree branches ahead of their land target (trunk or `main`); trunk-bound branches go first, then each trunk lands into `main` (`glue` first). Each: `check` → merge into `main`. Skips+reports on conflict or failed `check`. `integrate <name> --resolve` (main-bound only): on conflict leaves the merge in the main checkout, records last-good for `undo`, lists the files; resolve + `git add`, then `integrate --continue` (refuses on unmerged paths or `<<<<<<<`/`>>>>>>>` markers, commits through the hook bypass with the resolved files listed; state in `.git/skooma-resolve.json`) or `integrate --abort`. Added 2026-10-15: feature branches conflicting with main-only code had no landing path, since they may never merge main (feature-worldmap). Refuses if main is dirty; a running build only warns (2026-10-23: landing never waits on a build). T3 action "Integrate" |
| `retire [name \| .] [--force]` | Remove the worktree + branch once `git diff --quiet <base>...<branch>` is empty — i.e. upstream merged it and its content is already in the base. `--force` discards one that still carries work (spikes), printing what it dropped and the tip sha so `git branch <name> <sha>` puts it back. An amend is judged against its parent (the routine case, no `--force`); a feature whose base is a trunk is judged against `develop`. If `git worktree remove` fails after git has deregistered the worktree (Windows "Permission denied" on the emptied folder), retire warns and still deletes the branch; it never deletes a branch whose worktree is still registered. Also reverts out of `main` any `DO NOT LAND:` commit of this branch still sitting there (see below); that revert needs `--force` too, and a clean main checkout on `main`. `--force` overrides the content and spike checks **only**: it still refuses while sub-features are based on it, while the worktree is dirty or mid-operation, from the main checkout, and from inside its own worktree. Leaves `V:\Scratch\<name>` and the remote branch alone |

## Worktrees

- Root: `D:\viewers\alchemy-skooma\.worktrees\<short-name>` (e.g. `.worktrees/atmo-magic` for `feature-atmo-magic`). Excluded via `.git/info/exclude` (shared by all worktrees), so `git add .` can never turn it into a gitlink.
- **Build worktree**: `.worktrees/build` on `skooma-build` (a sidecar, base `develop`) holds the only configured Ninja tree. It is created on first `git skooma build` from a worktree, and `git skooma sync` keeps it current with `develop`; between builds it sits detached at whatever state was built last. Feature worktrees are source checkouts, so N branches cost one build tree instead of N, and a new branch's first build only pays for what that branch and `develop` added since the last one. `status` labels the row `build`; `clean build` reclaims its tree.
- Only a worktree made with `new --tests` has a build tree of its own (`ninja-os/`). Building a feature worktree alone == upstream-compatibility test.
- `git clean -fdx` skips nested repos; never use `-ff` in the main checkout.
- `.vscode/settings.json`: `files.exclude`/`search.exclude` `.worktrees/**`. Enable Windows long paths + `core.longpaths=true`.
- Untracked files don't follow worktrees: `CLAUDE.local.md`, `doc/skooma/.*`, `.vscode/` are per-checkout; `wt-setup` links/copies them.
- **Two build profiles.** Main checkout: Visual Studio generator in `vs2026-os/` (matches the untracked `.vscode` F5 task) — never delete it. Worktrees: **Ninja** in `ninja-os/` of the shared build worktree, configured on its first `git skooma build`.
- **Always `git skooma build [target]`** — Ninja needs the VS environment, which the command enters (VS ships ninja, nothing extra to install). Bare `cmake`/`ninja` will not find the compiler.
- Measured on this tree, 32 cores, Defender exclusion on `D:\viewers\alchemy-skooma`: configure 1.4 min · full build 6.4 min (1224 objects, 782 in newview) · null build 0.4s · one `.cpp` → library 3.2s · one `.cpp` → object 0.3s · header → dependents 10s · tree 12.5 GB. sccache was evaluated and **rejected**: it misses by definition on freshly edited files, so it cannot improve the edit loop.
- Benchmarks, captures and graphs go to `V:\Scratch\<feature>\` (create it when first needed), never into the repo — `check` would flag them and they would have to be stripped before an upstream PR.
- T3 Code actions: `Integrate` → `git skooma integrate` · `Sync upstream` → `git skooma sync` · `Check` → `git skooma check .` · `Build` → `cmake --build vs2026-os --config Release --parallel` · on-worktree-creation → `git skooma wt-setup`.

## Feature branch rules (goes into `AGENTS.md`)

Feature branches are upstream pull requests in waiting; every commit must be acceptable to Alchemy as-is.
- Upstream style, upstream file layout. `SK`/`sk` prefixes and `<SK:Nexii>` injection markers are expected (Alchemy carries Firestorm's already); product/channel branding is not. `Co-Authored-By:` trailers are fine (upstream uses them); no `Generated with` link lines.
- After `git merge develop`, put any build fixes in a **separate commit**, never inside the merge commit (rebase silently drops them).
- Small logical commits, imperative subject (`Fix crash when pasting into text editor`), body says why. Squash WIP before `integrate`.
- Build the worktree standalone before `integrate`.
- On GitHub "branch out-of-date": the *merge* update button, or `git skooma sync` and push again. Never rebase.

## Sending a feature upstream

Manual, by design — see **[DEVELOPING.md](DEVELOPING.md#sending-a-feature-upstream)** for the procedure. `git skooma sync`, `check`, `git push origin feature-<name>`, open the PR from it. Key point: `git rev-list --count HEAD..develop` must be **0**, or the branch is behind upstream and the PR's diff is not what it claims.

## Enforcement layers

1. Docs (`AGENTS.md` via `CLAUDE.local.md` `@` import; copies in worktrees).
2. Git hooks in shared `.git/hooks`, written in `sh` with first-line early exit: `reference-transaction` rejects any update of `refs/heads/develop` unless script-driven and new tip is ancestor-or-equal of `upstream/develop`; `main`/`publish` are not guarded yet. `pre-commit`: refuse on `develop`/`publish`; refuse forbidden paths staged on any upstream-bound class. (`pre-push` deferred.)
3. Branch protection on `origin` (GitHub): `develop`/`main`/`publish` no force push, push restricted to owner.
4. Claude-only: `.claude/settings.json` PreToolUse hook denying `git checkout|switch main|develop` and commits on protected branches. Codex/OpenCode rely on layers 1–3.

## Config (repo-level, shared by worktrees)

`remote.pushDefault=origin` · `push.default=current` · `merge.conflictStyle=zdiff3` · `core.longpaths=true` · `alias.skooma` (resolves the script via `git rev-parse --git-common-dir`, so it carries no absolute path and survives a move) · `branch.develop.remote=upstream`

Upstream branch is config, never hardcoded: `skooma.upstreamBranch=develop` (fetched as local `develop`), `skooma.prBase=develop`. If upstream moves to another main branch, retarget by hand (`git skooma retarget` is not implemented): ff when possible, else a script-gated move of `develop` + normal merges. `sync` warns when upstream HEAD ≠ configured branch.

## CI (later)

- Preferred: GitHub build-only mirror remote; `release.ps1` pushes `publish` + `AlchemySkooma_*` tags; cron nightly checks out `ref: main`. Reuse `.github/workflows/build.yaml` / `tag-release.yaml` with `publish` added to `push.branches` and the channel rename below.
- **Naming — two separate namespaces, do not unify them:**
  - *Code identifiers*: `SK`/`sk` prefixes, `skooma/` folders, `<SK:Nexii>` markers. Short, following the `LL` → `FS` → `AL` lineage. Never prefixed with "Alchemy".
  - *Product strings* (channel, app data dir, log/marker/mutex, tags, About window): **`Alchemy Skooma`**. The fork is officially *Alchemy: Skooma*, a derivative crediting Alchemy — so keep "Alchemy" and add "Skooma"; never rename these to bare `Skooma_*`. They must also stay *distinct* from Alchemy's own strings so settings, caches and the updater feed never collide.
- Alternative: Forgejo Actions with self-hosted Windows `act_runner` (VS 2026).
