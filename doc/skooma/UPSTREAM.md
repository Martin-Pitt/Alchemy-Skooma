# Upstreamable work — index

Parts of Skooma's features that can be offered to Alchemy on their own, and what each one
depends on. Major features are in [SKOOMA.md](SKOOMA.md). The Atmo rows follow the branch
sequence in `.atmo-spec.md` §13, which is authoritative. Update this file when a row
lands, is offered or is merged.

## List

| Branch | Part of | What | Base | PR? |
|---|---|---|---|---|
| `sky/seeded` | Atmo foundation | Seeded, deterministic star field (`ALNoiseHash`, `alstarfield.h`), so two clients see the same sky | `develop` | yes |
| `sky/layers` | Atmo foundation | Deferred sky drawn as a registered, ordered layer sequence; meteors become their own object | `develop` | yes (Rye offered) |
| `render/farplane` | Atmo foundation | Camera far plane follows depth precision (infinite projection on reverse-Z), reach per consumer; culling stays at the draw distance | `develop` | merged upstream (`alfarplane.h` in `develop`) |
| `sky/farlayer` | Atmo foundation | A sky layer drawn where depth is clear, for content past the far plane | `develop` + `sky/layers` | see spec |
| `render/compute` | Atmo foundation | Compute shader workaround | `develop` | no; retired, upstream compute (`ALGPUBuffer`) landed in `develop` |
| `atmo/registry` | Atmo Magic | Parameter registry, track model, document | `develop` | yes |
| `atmo/provider` | Atmo Magic | Evaluates the document and writes onto EEP | `atmo/registry` | yes |
| `atmo/sky` | Atmo Magic | `sky.*` parameters onto `LLSettingsSky` | `atmo/provider` | yes, minus transport |
| `atmo/celestial` | Atmo Magic | `celestial.*` on the track seed | `atmo/provider` | yes |
| `atmo/clouds` | Atmo Magic | Cloud density model and puff renderer | `atmo/sky` | see spec |
| `atmo/weather` | Atmo Magic | Weather cube, wind, precipitation | `atmo/clouds` | yes |
| `atmo/water` | Atmo Magic | Water renderer | `atmo/weather` | ask Rye first |
| `render/prim-split` | Its own feature | Untwisted flat-sided prims without the legacy edge splits | — | no; dropped 2026-11-09, upstream's `profile_split` and LOD invariance (`bf1cee1f79`) do it |
| `fix-llcommon/queuedthread-wake-race` | Its own fix | `llqueuedthread_test` checks a woken request's turns only while its 250 ms fallback is not yet due (failed on stalled macOS CI) | `develop` | yes |
| [`fix-scripteditor/publish-coro-mutex`](https://github.com/AlchemyViewer/Alchemy/pull/606) | Its own fix | Script editor server safe under its async handlers' coroutines: no `LLMutex` on their paths (Explore in IDE and `object.request` aborts), `object.item.create` no longer spins the main thread, subscribe and server-release races | `develop` | merged upstream: AlchemyViewer/Alchemy#606 |
| `fix-scripteditor/published-inventory-refetch` | Its own fix | A published prim whose inventory another path dropped is fetched again; a save that misses while it loads gets a retryable error, not "Item not found" |
| `fix-scripteditor/select-node-inventory-serial` | Its own fix | A new select node's first properties reply no longer deletes a prim's loaded inventory of the same serial |
| [`fix-render/texmem-coro-lock`](https://github.com/AlchemyViewer/Alchemy/pull/618) | Its own fix | `sTexMemMutex` (texture VRAM accounting) a plain `std::mutex`, so a profile's coroutine retagging a boosted texture no longer asserts in `LLMutex` | `develop` | open: AlchemyViewer/Alchemy#618 |
| `audio/loudness-normalization` | Its own feature | BS.1770 loudness measured during decode, gain trim on playback | `develop` | undecided |
| `ui/saturation` | Its own foundation | UI saturation on `LLRender` through `ALUISaturation` scopes; `uiF`/`solidcolorF` grey to Rec.709 luminance in gamma-2 light below 1, unchanged at 1 | `develop` | yes |
| `ui/text-ellipsis` | Its own foundation | Text shortened to a width with the ellipsis at the end, middle (keeping both ends, biased to the end) or start; `LLFontGL` mode enum that still takes a bool, `ellipsis` XUI param on text, buttons, tabs and list cells/columns | `develop` | yes |
| `ui/spellcheck-async` | Its own fix | Windows Spell Checking API on a worker thread with a word cache and timeouts, so a hung spelling service cannot freeze the viewer | `develop` | yes |
| [`fix-render/gltf-override-unknown-region`](https://github.com/AlchemyViewer/Alchemy/pull/640) | Its own fix | A GLTF material override from a region the viewer no longer has is dropped with a warning, not asserted on (killed the viewer near neighbour regions) | `develop` | open: AlchemyViewer/Alchemy#640 |
| [`ui/mesh-uploader-pivot`](https://github.com/AlchemyViewer/Alchemy/pull/621) | Its own feature | Mesh uploader places an unrigged mesh's origin anywhere in its bounding box (percent or metres), by shifting the stored position domains; preview draws the origin as gizmo arrows. Also fits the preview's display check boxes inside their panel (Joints was half outside, unclickable) | `develop` | open: AlchemyViewer/Alchemy#621 |
| [`ui/mesh-uploader-lod-names`](https://github.com/AlchemyViewer/Alchemy/pull/628) | Its own fix | Multi-object single-file glTF uploads parsed correctly | `develop` | open: AlchemyViewer/Alchemy#628 |
| [`ui/local-mesh-lod-names`](https://github.com/AlchemyViewer/Alchemy/pull/629) | Its own feature | Local Assets show a mesh's own LODs, physics and origins as an uploaded mesh would | `develop` | open: AlchemyViewer/Alchemy#629 |
| [`combat/ots`](https://github.com/AlchemyViewer/Alchemy/pull/638) | Its own feature | Over-the-shoulder mouselook: camera behind the shoulder, avatar drawn, shots converge on the crosshair (camera and HeadRotation aim from the eye); Combat menu options | `develop` | draft: AlchemyViewer/Alchemy#638 |
| [`combat/lockout`](https://github.com/AlchemyViewer/Alchemy/pull/639) | Its own feature | Combat render lockout: in mouselook and 5 s after, see-through-wall debug views off and forced render state, restored after; only on damage-enabled land; removes mouselook IFF arrows and crosshair name/position text | `develop` | draft: AlchemyViewer/Alchemy#639 |
| [`chat/contact-filters`](https://github.com/AlchemyViewer/Alchemy/pull/627) | Its own feature | Who can reach you per channel (IMs, calls, conferences incl. voice invites, group chat, group calls) as dropdowns on Chat > General, replacing the IM+call friends-only switch; chat notifications get their own tab | `develop` | open: AlchemyViewer/Alchemy#627 |
| [`font/colrv1-stale-surface`](https://github.com/AlchemyViewer/Alchemy/pull/600) | Its own fix | Clear the per-thread COLRv1 raster paint before each glyph, so a glyph that failed mid-paint cannot leave a larger root surface for the next one to over-read | `develop` | merged upstream: AlchemyViewer/Alchemy#600 |
| `world/roads` | World foundation | Road model on the census (pieces, lanes, joins, junctions, kits, signs) and the land it runs over; road bench tests | `develop` + `world/air` (+ `world/census`) | not yet |
| `world/pathfinding` | World foundation | Route search with pluggable costs, route data, route following | `world/roads` | not yet |
| `world/ground-autopilot` | World foundation | Drives a road vehicle along the model's lane; gears by user-supplied vehicle profile; recovery, region crossings, edge guard; seated picking no longer turns the avatar | `world/pathfinding` | not yet |
| `world/roads-overlay` | Its own feature | Draws what the ground autopilot sees and decides | `world/ground-autopilot` | not yet |
| `world/census` amends | Census | A cached part's texture entry and decoded body (`cachedPart`); physics-flag requests batched from a loop and sent to a cached part's own region | with `world/census` | with the census |

Fork-only (not offered): the Atmo editor and keyframes UI, notecard and parcel transport,
mini-map, the private worldmap survey and bot (`private-worldmap/*`), and everything under `doc/skooma/` and `scripts/skooma/`.

## Dependency graph

```
develop
├── sky/seeded
├── sky/layers ──────────┬─► sky/farlayer
├── render/farplane      │   (merged upstream)
├── audio/loudness-normalization
└── atmo/registry
    └── atmo/provider
        ├── atmo/celestial
        └── atmo/sky
            └── atmo/clouds      (+ render/farplane)
                └── atmo/weather
                    └── atmo/water   (+ render/farplane, sky/farlayer)
```

Later Atmo rows (orbital, system, surround, terrain, far shell, bodies, surface) and their
extra bases are in `.atmo-spec.md` §13.
