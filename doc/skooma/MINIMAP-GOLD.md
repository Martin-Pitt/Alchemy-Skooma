# Mini-map gold standard — index

## What it is

An offline, slow, exact-as-the-data-allows rendering of what the mini-map should show, computed from a **census
export** (the physics geometry the viewer captured) and nothing else, plus a harness that scores any candidate map
against it per layer, per class and per wall. It is the fixed target the fast map (`MINIMAP.md`) is benchmarked
against: "here is a wall per the census, your render is 0.5 m off", "this doorway is sealed", "this floor is drawn
on the storey below".

Lives in `V:\Scratch\minimap-gold\` (nothing in the repo). Built 2026-10-12. The parallel `V:\Scratch\minimap-reference\`
(another session's DeepSeek workers, same day) is an independent second reference with the same aim; the two can be
cross-checked, which is what the contract there asks for.

## Read in this order

| doc | what | status |
|---|---|---|
| `V:\Scratch\minimap-gold\README.md` | the loop: build the gold, score a candidate, browse, validate | current |
| `V:\Scratch\minimap-gold\CONTRACT.md` | frames, grids, `case.json`, `reference.npz`, candidates, metrics | v1 |
| `V:\Scratch\minimap-gold\GOLD-DESIGN.md` | how the gold is computed (column model, floors, layers, probes, comparison) and what it does not claim | current |
| `.minimap-gold-design.md` (this folder) | the design record: decisions, the seats, results, open questions | 2026-10-12 |

## Decisions at a glance

- **Column model**, not plane slicing: every 0.0625 m cell gets the exact union of solid z-intervals from every record
  (triangles conservatively per cell, solid bodies by parity, analytic shapes in closed form, terrain bilinear), at
  twice the navmesh's resolution and with no erosion or walkability filter. Shares no code with `skmapsection` or the
  scratch section oracle, so agreement is evidence.
- **Layers are storeys and connectors**, found from the geometry: flat floor floods within a climb (a 0.15 m kerb or a
  shallow ramp does not split a storey; a stair does); connectors join two storeys; roofs and hillsides are slopes.
  Buildings and rooms are not grouped (the owner's rulings make that topological; a later pass).
- **Cuts per cell from the floor reference** (nearest floor within 3 m): `low_cut` / `high_cut` at + 0.5 / + 1.5 m
  follow sloping floors and stairs exactly; `low_only` / `both` / `high_only` stay evidence, never semantic cover
  (the leaning-wall case).
- **Walls are scored as faces** (the solid's faces at the cut planes against the boundary of what the candidate
  draws, so a stroke and a filled wall score alike), floors on area, doors and holes by probes; a candidate never
  supplies its own validity mask, never regenerates a reference; wrong case id or grid is rejected.
- **Floors stop at 50°**: a support sample on a smooth face steeper than `max_slope` is a wall, not a floor
  (leaning walls, cliffs), whatever the census's 74° avatar class can climb. Found by the tilted-house fixture,
  together with the sheet thickness that must not hang under solid bodies.
- **Renders in the map's language**: height-shaded floors, poché walls, hatch, contours, flights with rungs and an
  arrow; one stacking level per overview, never every storey in one plate. The bar is proto2's look.
- **GLM 5.3 Flash seats as hands** (`seats/`): analytic fixtures with answers by construction (A1–A3), the navexport /
  proto2 / png adapters, the HTML browser, the comparator's adversarial tests; each in a sandbox with its brief.
  GLM finished the mechanical jobs; the two longer fixture briefs killed their seats silently (twice) and went to
  Opus subagents. An Opus read-only review of the engine found four high-severity defects, since fixed and pinned.
- **Image candidates are scored on their footprint** (the camera's, declared as `in_image`), with the footprint's
  share and an edge-displacement statistic reported, so a screenshot smaller than its case is judged on what it shows.

## Status (2026-10-12)

`python tests/run_all.py` passes 5 of 5 suites (column model, review regressions, comparator smoke, fixtures, the
twelve adversarial comparator tests); fixtures A1 8/8, A3 9/9. Resdayn region: 1371 layers in 12.7 min. The
navmesh export scores median floor IoU 0.86, wall-face recall 0.96, precision 0.82 on the 60 largest storeys; proto2
0.85 / 0.94 / 0.79; a proto2 screenshot 0.98 / 0.98 / 0.99 on its footprint. Browse `out/resdayn-20261008/index.html`.

## Standing rules

- The navmesh export is a **candidate**, never an input of the gold.
- Fixture expectations come from construction inequalities, never from a slicing algorithm.
- Keep this file and `.minimap-gold-design.md` current when the contract or the model changes.
