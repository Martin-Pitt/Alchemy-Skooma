# Developing Alchemy: Skooma

A guide for humans. For the terse reference see [WORKFLOW.md](WORKFLOW.md); for the rules
agents follow see [`AGENTS.md`](../../AGENTS.md).

Skooma is a fork of [Alchemy](https://git.paranoid.dev/alchemy/alchemy), which is itself a
fork of the Linden Lab viewer. Features are developed so that each one can be offered back
to Alchemy on its own. That single constraint explains most of what follows.

---

## One-time setup

```sh
git skooma bootstrap
```

Installs the hooks, sets the git config, and copies the scripts to `.git/skooma/` so they
keep working even when the checkout is mid-merge. Safe to re-run; do so after pulling
changes to `scripts/skooma/`.

---

## The shape of the repo

| Branch | What it is |
|---|---|
| `develop` | A read-only mirror of upstream. Never checked out, never committed to. Moved only by `git skooma sync`. |
| `main` | Your permanent checkout — the one you build and debug from with F5. Everything lands here. |
| `feature-<name>` | One feature, in its own worktree under `.worktrees/<name>`, branched from `develop`. |

Two build trees, deliberately different:

- **`vs2026-os/` in the main checkout** — Visual Studio generator, what F5 uses. Never delete it; a rebuild is hours.
- **`ninja-os/` in the shared build worktree** (`.worktrees/build`) — Ninja, for fast compile checks of any worktree, which `git skooma build` checks out there first. A null build is 0.4s.

`git skooma status` shows every worktree, what is ahead of `main`, what is behind its
base, and what is dirty. Run it whenever you lose your bearings.

---

## Starting a feature

```sh
git skooma new atmo/keyframes "Per-parameter keyframes across the day cycle"
```

Creates `feature-atmo/keyframes`, a worktree at `.worktrees/atmo/keyframes`, shares the
submodules with the main checkout, and makes
`V:\Scratch\atmo-keyframes\` for benchmarks and captures. Run with no arguments and it
asks for the name and description.

**Areas with a trunk.** Big features ship as one branch, the *trunk*: `feature-navmesh`
for `nav/*`, `feature-atmo-magic` for `atmo/*`, `feature-combat/minimap` for
`combat/minimap*`. A new branch in an area is cut from its trunk and lands back into it:

```sh
git skooma new nav/stair-fix          # base feature-navmesh, lands into feature-navmesh
git skooma trunk                      # list areas and their trunks
git skooma trunk render feature-render # add one
git skooma trunk --join nav/walls     # make an existing branch land into its trunk
```

`integrate` merges the area branch into the trunk, then the trunk into `main`, so F5
sees it either way. The trunk is the PR.

**Sub-features** build on other features rather than on `develop`:

```sh
git skooma new atmo/weather --base atmo/keyframes
git skooma new atmo/tornado --base atmo/weather
```

Any depth works. `sync` carries upstream down the chain (`develop` → keyframes → weather →
tornado), and `integrate` always lands parents before children. Use a stack when the work
genuinely depends on the work below it — that dependency is also the order you will send
the PRs in.

---

## Working

Compile constantly; it is cheap. Pick the smallest check that answers your question:

```sh
git skooma build --obj indra/newview/llenvironment.cpp   # 1-10s, does this file compile?
git skooma build llrender                                # ~3s, does the library build?
git skooma build                                         # ~7 min, the whole viewer
```

Always `git skooma build` rather than bare `cmake`/`ninja` — Ninja needs the Visual Studio
environment, which the command enters for you.

`test` runs a worktree's tests in the shared build worktree, which always has them
configured; a branch created with `--tests` gets a tree of its own instead:

```sh
git skooma new atmo/registry --tests        # own tree with AL_BUILD_TESTS + AL_ENABLE_GL_TESTS
git skooma test                             # build + run every test (benchmarks excluded); log tees to V:\Scratch\<name>\
git skooma test --match registry            # only tests whose name matches, built first
git skooma test --label gl                  # only the GL tests (hidden window, real GL)
git skooma test --bench                     # benchmarks, verbose — the numbers are the output
git skooma test --match llsd --no-build     # re-run without rebuilding
```

The test executables are not part of a library build, so `test` builds the selected
tests before running them; `--match` is the loop to iterate on one.

Tests are Catch2 and live in `indra/<lib>/tests/`, one binary a library, registered with
`al_add_test_binary` (`indra/cmake/ALAddTestBinary.cmake`); see `doc/TESTING.md`.

Things that belong **outside** the branch:

- Benchmarks, frame captures, graphs, profiling output → `V:\Scratch\<feature>\`
- Design notes, research, review output → `doc/skooma/.<topic>.md` in the main checkout
- Fork-only glue (scripts, branding, CI) → commit on `main`

A feature branch containing any of those will fail `git skooma check` — because it would
have to be stripped out before the branch could go upstream.

---

## Landing work

```sh
git skooma integrate
```

With no arguments it lands every worktree branch that is ahead of its target — its trunk,
or `main` — parents first, then each trunk that received work into `main`. Each is validated, dry-run merged so a conflict can never dirty your build tree,
then merged. It refuses if `main` is dirty or if the viewer or a compile is running —
so it cannot pull the rug from under a debugging session.

Then build and F5 in the main window as usual.

If a land breaks the build:

```sh
git skooma undo      # back to the last-known-good main
```

The feature branches keep their commits; fix and integrate again.

---

## Taking upstream changes

```sh
git skooma sync
```

Fast-forwards `develop` to upstream, then merges it into `main` and into every clean
worktree, deepest chains included. Dirty worktrees are skipped and listed. On a conflict
it stops and tells you which worktree to fix; resolve, commit, and run it again.

After merging `develop` into a feature, **put any resulting build fixes in their own
commit** — never amend them into the merge commit. Rebasing later silently discards
changes made inside a merge commit, which is how a PR ends up looking clean and not
building.

---

## Sending a feature upstream

Rare and manual by design — you want to write the description and handle review yourself.
`origin` is a GitHub fork of `AlchemyViewer/Alchemy`, so this is an ordinary cross-repo PR, and
the feature branch is what it is opened from. Nothing is rebased, copied or squashed.

```sh
git skooma sync
git skooma check atmo/keyframes

git push origin feature-atmo/keyframes
```

Then on GitHub open a PR from `Martin-Pitt/Alchemy-Skooma:feature-atmo/keyframes` into
`AlchemyViewer/Alchemy:develop`.

`sync` is what makes the branch presentable: it merges `develop` in, so the branch contains
upstream and the PR's diff against `develop` is the feature and nothing else. Check that
before opening it:

```sh
git rev-list --count HEAD..develop      # 0 — the branch is not behind upstream
git log --oneline develop..HEAD         # the commits the PR will carry
git diff --stat develop...HEAD          # the diff it will show
```

A non-zero first number means `sync` has not run since upstream moved; run it and push
again. The diff is also the last check that no fork-only path crept in — `check` enforces
that, but it is worth a look before a reviewer sees it.

Conventions upstream expects:

- Keep `Co-Authored-By:` trailers — upstream's own history is full of them. Strip only
  `🤖 Generated with` lines.
- Do not squash. Upstream merges PR branches verbatim, review commits and all.
- Keep `<SK:Nexii>` wrappers on injection sites — upstream carries Firestorm's equivalents and
  they are how a reader of the merged tree sees what is ours. Keep product/channel
  branding out of the diff.
- When GitHub says the branch is out of date, take the *merge* variant, or run
  `git skooma sync` and push again — both leave the same merge commit.
- A sub-feature can only target `develop` once its parent has landed upstream. Until then,
  send the parent first.

Never rebase a `feature-*` branch, before or after a PR. It is merged into `main`, and
rewriting it would cause conflicts forever after — which is also why the branch takes
upstream by merging rather than by rebasing onto it.

---

## When something goes wrong

| Symptom | What to do |
|---|---|
| A land broke the build | `git skooma undo` |
| `sync` stopped on a conflict | Fix it in the worktree it named, `git commit`, re-run `git skooma sync` |
| "develop would move non-fast-forward" | Something was committed on `develop`. `git log upstream/develop..develop` to see what, move it to a feature branch, then `git branch -f develop upstream/develop` |
| A command refuses because `main` is dirty | Commit or stash in the main checkout; `git -C D:\viewers\alchemy-skooma status` |
| `integrate` warns a build is running | It lands anyway; F5 again once that build ends to pick up what landed |
| Worktree on a `claude/*` or `t3code/*` branch | `git skooma wt-setup` in it — renames to `feature-*` and adds the agent files |
| A worktree directory will not delete | Something holds a handle. Close the editor/shell in it, then `git worktree prune` |

---

## Why the rules exist

Every restriction traces to one of three things:

1. **Features must stay upstreamable.** Hence branching from `develop`, no fork-only files in
   feature branches, upstream commit style.
2. **`main` is a live debugging session.** Hence the dry-run merge, the refusal to
   integrate while the viewer runs, and `undo`.
3. **`develop` must equal upstream exactly.** Hence it is never checked out and a hook
   refuses to move it. If it drifts, every feature's base is wrong and every diff lies.

The full reasoning, including the adversarial review that shaped it, is in
`doc/skooma/.workflow-design.md` and `.review-verdict.md` (untracked, main checkout only).
