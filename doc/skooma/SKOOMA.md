# Alchemy: Skooma

Skooma is an experimental derivative of the [Alchemy](https://www.alchemyviewer.org) viewer
for Second Life. Expect it to be unstable: this is where new ideas get built and tried
before they are ready for anyone else.

Lineage: Linden Lab viewer (`LL`) → Firestorm (`FS`) → Alchemy (`AL`) → Skooma (`SK`).
Features are written from scratch for Alchemy's current pipeline. Each feature is built so it
could be offered back to Alchemy on its own.

Skooma is a fork to carry over the work I (Nexii) originally started in Soapstorm, such as Atmo Magic.
Soapstorm is a viewer created by Soap Frenzy.
Orion Starchild also contributed to it through Skoomastorm, which was later merged into Soapstorm.
Some of the ideas are borrowed from Soapstorm, for example Over-The-Shoulder (OTS), Kill Feed, etc.


## Features

| Feature | What it does | Index |
|---|---|---|
| **Atmo Magic** | 1) Parameters over time instead of assets over time. A per-parcel document in which every parameter is: a) constant, b) keyframes for that one parameter, or c) driver, a function of time supplied by a module. It sits on top of EEP as a modifier and follows EEP's own edit > local > experience > parcel > region precedence; switched off, you see plain EEP. 2) With this foundation, modules can plug into that one parameter table, grouped under: Space (system, orbital, celestial), Atmosphere (sky, volumetric clouds), Weather (weather, wind, precipitation, events), Surface (surface, water, surround) and Effects (screenfx, acoustics) | [ATMO.md](ATMO.md) |
| **Vertical draw ratio** | The draw distance (the Graphics slider for how far objects are drawn) can be set shorter above and below the camera than it is horizontally | [INJECTIONS.md](INJECTIONS.md) |
| **Flat prim sides** (`render/prim-split`) | Untwisted box and prism prims are drawn as flat quads, two triangles a side, without the old edge splits that only served per-vertex lighting (a box goes from 108 to 12 triangles at the highest LOD). Twisted prims keep the splits, and a LOD change that builds the same mesh skips the rebuild. Develop > Render > Render Tests > Legacy Prim Edge Splits brings the splits back for comparison | [INJECTIONS.md](INJECTIONS.md) |
| **Minimap** | A full redesign of the minimap, drawn from the overview capture with census meshes and a floor cut from the air cells; optional layers for stairs, doors, ladders, elevators and contours. A faint Automap shows it over the world in mouselook. Settings live in their own floater | [MINIMAP.md](MINIMAP.md) |
| **Tactical overview** ("tacmap") | A modal view that replaces the world with a Battlefield-style tilt-shift overview of where you are. The world is drawn in a data-viz style (census meshes over a ghost of the real geometry, colour by relative height, outlines, no textures or lighting), with a floor cut for interiors that dissolves round people close up, nametags, projectiles, and vehicles as captured outlines. Alt-cam, flycam, WASD and a pan tool move around it | [MINIMAP.md](MINIMAP.md) |
| **Overview capture** (`skoverviewcapture`, part of `combat/minimap`) | An orthographic G-buffer render of the region and its neighbours in a data-viz style: colour by relative height, depth and normal outlines, no albedo or lighting. Census meshes, drawn from GPU slot pages, sit over a ghost of the real geometry; the floor is cut per indoor occupant from the air cells and dissolves round people close up; moving objects are patched in rather than recaptured. The base map of the mini-map and the tactical overview. Moves out to a foundation of its own when the 3D worldmap starts using it | [MINIMAP.md](MINIMAP.md) |
| **3D worldmap** (`feature-worldmap`, map mode landed; tabs and impostors planned) | A modal map that replaces the world view: zoom out, Dyson Sphere Program style, from your region through its neighbours to the whole grid, seen in 3D like Google Maps with a light tilt-shift and an overlay showing where people are. Alt-cam or fly-cam around it; entering and leaving restore each camera, and a home key re-centres on you. Tabs for search, the Destination Guide in 3D, and your own places: a searchable visit history (like Google Maps Timeline) with pins and suggestions. Regions you visit are kept as relightable impostors | [WORLDMAP.md](WORLDMAP.md) |
| **Alt-cam zoom to cursor** (`camera/zoom-to-cursor`) | In alt-cam the mouse wheel zooms toward or away from the point under the cursor, which stays fixed on screen, so the focus slides onto what you zoom into and orbiting then pivots around it. Picks as far as alt-click does (`altcam-click-distance`: the far plane, open water, the void). Sky, HUDs and appearance mode keep the classic zoom; debug setting `SKCameraZoomToCursor` off restores it everywhere | [INJECTIONS.md](INJECTIONS.md) |
| **Smooth camera follow** (`camera/follow`, in the `camera` trunk) | The alt-cam focus on a moving object or vehicle follows a filtered track of it instead of jumping with each update. Develop > Network > Smooth Camera Follow (`SKCameraFollowEnabled`) | [MOTION.md](MOTION.md) |
| **Object motion updates** (`object-motion-updates`) | Two fixes to how moving objects follow their updates. Clamped velocity recovery: velocities the terse update clamps to ±128 m/s (bullets, fast vehicles) are recovered, so they no longer snap back along their path (`SKMotionRecoverClampedVelocity`). Smooth update corrections: a moving object's position error at each update is bled out over a fraction of a second instead of snapping; large jumps still snap (`SKMotionSmoothCorrections`). Both under Develop > Network | [MOTION.md](MOTION.md) |
| **Ghosted object cleanup** (`workaround-ghosted-physical-objects`) | Temporary physical objects (bullets, thrown objects) that the viewer strands at a region border or that stop without being told to are hidden at once, sound included, then the region is asked whether they still exist; one still there a second later comes back. Judged by distance (speed × silence ≥ 5 m), not time, and never while the circuit is stalled. A workaround until the simulator sends their kills reliably | [INJECTIONS.md](INJECTIONS.md) |
| **Collapse to edge** (`minimize-vs-collapse`) | A minimized floater folds against the nearest screen edge as its title bar instead of going to the minimized row | [INJECTIONS.md](INJECTIONS.md) |
| **F13–F24** (`input/extended-fkeys`) | The extra function keys can be bound like F1–F12, on Windows and SDL | [INJECTIONS.md](INJECTIONS.md) |
| **Loudness Normalization** | BS.1770 loudness is measured while the sound decodes, so clips play back at a consistent level | [INJECTIONS.md](INJECTIONS.md) |
| **Over-the-shoulder mouselook** (`combat/ots`) | M opens mouselook with the camera behind the shoulder and the avatar drawn; scrolling in still gives first person. Shots converge on the crosshair: the camera and the body rotation sent to the region both aim from the eye at what the crosshair raycast hits, so guns using `llGetCameraRot` or `llGetRot` land where you aim. Options under the Combat menu | [COMBAT.md](COMBAT.md) |
| **Combat render lockout** (`combat/lockout`) | Against wallhacks: in mouselook (the shoulder view too) and for 5 s after, debug views that see through walls are off (render metadata, beacons, look-at, point-at), and wireframe, highlight transparent, hidden selection and hidden world geometry are forced back, your own settings restored when it lifts. Mouselook's offscreen arrows, crosshair name and position/health text are removed | [COMBAT.md](COMBAT.md) |
| **Combat** *(planned)* | Kill Feed, Detective Mode, Damage Numbers, hit markers and minimap deaths from the combat log (via the LSL Bridge) | [COMBAT.md](COMBAT.md) |
| **Squeeze** *(planned)* | Textures stay BC7-compressed in VRAM, about a quarter of the memory, encoded in the background. A from-scratch rewrite of Soapstorm's Squeeze, using the same encoder: Basis Universal's `bc7f`, vendored unmodified (Apache 2.0) | — |
| **Long sound upload** *(planned)* | Sounds longer than the grid's clip limit are split into numbered parts with click-free edges instead of being rejected, and loudness is normalised on upload, measured across all parts. Idea from Soapstorm | — |

## Wishlist

Ideas not yet committed to.

| Idea | Note |
|---|---|
| **HUD anti-aliasing** | Smooth the edges of HUD attachments. Rye plans MSAA for the HUD layer upstream (2026-10-03), which replaces Soapstorm's supersampling; wait for it to land in `next` |
| **Dynamic near clip** | Adjust the near clip plane as the camera moves. Skooma's depth setup differs from Soapstorm's focus-scaled near clip, so only the dynamic adjustment carries over |
| **Procedural mesh tools** | In-viewer generators for useful meshes: roads, streets and pavements, and 3D terrain tiles that look good, with erosion, cliffs and similar features |
| **Silhouette textures for creators** | Expose silhouette capture as a tool, so content creators can generate outline textures of weapons, vehicles and other objects for their own HUDs and UI |
| **Procedural textures** | An in-viewer tool for content creators to generate textures from node graphs, in the style of [Arcanigen](https://arcanigen.thatrobhuman.com/), built from GLSL SDF shapes ([Pixel Spirit Deck](https://github.com/patriciogonzalezvivo/PixelSpiritDeck/)) and Blender-style nodes. Spec: [TEXTURE-STUDIO.md](TEXTURE-STUDIO.md) |


## Foundations

Shared systems the features above build on. More features will be added on top of them.

The world foundations stack in processing order, each built on the one before and never calling its consumers: **physics shapes** → **census** (the physics world) → **air cells** (processing of the census) → **rooms and spaces** (processing of the air cells). Each is its own library (`skcensus`, `skair`, `skrooms`), so it can be built, tested and benchmarked alone. The Recast navmesh was dropped 2026-10-22; its useful parts moved into these layers. Plan: `.foundations-plan.md`.

| Foundation | What it provides | Used by | Index |
|---|---|---|---|
| **Physics shapes** (`world/physics-shapes`, built) | What an object's physics shape is, decided in one place: a mesh's `physics_havok` section is fetched and read, so the simulator's own collision triangles are known, not just the uploaded shape, with hulls, decompositions and prim shapes as the fallbacks. The physics shape display draws through it, coloured by cost or by where the shape came from | Census; physics shape display | [INJECTIONS.md](INJECTIONS.md) |
| **Census** (`world/census`, planned extraction; today inside `feature-navmesh` and combat) | The physics world: a record of every solid object near the agent and camera, from its physics shape, plus terrain, grouped into structures (clusters). It classifies doors, ladders, elevators and vehicles, and keeps cached objects past the draw distance as boxes. It runs off the main thread in small slices. Library `skcensus` | Air cells; mini-map and tactical overview (census meshes) | [NAVMESH.md](NAVMESH.md) |
| **Air cells** (`world/air`, planned extraction) | The empty space above each solid, built off the census and stored as a quadtree of 0.125–2 m cells, with wall face offsets. Each cell knows whether it is open to the sky, sheltered or indoors. Built eagerly out to 300 m. Library `skair`, rasterising census triangles into columns of its own | Rooms and spaces; tactical overview (floor cut, dissolve); mini-map (whereabouts, roof occupancy); Atmo weather and acoustics (later) | [AIR.md](AIR.md) |
| **Rooms and spaces** (`world/rooms`, planned extraction) | The air cells processed into enclosures, rooms, spaces and portals (doors, doorways, arches, openings), with walls, floors, wind and probe placement; stairs detected from the air (new). Answers "is this person indoors, and on which floor?" Library `skrooms` | Reflection probes; tactical overview; mini-map | [AIR.md](AIR.md) |
| **Modal view** (`modal-view`, landed) | A shared way for a view to take over from the world. It hides the world (render, probes, finalize, hover text) while the world keeps updating, snapshots and restores the camera, and routes the existing alt-cam and flycam to the mode, with a pan tool. The avatar stays safe: no movement, autopilot reports failure, no mouselook or snapshots, RLVa camera rules honoured. The sim, sound and voice stay at the avatar. `skmodalview` | Tactical overview; 3D worldmap | [WORLDMAP.md](WORLDMAP.md) |
| **Outline capture** (`skoutline/capture`, landed) | Renders an object as a clean outline image, with a policy for which captures to keep and a cache of the best one | Mini-map and tactical overview (vehicle outlines); Kill Feed and other combat UI (weapon outlines, planned) | — |
| **Hover context & overlays** (`ui/hover`, landed) | A hovered widget publishes what it is, and anything can read it. Registered drawers show 3D, world-anchored 2.5D or 2D overlays in response | Atmo editor (ghost keyframes); Mini-map; Combat | [UI-HOVER.md](UI-HOVER.md) |
| **LSL Bridge** *(planned)* | A script bridge for data only LSL can reach: combat log listening, notecard fetching, and more | Combat | — |
| **Local objects** (`render/local-objects`, planned) | Better support for viewer-local objects: the far flag (drawn past the draw distance out to the 8 km reach under the infinite projection), a purple selection outline and related polish. Sits between the far plane work and Atmo's surround | Atmo surround | — |


## Working on Skooma

| Doc | For |
|---|---|
| [DEVELOPING.md](DEVELOPING.md) | Humans: setup, branches, daily loop |
| [WORKFLOW.md](WORKFLOW.md) | Terse reference: `next` / `develop` / `feature-*`, `git skooma` commands |
| [UPSTREAM.md](UPSTREAM.md) | Parts that can go to Alchemy on their own, and what each depends on |
| [INJECTIONS.md](INJECTIONS.md) | Every `<SK:Nexii>` edit to upstream files, grouped by feature |
| [UI-LAYOUT.md](UI-LAYOUT.md) | XUI layout rules for floaters and panels |
| [UI-HOVER.md](UI-HOVER.md) | Hover feedback across the UI and overlays in the world |
