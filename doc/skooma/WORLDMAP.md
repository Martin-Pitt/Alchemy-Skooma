# 3D worldmap — index

## What it is

A modal map mode that replaces the world view: an alt-cam orbit or a free fly cam over the grid, zooming continuously from your region, through its neighbours, out to the whole grid: a 3D map like Google Maps in 3D, with a light tilt-shift, and an overlay showing where people are. World rendering stops while it is open; the agent stays connected. Entering and leaving each restore their own last camera, and a home action re-centres on you.

Tabs are overlays on the same 3D scene:
- **Search:** region names and places.
- **Destinations:** the Destination Guide in 3D.
- **Places:** a searchable visit history, pins, and light suggestions. SLMC combat regions are tagged automatically.

Regions you visit are cached as G-buffer impostors, relit with the current sun.

## Read in this order

| doc | what | status |
|---|---|---|
| `.worldmap-plan.md` | rulings, mode, zoom tiers, tabs, impostor cache, phases | design, 2026-10-14 |
| `.worldmap-research.md` | data sources (caps vs UDP), reusable code, citations | done, 2026-10-14 |

## Status

| branch | state |
|---|---|
| `feature-modal-view` | **integrated** 2026-10-16 (`a639a27c1d`): `SKModalView` (`skmodalview.*`, `skmodalviewmath.*`), the shared world-render gate, camera snapshot/restore and input routing; the tactical overview uses it too |
| `feature-worldmap` | **integrated** 2026-10-16 (`562f6607dc`) on the modal view: `SKWorldMapScene` (`skworldmapscene.*`, `skworldmaptiles.*`, `skworldmapmath.*`) with ground tiles, tilt-shift and agent dots; the G-buffer impostor cache and the tabs are not built |
| map pan tool | **integrated** 2026-10-18 (`42e2e8c245`): both modal maps pan through `sktoolmappan` instead of a click hook |

## Rulings

- World rendering stops in map mode. On entry, capture a live impostor unless one from this session is cached.
- Built independently of the tactical overview capture; compare later.
- Places: both automatic and curated. The history timeline is the core; suggestions are only a hint.
- Current services first. The legacy UDP map messages are kept only where nothing replaces them (see the research file).

## Overlap with the tactical overview (2026-10-16)

The tactical overview (`combat/minimap-tactical-*`, `skoverviewcapture.*`) is also a modal view that replaces the world: it skips the main world render (gate in `llviewerdisplay.cpp`, probes skipped in `llappviewer.cpp`), has its own orbit/flycam camera driven by the alt-cam controls (`LLToolCamera` intercepts, `LLAgentCamera::resetView`), tilt-shift, and the mini-map's people markers (`skmapdraw::personMarker`). Both modes intercept the same camera-tool paths; `fix-combat/minimap-worldmap-merge` merged `feature-worldmap` into the mini-map trunk so they coexist (tactical first while shown). Proposed next step: factor one shared modal gate and modal camera (snapshot/restore, orbit/fly, alt-cam routing) that both modes use, and consider the tactical census capture as the region impostor instead of a G-buffer one. Ruled 2026-10-16 (Nexii): shared foundation `feature-modal-view`, built by the world map session; the overview adopts it afterwards; no census impostor.

World map session's recommendation (2026-10-16):
- **Shared gate and camera: yes.** Put them in their own foundation branch, which both features build on, so neither PR depends on the other. It holds the world-render gate (one "modal view covers the world" query that the probe, finalize and hover-text skips read), the snapshot and restore, alt-cam routing, a flycam scale hook, and the avatar input blocks (keys, joystick, autopilot, mouselook). The world map's fixes for avatar safety, audio and voice, and AgentUpdate come with it.
- **The census capture as the impostor: no.** Its data-viz look is right for tactics, but the world map wants the real place, relit (PBR). Instead, share the **capture machinery**: the ortho region capture, every connected region, and the cache. Let the output differ: census/data-viz for the overview, and G-buffer (base colour, normal, ORM, depth) for the world map.
- **Shared people markers:** `skmapdraw::personMarker` is reused by the world map's dot stacks.
