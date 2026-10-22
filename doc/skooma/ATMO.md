# Atmo Magic — index

## Context

Ok, here is some context for why I started this.

I was building a day cycle, it had a few keyframes already setup for sunset, noon, sunrise and night. I wanted to make the sun and moon move properly across the sky. So I started with calculating the azimuth/elevations and added additional keyframes. This is where it all fell apart, because in EEP, a keyframe is not a value, it is a whole sky asset. Every keyframe I added was another complete asset to fill in. Every atmospheric value worked out by hand for that moment and pasted in. Changing how sunrise looked was not one edit, it was the same edit across every keyframe of the sunrise window.

Nothing that cycle knew that haze density was a curve over a graph across only specific times, but I knew. The file only held my computed samples of it.

In EEP, time and parameters are welded together. So if you want to finetune a detail, you had to re-author everything by how many keyframes you added.

Atmo Magic was instead my vision, a model similar to animation and video editing tools, with parameters over time instead of assets over time. One table declares every paramter, then each parameter is configured on its own row.

For parameters, the behind the scenes looks like:
- **constant**, this is where we make big savings in efficiency, there are a lot of parameters that never change usually
- **keyframes**, at whatever density and interpolation the author desires
- **driver**, a computed value driven by code, think of it like avatars procedural animations, for example the sun and moon positions can come out of an orbital model evaluated at whatever current time it is, exactly the thing I had been sampling by hand





Some context for why I started this.

I was building a day cycle where the sun and moon moved properly across the sky. Getting the motion right meant keyframes close together, and that's where it fell apart, because in EEP a keyframe isn't a value, it's a whole sky asset. Every keyframe I added was another complete asset to fill in, with every atmospheric value worked out by hand for that moment and pasted in. Changing how sunrise looked wasn't one edit, it was the same edit re-derived across every keyframe in the sunrise window.

Nothing in that cycle knew haze density was a curve. I knew. The file only held my hand-computed samples of it.

That's the part I kept coming back to. In EEP, time resolution and parameter authoring are welded together, so wanting finer detail in one thing costs you re-authoring everything, multiplied by however many keyframes you added.

So I've been building the other model, parameters over time instead of assets over time. One table declares every parameter once, and each parameter is then configured on its own, as one of three kinds:

- a **constant**
- **keyframes** (formerly "curve") for that one parameter, at whatever density it needs
- a **driver**, a function of time supplied by a module, so the orbital module puts the sun where orbital mechanics puts it and there is nothing to sample at all

Sunrise becomes one set of keyframes I reshape once, instead of thirty numbers I recompute per keyframe.

The part I think makes it worth your time is that it isn't a replacement for EEP, it's a modifier on top of it, and it decides whether to apply by mirroring your own layering. Same stack: edit, local, push, parcel, region. Whatever EEP produced is the base it writes onto, and a layer stands down whenever the environment showing comes from a more specific authority than itself. A region-wide document applies where a parcel is set to use region environment, and stands down where the parcel owner has set their own sky, so someone who buys an EEP asset and sets it on their parcel never finds it painted over by the estate. A parcel document stands down for an experience push, because agent scope outranks land scope and that's your precedence, not mine to invert. Switch it all off and you see exactly what you see today.

Two of those layers have no producer in a TPV. A region has nothing that can carry a document, and while experience pushes reach us perfectly well as EEP environments, nothing can push an Atmo document at an agent. That isn't a gap in the model, it's the part only you could fill.

Extending it means adding rows to the table. The first module was the EEP sky itself, 28 of its parameters exposed and written onto the sky the viewer already builds, and the table didn't change to accommodate it. Clouds, wind, precipitation and water all go the same way.

What I'd most like your eyes on is the format, because the client side is the easy half. EEP is already evaluated on the viewer: the sim stores a settings asset and says which one applies where. A document like this sits in that same slot, with no new work for the simulator. It would also give scripts something EEP can't, since parameters are addressable by name, so setting sky haze to 0.4 over ten seconds becomes a call rather than a new asset. Any EEP day cycle can be written as per-parameter keyframes. The reverse isn't true.











## What it is

Atmo Magic is a per-parcel document of up to four **tracks**, and each track is a self-contained world: a
Martian base, a fantasy planet under a gas giant, a space station, a tropical beach with tides. You are in
exactly one track, chosen by altitude band; only it is evaluated and rendered; crossing continuously
crossfades values and hard-cuts structure, teleporting snaps. Tracks, precipitation media and event clips
are **tradeable notecards**; the document is just the bundle a parcel loads. Each track has its own seed,
its own library, and an optional `represents` mapping that changes only what the observer experiences,
never where content-touching things sit.

Everything is authored per parameter through one registry: values, keyframes, drivers, and clips with a
replace-with-envelope over the track. The wild weather list is not thirty features but three authorable
kinds: **precipitation media** (look, behaviour, surface response, impacts, sound), **conditions** (the
weather cube plus extra params, archetypes as presets), and **events** (clips with phases over
event-local time, placed anywhere including the void, driving only registered params, deposits, emitters
and audio cues; flora, gravity, damage and HUD stay with LSL).

The module catalogue sits under five parent tabs. Space: system, orbital, celestial. Atmosphere: sky,
clouds. Weather: weather, wind, precipitation, events. Surface: surface, water, surround. Effects:
screenfx, acoustics. Debug is its own floater. Clouds use one density model behind two renderers chosen by
graphics preference; the far plane is raised on reverse-Z so surrounds and procedural terrain draw normally
out to tens of kilometres, with a squish sibling on macOS; the surround converts rezzed linksets into local
objects that may never overlap a connected region and are hidden rather than clamped when they do; water
gets its own renderer with tides as a visual modulator and foam as a first-class visual; the atmosphere
model is Rye's: authored as Bruneton density profiles (already in EEP), rendered Hillaire-style when he pushes it.

All design docs live in the **main checkout** `D:\viewers\alchemy-skooma\doc\skooma\` (gitignored
`.atmo-*.md`; read by absolute path from any worktree). The world field / Recast navmesh (spec §7.7,
`.worldfield-plan.md`) is now its own feature: see `NAVMESH.md` (being phased out for the census and air cells, `AIR.md`); Atmo's surface ledger and acoustics
consume its floor sheet and wall faces when they are built. Predecessor: Soapstorm `D:\viewers\soapstorm`
(lessons only, no code reuse; `ssatmoenvplanetarystate.*` maths and `SSAtmoLandscape`'s conversion state
machine are the two things worth carrying).

## Read in this order

| doc | what | status |
|---|---|---|
| `.atmo-status.md` | **handoff: where every branch and run stands, what to do next, gotchas** — read first in a new session | kept current |
| `.f5-checklist.md` | owner's F5 checklist: everything on develop not yet verified in the viewer, by area | kept current |
| `.atmo-spec.md` | **the spec v3** — description, D1–D7, invariants, document/track model, registry, provider, persistence, every module by parent tab, editor, gating, rendering rules, **branch sequence §13** | authoritative, 2026-09-17 |
| `.atmo-scope.md` | the five-round scope interrogation (questions, owner answers, consolidated understanding) | input to v3 |
| `.atmo-feature-inventory.md` | ~90-row inventory of every Soapstorm/Skooma/Alchemy feature with its status | input |
| `.atmo-cataclysmic-events.md` | owner's five multi-phase cataclysms (StarRupture, Stalker, Void-Bleed, Chrono-Tornado, H-Bomb) — the clip model's test cases | reference |
| `.atmo-review-verdict.md` | 2026-09-16 rulings with evidence, measurements M1–M10; its branch table is **superseded by spec §13** | audit |
| `.atmo-review-nexii.md`, `.atmo-judge-*.md`, `.atmo-review-adversarial-*.md` | the adversarial/judge round | audit |
| `.atmo-build-<branch>.md` | **per branch**: the synthesised `## Plan`, the build `## Result`, adjudication and fix pass. Exist for `sky-seeded`, `atmo-registry`, `atmo-provider`, `atmo-sky`, `atmo-orbital` (code complete 2026-09-22, awaiting integrate + F5), `atmo-celestial` (plan only; base blocked on `sky/seeded` reaching `next`), `atmo-editor` (code complete 2026-09-23, unrun; F5 checklist in `V:\Scratch\atmo-editor\astra-branch.md`) | current |
| `.atmo-workflow-v1.md` | how agents build the v1 branches: two runs per branch (plan → pause → build/review/verify/fix), constraints, per-branch briefs, the two scripts; §8.5 the 2026-09-22 overnight lessons (oracle-first, OpenCode `build` mode, Astra quota) | current |
| `.atmo-handoff-external-plan.md` / `.atmo-handoff-external-review.md` | the prompts handed to GPT-6 Astra (via `codex exec`) as the second model line: planner and judge, review lenses and refuter | current |
| `.atmo-scattering-postmortem.md` | Linden's 2018 precomputed-scattering attempt and why it died; constraints for any implementation | reference |
| `.atmo-reconcile.md` | R1–R43 contradictions between the older docs | historical |
| `.atmo-plan.md`, `.atmo-architecture.md`, `.atmo-schema-compat.md`, `.worldfield-plan.md`, `.atmo-ui-*.md`, `.atmo-soapstorm-lessons.md`, `.research-eep.md` | earlier research; read their banners first | superseded where they disagree |

## Decisions at a glance

- **No forked sky dome.** Layer seam + own cloud pool + one adapter injection in `LLEnvironment::update`.
- **Layers mirror EEP:** `Edit > Local > Push > Parcel > Region`; `Push`/`Region` have no producer in a TPV
  and say so. Atmo *modifies* whatever EEP produced rather than competing with it, and a layer stands down
  only when the EEP environment showing comes from a **more specific authority** — so a bought EEP set on a
  parcel is never painted over by the estate's region document, and "use region environment + my own parcel
  document" still works. Built with `atmo/parcel`; a no-op before it. Spec §5.
- **Tracks are worlds.** Per-track seed and library; isolated; bands by altitude; `represents` is observer-only;
  `day.length`/`day.offset` authored per track; transitions are a viewer debug setting for now.
- **Assets:** document / track / medium / clip are notecards in plaintext LLSD notation; refuse at 64 KB;
  Save vs **Save to Parcel** (compact `atmo;<z85>` marker, 255-char description).
- **Registry:** `module.param` once, values per track; tags `authored | source | world-local`; sources
  value | keyframes (formerly curve) | driver | library override; clips are the one replace-with-envelope; fields Add/Mul/Max/SignedAdd.
- **Clouds:** one density model, puff renderer (class 1–2) + raymarch (class 3); decks in SL z with quality
  shares; beer shadow map v2; far shell as a dome layer.
- **System:** geometry (sphere / plane / ring, cylinder, sphere interior) × motion (helio / geo / fixed +
  occluder) × atmosphere on/off; one atmosphere model (Rye's: EEP Bruneton inputs, Hillaire renderer); "Realistic" = a preset of drivers.
- **Surround:** rezzed→local objects (purple), beyond the edge, encroachment → hidden, anchor region/grid,
  budgets, 4 km megaprims on the 32 km reverse-Z far plane; procedural terrain.
- **Water:** our ocean renderer, tides visual-only, foam, ripples; Rye consulted first.
- **Determinism:** integer-exact / float-approximate; τ = `LLDate::now() + gUTCOffset`; state = anything
  another client can point at.
- **Client-render-only:** LSL keeps describing EEP; gaps accepted and mitigated (bake-to-EEP, schedule export).
- Names settled: `SKSignal*`, `SKAtmoProvider`, `SKFloaterAtmo`, Track tab.

## Branch sequence (`git skooma new <group>/<name> --base <parent>`)

Roots: `sky/seeded` · `atmo/registry` · `sky/layers` · `render/farplane`. Stack: registry → `atmo/provider`
→ `atmo/sky` → {`atmo/celestial`, `atmo/orbital`, `atmo/editor` → `atmo/keyframes` → `atmo/notecard` →
`atmo/parcel`} · `atmo/clouds` (base sky + farplane) → `atmo/weather` → `atmo/events` · `atmo/water` ·
`atmo/surround` (base editor + farplane) · `atmo/clouds-raymarch` · `atmo/farshell` · `atmo/bodies` ·
`atmo/surface` · later `screenfx`, `acoustics`, `debug`, `worldfield`. Full table with must/must-not: spec §13.

## Standing rules

- `git skooma sync` before any evidence pass (last: 2026-09-16, `next` `2fa5725fd8`).
- Cite `file:line` with the ref.
- Review rounds: `.atmo-review-adversarial-<x>.md` → `.atmo-judge-{technical,pragmatic}.md` →
  `.atmo-review-verdict.md` → spec revision; scope rounds: `.atmo-scope.md`. Tests in `V:\Scratch\atmo-*\`.
- Keep this file current when the spec's decisions or the branch sequence change.
