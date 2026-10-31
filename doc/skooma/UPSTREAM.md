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
| `render/prim-split` | Its own feature | Untwisted flat-sided prims without the legacy edge splits; Render Tests A/B toggle; geometry-equivalence test | `render` | yes |
| `fix-llcommon/queuedthread-wake-race` | Its own fix | `llqueuedthread_test` checks a woken request's turns only while its 250 ms fallback is not yet due (failed on stalled macOS CI) | `develop` | yes |
| `fix-scripteditor/publish-coro-mutex` | Its own fix | Explore in IDE no longer aborts on an uncached linkset: inventory loaded on `fetchInventoryFromCapCoro` is handed to the main loop before it reaches the script editor server's `LLMutex` | `develop` | yes |
| `audio/loudness-normalization` | Its own feature | BS.1770 loudness measured during decode, gain trim on playback | `develop` | undecided |
| `ui/saturation` | Its own foundation | UI saturation on `LLRender` through `ALUISaturation` scopes; `uiF`/`solidcolorF` grey to Rec.709 luminance in gamma-2 light below 1, unchanged at 1 | `develop` | yes |
| `ui/text-ellipsis` | Its own foundation | Text shortened to a width with the ellipsis at the end, middle (keeping both ends, biased to the end) or start; `LLFontGL` mode enum that still takes a bool, `ellipsis` XUI param on text, buttons, tabs and list cells/columns | `develop` | yes |
| `ui/spellcheck-async` | Its own fix | Windows Spell Checking API on a worker thread with a word cache and timeouts, so a hung spelling service cannot freeze the viewer | `develop` | yes |
| `font/colrv1-stale-surface` | Its own fix | Clear the per-thread COLRv1 raster paint before each glyph, so a glyph that failed mid-paint cannot leave a larger root surface for the next one to over-read | `develop` | merged upstream: AlchemyViewer/Alchemy#600 |

Fork-only (not offered): the Atmo editor and keyframes UI, notecard and parcel transport,
mini-map, and everything under `doc/skooma/` and `scripts/skooma/`.

## Dependency graph

```
develop
├── sky/seeded
├── sky/layers ──────────┬─► sky/farlayer
├── render/farplane      │   (merged upstream)
├── render/prim-split
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
