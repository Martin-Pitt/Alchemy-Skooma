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
| `render/vertical-reach` | Its own feature | Shorter draw distance above and below the camera than horizontally | `develop` | undecided |
| `render/prim-split` | Its own feature | Untwisted flat-sided prims without the legacy edge splits; Render Tests A/B toggle; geometry-equivalence test | `feature-render/vertical-reach` | yes |
| `audio/loudness-normalization` | Its own feature | BS.1770 loudness measured during decode, gain trim on playback | `develop` | undecided |

Fork-only (not offered): the Atmo editor and keyframes UI, notecard and parcel transport,
mini-map, and everything under `doc/skooma/` and `scripts/skooma/`.

## Dependency graph

```
develop
├── sky/seeded
├── sky/layers ──────────┬─► sky/farlayer
├── render/farplane      │   (merged upstream)
├── render/vertical-reach
│   └── render/prim-split
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
