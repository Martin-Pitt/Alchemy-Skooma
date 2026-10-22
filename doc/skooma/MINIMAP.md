# Mini-map — index

## What it is

A replacement for the classic mini-map (`LLNetMap` / `LLFloaterMap`), for everyone, not only combat.
Only the shell is inherited: the chrome-less transparent floater with drag and resize, and the
`mini_map` registration. Every interaction is redesigned in a UX pass; all options live in a settings
floater opened by a gear button beside the close button, not in a right-click menu. The navmesh
(`NAVMESH.md`) supplies the new content: a floorplan of walkable floor, wall lines with XCOM-style
full / half / no cover, showing navmeshes connected to your current one with ramps leading up/down marked,
clarity between ledges vs walls, and a future combat-log feed for kill/death lines.

## Read in this order

| doc | what | status |
|---|---|---|
| `.minimap-plan.md` | **the design**: the navmesh contract (§1), shell + inventory of classic behaviour (§2), new elements and references (§3), rendering + bake model (§4), fair-play visibility (§5), combat-log seam (§6), widget / floater / settings floater (§7), **UX pass first cut (§8)**, **rulings 2026-09-23 (§8.1: storeys out, connected-mesh elevation, no spotting, void, defaults, sketches first)** and **rulings 2026-09-24 (§8.2: stairs ≥ 30° drawn as rungs, drop edges as coloured navmesh edges, indoor cut, projectiles, wheel zoom, bake per component per tier with z-band layers, census dependencies)**, tests, branch, injections, measurements M1–M4, questions Q1–Q7 | authoritative, 2026-09-19; §8.1 and §8.2 supersede the rows they name |
| `.navmesh-plan.md` §4.3b–§4.5 | wall segments, levels/connectors, the read API the map consumes | supplier |
| `MINIMAP-GOLD.md` | **the gold standard**: an exact offline map from the census, and the harness that scores the map against it per layer, class and wall | built 2026-10-12 |

## Decisions at a glance

- Replaces, does not sit beside, the classic map; `SKMapClassic` is gone, the overview capture is the only base.
- `SKNetMap` + `SKFloaterMap` + `SKFloaterMapSettings` are new files; no mode inside `llnetmap.cpp`.
- Storeys are out (navmesh `4047a0b7cc` removed them); the map shows the mesh under you and the meshes
  connected to it, shaded by relative altitude (`.minimap-plan.md` §8.1).
- Stairs: a built run with median slope ≥ 30° draws as straight rungs per tread height; below it is a
  ramp (chevrons); terrain keeps contours. A drop ledge draws as the navmesh edge in the drop colour,
  not a triangle per link (§8.2).
- The floorplan **bakes per connected component** into 512/1024/2048 textures per zoom tier and draws
  as **z-band layers**, so the indoor/outdoor switch is layer selection, not a re-bake; void is region
  tiles with a screen-space hatch (§8.2).
- **Automap (Mouselook map):** in mouselook the same `SKNetMap` draws faintly over the world, centred, with centre/self/outer fades (study `V:\Scratch\minimap-overlay-study` rev 04); the corner floater hides meanwhile. Span follows the corner zoom; size and fades are `SKMapML*` in the settings floater's Automap tab. Landed 2026-10-13 (`5d0bae0a2d`); report `V:\Scratch\combat-ml-map\REPORT.md`.
- **Overview capture (experiment, 2026-10-14):** branch `combat/minimap-capture` tests replacing the census-baked base with an ortho G-buffer render of the region, resolved data-viz style (relative-height colour, depth/normal outlines, no albedo/lighting), floor cut by a clip plane at agent Z + 2.5 m, LOD by agent distance, invisible blockers forced visible; behind `SKMapCaptureBase` (off). Overlays unchanged. Same capture is meant to feed a tilt-shift tactical overview later. Brief/report: `V:\Scratch\combat-minimap-capture\` (`BRIEF.md`, `REPORT.md`). Verdict 2026-10-14 (offline proxy, live cost unmeasured): complement, not replace — 1024 + a back-face section pass reaches wall recall 0.88 / floor IoU 0.66 vs navexport 0.96 / 0.84; fails on low ceilings, slopes, multi-storey; use as fallback base where no census exists.
  - **Update 2026-10-18:** capture is the only base (`combat/minimap-cleanup`, develop `e99db575b9`): `SKMapCaptureBase`, terrain/object/plan paths, the classic seeding and the presets are gone. Opt-in layers (off): `SKMapLayerStairs/Doors/Ladders/Elevators/Contours` via `sk_class_mask` in `overviewResolveF.glsl` (mini-map only, tactical unaffected). `SKMapLayerFloorplan` (mini-map) and `SKMapMLFloorplan` (automap) hide the base. Region view: `SKMapViewMode` 1 + `SKMapRegionMargin` (0.05–0.2); census range 384 m can miss far-margin neighbour geometry. Later: presets may return as user-editable modes. Leftover: `skmapplan.*` code only the deleted composite used.
  - **Update 2026-10-15:** census meshes over a ghost of real geometry, navmesh floor cut, BF6 styling (all `combat/minimap-tactical-*`, landed). Owner tried `SKMapCaptureBase` live and likes it: "works really nicely". The offline verdict above predates the census layer; replacing the baked base is now on the table (parity audit pending).
- **Tactical overview** ("tacmap"; `skoverviewcapture.*`, `skoverviewcensus.*`, `skmodalview.*`; Combat menu "Tactical Overview", `SKTacticalOverview`): a modal view under the viewer's camera drawing census meshes over a ghost of real geometry, Battlefield (north-up, region-fitting, tiltable) or Follow camera (`SKTacticalCameraMode`).
  - Key tap within `SKTacticalTapSeconds` (0.35 s) or two frames latches it open, a longer hold closes on release; pans by left drag through `SKToolMapPan` (`sktoolmappan.*`) and by the movement keys (`SKTacticalKeyPanSpeed`, Shift triples); options from its right-click menu (`SKFloaterTacticalOptions`, `floater_sk_tactical_options.xml`).
  - Cuts an indoor occupant's building under their storey's ceiling, held `SKOverviewCutReleaseSeconds`; nothing is cut for someone in outdoor air, read from the air at their feet.
  - Dissolve round each marked person (`SKTacticalDissolve*`) only within `SKTacticalDissolveReach` (64 m) of the camera and only for people in outdoor air.
  - Zoomed out, below `SKTacticalFlatMarkerPixelsPerMetre` (10) avatars crossfade to the mini-map's flat person markers at constant pixel size, drawn over everything; close avatars get name tags (`SKTacticalNametagPixelsPerMetre`); avatars are drawn as their measured physics shapes (`skcensusavatarshape.*`, `COMBAT.md` P1).
  - Shaders (`combat/minimap-early-z`, 2026-10-20): eight programs from `LLViewerShaderMgr::loadShadersDeferred` (`SKOverviewCapture::loadShaders`, table `skoverviewprograms.h`); geometry permutations `SK_CUT_ATTRIBUTE`, `SK_DISSOLVE` (the only `discard`), `SK_CAP_LIFT` (the only `gl_FragDepth`, conservative depth); clip plane, ceiling and cut are `gl_ClipDistance`; an open part rides the cap-sign clip distance. Log `overview shaders loaded`; a failure turns only the overview off until Reload shaders.
  - Tactical repair: movers and dissolve circles patch a copy of the static capture by dirty rect (restore ∪ patch, resolve grown 29 px); the census drawn from slot pages (linkset buffers, below). `SKOverviewCost` ends with GPU spans (`ALQueryPool`): `gpu tactical`, `patch`, `movers`, `resolve` ms.
  - Logs (`combat/minimap-quick-fixes`, 2026-10-20): `census cover in …: N judged in X ms of verdicts, build Y ms` (verdicts timed apart from the build); `SKNavCensusLog` adds `physics unknown U / M meshes, known after mean A s, max X s` (meshes only). The tactical key holds the region handle, not a pointer.
  - Census churn (`combat/minimap-churn`, 2026-10-21): `SKWorldCensus`'s stamp moves only when the scan's content hash does (records, features, doors, window, gap; `sknavcensusdelta.*`), and the snapshot carries `mAdded/mRemoved/mChanged`. Log `census stamp N: +A -R ~C of T records` per move; WARNS `census full walk found N record entries the stamp missed` must never show.
  - Cut rooms come from the air's room graph (`roomOf`), footprint mask in the room's own cell frame; no room (outdoor, roof, flight, frozen, no graph, sheltered without a building) takes the column cut. Overview air caches key on `airRevision()`; a cluster-heights-only change refreshes bases in place.
  - Cut table (`combat/minimap-cut-table`, 2026-10-21): every census vertex carries its linkset id (`weight` channel); cuts live in a per-linkset table (`skoverviewcuttable.*`, 16 B rows) published through `ALGPUStream`, read as storage block `SKOverviewCutBlock` under `LLRender::sCompute`, else `samplerBuffer sk_cut_table` (GL 4.1). `mCutValues` and the per-vertex cut pass are gone. Log `cut table: <rows> rows, <bytes> B published in <us> us`; Tracy `sk cut table flush`.
  - Cover (`combat/minimap-cover`, 2026-10-21): an invisible part's verdict depends on whether its visible neighbours collide. PROXY means a non-colliding neighbour holds it (OBB + 0.15 m) or the clip leaves under 0.05 m² outside. EXTRA hatches only the pieces left outside its colliding neighbours. BARRIER is a part with no neighbours, or only non-colliding ones that do not hold it, and is hatched whole. UNKNOWN_PHYSICS means a type or look is still pending or the part is too fine (`mMaxPartTris` 4096, `mMaxPieces` 65536), and it is never hatched.
  - The clip (`sknav::coverVerdict`, `sknavinvisible.cpp`) cuts the part's triangles by the neighbours' convex pieces (hulls, boxes, prisms around mesh triangles) with exact planes; no sampling. Tracy `sk overview cover clip`.
  - The top-LOD copy (`skCensusRenderFaces`, `CoverEntry::mRaw`) and the 32×32 mask are gone; neighbours are `const sknav::Record*` into the job's snapshot. Census export version 6 adds `collides` and `physics_known` per record and a `seen` sidecar; version 5 still reads (as colliding). Log `census cover in <regions>: N invisible parts: P proxies, E extra (M m2 hatched), B barriers, U not known yet (F too fine), D large dropped, J judged in X ms of verdicts, build Y ms`; DEBUGS `SKOverviewCover` per part.
  - Census dirty set (`nav/census-incremental`, 2026-10-21): a scan visits only parts an object update, kill, name arrival or roles change dirtied, plus pending parts and surrounds, and re-takes the rest from the part cache. There is a full sweep on a region change, a 16 m move or every 10 s, and dirty scans run at most four a second. Log `census scan: dirty <n> (<p> pending) of <live> objects, full <y|n> (<why>), last scan <ms> ms`; WARNS `census full sweep found <n> parts the dirty set missed` and `rooms full walk found <n> meshes the change list missed` must never show. `mVisibleOnly` is counted in `take()`, so re-taken records count too.
  - Linkset buffers (`combat/minimap-linkset-buffers`, 2026-10-21): the census draws from slot pages, one vertex buffer per region and class (`skoverviewslots.*`, 28 B a vertex), a run per linkset; the batch split and the 16M-vertex cap are gone, and only a linkset with one class run over 16,777,216 vertices is refused (WARN once per change).
  - Ids are stable per linkset root (`mLinksetIds` + `IdPool`, lowest free first, released only at `swapIn`); a rebuild sub-uploads only the linksets that changed, and the rest keep their runs.
  - Draw commands come from a compute shader (`overviewCommandsC.glsl`, `multiDrawArraysIndirect`) under `LLRender::sCompute`, and from the CPU builder (`skoverview::buildCommands`) on GL 4.1 and patch renders; both run the same level and plane test.
  - Logs: `census upload: R regions, T triangles (records X, sculpts Y, surrounds Z, hatch copies H); ten largest linksets: …` when the totals move; `census upload: C linksets (E emitted), V vertices sub-uploaded of T` every rebuild; `census pages: …` per page when a page is made or freed. Tracy `sk census cache`, `sk census slot upload`, `sk overview commands`.
  - `SKOverviewPageVertices` (3,145,728) caps a page; pages are sized 1.25x the build's demand per region and class, with a floor of 24,576, so watch `census pages:` for page creep under churn.
  - Stage 0 (`V:\Scratch\combat-minimap-linkset-buffers\out\stage0.md`): the 160M-vertex region is render-derived geometry, sculpts and surrounds; both exports hold under 3.5M record vertices (Resdayn 0.42M, combat 3.50M). Mesh soups are under 3x instancing (2.94 / 1.56; Resdayn's 4.2 overall is boxes and cylinders), so instancing was not built.
  - GPU probes (`combat/minimap-id-readback`, 2026-10-21, `skoverviewprobes.*`, llrender only): room, column and cover membership are exact occlusion queries, one per candidate linkset, over a footprint mask drawn into the depth; the surface under the feet is a 1×1 id render stamped with the buffers' signature. Results come back through a pack buffer and `ALGPUReadback`, one path on every context.
  - An answer lands a frame or two after the view opens or a person moves; the old marks stand meanwhile, covered by the 0.5 s release and the 1 s roof hold.
  - `touchesRoom`, `overColumn`, `standsOn`, `heightAt`, `planDistance` and `SKOverviewCensus::raycast` are gone; `Group::mGeom` is the upload source and keep-test identity only; the mark reason is plain "room".
  - Room and column queries draw each candidate filled, then as lines (a wall edge-on), then as points (a pole thinner than a texel). A 60–90° face over the feet now counts as cover.
  - Up to `SKOverviewProbeBudget` (8) people a frame, the agent first and the rest longest-served first; up to `SKOverviewProbeQueries` (256) queries, the frame's first issue always admitted. `SKOverviewProbeDump` writes `room-ids-<who>.ppm` (to `SK_PROBE_DUMP_DIR` or the log folder).
  - Picks read the shown frame's depth under the cursor (`mSceneDepth`), unprojected a frame later, so they land on movers and roofs; else terrain, else the floor plane.
  - `SK Overview Geometry Id` (`SK_ID_WRITE`), the fifth geometry program: writes `frag_id` only, with no discard and no depth write (C3 intact).
  - Logs: `probe: <n> issued, <m> pending, <q> queries (<k> in a frame at most, <r> refused), <f> frames latency` once a second; `probe room|column for <who>: n of m linksets [ids], f frames late` on a changed answer; `probe dump written to …`; `pick: <t> by texel, <f> by terrain or plane`; WARN `probe target did not allocate`. Tracy `sk cut probes issue` / `sk cut probes collect` beside `sk cut marks` / `sk cut roofs`.
  - Debug only: while the shader profiler runs, its `Samples` reserve refuses every probe query, so no cut appears and the `probe:` line counts them as refused.
  - Interest list: while shown, the agent update reports the overview camera, widens Far (`SKTacticalInterestFarScale`, cap `SKTacticalInterestFarCap`) and asks for whole-region updates (`SKTacticalSendCamera`), reverting on close.
- **Projectiles** are drawn from the live object list on the mini-map and tactical overview (`SKMapPeople` in `skmappeople.*`, `SKMapLayerProjectiles`, `SKMapProjectile*`): bullets as fading streaks, other fast physical munitions as their footprint. Vehicles are a dynamic layer too (`SKMapLayerVehicles`); fair-play policy is Q6.
- The **census expansion** (doors, ladders, elevators, vehicles, outdoor/sheltered/indoor coverage,
  other agent, WIP) is the map's next input and retires its geometric indoor heuristics (§8.2).
- 60 s recorded avatar motion from the nav console (shift-click Export) is requested, not built; it
  doubles as the bake-invalidation harness (M3).
- Enemy spotting is deferred; team-mates, cover/ledges and relative elevation are the priority.
- Fair play: `visibilityOf()` stays the one seam; its first body is classic behaviour.
- Combat-log events enter through `SKNetMap::addEvent()` only; no parser in this branch. Logs are formatted JSON per http://wiki.secondlife.com/wiki/Combat_Log. Kill line 0.5 s, death icon 2 s.
- Defaults: square, north-up, camera-FOV sight cone; object floors subtle grey, terrain subtle green, water subtle blue with edge gradients; void cross-hatched, void boundary thick solid, region boundary thick dotted.
- Static sketches land in `V:\Scratch\map-minimap\` before code; implementation starts on sign-off.
- Branch `combat/minimap` (renamed from `nav/minimap`: `nav/*` is for navmesh foundations, the map only consumes it), base `fix-nav/slope-grid`. Landed on `develop` 2026-10-05 (`b7f8571b51`) and 2026-10-07 (`5fcda1f319`). Built by a multi-model competition; design, brief, rulings and reports in `V:\Scratch\minimap-impl\` (`BUILDER-BRIEF.md` §13 is the F5 checklist).

## Standing rules

- UX decisions are recorded in `.minimap-plan.md` §8 as they are made; mocks in `V:\Scratch\map-minimap\`.
- Measurements M1–M4 there too. Injection rows into `INJECTIONS.md` at integrate.
- Keep this file current when the plan's decisions change.
