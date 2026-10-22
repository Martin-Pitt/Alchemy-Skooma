# Skooma Branch Workflow

**Humans: read [DEVELOPING.md](DEVELOPING.md) instead** — this file is the terse reference.

Compact index. Full rationale, algorithms and open items: `doc/skooma/.workflow-design.md` (untracked).
Status (2026-10-18): **implemented and in daily use.** `git skooma status|new|wt-setup|sync|check|integrate|trunk|undo|retire|build|test|clean|bootstrap` works; `next` is hook-protected; agent stubs and the Claude worktree redirect are installed. `develop` is deliberately left **unlocked** for direct commits during fork setup. Deferred: `publish`, CI. Reviews: `.review-verdict.md`. Docs backed up to `V:\Scratch\skooma-docs`.

## Remotes

| Remote | URL | Role |
|---|---|---|
| `upstream` | `ssh://git@git.paranoid.dev:222/alchemy/alchemy.git` | Alchemy (Forgejo). Main branch `next`. Read-only for us except PR branches |
| `origin` | `ssh://git@git.paranoid.dev:222/Nexii/alchemy-skooma.git` | Skooma fork. All our branches live here |

`github.com/AlchemyViewer/Alchemy` is a mirror, never a sync source.

## Branches

| Branch | Role | Checked out? | Moves by |
|---|---|---|---|
| `next` | Mirror of `upstream/next`. Zero Skooma commits, ever | Never | `git fetch upstream next:next` (inherently ff-only) |
| `develop` | Permanent integration branch. F5/debug checkout. Direct commits **allowed for now** (fork setup: README, branding, glue); feature work still belongs in a worktree | Main checkout `D:\viewers\alchemy-skooma` | `git skooma sync` / `integrate`, plus setup commits |
| `skooma-glue` | Fork-only glue: `AGENTS.md`, `scripts/skooma`, hooks, skins, CI. Never upstreamed | `.worktrees/glue` | Commits; `git merge next` |
| `feature-<name>` | One feature, branched from `next`, upstreamable as-is | `.worktrees/<name>` | Commits; `git merge next` |
| trunk (`feature-navmesh`, `feature-atmo-magic`, `feature-combat/minimap`) | The branch an area ships as one PR. Area worktrees are cut from it and land into it; it lands into `develop` | `.worktrees/<name>` | `integrate` merges area branches; `git merge next` |
| `fix-<name>`, `workaround-<name>`, `<class>-<name>` | Same thing under a different *classification* — a label chosen at `git skooma new`, purely to say what the branch is for. Identical rules, identical handling | `.worktrees/<name>` | Commits; `git merge next` |
| `spike-<name>` | Throwaway instrumentation answering one question. May be integrated into `develop` so it runs in the F5 build; never upstreamable | `.worktrees/<name>` | Commits; `git merge next` |
| `skooma-<name>` | Optional fork-side sidecar for one feature (settings defaults, skin placement) | `.worktrees/<name>-sk` | Commits; `git merge feature-<name>` |
| `publish` | Release pointer for CI (deferred: not created yet) | Never | `git merge --ff-only develop` |

```
upstream/next ──fetch──▶ next ──merge──▶ develop ──ff──▶ publish
                          │                ▲
                          └──▶ feature-* ──┴──▶ Forgejo PR to alchemy/alchemy:next
                                 (merge)      (pushed as-is)
```

Nothing ever flows *into* `next`. No branch is ever rewritten. Merge commits are fine; git
decides ff vs merge. A feature takes upstream by merging `next`, never by rebasing onto it,
and that same branch is what an upstream PR is opened from.

## Invariants

1. `next` == `upstream/next` at all times. Never check it out, never commit/merge onto it.
2. `feature-*` branch from `next`. Never merge `develop` or another feature into a feature (declare a dependency instead: branch off the dependency, PR it first).
3. Fork-only content never enters `feature-*`: `AGENTS.md`, `CLAUDE.local.md`, `doc/skooma/`, `scripts/skooma/`, `.githooks/`, `.worktrees/`, Skooma skins/branding/CI. `<SK:Nexii>` markers are *not* fork-only — they belong on injection sites on every branch class.
4. Fork-only content goes on `skooma-glue`; feature-specific fork tweaks on a `skooma-<name>` sidecar. `check` allows fork paths/markers on `skooma-*`, forbids them on `feature-*`.
4b. A branch's **class** (`feature`, `fix`, `workaround`, or any custom one) is a label, not a rule: every class is handled identically and every class is upstream-bound. Known classes = the four built in plus whatever `skooma.class` has collected; `branch.<b>.skooma` records the class of each branch. `spike` is the one class that is *not* just a label — see 4c.
4c. A `spike-*` branch is throwaway and never upstream-bound. Commits carrying its instrumentation are marked with a `DO NOT LAND:` subject prefix, and **`DO NOT LAND` means "never reaches `next`", not "never reaches `develop`"** — a spike has to run in a real build to measure anything, so integrating it is expected. `check` makes such a commit fatal on any upstream-bound branch and a warning on `spike-*`. Because the spike's code is then in `develop`, retiring the branch does not discard it: `retire` refuses while those commits are live in `develop`, and `--force` reverts them out first. This trap has bitten once already — the `spike-atmo/probe` branch was retired cleanly while all 740 of its lines stayed in `develop`, because `retire`'s "content is already in the base" check was *satisfied* by the merge that put it there.
5. Landing work = `git skooma integrate` (all worktree branches) or `integrate <name>` / `integrate .`. Nobody commits on `next`/`publish`: `pre-commit` refuses both, and `reference-transaction` rejects any move of `next` not made by `sync`. `develop` is not hook-guarded yet (fork setup).
6. Upstream is taken by `git skooma sync`: `next` first, then `develop`, then every active feature worktree.
7. **Trunks.** An area (`nav`, `atmo`, `combat/minimap`) maps to one trunk branch: `skooma-trunk.<prefix>.branch` in repo config, set with `git skooma trunk <prefix> <branch>`. `new <prefix>/<x>` (or `<prefix>-<x>`; longest prefix wins) records `branch.<b>.skooma-trunk` and bases on the trunk unless `--base` stacks it on a sibling. `integrate` lands such a branch into the trunk (merged in the trunk's worktree, which must be clean), then the trunk into `develop` in the same run. `check` lets a trunk merge any branch that carries no develop-only commit. `undo` only resets `develop`; trunk merges stay. Existing branches join with `trunk --join <name>`. Current map: `nav` → `feature-navmesh`, `atmo` → `feature-atmo-magic`, `combat/minimap` → `feature-combat/minimap`.

## Commands (`git skooma …`)

| Command | Does |
|---|---|
| `bootstrap` | Copies scripts to `.git/skooma/`, installs `sh` hooks into shared `.git/hooks`, git config, `.git/info/exclude` entries, `alias.skooma` |
| `status` | Which worktree/branch am I in, ahead/behind `develop` and `next`, dirty state, last-known-good `develop` |
| `new <name> ["desc"] [--class <kind>] [--base <branch>] [--tests]` | `sync` `next` when the base is `next`; `git worktree add .worktrees/<name> -b <class>-<name> <base>` (base: `--base`, else the area's trunk, else `next`); submodules `--reference`; sets `branch.<b>.skooma=<class>`; `wt-setup`; prints agent launch lines. No local build tree: `git skooma build` uses the shared build worktree, except with `--tests`, which configures a local tree for CTest. Class defaults to `feature`; run `new` with no name to be asked (list of known classes + "other...", which remembers what you type in `skooma.class`) |
| `wt-setup` | In a worktree: `CLAUDE.local.md` import stub, `AGENTS.md` + `.vscode` copies, branch marker/rename for tool-created worktrees. **No junction** — read `D:\viewers\alchemy-skooma\doc\skooma\` by absolute path. Also the T3 Code "on worktree creation" action |
| `sync` | `git fetch upstream`, guarded `update-ref next`; merge `next` into `develop`; merge `next` into each clean active worktree (+`submodule update`); refresh copies; back up docs; stops at first conflict |
| `check [name \| .]` | No argument from a feature worktree checks that branch, from the main checkout every worktree branch. Merge parents ⊆ `next`, no forbidden paths in `git diff next...feature-<name>`, merge trees == `merge-tree` of parents, no `DO NOT LAND:` commit unless the branch is a `spike-*` |
| `undo` | Reset `develop` to the last-known-good sha recorded by `integrate` (requires clean checkout) |
| `build [target]` | Build in the shared build worktree (`.worktrees/build`) or the main checkout (VS), entering the VS environment first. The calling worktree's tracked working state is checked out there first (`git stash create`), so uncommitted edits compile; untracked files are invisible until `git add`. A worktree that has its own tree (`--tests`) builds locally. `git skooma build llrender` for a fast single-library check; `--obj <file>` compiles one translation unit |
| `test [--match re] [--label l] [--bench] [--no-build]` | Build the selected tests, then run CTest on this worktree's state: in its own tree if it was created with `new --tests`, otherwise in the shared build worktree, which is always configured with `AL_BUILD_TESTS` + `AL_ENABLE_GL_TESTS`. GL tests carry the label `gl`. Benchmarks (label `benchmark`) only with `--bench`. Log tees to `V:\Scratch\<name>\ctest-<config>.log`. Long option names only: a single-letter `-R` binds to the script's `-Rest` parameter |
| `clean [name \| --all]` | Delete a worktree's `ninja-os` build tree to reclaim disk; never the main checkout's. Rebuilding takes about a minute because the shared vcpkg install and downloads stay warm. `clean build` targets the shared build worktree |
| `trunk [prefix branch \| --join name \| --unset prefix]` | List, set or clear area → trunk mappings; `--join` makes an existing branch land into its trunk (base unchanged) |
| `integrate [name \| .]` | No arg = all `skooma-*`/`feature-*` worktree branches ahead of their land target (trunk or `develop`); trunk-bound branches go first, then each trunk lands into `develop` (`skooma-glue` first). Each: `check` → merge into `develop`. Skips+reports on conflict or failed `check`. `integrate <name> --resolve` (develop-bound only): on conflict leaves the merge in the main checkout, records last-good for `undo`, lists the files; resolve + `git add`, then `integrate --continue` (refuses on unmerged paths or `<<<<<<<`/`>>>>>>>` markers, commits through the hook bypass with the resolved files listed; state in `.git/skooma-resolve.json`) or `integrate --abort`. Added 2026-10-15: feature branches conflicting with develop-only code had no landing path, since they may never merge develop (feature-worldmap). Refuses if develop dirty or a build is running (`--force` overrides the build check). T3 action "Integrate" |
| `retire [name \| .] [--force]` | Remove the worktree + branch once `git diff --quiet <base>...<branch>` is empty — i.e. upstream merged it and its content is already in the base. `--force` discards one that still carries work (spikes), printing what it dropped and the tip sha so `git branch <name> <sha>` puts it back. Also reverts out of `develop` any `DO NOT LAND:` commit of this branch still sitting there (see below); that revert needs `--force` too, and a clean main checkout on `develop`. `--force` overrides the content and spike checks **only**: it still refuses while sub-features are based on it, while the worktree is dirty or mid-operation, from the main checkout, and from inside its own worktree. Leaves `V:\Scratch\<name>` and the remote branch alone |

## Worktrees

- Root: `D:\viewers\alchemy-skooma\.worktrees\<short-name>` (e.g. `.worktrees/atmo-magic` for `feature-atmo-magic`). Excluded via `.git/info/exclude` (shared by all worktrees), so `git add .` can never turn it into a gitlink.
- **Build worktree**: `.worktrees/build` on `skooma-build` (a sidecar, base `next`) holds the only configured Ninja tree. It is created on first `git skooma build` from a worktree, and `git skooma sync` keeps it current with `next`; between builds it sits detached at whatever state was built last. Feature worktrees are source checkouts, so N branches cost one build tree instead of N, and a new branch's first build only pays for what that branch and `next` added since the last one. `status` labels the row `build`; `clean build` reclaims its tree.
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
- After `git merge next`, put any build fixes in a **separate commit**, never inside the merge commit (rebase silently drops them).
- Small logical commits, imperative subject (`Fix crash when pasting into text editor`), body says why. Squash WIP before `integrate`.
- Build the worktree standalone before `integrate`.
- On Forgejo "branch out-of-date": the *merge* update button, or `git skooma sync` and push again. Never rebase.

## Sending a feature upstream

Manual, by design — see **[DEVELOPING.md](DEVELOPING.md#sending-a-feature-upstream)** for the procedure. `git skooma sync`, `check`, `git push origin feature-<name>`, open the PR from it. Key point: `git rev-list --count HEAD..next` must be **0**, or the branch is behind upstream and the PR's diff is not what it claims.

## Enforcement layers

1. Docs (`AGENTS.md` via `CLAUDE.local.md` `@` import; copies in worktrees).
2. Git hooks in shared `.git/hooks`, written in `sh` with first-line early exit: `reference-transaction` rejects any update of `refs/heads/next` unless script-driven and new tip is ancestor-or-equal of `upstream/next`; `develop`/`publish` are not guarded yet. `pre-commit`: refuse on `next`/`publish`; refuse forbidden paths staged on any upstream-bound class. (`pre-push` deferred.)
3. Forgejo branch protection on `origin`: `next`/`develop`/`publish` no force push, push restricted to owner.
4. Claude-only: `.claude/settings.json` PreToolUse hook denying `git checkout|switch develop|next` and commits on protected branches. Codex/OpenCode rely on layers 1–3.

## Config (repo-level, shared by worktrees)

`remote.pushDefault=origin` · `push.default=current` · `merge.conflictStyle=zdiff3` · `core.longpaths=true` · `alias.skooma` (resolves the script via `git rev-parse --git-common-dir`, so it carries no absolute path and survives a move) · `branch.next.remote=upstream`

Upstream branch is config, never hardcoded: `skooma.upstreamBranch=next` (fetched as local `next`), `skooma.prBase=next`. If upstream moves to another main branch, retarget by hand (`git skooma retarget` is not implemented): ff when possible, else a script-gated move of `next` + normal merges. `sync` warns when upstream HEAD ≠ configured branch.

## CI (later)

- Preferred: GitHub build-only mirror remote; `release.ps1` pushes `publish` + `AlchemySkooma_*` tags; cron nightly checks out `ref: develop`. Reuse `.github/workflows/build.yaml` / `tag-release.yaml` with `publish` added to `push.branches` and the channel rename below.
- **Naming — two separate namespaces, do not unify them:**
  - *Code identifiers*: `SK`/`sk` prefixes, `skooma/` folders, `<SK:Nexii>` markers. Short, following the `LL` → `FS` → `AL` lineage. Never prefixed with "Alchemy".
  - *Product strings* (channel, app data dir, log/marker/mutex, tags, About window): **`Alchemy Skooma`**. The fork is officially *Alchemy: Skooma*, a derivative crediting Alchemy — so keep "Alchemy" and add "Skooma"; never rename these to bare `Skooma_*`. They must also stay *distinct* from Alchemy's own strings so settings, caches and the updater feed never collide.
- Alternative: Forgejo Actions with self-hosted Windows `act_runner` (VS 2026).
