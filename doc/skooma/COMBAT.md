# Combat — index

## What it is

Viewer features for combat in Second Life. Most of them read the combat log, the events the
region reports for damage, deaths and kills, which reaches the viewer through the **LSL
Bridge** (a foundation, see [SKOOMA.md](SKOOMA.md)). The camera and the render lockout work
without it. Branches are named `combat/<name>`.

Status: `combat/ots` landed 2026-10-04; `combat/lockout` landed 2026-10-14 (also removed the mouselook IFF markers, crosshair name and position/health overlay) (hitbox convergence is the Avatar Hitboxes box until P1; options under the new Combat menu); `combat/minimap` (mini-map and tactical overview, "tacmap", see [MINIMAP.md](MINIMAP.md)) lands continuously; the rest planning. Written from scratch; ideas from Soapstorm's combat branches
(`t3code/combat-log-analysis-tool`, `feature-combat-render-lockout`) are summarised in
`.combat-log-soapstorm-lessons.md`.

## Sub-features

| Sub-feature | What it does |
|---|---|
| **Kill Feed** | An interactive, scrollable overlay that shows who killed whom with what |
| **Detective Mode** | Browse the combat log and review how a combat session went |
| **Damage Numbers** | RPG-style popups over the damaged object: a damage-type icon followed by the amount. Further hits add to the number and restart the popup's lifetime. Icons from Soapstorm, which took them from [NexiiLSL `damage-types.lsl`](https://github.com/Martin-Pitt/NexiiLSL/blob/main/damage-types.lsl). Nexii built an earlier LSL HUD version |
| **Hit markers** (`combat/hitmarkers`) *(Soapstorm port)* | A crosshair flash and sound when your hit lands, as `fscombathitmarker` did in Soapstorm. Filters for agent-only and collision damage, its own volume, a test-sound button |
| **Over-the-shoulder camera** (`combat/ots`) *(Soapstorm port)* | Third-person aiming camera over the shoulder that uses mouselook input (Soapstorm's `CAMERA_MODE_OTS`, `floater_fs_ots_options.xml`). The camera passes through phantom prims and tracks the avatar's live position while changing modes. Crosshair convergence aims where weapon rays land: on the target avatar's real physics shape (see pin P1), ignoring bystanders, attachments and phantom surfaces, smoothed at point-blank range |
| **Render lockout** (`combat/lockout`) *(Soapstorm port)* | **Landed 2026-10-14.** Anti-wallhack: while in mouselook or OTS and for 5 s after, render metadata, beacons, look-at and point-at are hidden, and wireframe, highlight transparent, hidden selection and the world geometry render types are forced, your own state restored when it lifts (`SKCombatLockout`; Soapstorm `feature-combat-render-lockout`, commit `8f635810aa`). Client-side, no sim flag. Also removed mouselook's offscreen IFF arrows, crosshair name and position/health overlay. Every combat feature has to respect it |
| **Combat menu** | **Exists.** The top-bar Combat menu: Over-the-Shoulder Mouselook and its options, Tactical Overview and its options, Mini-map Options |
| **Target markers** *(Soapstorm port)* | Markers on targets within a set range, optionally only with line of sight, optionally with the target's name |
| **Rez queue indicator** *(Soapstorm port)* | A line above the crosshair while the region's asset download queue is backed up and the sim refuses to rez (weapons stop firing), then a fading "cleared" line with how long it lasted. Read from the pending downloads/uploads in SimStats, so no script is needed. Soapstorm `fsrezqueue.*`, commit `e794041da8`, a port of Nexii's LSL rez queue checker and its thresholds |
| **Minimap deaths** | The [minimap](MINIMAP.md) marks where allied combatants and vehicles died (the kill feed seam, `.minimap-plan.md` §6) |
| **Group colours** | Colour by group so sides are easy to tell apart, on nametags and in the Kill Feed. Seeded from SLMC group data (Nexii to share). Not on the minimap: in damage-enabled parcels it shows allies only, never enemies, to prevent cheating, the same goal as the render lockout. Idea from Soapstorm's group nameplate tinting (a Megapahit port) |
| **Ghost temp attachment cleanup** *(candidate)* | Remove temp attachments that fail to detach after a teleport (Soapstorm). Only if it still happens in Skooma; not yet seen, unlike Firestorm |

## Depends on

- **LSL Bridge**: combat log listening
- **Silhouette capture** (`nav/capture`): weapon outlines for the Kill Feed and other combat UI
- **Minimap**: for the minimap deaths only
- `combat/ots` and `combat/lockout` need no bridge; the lockout covers OTS, so it lands with or after OTS

## Pins

| # | Open question | Blocks |
|---|---|---|
| P1 | **The avatar's real hitbox: measured 2026-10-15.** Not Firestorm's Avatar Hitboxes box (Soapstorm converged on it, `fscombathitmarker.cpp:599`, wrong at the feet and edges): `llCastRay` box scans fit sharp convex polytopes within 2 mm, one per state, in a frame of x forward, y left, z up, origin `OBJECT_POS`, turned by the body rotation (`llGetRot`). **Standing** (24 faces): a spindle from a ±0.051 m square at z = -h through two rings to a ±0.113 x ±0.101 m top at z = +h, h = agent height / 2 + 0.05; widest ±0.301 m across, 0.225 m forward. Ring B's height rule is fitted to two sizes only. **Ground sit** (8 faces): a frustum from ±0.226 x ±0.151 m at z 0 to ±0.325 x ±0.451 m at -0.446. **Prim sit** (9 faces): a wedge, flat back at x = -0.538. Both sits measured at one height (1.189 m) and zero sit rotation. Agent x/y are always 0.45/0.60 whatever the height. In the viewer as `skcensusavatarshape.*`, drawn by the tactical overview; models, probe and fitter in `V:\Scratch\avatar-shape`. Open: a third standing height, other sit heights | OTS convergence can now build on it |

## Read in this order

| doc | what | status |
|---|---|---|
| `.combat-log-soapstorm-lessons.md` | What Soapstorm's combat log work did, per sub-feature: SL combat log facts and limits, bridge wire contract, the 12-entry Detective design round, damage-type icon atlas, render lockout | done, 2026-09-27 |
