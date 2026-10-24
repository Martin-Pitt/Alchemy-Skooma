# Injections

Every edit Skooma makes to a file it does not own is wrapped in `<SK:Nexii>` …
`</SK:Nexii>`, or tagged `// <SK:Nexii>` for a single line. This index says where they are
and which feature they belong to, so a reader of the merged tree can tell ours from
upstream's without a diff.

Files we own outright (`sk*` / `SK*`) are listed too, but carry no markers — the prefix is
the marker.

Convention and rationale: [`CLAUDE.local.md`](../../CLAUDE.local.md). Alchemy already
carries Firestorm's equivalents (`<FS:ND>`, `<FS:AR>`), so markers are no obstacle to an
upstream PR.

---

## Loudness normalization

Branch `feature-audio/loudness-normalization`. Design note: `.audio-loudness-design.md`.

### Files we own

| File | Role |
|---|---|
| `indra/llaudio/sklufs.h` / `.cpp` | BS.1770-4 integrated loudness meter, gain solving, WAV measurement |
| `indra/llaudio/tests/sklufs_test.cpp` | Unit tests for the above |

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/llaudio/llaudiodecodemgr.cpp` | Meter member on `LLVorbisDecodeState`; fed in `decodeSection()`, closed in `finishDecode()`, handed to `LLAudioData` in `tryFinishAudio()` | The decode already streams the whole clip past, so measuring costs no extra pass. Closed before the loop-point crossfade rewrites head and tail samples |
| `indra/llaudio/llaudioengine.h` / `.cpp` | `LLAudioSource::getGain()` multiplies by a slewed `mLoudnessGain`; `LLAudioData` holds the measurement and derived trim; engine holds the target, the background measure and the persisted index | Source gain is the one multiply all three backends apply unconditionally. The per-type secondary gain is **not** usable — FAudio applies it on a shared group submix and skips it per-voice |
| `indra/llaudio/llaudioengine_openal.cpp` | `AL_MAX_GAIN` raised from its 1.0 default at source creation | OpenAL would otherwise silently discard the boost half of a trim |
| `indra/llaudio/llvorbisencode.h` / `.cpp` | `encode_vorbis_file()` takes an optional gain, applied to the float samples before analysis | One place covering all four WAV unpack branches; float domain means a boost cannot clip against the 16-bit input |
| `indra/newview/llviewerassetupload.cpp` | Measures the source WAV (mono downmix) and passes the solved gain to the encoder | Uploads bake the correction into the asset, unlike playback |
| `indra/newview/llvieweraudio.cpp` | Pushes `SKSoundLoudnessNormalize` / `SKSoundLoudnessTargetLUFS` to the engine each tick | Alongside the existing secondary-gain push; the setter early-outs when unchanged |

### Unmarked additions

XML has no comment convention here, so these are identified by their `SK` prefix instead:

- `indra/newview/app_settings/settings_alchemy.xml` — `SKSoundLoudnessNormalize`,
  `SKSoundLoudnessTargetLUFS`, `SKSoundLoudnessStrength`, `SKSoundLoudnessNormalizeUploads`
- `indra/newview/skins/default/xui/en/floater_sound_preview.xml` — `normalize_loudness`
  checkbox bound to `SKSoundLoudnessNormalizeUploads`
- `indra/llaudio/CMakeLists.txt` — `sklufs.cpp` / `sklufs.h` sources, `al_add_test(sklufs …)`

---

## Reverb send rolloff

Branch `feature-audio/room-rolloff`. Far sounds filled the reverb (most audible underwater, rolloff 5) with no direction.

| File | What | Why there |
|---|---|---|
| `indra/llaudio/llaudioengine_openal.cpp` | `AL_ROOM_ROLLOFF_FACTOR` set to the listener's rolloff in `LLAudioChannelOpenAL::updateBuffer` | EFX presets and the source default leave room rolloff at 0, so the send never fades with distance |
| `indra/llaudio/lllistener_faudio.cpp` | `emitter.pReverbCurve = &kVolumeCurve` in `apply3D` | F3DAudio's null default is linear over `CurveDistanceScaler` and ignores rolloff |

FMOD needs no change: its reverb send follows the 3D attenuation.

---

## Seeded star placement

Branch `feature-sky/seeded` (upstream-bound, so the new files are `AL`-named). Plan and
review record: `.atmo-build-sky-seeded.md`.

### Files we own

| File | Role |
|---|---|
| `indra/newview/alstarfield.h` | The star field as a pure function of (seed, tier, index): tier table, `sample()`, `build()`, `tierCount()`, `kDefaultSeed` |
| `indra/newview/tests/alstarfield_test.cpp` | Goldens for the default seed, pinned tier table, colour packing, dome-radius scaling |

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/noise.h` | `struct ALNoiseHash` (`mix` / `combine` / `key` / `at` / `unorm`), between `ALNoiseRand` and `turbulence2` | The index-addressable hash the star field samples from lives beside the viewer's other noise primitives |
| `indra/newview/llvowlsky.cpp` | Single-line marker on the `alstarfield.h` include; `initStars()` body replaced by `ALStarField::build()` at `kDefaultSeed` | Every client builds the same stars. The meteors, which keep their own random draws, moved to `skskymeteors.cpp` with sky/layers |

### Unmarked additions

- `indra/newview/tests/noise_test.cpp` — tests 5–8 for `ALNoiseHash` (owner ruling: markers
  are required in `noise.h` and `llvowlsky.cpp` only)
- `indra/newview/CMakeLists.txt` — `alstarfield.h` header, `alstarfield` and `noise` tests

---

## Atmo Magic — registry

Branch `feature-atmo/registry`. Spec: `.atmo-spec.md`; plan and review record:
`.atmo-build-atmo-registry.md`. No injection sites: the registry, curves, limits, document
model, notation and evaluator are all new files.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmotypes.h` / `.cpp` | Value types, compose ops and identities, interpolation |
| `indra/newview/skatmoregistry.h` / `.cpp` | `SKAtmoParam` / `SKAtmoModuleDecl` registry, `module.param` keys, capability gating |
| `indra/newview/skatmocurve.h` / `.cpp` | Authored curves and `valueAt` |
| `indra/newview/skatmolimits.h` | Parse limits and `SKAtmoParseContext` (trust, registry, limits) |
| `indra/newview/skatmodocument.h` / `.cpp` | Document / track / medium / clip model, per-track budgets |
| `indra/newview/skatmonotation.h` / `.cpp` | Plaintext LLSD notation with the `atmo.<kind> 1` magic, size and depth refusals |
| `indra/newview/skatmoeval.h` / `.cpp` | Track evaluation: value sources, clip envelopes, field composition |
| `indra/newview/tests/skatmofixture.h`, `skatmo{registry,curve,document,notation,eval}_test.cpp`, `skatmonotation_bench.cpp` | Unit tests and the notation byte-model bench |

### Unmarked additions

- `indra/newview/CMakeLists.txt` — the six `skatmo*.cpp` sources, seven headers, five
  `al_add_test(skatmo… UNIT …)` entries and `al_add_bench(skatmonotation …)`

---

## Unmarked edits to upstream files

These change files we do not own without a `<SK:Nexii>` marker. They predate the marker
rule or are upstream-bound; each is listed here so the merged tree still has a record.

| File | Branch | What | Marker |
|---|---|---|---|
| `indra/newview/llviewerjoystick.cpp` | `feature-flycam-defaults` (`8e9dd18fd6`) | `setSNDefaults()` rewritten: SpaceNavigator axis scales, dead zones and feathering; `AutoLeveling` off, `JoystickAvatarEnabled` off, `JoystickEnabled` on | none — add one if the branch is not going upstream |
| `indra/newview/llvoavatar.cpp` | `feature-nametag-dist-default` (`2470788efe`) | `NameTagShowDistance` cached-control fallback `true` → `false` | none — one-line default flip |
| `indra/newview/app_settings/settings_alchemy.xml` | `feature-nametag-dist-default` | `NameTagShowDistance` default `1` → `0` | XML, no marker convention |
| `README.md` | `main` glue | Alchemy: Skooma branding and description | fork-only, never leaves `main` |
| `indra/CMakeLists.txt`, `indra/cmake/Tracy.cmake`, `indra/cmake/ConfigurationReport.cmake`, `indra/llcommon/llprofiler.h` | `fix-tracy/memory-option` (`58fae983a2`) | `AL_ENABLE_TRACY_MEMORY` build option, off by default, gating `LL_PROFILE_ALLOC`/`LL_PROFILE_FREE`; a plain capture no longer carries every allocation | none — build option, upstream-bound |

---

## Atmo Magic — provider

Branch `feature-atmo/provider` (base `feature-atmo/registry`). Plan, review record and adjudication:
`.atmo-build-atmo-provider.md`.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmoclock.h` / `.cpp` | τ from the OS clock plus `gUTCOffset`, drift, the effective per-track day clock, the five `day.*` source params |
| `indra/newview/skatmostate.h` / `.cpp` | Pure core: layer stack (document + parse report), band selection, crossing classifier, transitions, value-only blend, gate report, readout text |
| `indra/newview/skatmoubo.h` / `.cpp` | `SKAtmoUBOData` std140 mirror, `shaderKey` field table, CPU-side pack (no GL until `atmo/sky`) |
| `indra/newview/skatmoprovider.h` / `.cpp` | The shell: collects the frame from `gAgent` / parcel / `LLEnvironment`, ticks the core, packs, logs `SKAtmo` to the debug console. The only atmo TU with viewer globals |
| `indra/newview/tests/skatmo{clock,state,ubo}_test.cpp` | UNIT tests linking only `llcommon`/`llmath` |

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/llenvironment.cpp` | The `skatmoprovider.h` include (single-line marker) and one marked call `skAtmoProviderUpdate(!gCubeSnapshot)` in `LLEnvironment::update()` before `updateSettingsUniforms()` | After `applyTimeDelta`, so the tick sees this frame's EEP state; `!gCubeSnapshot` keeps τ, band and transition advancing once per real frame although `update()` is re-entered per reflection-probe face |
| `indra/llrender/llglslshader.h` | `UB_ATMO` after `UB_MATRICES` in `UniformBlock`, `"Atmo"` after `"Matrices"` in `UNIFORM_BLOCK_NAMES`; two single-line markers, `NUM_UNIFORM_BLOCKS` 8 → 9 | Reserves the binding slot in the ledger; inert until a shader declares the block (the post-link remap skips `GL_INVALID_INDEX`) |

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKAtmoTransitionTime` (the crossing crossfade
  time, a debug setting for now)
- `indra/newview/CMakeLists.txt` — the four source/header pairs and three `al_add_test(... UNIT …)` targets
- Registry files this branch also edits (ours, no marker): `skatmoeval.h`, `skatmoregistry.h` / `.cpp`
  (τ widened to F64, `day.*` shader keys, the offset comment), `tests/skatmofixture.h`,
  `tests/skatmoeval_test.cpp`, `tests/skatmoregistry_test.cpp`

---

## Atmo Magic — sky

Branch `feature-atmo/sky` (base `feature-atmo/provider`). Plan, review record and adjudication:
`.atmo-build-atmo-sky.md`. **No injection sites**: the provider's `llenvironment.cpp` seam already runs
the sky module before the uniforms are packed.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmosky.h` / `.cpp` | The 29 `sky.*` registrations, the access-adapter table onto `LLSettingsSky`, the per-key baseline shadow, the ownership predicate (adopt / clone / skip), `skAtmoApplySky`, `skAtmoSkyCrossfade` |
| `indra/newview/skatmonotecard.h` / `.cpp` | The notecard read path's pure part: text unwrap (`LLNotecard::importStream`), decode at owner trust, reload decision on the item's asset id |
| `indra/newview/tests/skatmosky_test.cpp`, `tests/skatmonotecard_test.cpp` | UNIT tests linking `llinventory` and its closure, no viewer library |

Also edited (ours): `skatmoeval.h` / `.cpp` (`authored` per-key weights), `skatmostate.h` / `.cpp`
(single-attenuation crossfade), `skatmoprovider.h` / `.cpp` (sky registration, borrowed-sky ownership,
notecard resolve / fetch / poll — the only inventory and asset calls in atmo: read-only, own inventory,
`getInvItemAsset` as the notecard preview does), and their tests.

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKAtmoLocalDocument` (a notecard name or item
  UUID in the user's inventory; the Local layer's document)
- `indra/newview/CMakeLists.txt` — two source/header pairs, two `al_add_test(... UNIT …)` targets

---

## Navmesh — world census

Branch `feature-nav/census`. A time-sliced scan of the object list that feeds the navmesh
data model. See `NAVMESH.md` for the wider feature.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skworldcensus.h` / `.cpp` | `SKWorldCensus` singleton: time-sliced scan, snapshot publishing, region-cache coverage, nav-role lookups |

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/llappviewer.cpp` | `#include "skworldcensus.h"`; `SKWorldCensus::instance().update()` once per frame after object motion; `SKWorldCensus::deleteSingleton()` explicitly in `cleanup()`, before `SUBSYSTEM_CLEANUP(LLAvatarAppearance)` | The scan needs a post-motion tick; the explicit `deleteSingleton()` releases its held `LLPointer<LLViewerObject>`s before `LLAvatarAppearance::cleanupClass()` deletes the appearance dictionary — left to the generic singleton teardown pass this ran too late and could crash tearing down the self avatar on logout |
| `indra/newview/pipeline.cpp` | `SKWorldCensus::instance().renderDebug()` from the debug render pass | Draws the census's boxes/clusters alongside the other `RENDER_DEBUG_*` overlays |

---

## Navmesh — Recast dependency

Branch `feature-nav/recast`. Design: `.navmesh-plan.md` (index `NAVMESH.md`). No code beyond the test yet, so
no `<SK:Nexii>` markers; everything here is CMake or JSON, identified by name.

### Files we own

| File | Role |
|---|---|
| `indra/cmake/ports/recastnavigation/` | Overlay port over vcpkg's `recastnavigation` 1.6.0: the stock port plus `fix-detail-mesh-empty-tris.patch` (upstream #796, one line in `RecastMeshDetail.cpp`), `fix-detail-mesh-edge-detection.patch`, `fix-closest-point-on-empty-detail.patch` |
| `indra/newview/tests/sknavrecast_test.cpp` | Integration-style smoke test: floor + block through the whole Recast pipeline into one Detour tile, a path round the block, the block top unreachable |

### Unmarked additions

- `indra/vcpkg.json` — `recastnavigation` dependency
- `indra/vcpkg-configuration.json` — `overlay-ports: ["cmake/ports"]` beside the existing `overlay-triplets`
- `indra/cmake/Dependencies.cmake` — `al_import(ll::recast PACKAGE recastnavigation CONFIG TARGETS RecastNavigation::Recast RecastNavigation::Detour)`
- `indra/newview/CMakeLists.txt` — `al_add_test(sknavrecast PROJECT … LIBRARIES ll::recast)` after `llsecapi`

---

## Render — far plane (superseded upstream)

Our `feature-render/farplane` was reworked and merged upstream as AlchemyViewer/Alchemy#432 (`ALFarPlane`, infinite reverse-Z, the sky drawn after the world on the far plane). The branch and `pr-render/farplane-pr` were retired on 2026-10-10, upstream winning. Its pillars, finite-plane forcing, sky pin, edge-water rim and `altexture3d` slice attach are gone. The only part still in the tree is the far mark Atmo surrounds use, listed below.

### Atmo surrounds — far mark

Ported onto `ALFarPlane` by the Atmo trunk. A far-flagged object is drawn past the draw distance, out to `skFarReach(getProjectionFar(), count)`.

| File | What | Why there |
|---|---|---|
| `indra/newview/skfarplane.h` / `.cpp` (ours) | Reduced to `SK_REACH_*` constants, `SKFarSquish` for forward-Z, `skFarReach`, `skApplyFarMark` | What Atmo needs that ALFarPlane does not cover |
| `indra/newview/llviewerobject.h` / `.cpp` | `mFarFlagged`, `setFarFlagged()`, `isFarFlagged()`; cleared in the destructor and `markDead()` | The mark lives on the object, so every drawable and bridge built for it carries it |
| `indra/newview/lldrawable.h` / `.cpp` | `isFarFlagged()`, `sFarFlaggedCount` | Far content opts out of the draw-distance cull per drawable |
| `indra/newview/llspatialpartition.cpp` | `LLOctreeCullFar` for the main view while far-flagged drawables exist | The partition cull |

---

## Sky — layers

Branch `feature-sky/layers`. The deferred sky pool draws the sky as an ordered sequence of named layers; meteors are their own object.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skskylayers.h` / `.cpp` | Ordered, named sky layer list |
| `indra/newview/skskymeteors.h` / `.cpp` | Meteor simulation and geometry, moved out of `LLVOWLSky` |
| `indra/newview/skskyutil.h` / `.cpp` | Black-body colour and direction helpers for the meteors |
| `indra/newview/tests/skskylayers_test.cpp`, `skskyutil_test.cpp` | Layer ordering; helper goldens |

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/lldrawpoolwlsky.h` / `.cpp` | Layer list member, `registerBuiltinLayers()`; `renderDeferred` walks the layers | The pool owns the sky draw order |
| `indra/newview/llvowlsky.h` / `.cpp` | Meteor methods replaced by an `SKSkyMeteors` member; `resetVertexBuffer()` in reset/cleanup | The dome keeps static geometry only |

### Unmarked additions

- `indra/newview/CMakeLists.txt` — `skskylayers` / `skskymeteors` / `skskyutil` sources, `skskylayers` and `skskyutil` tests
- `doc/ARCHITECTURE.md` — sky layers note

---

## Sky — far layer

Branch `feature-sky/farlayer`. A `far` sky layer (key 550) draws registered content beyond reach and writes a far-distance target; irregular content goes through a six-face observer bake.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skskyfar.h` / `.cpp` | Far sources, shading hook, far-distance target |
| `indra/newview/skskyfarlayer.h` / `.cpp` | The `far` layer: registration, shader loading, GL cleanup |
| `indra/newview/skobserverbake.h` / `.cpp` | Observer bake scheduling: one cube face per frame |
| `indra/newview/skobserverbaketarget.h` / `.cpp` | Six-face bake atlas |
| `indra/newview/skskyfardebug.h` / `.cpp` | Debug far source (`SKFarLayerDebug*` settings) |
| `app_settings/shaders/class1/deferred/skFarLayerV.glsl`, `skFarLayerUtilF.glsl`, `skFarBakeF.glsl`, `skFarBakeReadF.glsl`, `skFarDebugF.glsl` | Far layer, bake write/read with depth reprojection, debug shading |
| `indra/newview/tests/skskyfar_test.cpp`, `skobserverbake_test.cpp`, `skskyfar_gl_test.cpp` | Unit tests; GL test links the shipped shaders |

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/lldrawpoolwlsky.cpp` | Include; `SKSkyFarLayer::registerWith(mSkyLayers)` in the constructor; `SKSkyFarLayer::cleanupGL()` in `cleanupGL()` | The pool owns the layer list and sky GL lifetime |
| `indra/newview/llviewershadermgr.cpp` | Include; `SKSkyFarLayer::loadShaders()` in `loadShadersDeferred()` | Deferred shader load; a failure disables only the far layer |
| `indra/newview/llviewermenu.cpp` | `SKAdvancedCheckFarLayerDebugShape` / `SKAdvancedSetFarLayerDebugShape` listeners and their registration | Develop menu callbacks |
| `indra/newview/skins/default/xui/en/menu_viewer.xml` | Develop > Rendering > Far Layer submenu: test sphere off / per pixel / observer bake (`SKFarLayerDebugShape`) | Debug tools |

### Unmarked additions

- `indra/newview/skskylayers.h` / `.cpp`, `tests/skskylayers_test.cpp` — `SKSkyLayerContext::mPass`, filled in by `render()` (Skooma files from `sky/layers`)
- `indra/newview/app_settings/settings_alchemy.xml` — `SKFarLayerDebugOffset`, `SKFarLayerDebugRadius`, `SKFarLayerDebugShape`
- `indra/newview/CMakeLists.txt` — `skobserverbake*` / `skskyfar*` sources, `skobserverbake` and `skskyfar` unit tests, `skskyfar_gl` GL test
- `doc/ARCHITECTURE.md` — far layer note in Sky rendering

---

## Navmesh — air grid and annotations

Branches `feature-nav/sheet` (air grid) and `feature-nav/annotate` (in-world marks and capture
bundles). Design: `.navmesh-rooms-plan.md`, `.nav-annotate-brief.md` (index `NAVMESH.md`).

### Files we own

| File | Role |
|---|---|
| `indra/sknavmesh/sknavair.h` / `.cpp`, `tests/sknavair_test.cpp` | Air grid: every gap above a solid per column, off the `WEATHER` heightfield |
| `indra/sknavmesh/sknavannotate.h` / `.cpp`, `tests/sknavannotate_test.cpp` | Session model of annotation marks |
| `indra/newview/sknavannotator.h` / `.cpp` | Annotation tool, world drawing, capture export |
| `indra/newview/skfloaternavannotate.h` / `.cpp`, `skins/default/xui/en/floater_sknav_annotate.xml` | Annotation floater |

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/pipeline.cpp` | Include plus a `<SK:Nexii>` block after the navmesh overlay that draws the marks | Marks draw with the other navmesh debug overlays |
| `indra/newview/llviewerfloaterreg.cpp` | Include plus `sk_nav_annotate` registration in a `<SK:Nexii>` block | Floater registry |
| `indra/newview/llreflectionmap.h` / `.cpp` | `mNavRooms` owner kind and `mSuppressedByNavRooms`; `isRelevant()` false for a suppressed probe and for rooms probes below level 3; `getNearClip()` gives rooms probes the 0.1 m minimum, not half their radius (`<SK:Nexii>` "Navmesh rooms probes") | Rooms probes are automatic probes with their own owner (`nav/probes`) |
| `indra/newview/llreflectionmapmanager.h` / `.cpp` | `registerNavRoomsProbe`, `setNavRoomsVolumes`, `getNavRoomsContext`, `mNavRoomsVolumes`; octree probes inside finished room parts suppressed (cached, recomputed on change); `shift()` moves the part boxes (all marked) | Octree auto probes are replaced where a room exists; placed probes are never touched |
| `indra/newview/llreflectionmapmanager.h` / `.cpp` | `SKProbeKind`, `getSKProbeKind`, `getSKProbes`; the kind written into `refParams.w` bits 24–27 in `updateUniforms` only while `RenderReflectionProbeVolumes` is on (`fix-nav/probe-debug`) | Probe kinds for the census export and the scan-pulse display; UBO layout unchanged |
| `indra/newview/pipeline.cpp` | `sk_probe_inv_view` uniform in the probe display pass (`fix-nav/probe-debug`) | The pulse reconstructs world position and normal |
| `indra/newview/app_settings/shaders/class3/deferred/reflectionProbeF.glsl` | `skProbeKindColor`, `skProbeDistance`, `skProbeRawWeight`, `skProbePulseDebug`: every probe drawn as a frozen scan pulse, blends hatched (`fix-nav/probe-debug`) | Reuses the shader's own probe selection, so the hatch shows real blends |
| `indra/newview/app_settings/shaders/class2/deferred/reflectionProbeF.glsl` | Empty `skProbePulseDebug` (`fix-nav/probe-debug`) | Probes-off class must still link |
| `indra/newview/app_settings/shaders/class2/interface/reflectionprobeF.glsl` | Skips sky pixels and calls `skProbePulseDebug` (`fix-nav/probe-debug`) | The `RenderReflectionProbeVolumes` display pass |
| `indra/newview/llreflectionmapmanager.cpp` | `skProbeKindColor`, `SKProbeLinkEnd`/`skProbeLinkEnd`/`skProbeLinkWeight`/`skProbeLinkShares` and the link loop in `renderDebug`; `renderReflectionProbe`'s plain orange/yellow neighbour lines disabled (`fix-nav/probe-links`, `fix-nav/probe-link-weights`) | Each neighbour pair drawn once in kind colours, sampled every 0.05 m with the shader's weights (`skProbeRawWeight` mirror, manual/automatic classes): solid in the dominant colour at >= 90% share, two-colour dashes 0.05 m (even) to 0.5 m (near 90%) otherwise, never under 0.05 m, grey where neither reaches; debug drawing only |
| `indra/newview/llreflectionmapmanager.cpp` | `probeDebugColor` removed; the axis markers in `renderDebug` take `skProbeKindColor(getSKProbeKind())`, census-blocked probes red (`fix-nav/probe-cache`) | One palette for markers, links and the scan pulse; debug drawing only |
| `indra/newview/llreflectionmap.h` / `llreflectionmapmanager.h` / `.cpp` | `mLinkedRelevant`; `updateRelevanceLinks()` called after the manual-eclipse pass, `updateNeighbors` records relevance; `countNavOctreeLinks`, `getRelevanceRelinks`, `mRelevanceRelinks` (`fix-nav/probe-cache`) | A probe whose relevance flipped (nav suppression, census block, manual eclipse) is relinked in one sweep per update; inherited overlap rule unchanged |
| `indra/newview/llreflectionmapmanager.cpp` | `cleanup()` closes and resets `mSolidWorkers` (`fix-nav/pool-shutdown`) | The ProbeSolids pool is joined at pipeline cleanup; left to `gPipeline`'s static destructor its close threw from boost.fiber and quitting aborted |
| `indra/newview/llreflectionmap.h` / `.cpp`, `llreflectionmapmanager.h` / `.cpp`, `class3/deferred/reflectionProbeF.glsl` | `mClusterFill`; `registerClusterProbe`, `updateClusterProbes`, `mClusterProbes` (`SKClusterProbes`), `SK_PROBE_CLUSTER`; cluster probes sorted first, never a search's first probe, their index range in bucket 0 (`feature-nav/cluster-probes`, merge `2fbdb854ea`) | A fill tier over each census cluster, between the default probe and the rest |
| `indra/newview/pipeline.cpp` | `mReflectionMapManager.renderDebug()` moved from `renderDebug()` to `renderFinalize`, after the navmesh overlay, depth-tested without writes (`fix-nav/probe-debug-colors`) | Probe debug lines and markers skip exposure and tonemapping, so their kind colours survive dim scenes |

### Unmarked additions

- `indra/newview/CMakeLists.txt` — `sknavannotator`, `skfloaternavannotate`, `sknavrooms`, `sknavroomprobes` sources and headers
- `indra/newview/app_settings/settings_alchemy.xml` — `SKClusterProbes`, `SKClusterProbesMax`, `SKClusterProbesCellM`, `SKClusterProbesHeight`
- `indra/newview/app_settings/settings_alchemy.xml` — `SKNavRoomProbes`, `SKNavRoomProbesMax` (default 128 → 240 by `fix-nav/street-probes`)
- `indra/sknavmesh/CMakeLists.txt` — `sknavprobes` source and test
- `indra/sknavmesh/CMakeLists.txt` — `sknavair`, `sknavannotate` sources and tests
- `indra/newview/app_settings/settings_alchemy.xml` — `SKNavAirRadius`, `SKNavAirAgentRadius` (`nav/air-agent-disc`: fine air in a disc round the agent), `SKNavAirBudgetMB`, `SKNavAirMinGap`, `SKNavDiskCacheMB`, `SKNavRoomsSettleMs`, `SKNavOverlayEnclosure` (since removed, `8ebc9cb749`), `SKNavOverlaySpaces`, `SKNavOverlayWind`, `SKNavOverlayWindHeading`, `SKNavWindSettleMs` (wind in its own marker block), `SKNavSealedIndoors`, 
- `indra/sknavmesh/CMakeLists.txt` — `sknavairdistil`, `sknavskymap`, `sknavtilecache`, `sknavflights`, `sknavenclosure`, `sknavspaces`, `sknavwind`, `sknavcensusio`, `sknavlayout`, `sknavexport`, `sknavreplay` sources and tests, `tests/sknavtestscene.h` (plus the source-hash build step)

---

## Atmo Magic — system

Branch `feature-atmo/system` (base `feature-atmo/clouds`). Plan and review record:
`.atmo-build-atmo-system.md`. **No injection sites**: every edit is to Skooma files plus CMake.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmosystem.h` / `.cpp` | A track's system: geometry (sphere, plane, ring/cylinder/sphere interiors), air and horizon, resolved once per document and each tick |
| `indra/newview/tests/skatmosystem_test.cpp` | UNIT test for the system resolve |

Also edited (ours): `skatmobake`, `skatmoeditor`, `skatmoeditorpresets`, `skatmoorbital`,
`skatmoprovider`, `skatmostate`, `skfloateratmo` (`.h` / `.cpp`), `panel_sk_atmo_track.xml` (the Track
tab's system editor), `panel_sk_atmo_orbital.xml`, and the `skatmo{bake,editor,orbital,sky,state}` tests.

### Unmarked additions

- `indra/newview/CMakeLists.txt` — `skatmosystem.cpp` / `.h`, the `skatmosystem` UNIT test, and
  `skatmosystem.cpp` / `skatmoclouddeck.cpp` added to the existing atmo test targets' sources
- `indra/newview/app_settings/settings_alchemy.xml` — `SKAtmoDiscRimFalloff` (metres past a flat
  world's rim over which the air fades; parked, nothing reads it yet)
- `indra/newview/app_settings/settings_alchemy.xml` — `SKAtmoEditorState` (LLSD: the Atmo editor's
  window shape, torn-off tabs and their positions; from `feature-atmo/editor`, `5c826e4e75`)
- `indra/newview/app_settings/settings_alchemy.xml` — `SKAtmoNotecardEnabled` (save environments as
  notecards in the Settings/Atmo folder; **defaults on**; from `feature-atmo/editor`, `343d80008a`)

## Atmo Magic — parcel

Branch `feature-atmo/parcel` (base `feature-atmo/menu`). Plan and review record:
`.atmo-build-atmo-parcel.md`. **No injection sites**: every edit is to Skooma files plus CMake and settings.
Per-owner consent was removed: there is no consent notification or consent setting.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmoparcelmarker.h` / `.cpp` | Pure codec for the `atmo:` / `atmo;` Z85 marker in a parcel description |
| `indra/newview/skatmoparceldiscovery.h` / `.cpp` | Discovery state: which parcel's marker applies, fail-closed readout |
| `indra/newview/skatmoparcelfetch.h` / `.cpp` | Fetches the notecard a parcel's marker names |
| `indra/newview/skatmoparcelcamera.h` / `.cpp` | Discovers the parcel under the camera, not the agent |
| `indra/newview/skatmoparcelsquares.h` / `.cpp` | Pure parcel-grid square lookup used by the camera parcel |
| `indra/newview/tests/skatmoparcel{marker,discovery,squares}_test.cpp` | UNIT tests |

Also edited (ours): `skatmoprovider` (`.h` / `.cpp`; the track follows the camera's altitude), `skatmostate.h`,
`skfloateratmo.cpp`.

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKAtmoParcelDiscovery` (use parcel Atmo
  environments; **defaults on**)
- `indra/newview/skins/default/xui/en/notifications.xml` — `SKAtmoSaveAs` (editor Save As) and
  `SKAtmoParcelDocumentErrors` (XML comment markers)
- `indra/newview/CMakeLists.txt` — the five `skatmoparcel*` source/header pairs and three
  `al_add_test(... UNIT …)` targets

---

## UI — hover context and overlays

Branch `feature-ui/hover`. How-to: [UI-HOVER.md](UI-HOVER.md).

### Files we own

| File | Role |
|---|---|
| `indra/newview/skhover.h` / `.cpp` | The hovered view set and leaf, and the context a hovered widget publishes for distant consumers |
| `indra/newview/skoverlay.h` / `.cpp` | Registry of overlay drawers for the 3D, under-UI and over-UI layers, with the world-to-screen helper |

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/llviewerwindow.cpp` | `SKHover::update(mMouseHoverViews)` right after `updateUI` swaps in the frame's hover set | The set is private with no getter; this is the one place it is complete for the frame |
| `indra/newview/pipeline.cpp` | `SKOverlays::draw(World)` after `renderDebug()` in the `!gCubeSnapshot && !sImpostorRender` block | Same guard and GL state as the debug displays, so overlays never land in probe captures |
| `indra/newview/llviewerdisplay.cpp` | `SKOverlays::draw(ScreenUnderUI)` before the UI buffer block and `draw(ScreenOverUI)` after `gViewerWindow->draw()` in `render_ui_2d` | Either side of the UI in the 2D pass, covering both the cached-UI and direct-draw paths |

### Unmarked additions

- `indra/newview/CMakeLists.txt` — `skhover` and `skoverlay` source/header pairs


---

## Atmo Magic — bodies

Branch `feature-atmo/bodies` (`1ba8c4177a`, `7a1aed43f9`, `9f128893c9`). The orbital module's `visual` records, placed each frame by the Atmo provider and drawn as direction-only discs by the far layer.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmobodies.h` / `.cpp` | Places the orbital module's visual bodies on its own geometry |
| `indra/newview/skskybodies.h` / `.cpp` | `SKSkyBodies`: the far source that draws them; registration, shader load, GL cleanup |
| `indra/newview/app_settings/shaders/class1/deferred/skFarBodyF.glsl` | Body disc shading |
| `indra/newview/tests/skatmobodies_test.cpp`, `skskybodies_gl_test.cpp` | UNIT test; GL test blends the shipped shader over a cleared dome on the hidden window. No Python oracle |

Also edited (ours): `skatmoorbital`, `skatmoprovider` (`.h` / `.cpp`), `skskyfar.h` / `.cpp` (the far layer's "active" check now counts the bodies source), `skskyfarlayer.cpp`, `tests/skskyfar_test.cpp`.

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/lldrawpoolwlsky.cpp` | Include; `SKSkyBodies::registerWith(SKSkyFarLayer::instance().getSources())` on pool creation; `SKSkyBodies::cleanupGL()` in `cleanupGL()` (all marked `<SK:Nexii> atmo/bodies`) | The pool owns the sky layers and sky GL lifetime |
| `indra/newview/llviewershadermgr.cpp` | Include; `SKSkyBodies::loadShaders()` in `loadShadersDeferred()` (marked `<SK:Nexii> atmo/bodies`) | Deferred shader load; a failure leaves only the bodies off |

### Unmarked additions

- `indra/newview/CMakeLists.txt` — `skatmobodies.*` and `skskybodies.*` sources, `skatmobodies` UNIT test, `skskybodies_gl` GL test
- `doc/ARCHITECTURE.md` — Atmo's visual bodies note in Sky rendering

---

## Atmo Magic — clouds

Branch `feature-atmo/clouds` (R1 to R7, merged into `develop`). Cloud decks are typed records sharing one puff budget; the field rebuilds on a worker from a frame snapshot. R1 to R3 touched only Skooma files plus CMake and settings; R4 to R7 (the renderer) add the injection sites below. Build doc: `.atmo-build-atmo-clouds.md`.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmoclouds.h` / `.cpp` | The clouds module declaration and the four params applied to decks |
| `indra/newview/skatmocloudfield.h` / `.cpp` | Field rebuild on a worker from a snapshot; one rebuild at a time, no cost for a clear sky |
| `indra/newview/skatmoclouddeck.*`, `skatmoclouddensity.*`, `skatmocloudlattice.*` | Deck records, density model, puff lattice |
| `indra/newview/skatmoeditorclouds.h` / `.cpp` | Editor Clouds page model |
| `indra/newview/skins/default/xui/en/panel_sk_atmo_clouds.xml` | Editor Clouds page: one slot per deck role |
| `indra/newview/skdrawpoolclouds.h` / `.cpp` | `SKDrawPoolClouds` (`POOL_SK_CLOUDS`): puff and sheet draws, reflection-probe flat proxies |
| `indra/newview/skatmocloudshape.*`, `skatmocloudquads.*`, `skatmocloudsheet.*` | Puff erosion shape, camera-facing quads, displaced sheet decks |
| `indra/newview/app_settings/shaders/class1/deferred/skCloudPuff{V,F}.glsl`, `skCloudSheet{V,TC,TE,F}.glsl` | Puff and tessellated sheet programs |
| `indra/newview/tests/skatmocloudshape_test.cpp`, `skatmocloudquads_test.cpp`, `skatmocloudsheet_test.cpp`, `skcloudpuff_gl_test.cpp` | UNIT / GL tests |
| `indra/newview/tests/skatmoclouds_test.cpp`, `skatmocloudfield_test.cpp`, `skatmoeditorclouds_test.cpp` (plus deck / density / lattice tests) | UNIT tests |

Also edited (ours): `skatmoprovider.cpp`, `skatmoeditor.*`, `skfloateratmo.*`, `skfarplane.*` (deck reach; projection hold dropped 2026-10-10), `skatmocurve.*`, `skatmoclouds.*`, `skatmocloudfield.*`.

### Injection sites

| File | What |
|---|---|
| `indra/newview/lldrawpool.h` / `.cpp` | `POOL_SK_CLOUDS` enum entry; `skdrawpoolclouds.h` include; `createPool` case |
| `indra/newview/pipeline.h` | `RENDER_TYPE_SK_CLOUDS`; `mSKCloudsPool` |
| `indra/newview/pipeline.cpp` | Include; `getPool(POOL_SK_CLOUDS)` at init (faceless pool); `releaseGL` in the context teardown; `POOL_SK_CLOUDS` cases in `findPool`, `addPool` and `removePool`; `RENDER_TYPE_SK_CLOUDS` in two render-type lists |
| `indra/newview/llviewershadermgr.h` / `.cpp` | `gSKCloudPuffProgram`, `gSKCloudSheetProgram` declarations and definitions, unload, and the load block (puff: V/F; sheet: V/TC/TE/F) |
| `indra/newview/llviewermenu.cpp` | `"sk_clouds"` in `render_type_from_string` |
| `indra/newview/skins/default/xui/en/menu_viewer.xml` | "Atmo Clouds" rendering-type check item |
| `indra/newview/llreflectionmapmanager.cpp` | `RENDER_TYPE_SK_CLOUDS` added to the probe-face render-type list (flat proxies); the neighbouring line was re-wrapped |

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKRenderAtmoClouds`, `SKCloudPuffBudget`, `SKCloudPuffReach`
- Shaders under `app_settings/shaders/class1/deferred/` are new files found by name; no list to edit
- `indra/newview/CMakeLists.txt` — the cloud source/header pairs, `skatmoeditorclouds`, and the cloud UNIT tests

---

## UI — studio floater declared size

Branch `feature-atmo/editor` (`982966f5af`). The Atmo editor saves its window shape through `ALStudioFloater`.

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/llui/studio/alstudiofloater.cpp` / `.h` | Marked `<SK:Nexii> Atmo Magic editor: a re-laid-out XUI must not restore the old rect`: `llsdutil.h` include; saved-size handling in state save / restore; `loadState` records the declared size from the XUI rect (`mDeclaredWidth` / `mDeclaredHeight`, first call only); the member's declaration in the header | A re-laid-out XUI must not restore a stale saved size |

---

## Object motion recorder

Branch `feature-motion/recorder`. Design: `.motion-plan.md`, index `MOTION.md`.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skmotionrecorder.h` / `.cpp` | Static recorder: buffers per-frame FRAME / CAMERA / UPDATE / OBJECT / INFO records and streams them to `logs/skmotion-<UTC>.skm` behind a JSON header |

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/llviewerobject.cpp` | Include; pre-update state after the region-listed check; UPDATE emits on the stale-terse, stale-full, invalid-region and parent-cycle exits and at the end; raw decoded values before ping interpolation; ping delay; OBJECT emits in `idleUpdate`; phase-out and region-edge-clip stores in `interpolateLinearMotion` | The residual between the interpolated and the received state only exists inside `processUpdateMessage`, and `phase_out` is a block local |
| `indra/newview/llviewerobject.h` | `friend class SKMotionRecorder` | Reads `mStatic` and `sPredictionTuning` without adding accessors |
| `indra/newview/llagentcamera.h` | `getFocusObjectOffset()` | The alt-cam focus offset was private |
| `indra/newview/llappviewer.cpp` | Include; frame begin at the top of `idle()`; timing around `idleNetwork()`; frame end after `updateCamera()`, also on the teleport and disconnect early returns | The camera record must follow this frame's camera update, and the frame must close on every exit |
| `indra/newview/llviewermenu.cpp` | `Advanced.ToggleMotionRecord` / `Advanced.CheckMotionRecord` | Develop → Network handlers live here |
| `indra/newview/CMakeLists.txt` | `skmotionrecorder.cpp` / `.h` in the source and header lists | |

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKMotionRecordSeconds`, `SKMotionRecordRadius`, `SKMotionRecordMaxObjects`
- `indra/newview/skins/default/xui/en/menu_viewer.xml` — "Record Object Motion" check item under Develop → Network, after Ping Interpolate (marked with an XML comment)

---

## Atmo Magic — region terrain

Branch `feature-atmo/regionterrain`. `system.region_terrain` (show / hide) hides the region's terrain and water in every pass, visual only, flipping at the midpoint of a track fade.

### Files we own

`skatmosystem`, `skatmostate` (`regionTerrain`/`departingTerrain`, `skAtmoRegionTerrainGate`), `skatmoprovider` (`updateRegionTerrain()`), `skfloateratmo` and `panel_sk_atmo_system.xml` (System page combo `sk_system_region_terrain`, `[TERRAIN]` readout), with tests in `skatmostate_test.cpp` and `skatmosystem_test.cpp`.

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/pipeline.h` | `setRegionGroundHidden()` / `isRegionGroundHidden()` and `mRegionGroundHidden` | The one gate the provider drives |
| `indra/newview/pipeline.cpp` | `hasRenderType()` returns false for terrain, water and void water while gated; `isWaterClip()` false while gated; `hasRenderTypeControl` and its negation read `mRenderTypeEnabled` directly; `hasAnyRenderType()` goes through `hasRenderType()` | Culling, pools, shadows, probes and mirrors all ask `hasRenderType`; the menu must show the user's toggle, not the gate |
| `indra/newview/llviewercamera.cpp` | `cameraUnderWater()` returns false while gated | Hidden water fogs nothing |
| `indra/newview/llviewerparceloverlay.cpp` | Early return in the property-line draw while gated | Lines go with the ground |

### Unmarked additions

- `indra/llui/tests/skatmoxui_test.cpp` — System page control list gains the terrain combo
- `indra/newview/tests/skatmosky_test.cpp` — dropped two asserts on `LLSettingsSky::getHDRMin()`, which upstream removed

---

## Atmo Magic — interiors

Branch `feature-atmo/interior`. Precomputed scattering volumes for a cylinder, ring or hollow sphere, built on a worker.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmointerior.h` / `.cpp` | Interior scattering volumes (one-bounce at half resolution) |
| `indra/newview/tests/skatmointerior_test.cpp` | Unit test |

### Injection sites

None in upstream files for the volumes (`skatmoprovider` is ours). The air pass below adds sites.

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKAtmoInteriorResolution` (S32, 48)
- `indra/newview/CMakeLists.txt` — `skatmointerior.cpp` / `.h`, `skatmointerior` unit test

### Interior air pass

Branch `feature-atmo/interior-render` (merged into `develop`). Build doc: `.atmo-build-atmo-interior.md` (Phase 2). Inside an interior the haze pass is replaced by a per-pixel march through the baked scattering volume; clouds inside one are hazed by the same air.

Files we own: `indra/newview/skinteriorair.h` / `.cpp` / `skinteriorairgl.h` (the pass, volume upload, GL cleanup); `app_settings/shaders/class1/deferred/skInteriorAirF.glsl`, `skInteriorAirUtilF.glsl`; `tests/skinteriorair_gl_test.cpp`.

Also edited (ours): `skatmointerior.*` (volume additions), `skatmoprovider.h`, `skskyshell.*` and `skFarShellF.glsl` (`sk_shell_air_pass` skips the shell's own attenuation while the air pass runs), `skdrawpoolclouds.cpp`, `skCloudMarchF.glsl` (`SK_INTERIOR_AIR` permutation), `skatmoclouddeck.cpp` / `skatmotypes.cpp`.

| File | What | Why there |
|---|---|---|
| `indra/newview/pipeline.cpp` | Include; in `LLPipeline::doAtmospherics`, `if (SKInteriorAir::draw(mWaterDis)) return;` before the haze draw (marked) | The air replaces the planetary haze inside an interior; `mWaterDis` is the scratch target |
| `indra/newview/lldrawpoolwlsky.cpp` | Include; `SKInteriorAir::cleanupGL()` in `cleanupGL` (marked) | The pool owns sky GL lifetime |
| `indra/newview/llviewershadermgr.cpp` | Include; `SKInteriorAir::loadShaders()` after the deferred shaders load, guarded by `success` (marked); `skInteriorAirUtilF.glsl` pushed onto `gSKCloudMarchCurvedProgram` with the `SK_INTERIOR_AIR` permutation (comment-tagged `atmo/interior-render`, unmarked) | A failure leaves interiors under the haze; clouds share the air's functions |

Unmarked additions:

- `indra/newview/app_settings/settings_alchemy.xml` — `SKAtmoInteriorAirSteps`, `SKAtmoInteriorAirDebug`
- `indra/newview/CMakeLists.txt` — `skinteriorair.cpp` / `.h` / `skinteriorairgl.h`, GL test `skinteriorair_gl` (`SK_AIR_SHADER_DIR`)

---

## Atmo Magic — megastructure

Branch `feature-atmo/megastructure`. The far layer draws the active track's ring, cylinder, hollow sphere or planet below as analytic geometry, lit by the air model.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmoshell.h` / `.cpp` | Shell geometry and lighting maths |
| `indra/newview/skskyshell.h` / `.cpp` | The far source: registration, shader loading, GL cleanup |
| `app_settings/shaders/class1/deferred/skFarShellF.glsl` | Shell shader |
| `indra/newview/tests/skatmoshell_test.cpp`, `skskyshell_gl_test.cpp` | Unit test; GL test links the shipped shader |

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/lldrawpoolwlsky.cpp` | Include; `SKSkyShell::registerWith(SKSkyFarLayer::instance().getSources())` in the constructor; `SKSkyShell::cleanupGL()` | The pool owns sky GL lifetime |
| `indra/newview/llviewershadermgr.cpp` | Include; `SKSkyShell::loadShaders()` after the deferred shaders load, guarded by `success` | A failure leaves only the megastructure off |

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKAtmoShellDraw` (Boolean, on)
- `indra/newview/CMakeLists.txt` — `skatmoshell` / `skskyshell` sources and headers, `skatmoshell` unit test, `skskyshell_gl` GL test

---

## Atmo Magic — black hole

Branch `feature-atmo/blackhole`. A track can carry a Schwarzschild black hole with a lensed disc bake, edited on the Planetary page.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmoblackhole.h` / `.cpp` | Deflection and disc maths |
| `skins/default/xui/en/panel_sk_atmo_blackhole.xml` | Editor page |
| `indra/newview/tests/skatmoblackhole_test.cpp` | Unit test |

Edits to `skatmoeditor`, `skatmoorbital`, `skfloateratmo`, `panel_sk_atmo_orbital.xml` and their tests are to our own files.

### Injection sites

None in upstream files for the maths and editor. The render below adds sites.

### Unmarked additions

- `indra/newview/CMakeLists.txt` — `skatmoblackhole.cpp` / `.h`, `skatmoblackhole` unit test

### Black hole render

Branch `feature-atmo/blackhole-render` (merged into `develop`). Build doc: `.atmo-build-atmo-blackhole.md` (Phase 2). A far-layer source draws the hole in its body's place and bends the sky behind it from a radial deflection table.

Files we own: `indra/newview/skskyblackhole.h` / `.cpp`; `app_settings/shaders/class1/deferred/skFarBlackHoleV.glsl`, `skFarBlackHoleF.glsl`, `skFarBlackHoleLensF.glsl`; `tests/skskyblackhole_gl_test.cpp`.

Also edited (ours): `skatmoblackhole.*` (bakes: what to draw, disc and sky look, radial table), `skatmobodies.*`, `skatmoprovider.*` (the hole stands in its body's place each frame).

| File | What | Why there |
|---|---|---|
| `indra/newview/lldrawpoolwlsky.cpp` | Include; `SKSkyBlackHole::registerWith(SKSkyFarLayer::instance().getSources())` in the constructor; `SKSkyBlackHole::cleanupGL()` in `cleanupGL` (all marked) | The pool owns sky GL lifetime |
| `indra/newview/llviewershadermgr.cpp` | Include; `SKSkyBlackHole::loadShaders()` after the deferred shaders load, guarded by `success` (marked) | A failure leaves only the black hole off |

Unmarked additions:

- `indra/newview/app_settings/settings_alchemy.xml` — `SKAtmoBlackHoleDraw`, `SKAtmoBlackHoleLensing`
- `indra/newview/CMakeLists.txt` — `skskyblackhole.cpp` / `.h`, GL test `skskyblackhole_gl` (`SK_FAR_SHADER_DIR`)

---

## Atmo Magic — weather

Branch `feature-atmo/weather`. Build doc: `.atmo-build-atmo-weather.md`. **No injection sites**: weather, wind and precipitation are new files; the provider, cloud field and editor they hook into are ours.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmoweather.*`, `skatmoweathergen.*` | Weather as pure data an environment authors; generator |
| `indra/newview/skatmowind.*`, `skatmoprecip.*` | Wind field and precipitation model |
| `indra/newview/skins/default/xui/en/panel_sk_atmo_weather.xml` | Editor Weather page |
| `indra/newview/tests/skatmoweather_test.cpp`, `skatmoweathergen_test.cpp`, `skatmowind_test.cpp`, `skatmoprecip_test.cpp` | UNIT tests |

Also edited (ours): `skatmoprovider.*` (weather carried into the clouds), `skatmoeditor.*`, `skfloateratmo.*`, `skatmocloudfield.*`, `skatmotypes.h`, `skatmostate.*`, `skatmoeval.*`, `skatmoregistry.h` and their tests.

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKRenderAtmoWeather`, `SKRenderAtmoPrecip`
- `indra/newview/CMakeLists.txt` — the weather, weathergen, wind and precip sources/headers and their UNIT tests

---

## Atmo Magic — far cloud shell

Branch `feature-atmo/farshell`. Build doc: `.atmo-build-atmo-farshell.md`. The decks continue past the puffs' reach as a `far_clouds` layer on the sky.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmocloudshell.h` / `.cpp` | Shell geometry, seam and depth moments as pure functions |
| `indra/newview/skcloudshell.h` / `.cpp` | `SKCloudShellLayer`: the sky layer, slice and composite passes |
| `indra/newview/app_settings/shaders/class1/deferred/skCloudShell{V,F,SliceF,CompositeF,UtilF}.glsl` | Shell shaders |
| `indra/newview/tests/skatmocloudshell_test.cpp` | UNIT test |

Also edited (ours): `skskyfarlayer.*`, `skskyshell.cpp` (seam share averaged over the slab), `skatmoclouds.*`, `skfloateratmo.*`.

### Injection sites

| File | What |
|---|---|
| `indra/newview/lldrawpoolwlsky.cpp` | `skcloudshell.h` include; `SKCloudShellLayer::registerWith(mSkyLayers)` in the constructor; the built-in `"clouds"` layer skips `renderSkyCloudsDeferred` while `SKCloudShellLayer::drawsBand()` |

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKCloudShellMode`, `SKCloudShellBand`, `SKCloudShellRadius`, `SKCloudShellSlices`
- `indra/newview/CMakeLists.txt` — `skatmocloudshell`, `skcloudshell` and the shell UNIT test
- Shaders: new files, found by name

---

## Atmo Magic — clouds raymarch

Branch `feature-atmo/clouds-raymarch` (merged into `develop`). Clouds are raymarched per pixel over density tables, a baked detail volume and a cover square, with sun shadow volumes, curved-world marching and a `SKCloudRenderer` switch back to the puffs. Build doc: `.atmo-build-atmo-clouds-raymarch.md`.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skcloudmarch.h` / `.cpp` | The raymarcher: detail volume, cover square, occupancy pyramid, shadow volume bakes and the march pass |
| `indra/newview/skatmoclouddetail.*`, `skatmocloudlayers.*`, `skatmocloudorg.*` | Detail volume model, per-layer table packing, weather organisation read from the applied weights |
| `indra/newview/app_settings/shaders/class1/deferred/skCloudFullscreenV.glsl`, `skCloudCoverBakeF.glsl`, `skCloudOccupancyF.glsl`, `skCloudShadowBakeF.glsl`, `skCloudMarchF.glsl`, `skCloudMarchUtilF.glsl` | Raymarch shaders; `skCloudMarchF` has the `SK_CLOUD_CURVED` permutation |
| `indra/newview/tests/skatmoclouddetail_test.cpp`, `skatmocloudlayers_test.cpp`, `skatmocloudorg_test.cpp` | UNIT tests |
| `indra/newview/tests/skcloudmarch_gl_test.cpp` | GL test (hidden window; compiles the shaders from `SK_CLOUD_SHADER_DIR`) |

Also edited (ours): `skatmoclouddensity.*` (density tables), `skdrawpoolclouds.*` (march draw after the alpha pools, renderer switch), `skcloudshell.*` (reads the density tables), `skatmocloudsheet.*`, `skatmoweather.*` (organisation exposed).

### Injection sites

| File | What |
|---|---|
| `indra/newview/llviewershadermgr.h` / `.cpp` | `gSKCloudShadowBakeProgram`, `gSKCloudCoverBakeProgram`, `gSKCloudOccupancyProgram`, `gSKCloudMarchProgram`, `gSKCloudMarchCurvedProgram`: declarations (marked), definitions (marked), unload lines (marked) and five load blocks (comment-tagged `atmo/clouds-raymarch`, unmarked) |

`lldrawpool*`, `pipeline.*`, `llviewermenu.cpp`, `llreflectionmapmanager.cpp`, `lldrawpoolwlsky.cpp`, `altexture3d*` and `llrender*` are untouched by this branch.

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKCloudRenderer` (`raymarch` or `puffs`), `SKCloudShadowSlices`
- `indra/newview/CMakeLists.txt` — the detail, layers, org and raymarch sources/headers; UNIT tests `skatmoclouddetail`, `skatmocloudlayers`, `skatmocloudorg`; GL test `skcloudmarch_gl`
- Shaders: new files, found by name

### Follow-ups: ground cloud shadows

Branch `feature-atmo/clouds-followups` (merged into `develop`). Build doc: `.atmo-build-atmo-clouds-raymarch.md` (Follow-ups). The sun shadow volumes are baked into a ground shadow map that the deferred sun light samples; also a size and half-height budget for the volumes, place-bounded billows and the far shell lit as the raymarcher does.

Files we own: `app_settings/shaders/class1/deferred/skCloudGroundShadowBakeF.glsl`, `skCloudGroundShadowF.glsl` (defines `skCloudGroundShadow`, linked into the sun programs); `skCloudShellUtilF.glsl`.

Also edited (ours): `skcloudmarch.*` (ground shadow bake, volume budget), `skdrawpoolclouds.*` (`bindGroundShadow` / `unbindGroundShadow`), `skcloudshell.*`, `skatmocloudlayers.*`, `skatmoclouddetail.*`, `skCloudMarchUtilF.glsl`, `skCloudOccupancyF.glsl`, `skCloudShellF/SliceF.glsl`, `skatmocloudsheet.cpp`, `skatmocloudlattice.cpp`.

| File | What | Why there |
|---|---|---|
| `indra/newview/pipeline.cpp` | In `renderDeferredLighting`, `SKDrawPoolClouds::bindGroundShadow(...)` before the sun draw and `unbindGroundShadow(...)` after it (marked) | The sun pass is where the ground's direct light is taken; probes (`gCubeSnapshot`) skip it |
| `indra/newview/llviewershadermgr.h` / `.cpp` | `gSKCloudGroundShadowBakeProgram`: declaration, definition and unload (marked), load block (comment-tagged `atmo/clouds-followups`, unmarked); `skCloudGroundShadowF.glsl` pushed onto `gDeferredSunProgram` and `gDeferredSunProbeProgram` (marked) | The sun programs call `skCloudGroundShadow` |
| `indra/newview/app_settings/shaders/class2/deferred/sunLightF.glsl`, `sunLightSSAOF.glsl` | Prototype of `skCloudGroundShadow`; `col.r *= skCloudGroundShadow(pos.xyz);` (marked) | `col.r` carries the sun's shadow term |

Unmarked additions:

- `indra/newview/app_settings/settings_alchemy.xml` — `SKCloudGroundShadows`, `SKCloudShadowVolumeSize`, `SKCloudShadowHalfHeight`
- `indra/newview/CMakeLists.txt` — `skatmocloudsheet.cpp` and `skatmocloudlattice.cpp` added to a test target's sources; no new targets

---

## Atmo Magic — events

Branch `feature-atmo/events`. Build doc: `.atmo-build-atmo-events.md`. Events are placed clips on the document clock; the weather can roll them (lightning is the storm's alone). **No injection sites.**

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmoevents.h` / `.cpp` | Event clips, placement, seeded rolls, playback |
| `indra/newview/skins/default/xui/en/panel_sk_atmo_events.xml` | Weather > Events page |
| `indra/newview/tests/skatmoevents_test.cpp`, `skatmoevents_golden.inc` | UNIT test and golden data |

Also edited (ours): `skatmoprovider.*` (plays placed events), `skatmoweather.*`, `skatmoprecip.*`, `skatmoeditor.*`, `skfloateratmo.*`.

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKRenderAtmoEvents`
- `indra/newview/CMakeLists.txt` — `skatmoevents` sources and UNIT test

---

## Atmo Magic — motion

Branch `feature-atmo/motion`. Build doc: `.atmo-build-atmo-motion.md`. The tick moves the observer, sun and clouds with the vehicle: rhumb lines and circles, a ring's turning sky, a flat world's rim. **No injection sites.**

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmomotion.h` / `.cpp` | Closed-form treadmill position, rhumb and great-circle heading, ring and rim handling |
| `indra/newview/skins/default/xui/en/panel_sk_atmo_motion.xml` | Editor Motion page |
| `indra/newview/tests/skatmomotion_test.cpp` | UNIT test |

Also edited (ours): `skatmoorbital.*`, `skatmoprovider.*`, `skatmoclouds.*`, `skatmoeditor.*`, `skfloateratmo.*`, `skskyshell.cpp` (anchor wiring).

### Unmarked additions

- `indra/newview/CMakeLists.txt` — `skatmomotion` sources and UNIT test
- No new settings

---

## Clamped velocity recovery

Branch `feature-motion/unclamp` (base `feature-motion/recorder`). Design: `.motion-plan.md` § Findings F1, brief `.motion-unclamp-brief.md`.

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/llviewerobject.h` / `.cpp` | `skRecoverClampedVelocity()` and its members (last raw sample, speed EWMA, recovered velocity and method); called in the terse path after the recorder's raw capture and before ping interpolation; `setRegion()` resets the sample | The terse decode clamps velocity to ±128 m/s and acceleration to ±64 m/s² per axis; the fix needs the raw decoded values and the previous raw position, which only exist here |
| `indra/newview/skmotionrecorder.h` / `.cpp` | `onUpdateRecovered`; UPDATE gains `used_vel` and `recover`, header version 2, `SKMotionRecoverClampedVelocity` in the header settings; later per-stage frame timers in FRAME (version 3, fields describe the previous frame) | Our own file, no markers |
| `indra/newview/llappviewer.cpp` | Stage timers around `gObjectList.update`, `gPipeline.updateMove`, `gAgentCamera.updateCamera` and the whole `idle()` | The stages live there |
| `indra/newview/llviewerdisplay.cpp` | Include; timers around the geometry rebuild block (`createObjects` / `processPartitionQ` / `updateGeom`), the rest of `display()`, and `swapBuffers` in `swap()`; rebuild counter | To name the stage that stretches a frame |

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKMotionRecoverClampedVelocity`
- `indra/newview/skins/default/xui/en/menu_viewer.xml` — "Recover Clamped Velocities" check item under Develop → Network, between Ping Interpolate and Record Object Motion (XML comment marker)

---

## Camera follow

Branch `feature-camera/follow` (base `feature-motion/unclamp`). Brief `.camera-follow-brief.md`; oracle `V:\Scratch\motion-recorderollower.py`.

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/llagentcamera.h` / `.cpp` | `smoothSKFollow()` and its state; applied to the alt-cam focus object's render position in `calcFocusPositionTargetGlobal` (offset added after, unfiltered), feed-forward root velocity + ω×r; resets in `resetView` and `setFocusOnAvatar`; raw-focus getter for the recorder | The alt-cam focus is computed only here, and every network correction of the focus object went straight into the view |
| `indra/newview/skmotionrecorder.h` / `.cpp` | CAMERA gains `focus_raw`; header version 4; `SKCameraFollow*` in the header settings | Our own file |

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKCameraFollowEnabled`, `SKCameraFollowSmoothing`
- `indra/newview/skins/default/xui/en/menu_viewer.xml` — "Smooth Camera Follow" under Develop → Network (XML comment marker)

---

## Smoothed update corrections

Branch `feature-motion/smooth` (base `feature-motion/unclamp`, merged sibling `feature-camera/follow`). Brief `.motion-smooth-brief.md`; oracle `V:\Scratch\motion-recorder\interp_variants.py`.

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/llviewerobject.h` / `.cpp` | `skSmoothUpdate` / `skBleedCorrection` / `skNudge` and `mSKPendingCorrection`; the shown position captured before the update, the error stored where `setPositionParent` applied it, the bleed in `interpolateLinearMotion` with the step's dilated dt before the clamps; cleared in `setRegion`, on selection, attachment, static retirement and the snap cases | The snap-versus-keep decision only exists where the update is applied, and the bleed must ride the same per-frame step as the motion |
| `indra/newview/skmotionrecorder.h` / `.cpp` | `onUpdateSmoothed`; UPDATE `snapped` / `correction`, OBJECT `pending`; header version 5 with the `SKMotionSmooth*` settings | Our own file |

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKMotionSmoothCorrections`, `SKMotionSmoothTau`
- `indra/newview/skins/default/xui/en/menu_viewer.xml` — "Smooth Update Corrections" under Develop → Network (XML comment marker)


## Probe shot export

Branch `feature-probe-shot`. Develop → Render Tests → Export Probe Shot saves a probe's six faces at the camera, local and default-probe masks, to `<logs>/probe_shots/`.

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/llreflectionmapmanager.h` / `.cpp` | `requestProbeShot` / `takeProbeShot`, `mProbeShotPending`; the shot runs in `update()` after the in-flight probe face | The manager owns the probe render state (radiance pass, light scale, aux target) the shot must reproduce |
| `indra/newview/llviewermenu.cpp` | `Advanced.ExportProbeShot` commit callback | Menu wiring |
| `indra/newview/skins/default/xui/en/menu_viewer.xml` | "Export Probe Shot" under Develop → Render Tests (XML comment marker) | Menu wiring |


## Atmo Magic — terrain

Branch `feature-atmo/terrain` (merged into `develop`). Build doc: `.atmo-build-atmo-terrain.md`. A seeded procedural height law draws ground beyond the region (CDLOD patches into the G-buffer and sun shadows) and continues it as a far-layer source; `replace` draws it inside the region too.

### Files we own

| File | Role |
|---|---|
| `indra/newview/skatmoterrain.h` / `.cpp` | Pure: height law, F32 shader sequence, route, edge blend, classifier, follow, CDLOD selection and morph chain, grid mesh |
| `indra/newview/skdrawpoolterrain.h` / `.cpp` | `SKDrawPoolTerrain`: the patch pass and shadow pass, per-frame view, region edge profiles |
| `indra/newview/skskyterrain.h` / `.cpp` | `SKSkyTerrain`: the far source (order 20) |
| `indra/newview/app_settings/shaders/class1/deferred/skTerrain{V,F,Util}.glsl`, `skFarTerrainF.glsl` | Patch and far shaders |
| `indra/newview/skins/default/xui/en/panel_sk_atmo_surround.xml` | Surrounds page in the Atmo editor |
| `indra/newview/tests/skatmoterrain_test.cpp`, `skatmoterrain_golden.inc`, `skterrain_gl_test.cpp` | UNIT test (oracle pins) and GL test |

Also edited (ours): `skatmostate.*` (terrain lift and follow cache), `skatmomotion.h`, `skatmoprovider.cpp`, `skatmosystem.*`, `skfloateratmo.*`, `panel_sk_atmo_system.xml` (replace string), `tests/skatmosystem_test.cpp`, `indra/llui/tests/skatmoxui_test.cpp` (Surrounds page).

### Injection sites

All marked `<SK:Nexii> atmo/terrain`.

| File | What | Why there |
|---|---|---|
| `indra/newview/lldrawpool.h` | `POOL_SK_TERRAIN`, placed before `POOL_WL_SKY` since upstream draws the sky after the world (2026-10-10) | Pool type enum; render order follows it |
| `indra/newview/lldrawpool.cpp` | `skdrawpoolterrain.h` include; `createPool` case | Pool factory |
| `indra/newview/pipeline.h` | `RENDER_TYPE_SK_TERRAIN`; `mSKTerrainPool` | Render type and quick-lookup slot |
| `indra/newview/pipeline.cpp` | Include; `getPool(POOL_SK_TERRAIN)` in `init` (faceless, nothing else creates it); `releaseGL` in `releaseGLBuffers`; `findPool`, `addToQuickLookup` (duplicate guard), `removeFromQuickLookup` cases | Pool lifetime and lookup |
| `indra/newview/pipeline.cpp` | `RENDER_TYPE_SK_TERRAIN` in the `generateSunShadow` render-type mask | Without it the shadow pass never ran |
| `indra/newview/pipeline.cpp` | `RENDER_TYPE_SK_TERRAIN` in the `generateImpostor` clear mask | Terrain must not enter avatar impostors |
| `indra/newview/llreflectionmapmanager.cpp` | `RENDER_TYPE_SK_TERRAIN` in the probe render-type mask | Probe snapshots draw a coarser tree |
| `indra/newview/llviewershadermgr.cpp` | Includes; `SKDrawPoolTerrain::loadShaders` and `SKSkyTerrain::loadShaders` after the deferred shaders load, guarded by `success` | A failure leaves only the terrain off |
| `indra/newview/lldrawpoolwlsky.cpp` | Include; `SKSkyTerrain::registerWith(SKSkyFarLayer::instance().getSources())` in the constructor; `SKSkyTerrain::cleanupGL()` in `cleanupGL` | The pool owns sky GL lifetime |

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SKAtmoTerrainDraw`
- `indra/newview/CMakeLists.txt` — `skatmoterrain`, `skdrawpoolterrain`, `skskyterrain` sources and headers; UNIT test `skatmoterrain`; GL test `skterrain_gl` (`SK_TERRAIN_SHADER_DIR`)
- Shaders: new files, found by name


## Atmo Magic — fold landings (clock, terrain, blackhole, storm-shape, interior)

Five branches landed on `develop` as merge commits after `d400a1613c`. Only one upstream file was touched by all five together: `indra/newview/CMakeLists.txt`. No new `<SK:Nexii>` markers, no other upstream edits.

### atmo/clock-fold (`a4bac4facf`)

Plain document clock for clouds, wind and weather; warp recovery; Held vs Authored module state.

- New/changed Skooma files: `skatmoclock.*`, `skatmobake.*`, `skatmoclouds.*`, `skatmoeval.*`, `skatmomotion.h`, `skatmoprovider.*`, `skatmostate.*`, `skatmoweather.*`, `skatmowind.*`; tests `skatmoclock_test.cpp`, `skatmoclouds_test.cpp`, `skatmostate_test.cpp`, `skatmoweather_test.cpp`, `skatmowind_test.cpp`.
- Injections: `indra/newview/CMakeLists.txt` (unmarked), see the shared table below.

### atmo/terrain-fold (`9eab6a00a6`)

Terrain fold: authored edge heights, replaced regions on the law, altitude-rail track add/delete, route channel along the past path.

- New/changed Skooma files: `skatmoterrain.*`, `skdrawpoolterrain.*`, `skatmostate.*`, `skatmoeditor.*`, `skfloateratmo.*`, `skTerrainUtil.glsl`, `floater_sk_atmo.xml`, `panel_sk_atmo_motion.xml`, `panel_sk_atmo_surround.xml`; tests `skatmoterrain_test.cpp`, `skatmoeditor_test.cpp`, `skterrain_gl_test.cpp`, `indra/llui/tests/skatmoxui_test.cpp`.
- Injections: none.

### atmo/blackhole-fold (`1794664007`)

Black hole sized from solar masses, seen from any inclination, cross-faded between tracks, disc texture turned from the clock.

- New/changed Skooma files: `skatmoblackhole.*`, `skatmobodies.*`, `skatmoorbital.*`, `skatmoprovider.*`, `skatmoeditor.cpp`, `skfloateratmo.*`, `skskyblackhole.cpp`, `skFarBlackHoleF.glsl`, `skFarBlackHoleV.glsl`, `panel_sk_atmo_blackhole.xml`; tests `skatmoblackhole_test.cpp`, `skatmobodies_test.cpp`, `skatmoeditor_test.cpp`, `skatmoorbital_test.cpp`, `skskyblackhole_gl_test.cpp`, `indra/llui/tests/skatmoxui_test.cpp`.
- Injections: none.

### atmo/storm-shape (`cd836a8d50`)

Cumulonimbus storm systems (towers, anvils, plumes) marched in the cloud shader.

- New Skooma files: `skatmocloudstorm.h` / `.cpp`; test `tests/skatmocloudstorm_test.cpp`.
- Changed Skooma files: `skatmoclouddensity.*`, `skatmocloudlayers.*`, `skcloudmarch.*`, `skcloudshell.*`, `skdrawpoolclouds.cpp`; shaders `skCloudCoverBakeF.glsl`, `skCloudMarchF.glsl`, `skCloudMarchUtilF.glsl`, `skCloudOccupancyF.glsl`, `skCloudShellUtilF.glsl`; tests `skatmocloudlayers_test.cpp`, `skcloudmarch_gl_test.cpp`.
- Injections: `indra/newview/CMakeLists.txt` (unmarked), see below.

### atmo/interior-fold (`cea3b9dbf1`, via `10a07f26c9`)

Interiors light their night side with ringshine; hollow spheres pool air at the spin equator; rings and stations show their parent body; shell anchor kept across region crossings.

- New Skooma files: `tests/skatmointerior_pooled.inc`, `tests/skatmointerior_shine.inc`, `tests/skatmointerior_weights.inc`.
- Changed Skooma files: `skatmointerior.*`, `skatmoshell.*`, `skatmobodies.*`, `skatmoorbital.cpp`, `skatmoprovider.*`, `skatmosystem.*`, `skatmoeditor.cpp`, `skdrawpoolclouds.cpp`, `skfloateratmo.*`, `skinteriorair.cpp`, `skskyshell.*`, `skFarShellF.glsl`, `skInteriorAirUtilF.glsl`, `panel_sk_atmo_body.xml`, `panel_sk_atmo_system.xml`; tests `skatmointerior_test.cpp`, `skatmoorbital_test.cpp`, `skatmoshell_test.cpp`, `skatmobodies_test.cpp`, `skinteriorair_gl_test.cpp`, `skskyshell_gl_test.cpp`.
- Conflict-resolved, both sides kept, in the interior-fold merge `10a07f26c9` (blackhole-fold into interior-fold): `indra/newview/skatmobodies.cpp`, `indra/newview/skatmoprovider.cpp`, `indra/newview/tests/skatmobodies_test.cpp`.
- Injections: none.

### Injection sites (unmarked)

| File | What | Why there |
|---|---|---|
| `indra/newview/CMakeLists.txt` | `skatmocloudstorm.cpp` / `.h` in the source and header lists; `skatmocloudstorm` added to the `skatmoclouddensity`-family sources; new UNIT test `skatmocloudstorm`; `skatmocloudstorm.cpp` added to the cloud GL test sources (`skatmobake.cpp` and its comment dropped there) | Build registration (storm-shape; clock-fold adjusted source lists) |



## Atmo Magic — seven landings (interior-light, editor-clock, blackhole-dynamic, precip-render, terrain-materials, storm-underside, surround) and nav/annotate-freeze

Landed on `develop` after `c0df3d8350`: atmo/interior-light (`5be3e7636a`, first land `02ab6b5252`), atmo/editor-clock (`392a3cca24`, first `19bf8efd9b`), atmo/blackhole-dynamic (`9d736186b3`, first `244ac19b1b`), atmo/precip-render (`ad0cc105c7`, first `b59a4df11c`), atmo/terrain-materials (`76961c68ee`), atmo/storm-underside (`1ab692cc4b`), the first three commits of atmo/surround (`7555979a56`, branch still in progress) and fix-nav/annotate-freeze (`f49d9d121c`). Upstream-file edits are marked `<SK:Nexii>` unless stated; `indra/newview/CMakeLists.txt` and `settings_alchemy.xml` are unmarked build/settings registrations.

### atmo/interior-light

The flat cloud march inside an interior too small for the curved chart, hazed and lit by its air.

- Skooma files: `skatmointerior.*`, `skatmoprovider.*`, `skcloudmarch.h`, `skcloudshell.cpp`, `skdrawpoolclouds.*`, `skinteriorair.*`, `skinteriorairgl.h`, `skskyshell.cpp`; shaders `skCloudMarchF.glsl`, `skCloudShellUtilF.glsl`, `skFarShellF.glsl`, `skInteriorAirUtilF.glsl`; tests `skatmointerior_test.cpp`, `skatmointerior_light.inc`, `skcloudmarch_gl_test.cpp`, `skcloudpuff_gl_test.cpp`, `skinteriorair_gl_test.cpp`, `skskyshell_gl_test.cpp`.
- Upstream files:
  - `indra/newview/llviewershadermgr.cpp` (marked): `gSKCloudMarchInteriorProgram` defined, unloaded in `loadShadersDeferred`, and built there with `SK_INTERIOR_AIR`.
  - `indra/newview/llviewershadermgr.h` (marked): the `extern` for `gSKCloudMarchInteriorProgram`.
  - `indra/newview/CMakeLists.txt` (unmarked): `skatmointerior.cpp` and the pure-half sources added to the interior UNIT test.
  - `indra/newview/app_settings/settings_alchemy.xml` (unmarked): `SKCloudInteriorLight`.

### atmo/editor-clock

Clock controls in the Atmo editor; storm and cloud-org plumbing for it.

- Skooma files: `skatmoclock.*`, `skatmoclouds.*`, `skatmocloudlayers.cpp`, `skatmocloudorg.*`, `skatmocloudstorm.*`, `skatmoeditor.*`, `skatmoprovider.cpp`, `skatmoweather.*`, `skcloudmarch.*`, `skdrawpoolclouds.cpp`, `skfloateratmo.cpp`, `floater_sk_atmo.xml`, `skCloudCoverBakeF.glsl`; tests `skatmoclock_test.cpp`, `skatmocloudstorm_test.cpp`, `skatmoeditor_test.cpp`, `skatmoweather_test.cpp`, `indra/llui/tests/skatmoxui_test.cpp`.
- Upstream files: none.

### atmo/blackhole-dynamic

Dynamic (moving-observer) black hole rendering on Bruneton's ray tables.

- Skooma files: `skatmoblackholetables.*`, `skatmoorbital.*`, `skfloateratmo.*`, `skskyblackhole.*`, `skFarBlackHoleDynamicF.glsl`, `panel_sk_atmo_blackhole.xml`; tests `skatmoblackholetables_test.cpp`, `skatmobodies_test.cpp`, `skatmoorbital_test.cpp`, `skskyblackhole_gl_test.cpp`, `indra/llui/tests/skatmoxui_test.cpp`.
- Upstream files:
  - `indra/newview/CMakeLists.txt` (unmarked): `skatmoblackholetables.cpp/.h`, UNIT test `skatmoblackholetables`, table source added to the black hole GL test.
  - `indra/newview/app_settings/settings_alchemy.xml` (unmarked): `SKAtmoBlackHoleQualityFloor`.

### atmo/precip-render

Precipitation: a top-down shelter capture, particles and impacts drawn after water and the alpha pools.

- Skooma files: `skprecip.*`, `skprecipgl.h`, `skatmoprecipdraw.*`, `skfloateratmo.cpp`, `panel_sk_atmo_weather.xml`; shaders `skPrecipF.glsl`, `skPrecipShelterF.glsl`, `skPrecipUtil.glsl`, `skPrecipV.glsl`; tests `skatmoprecipdraw_test.cpp`, `skatmoprecipdraw_golden.inc`, `skprecip_gl_test.cpp`, `indra/llui/tests/skatmoxui_test.cpp`.
- Upstream files:
  - `indra/newview/llviewercamera.h` (marked): `CAMERA_SK_PRECIP_SHELTER` camera enum.
  - `indra/newview/llviewerdisplay.cpp` (marked): include `skprecip.h`; `SKPrecip::capture()` before the shadow pass, with or without sun shadows.
  - `indra/newview/llviewershadermgr.cpp` (marked): include; `SKPrecip::loadShaders` after the deferred shaders load, guarded by `success`.
  - `indra/newview/pipeline.cpp` (marked): include; `SKPrecip::cleanupGL()` in `releaseGLBuffers`; `SKPrecip::draw()` in `renderGeomPostDeferred` after water and alpha, before glow.
  - `indra/newview/CMakeLists.txt` (unmarked): `skprecip` and `skatmoprecipdraw` sources/headers, UNIT test `skatmoprecipdraw`, GL test `skprecip_gl` (`SK_PRECIP_SHADER_DIR`).
  - `indra/newview/app_settings/settings_alchemy.xml` (unmarked): `SKAtmoPrecipPreview`, `SKAtmoPrecipPreviewMedium`.

### atmo/terrain-materials

Procedural ground materials, terrain page cache and a baked horizon shadow so mountains shadow land past the cascades.

- New/changed Skooma files: `skatmoterrainpages.*`, `skterrainpages.*`, `skatmoterrain.*`, `skdrawpoolterrain.*`, `skskyterrain.cpp`, `skfloateratmo.*`, `panel_sk_atmo_surround.xml`; shaders `skTerrainF.glsl`, `skFarTerrainF.glsl`, `skTerrainUtil.glsl`, `skTerrainHorizonShadowF.glsl`, `skTerrainMaterialUtil.glsl`, `skTerrainPagesUtil.glsl`; tests `skatmoterrainpages_test.cpp`, `skterrain_gl_test.cpp`, `indra/llui/tests/skatmoxui_test.cpp`.
- Upstream files:
  - `indra/newview/app_settings/shaders/class2/deferred/sunLightF.glsl` (marked): `skTerrainHorizonShadow` declaration and `col.r = min(col.r, ...)`.
  - `indra/newview/app_settings/shaders/class2/deferred/sunLightSSAOF.glsl` (marked): the same two lines.
  - `indra/newview/llviewershadermgr.cpp` (marked): `skTerrainHorizonShadowF.glsl` and `skTerrainPagesUtil.glsl` added to `gDeferredSunProgram` and `gDeferredSunProbeProgram`.
  - `indra/newview/pipeline.cpp` (marked): `SKDrawPoolTerrain::bindHorizonShadow` / `unbindHorizonShadow` around the sun shader in `renderDeferredLighting`.
  - `indra/newview/CMakeLists.txt` (unmarked): `skatmoterrainpages`, `skterrainpages` sources/headers, UNIT test `skatmoterrainpages`, `skatmoterrainpages.cpp` into the terrain GL test.
  - `indra/newview/app_settings/settings_alchemy.xml` (unmarked): `SKAtmoTerrainPages`, `SKAtmoTerrainHorizon`.
- Conflict: the `CMakeLists.txt` conflict was resolved by merging the landed sibling branches in `70156d2630`.

### atmo/storm-underside

Storm cloud undersides, events and precipitation hooks; air texture unit moved past the storm units.

- Skooma files: `skatmoclouddensity.*`, `skatmocloudlayers.*`, `skatmocloudorg.*`, `skatmocloudstorm.*`, `skatmoevents.*`, `skatmoprecip.*`, `skatmoprovider.cpp`, `skatmoweather.*`, `skcloudmarch.*`, `skcloudshell.*`, `skdrawpoolclouds.cpp`; shaders `skCloudCoverBakeF.glsl`, `skCloudMarchF.glsl`, `skCloudMarchUtilF.glsl`, `skCloudOccupancyF.glsl`, `skCloudShellUtilF.glsl`; tests `skatmocloudlayers_test.cpp`, `skatmocloudstorm_test.cpp`, `skatmoevents_test.cpp`, `skatmoprecip_test.cpp`, `skatmoweather_test.cpp`, `skcloudmarch_gl_test.cpp`.
- Upstream files:
  - `indra/newview/CMakeLists.txt` (unmarked): `skatmocloudorg.cpp` and `skatmocloudstorm.cpp` added to three test source lists.
  - `vcpkg` (submodule pointer): moved by a merge from `next`, not a Skooma edit.
- Conflicts resolved in merge `0b1f62574f` (editor-clock into storm-underside): `skCloudMarchF.glsl`; `skcloudshell.cpp` (air texture unit moved to k=5/6, past the fourth storm unit); `skcloudmarch_gl_test.cpp` (both namespaces kept, rain test renumbered to 21).

### atmo/surround (partial: `4e82986a5d`, `8ac4c74d39`, `059fdbfa16`)

Surround model for the terrain edge; branch still in progress.

- Skooma files: `skatmosurround.*`, `skatmolimits.h`, `skatmoterrain.cpp`; test `skatmosurround_test.cpp`.
- Upstream files:
  - `indra/newview/CMakeLists.txt` (unmarked): `skatmosurround` sources/headers and UNIT test.

### atmo/surround (rest: `4fc4533a9b`, `d0bcd042fa`, `27a236aa27`, `b98ef6fc4f`; landed by merge `155f249bbf`)

Rezzed linksets convert to surround objects (4 km scale, free of region clipping, purple outline), baked beyond reach into the far layer, listed on the Surrounds tab. All upstream edits are marked `<SK:Nexii> atmo/surround` unless stated.

- Skooma files: new `skatmosurroundobjects.*`, `skskysurround.*`, `skSurroundBakeF.glsl` (class1/deferred), tests `skskysurround_gl_test.cpp`; changed `skatmoprovider.cpp` (sync/update/clear), `skatmosurround.*`, `skskyfarlayer.cpp` (sync, shader load/cleanup), `skfloateratmo.*`, `panel_sk_atmo_surround.xml`, `skatmosurround_test.cpp`, `indra/llui/tests/skatmoxui_test.cpp`.
- Upstream files:
  - `indra/newview/llselectmgr.cpp`: purple outline for surround parts in the mesh and silhouette highlight branches.
  - `indra/newview/llmaniptranslate.cpp`: skips `clipToVisibleRegions` for surround parts (root clip).
  - `indra/newview/llmanipscale.cpp`: two region clips skipped, two max scales and `partToMaxScale` raised to the surround limit.
  - `indra/newview/llmaniprotate.cpp`: root clip skipped for surround parts.
  - `indra/newview/llpanelobject.cpp`: two max scales raised; typed-position region check bypassed for surround parts.
  - `indra/newview/CMakeLists.txt` (unmarked): surround object, sky surround and bake sources, headers and tests.
- Conflicts resolved in merge `4a4bfd81d2` (terrain-materials into surround): `skatmoxui_test.cpp`, `skfloateratmo.cpp/.h`, `panel_sk_atmo_surround.xml`, both sides kept.

### nav/annotate-freeze (`f49d9d121c`, commit `c7349d5c4b`)

Navmesh tiles are not rebuilt under the annotator while it is armed.

- Skooma files: `indra/newview/sknavmesh.cpp` only.
- Upstream files: none.

### atmo/surface (landed by merge `802346e823`)

The weather's marks (wet, puddles, snow, frost, stains) on terrain, objects and avatars: a ledger, an overlay pass on the G-buffer, a Surface page. Upstream edits are marked `<SK:Nexii> atmo/surface` unless stated.

- Skooma files: new `skatmosurface.*`, `skatmosurfacemodule.*`, `sksurface.*`, shaders `skSurfaceCopyF.glsl`, `skSurfaceOverlayF.glsl` (class1/deferred), `panel_sk_atmo_surface.xml`, tests `skatmosurface_test.cpp`, `skatmosurfacemodule_test.cpp`, `skatmosurface_gl_test.cpp` and the goldens `skatmosurface_golden.inc`, `skatmosurfacedrain_golden.inc`; changed `skatmoprovider.*`, `skdrawpoolterrain.cpp` (snow atlas), `skprecip.*`, `skfloateratmo.*`, shaders `skPrecipShelterF.glsl`, `skTerrainV.glsl`.
- Upstream files:
  - `indra/newview/llviewerdisplay.cpp`: include plus `SKSurface::render()` after `renderGeomDeferred`, before any light reads the G-buffer.
  - `indra/newview/llviewershadermgr.cpp`: include plus `SKSurface::loadShaders()` after the deferred set loads (block-marked).
  - `indra/newview/pipeline.cpp`: include plus `SKSurface::cleanupGL()` in `cleanupGL`.
  - `indra/newview/app_settings/settings_alchemy.xml` (unmarked): `SKRenderAtmoSurface`, `SKAtmoSurfacePagesPerFrame`.
  - `indra/newview/CMakeLists.txt` (unmarked): surface sources/headers, UNIT tests `skatmosurface` and `skatmosurfacemodule`, GL test `skatmosurface_gl`.
- Conflicts resolved in merge `3c64c1355a` (surround into surface): `skdrawpoolterrain.cpp` (snow atlas binds at unit 1 + `SK_TERRAIN_PAGES_UNITS` + `SK_TERRAIN_MATERIAL_UNITS`, past the terrain pages and material); `skfloateratmo.cpp/.h` (both sides kept).

### atmo/surface-2 (landed by merge `42c04cb0a3`)

Wet glass, drips off eaves, probe marks and terrain surface work on top of atmo/surface. Upstream edits are marked `<SK:Nexii> atmo/surface-2` unless stated.

- Skooma files: new shaders `skDripF.glsl`, `skDripV.glsl` (class1/deferred); changed `skatmosurface.*`, `sksurface.*` (`bindWet`, `setWetUnit`, `unbindWet`, `renderProbe`, `drawDrips`), `skSurfaceOverlayF.glsl`, `terrainSurface.glsl`.
- Upstream files:
  - `indra/newview/lldrawpoolalpha.cpp`: include; `prepare_alpha_shader` takes `wet_glass` and calls `SKSurface::bindWet`; `renderPostDeferred` collects the primed programs for `SKSurface::setWetUnit` and calls `unbindWet` at the end.
  - `indra/newview/lldrawpoolterrain.cpp`: snow atlas unit is `shader->mActiveTextureChannels` (replaces the channel scan).
  - `indra/newview/llviewerdisplay.cpp`: `SKSurface::renderProbe()` after `renderGeomDeferred` in `display_cube_face`, so puddles reflect.
  - `indra/newview/pipeline.cpp`: `SKSurface::drawDrips()` after `SKPrecip::draw()` in `renderGeomPostDeferred`.
  - `indra/newview/app_settings/shaders/class1/deferred/deferredUtil.glsl` (block-marked): `skSurfaceWet()` and its uniforms, the ledger's wet film for forward alpha shaders.
  - `indra/newview/app_settings/shaders/class2/deferred/alphaF.glsl`: declaration, wet gloss in the probe sample, specular sheen on wet glass.
  - `indra/newview/app_settings/shaders/class2/deferred/pbralphaF.glsl`: declaration, roughness lowered by the wet film.
  - `indra/newview/app_settings/settings_alchemy.xml` (unmarked): `SKRenderAtmoSurfaceProbes`, `SKAtmoSurfaceRegionsKept`.

### atmo/storm-anvil (landed by merge `25d161a36b`)

Storm cover blended by the bake step's phase, anvil shaping in the cloud march (also carries the already-indexed storm-underside). Upstream edits are marked `<SK:Nexii> atmo/storm-anvil`.

- Skooma files: new shader `skCloudStormBlendF.glsl`; changed `skCloudMarchF.glsl`, `skCloudMarchUtilF.glsl`, `skCloudOccupancyF.glsl`, `skCloudShellUtilF.glsl`, `skatmoclouddensity.*`, `skatmocloudlayers.*`, `skcloudmarch.*`, `skcloudshell.*`, `skdrawpoolclouds.*`.
- Upstream files:
  - `indra/newview/llviewershadermgr.cpp`: `gSKCloudStormBlendProgram` definition, unload and block-marked load.
  - `indra/newview/llviewershadermgr.h`: its `extern`.

### atmo/screenfx (landed by merge `d32781b34b`)

Fog, the veil of falling weather, lens drops, heat shimmer and event flashes: a scene pass in the linear HDR chain and a display pass after tonemap and AA. Upstream edits are marked `<SK:Nexii> atmo/screenfx` unless stated.

- Skooma files: new `skscreenfx.*`, `skscreenfxgl.h`, `skatmoscreenfx.*`, shaders `skScreenFxSceneF.glsl`, `skScreenFxDropsF.glsl`, `skScreenFxDisplayF.glsl`; changed `skatmoprovider.*`, `skfloateratmo.*`, `sksurface.*`.
- Upstream files:
  - `indra/newview/llviewershadermgr.cpp`: include plus block-marked `SKScreenFx::loadShaders()` after the deferred set loads.
  - `indra/newview/pipeline.cpp`: include; `SKScreenFx::cleanupGL()` in `cleanupGL`; `SKScreenFx::drawScene()` after the luminance read; `SKScreenFx::drawDisplay()` after tonemap and AA (swaps source and target when it draws).
  - `indra/newview/app_settings/settings_alchemy.xml` (unmarked): `SKRenderAtmoScreenFx`.
  - `indra/newview/CMakeLists.txt` (unmarked): `skscreenfx`, `skatmoscreenfx` sources/headers, UNIT test `skatmoscreenfx`, GL test `skscreenfx_gl`.

### atmo/acoustics (landed by merge `6a1f729e1c`)

Weather sound: rain, snow, hail and sand beds, roof rain, Atmo's wind, thunder after lightning, wet and snowy footsteps. Upstream edits are marked `<SK:Nexii> atmo/acoustics` unless stated.

- Skooma files: new `skacoustics.*`, `skatmoacoustics.*`, `skatmoacousticsynth.*`; changed `skatmoprecip.cpp`, `skatmoprovider.*`, `skfloateratmo.*`, `sksurface.*`.
- Upstream files:
  - `indra/newview/llappviewer.cpp`: include; `SKAcoustics::update()` in the frame loop before the wind it feeds.
  - `indra/newview/llvieweraudio.cpp`: include; Atmo's wind at the listener replaces the region's wind vector; its share raises or lowers `mMaxWindGain` (block-marked).
  - `indra/newview/llvoavatar.cpp`: include; `SKAcoustics::footstep()` for the own avatar's footsteps.
  - `indra/newview/app_settings/settings_alchemy.xml` (unmarked): `SKAudioAtmoAcoustics`.
  - `atmo/rulings-oct5` Q165 (`c4ffbc78fa`): Atmo drives the reverb preset from the enclosure; marked `llfloaterpreference.cpp` (preset row handling gone, send-scale slider kept) and XML comments in `en/` and `de/panel_preferences_sound.xml` (preset combo removed).
  - `indra/newview/CMakeLists.txt` (unmarked): acoustics sources/headers, UNIT tests `skatmoacoustics`, `skatmoacousticsynth`.

---

## Floater collapse (replaces minimize)

Branch `feature-minimize-vs-collapse`. Floaters fold against the nearest screen edge
instead of minimizing, and a floater restored at login comes back collapsed.

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/llui/llfloater.h` / `.cpp` | Edge picking (`pickCollapseEdge`, `calcCollapsedRect`, `calcExpandedRect`), turned drag handle, directional collapse icons, bar drawing, `mDraggedWhileCollapsed`; `storeRectControl` saves the expanded rect while collapsed; `mCollapseStateControl` + `storeCollapseStateControl()` written at the end of `setMinimized` (not while quitting) | Collapse is `setMinimized`, so every path in and out passes through it |
| `indra/llui/llfloaterreg.h` / `.cpp` | `declareCollapseStateControl` / `getCollapseStateControlName` (`floater_collapsed_<name>`); `showInitialVisibleInstances` re-collapses a restored floater; `registerControlVariables` declares saved ones | Beside the visibility control it rides on, at the one place login reopens floaters |
| `indra/llui/llfloater.cpp` (`LLFloaterView::getCollapsedPosition`) | Where a bar lands against an edge, stacked past toolbars and other bars | The floater view owns the toolbar rects |
| `indra/llui/lldraghandle.h` / `.cpp` | `setTurned` / `mTurned`: a side-collapsed header drawn turned 90°, buttons at its far end | The header draws the bar |
| `indra/llrender/llrender2dutils.h` / `.cpp` | Helper drawing what follows turned 90° counter-clockwise | Used by the turned header |
| `indra/llui/lltextbase.h` | `setClip()` | Turned text cannot use a screen-space scissor |
| `indra/llui/lltoolbar.h` / `.cpp` | `mHasButtons`; button-area query for bars to keep clear of; empty toolbars ignored | Bars stack past toolbars |
| `indra/llui/lldockablefloater.cpp`, `indra/newview/llimview.cpp` | Collapse button instead of minimize; `onClickRestore` for collapsed IM floaters | Minimize callers |

### Unmarked additions

- `indra/newview/app_settings/settings_alchemy.xml` — `SkoomaFloaterCollapse*` settings
- Minimize → collapse one-liners (`setCanMinimize` dropped or `isMinimizeable` → `isCollapsible`): `llui/llmenugl.cpp`, `llmodaldialog.cpp`, `llmultifloater.h` / `.cpp`; `newview/alfloaterxuistudio.cpp`, `llfloaterimcontainer.cpp`, `llscenemonitor.cpp`, `llsceneview.cpp`, `llsettingspicker.cpp`, `lltexturectrl.cpp`, `lltoastalertpanel.cpp`; `llpaneltopinfobar.h` / `.cpp` (minimize hook removed)
- `skins/{default,gemini,heretic}/xui/en/widgets/floater.xml`, `skins/xui.xsd` — collapse attributes replace minimize ones
- `skins/default/textures/textures.xml` and `textures/windows/Icon_Collapse_{Up,Down,Left,Right}_{Foreground,Press}.png` (default and heretic skins) — directional collapse icons

---

## Over-the-shoulder mouselook

Branch `feature-combat/ots`. Soapstorm port (`CAMERA_MODE_OTS`, vehicle PRs #65/#69), rebuilt as a view inside mouselook rather than a camera mode. Own files: `indra/newview/skotscamera.h` / `.cpp` (`SKOTSCamera`).

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/llagentcamera.cpp` | Shoulder camera and focus in the mouselook branches of `calcCameraPositionTargetGlobal` / `calcFocusPositionTargetGlobal` (seat camera offset passed through); first-person flags and avatar-following start in the `updateCamera` animation; head-tracking skipped in the shoulder view; `onEnterMouselook` in `changeCameraToMouselook`; glide out in `changeCameraToThirdPerson`; animation start frame in `startCameraAnimation` | Mouselook placement and its transitions live only here |
| `indra/newview/llagent.cpp` | `needsRenderAvatar` / `needsRenderHead` show the avatar in the shoulder view | The one test for drawing self |
| `indra/newview/llvoavatarself.cpp` | `updateAttachmentVisibility` treats the shoulder view as third person | Every caller passes the camera mode through it |
| `indra/newview/llviewermessage.cpp` | `send_agent_update` reports the eye aimed at the crosshair and turns HeadRotation onto the same aim (not when seated); BodyRotation is left alone, since the region moves the avatar along it | The camera and body rotation the region sees are built here |
| `indra/newview/llviewermenu.cpp` | `LLViewMouselook` (M) requests the shoulder view | Only M opens it; scroll-in and scripts stay first person |
| `indra/newview/llviewerinput.cpp` | `ots_toggle_view`, `ots_swap_shoulder` actions | Keyboard action registry |
| `indra/newview/llkeyconflict.cpp` | `generatePlaceholders` blocks `ots_toggle_view`/`ots_swap_shoulder` outside first person | Per-mode key map placeholders live here |
| `indra/newview/llviewerfloaterreg.cpp` | `sk_ots_options` beside `camera` | Floater registry |

### Unmarked additions

- `indra/newview/CMakeLists.txt` — `skotscamera.cpp` / `.h` beside `llagentcamera`
- `indra/newview/app_settings/settings_alchemy.xml` — `SKOTS*` settings beside `AlchemyMouselookIFF` (XML comment marker)
- `indra/newview/skins/default/xui/en/menu_viewer.xml` — new top-level **Combat** menu between World and Build (XML comment marker)
- `indra/newview/skins/default/xui/en/control_table_contents_movement.xml` — Shoulder / First Person, Swap Shoulder rows (XML comment marker)
- `indra/newview/skins/default/xui/en/floater_sk_ots_options.xml` — new

---

## Combat render lockout

Branch `feature-combat/lockout` (merge `f8f5aa71e5`). Soapstorm port (`feature-combat-render-lockout`, `8f635810aa`). While in mouselook (the shoulder view included) and for 5 s after, see-through debug views are off. Own files: `indra/newview/skcombatlockout.h` / `.cpp` (`SKCombatLockout`).

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/newview/llviewerdisplay.cpp` | `SKCombatLockout::update()` after the camera settles in `display()` | Once a frame, before any pass and outside every render type push/pop |
| `indra/newview/pipeline.h` | `hasRenderDebugMask` filters the locked bits; `isRenderTypeEnabled` exposes the user's toggle | Every debug-view reader goes through it |
| `indra/newview/pipeline.cpp` | Beacons block skipped while locked; `toggleRenderTypeControl`/`hasRenderTypeControl`/`toggleRenderTypeControlNegated` go through the lockout (merged with the Atmo terrain gate's `type != 0`); `toggleRenderDebug`/`toggleRenderDebugControl` read the raw mask; debug text cleared on the filtered mask | Menu state stays the user's while the live flags are forced |
| `indra/newview/llviewermenu.cpp` | Wireframe, Show Hidden Selection, Highlight Transparent toggles and checks through the lockout's stash | Toggles made while locked apply when it lifts |
| `indra/newview/llhudeffectlookat.cpp`, `llhudeffectpointat.cpp` | `render()` skipped while locked | An unseen opponent's aim |
| `indra/newview/lltoolgun.cpp` | Mouselook IFF removed: offscreen markers, crosshair name and distance, and the group-only marker helpers | They found avatars through walls |
| `indra/newview/llviewerwindow.cpp` | Position and health overlay removed from `drawMouselookInstructions` | Not wanted in combat |
| `indra/newview/CMakeLists.txt` | `skcombatlockout.cpp` / `.h` | Build |

Unmarked: `AlchemyMouselookIFF`, `AlchemyMouselookIFFMarkers`, `AlchemyMouselookIFFRange` and `AlchemyMouselookPosition` removed from `settings_alchemy.xml`, and their controls from `en/panel_preferences_move_mlook.xml`.

---

## Prim edge splits

Branch `feature-render/prim-split` (merge `257736e19c`). Untwisted square and triangle prims on a line path drop the legacy per-vertex-lighting edge splits (box 108 → 12 triangles at the top LOD). Own test: `indra/llmath/tests/llvolumesplit_test.cpp`.

### Injection sites

| File | What | Why there |
|---|---|---|
| `indra/llmath/llvolume.cpp` | `split = 0` condition also covers untwisted paths unless `sSplitFlatSides`; the static's definition | Where `LLVolume::generate` picks the split |
| `indra/llmath/llvolume.cpp` | `createSide` and `createUnCutCubeCap` partial builds only when the vertex and index counts are unchanged (merge `955852d822`) | A face shrunk by an in-place regen kept stale indices |
| `indra/llmath/llvolume.h` | `static bool sSplitFlatSides`; `getProfileSplit`, `isSameMesh` | A/B toggle, the test, LOD equivalence |
| `indra/llmath/llvolume.cpp` | The split rule moved into `getProfileSplit`; `isSameMesh` compares path and profile at two details (merge `fe9247088c`) | Shared by `generate()` and the LOD check |
| `indra/llmath/llvolumemgr.h` / `.cpp` | `LLVolumeLODGroup::regenPrims`, `getCanonicalLOD` (cached `mCanonicalLOD`); `LLVolumeMgr::regenPrims` | Per-shape LOD cache lives in the group |
| `indra/newview/llvovolume.cpp` | `calcLOD` returns no change between LODs with the same canonical LOD | Skips rebuilds that change nothing |
| `indra/newview/llviewercontrol.cpp` | `handleLegacyPrimSplitsChanged` regenerates every cached prim LOD and unique volumes; listener; three includes | Settings listeners live there |
| `indra/newview/skins/default/xui/en/menu_viewer.xml` | Render Tests > Legacy Prim Edge Splits | Debug A/B |
| `indra/newview/app_settings/settings_alchemy.xml` | `SKRenderLegacyPrimSplits` (unmarked, next to the `SKRender*` entries) | Setting |
| `indra/llmath/CMakeLists.txt` | `llvolumesplit` test registered (marked TUT guard) | Build |
| `indra/llmath/llvolume.cpp` | End of `createSide`: a sculpt's degenerate (pinched) triangles dropped from the indices (unmarked; `fix-sculpt-degenerate`, merge `3f39170197`) | Each rasterised as a stripe across the region |
| `indra/llmath/tests/llvolume_test.cpp` | New TUT test for the above (unprefixed, unmarked), registered in a marked guard in `llmath/CMakeLists.txt` | Build |

---

## Navmesh — capture export

Branch `feature-nav/capture` (landed 2026-10-05): the vehicle export generalised into a capture export of
any source kind, with per-face `textureLoaded` / `teMissing` and PBR alpha from the base colour. Plan:
`.outline-capture-buildout.md` (index `NAVMESH.md`).

### Files we own

| File | Role |
|---|---|
| `indra/newview/skcaptureexport.h` / `.cpp` | Capture export (vehicle / attachments / avatar) and the shared face-visibility helpers (`skcapture::faceSeeThrough`, `seeThroughShare`) the census and navmesh call |

### Unmarked additions

- `indra/newview/CMakeLists.txt` — `skcaptureexport.cpp` / `.h`

---

## Atmo Magic — wind-slab, debug, events-render, interior-probes, notation-sweep

Landed on `develop` after `eec74cf80a`: atmo/wind-slab (`e8dfe154e6`), atmo/debug (`93271885c5`), atmo/events-render (`7065cbdfad`), atmo/interior-probes (`2ae46f2ab6`), atmo/notation-sweep (`ebb33ef76d`). The re-merge of atmo/acoustics (`a68e61d47d`) changed no files. Upstream edits are marked `<SK:Nexii>` unless stated; `indra/newview/CMakeLists.txt` and `settings_alchemy.xml` are unmarked build/settings registrations.

### atmo/wind-slab (merge `e8dfe154e6`)

A wind slab Atmo samples, and feeds back as the region's wind so flexi prims, particles and avatar hair follow it.

- Skooma files: new `skatmowindslab.*`, `skwindslab.*`; changed `skatmowind.*`, `skfloateratmo.*`, `skprecip.cpp`, `skPrecipV.glsl`, `panel_sk_atmo_weather.xml`; tests `skatmowindslab_test.cpp` (new), `skatmoweather_test.cpp`, `skatmowind_test.cpp`, `skprecip_gl_test.cpp`.
- Upstream files:
  - `indra/newview/llwind.h` / `.cpp` (marked): `SKWindOverride` hook (`setSKOverride`, `sSKOverride`); `getVelocity` and `getVelocityNoisy` return Atmo's wind at the global point while the hook answers.
  - `indra/newview/llvoavatar.cpp` (marked): `idleUpdateWindEffect` asks the region's wind about a region position, not an agent one.
  - `indra/newview/CMakeLists.txt` (unmarked): `skatmowindslab` / `skwindslab` sources and headers, UNIT test `skatmowindslab`.
- `llflexibleobject` and `llviewerpartsim` were not edited: they read wind through `LLWind`.

### atmo/debug (merge `93271885c5`)

Atmo debug floater with info views drawn over the world, and a signal graph floater. Also merged atmo/surface-2 and atmo/storm-anvil in (see conflict below).

- Skooma files: new `skatmoinfolegend.*`, `skatmoinfoview.*`, `skatmoinfoviews.*`, `skatmoinfoviews{air,ground,sky,storm}.cpp`, `skatmosignalgraph.*`, `skfloateratmodebug.*`, `skfloateratmosignals.*`, `floater_sk_atmo_debug.xml`, `floater_sk_atmo_signals.xml`; changed `skprecip.*`, `sksurface.*`; tests `skatmoinfolegend_test.cpp`, `skatmosignalgraph_test.cpp`, `indra/llui/tests/skatmoxui_test.cpp`.
- Upstream files:
  - `indra/newview/pipeline.cpp` (marked): include `skatmoinfoview.h`; `SKAtmoDebug::drawOverlay()` in `renderFinalize` after tonemapping.
  - `indra/newview/llviewerfloaterreg.cpp` (marked): includes and registration of `sk_atmo_debug` and `sk_atmo_signals`.
  - `indra/newview/skins/default/xui/en/menu_viewer.xml` (XML comment marker): **Atmo** submenu (Atmo Debug, Atmo Signals) in the World menu.
  - `indra/newview/CMakeLists.txt` (unmarked): the info view, signal graph and two floater sources/headers, UNIT tests `skatmoinfolegend`, `skatmosignalgraph`.
  - `indra/newview/app_settings/settings_alchemy.xml` (unmarked): `SKAtmoDebugView`.
- Merge resolution (surface-2 into debug): `sksurface.h` conflicted between surface-2's additions and debug's; both sides kept.

### atmo/events-render (merge `7065cbdfad`)

Atmo events (lightning, tornado funnels and the like) drawn inside the cloud march, plus ground-side event effects and an events panel.

- Skooma files: new `skatmoeventdraw.*`, `skatmoeventground.*`, `skEventDrawF.glsl`, `panel_sk_atmo_events.xml`; changed `skCloudMarchF.glsl`, `skCloudMarchUtilF.glsl`, `skatmoprecip.*`, `skatmoprovider.cpp`, `skdrawpoolclouds.*`, `skfloateratmo.*`, `skprecip.cpp`, `skscreenfx.cpp`, `sksurface.cpp`, `skwindslab.cpp`; tests `skatmoeventdraw_test.cpp` + `_golden.inc`, `skatmoeventground_test.cpp` + `_golden.inc`, `skeventdraw_gl_test.cpp` (new), `skcloudmarch_gl_test.cpp`, `indra/llui/tests/skatmoxui_test.cpp`.
- Upstream files:
  - `indra/newview/llviewershadermgr.cpp` (marked): `skEventDrawF.glsl` linked into the shadow bake, ground shadow bake, march, curved march and interior march programs.
  - `indra/newview/CMakeLists.txt` (unmarked): event draw/ground sources and headers, UNIT tests `skatmoeventdraw`, `skatmoeventground`, GL test `skeventdraw_gl` (`SK_CLOUD_SHADER_DIR`).

### atmo/interior-probes (merge `2ae46f2ab6`)

Reflection probe captures see an interior's air, and the flat cloud proxy is hazed and lit by it inside one.

- Skooma files: changed `skinteriorair.*`, `skatmointerior.*`, `skatmocloudshell.*`, `skcloudshell.cpp`, `skdrawpoolclouds.cpp`, `skskyfarlayer.cpp`, `skskybodies.cpp`, shaders `skCloudSheetF.glsl`, `skCloudShellUtilF.glsl`; tests `skatmointerior_test.cpp`, `skatmointerior_shine2.inc`, `_shine3.inc`, `skatmocloudshell_test.cpp`, `skatmocloudshell_chart.inc`, `skcloudpuff_gl_test.cpp`, `skinteriorair_gl_test.cpp`, `skskyshell_gl_test.cpp`.
- Upstream files:
  - `indra/newview/llviewershadermgr.h` / `.cpp` (marked): `gSKCloudSheetInteriorProgram` declared, defined, unloaded in `loadShadersDeferred` and built there with `SK_INTERIOR_AIR`.
  - `indra/newview/CMakeLists.txt` (unmarked): the interior model added to the `skcloudpuff_gl` test sources.
  - `indra/newview/app_settings/settings_alchemy.xml` (unmarked): `SKInteriorAirProbes`.
- `llreflectionmapmanager` was not edited: the probe hooks sit in Skooma files.
- Merge resolution (`01098353ed`, storm-anvil into interior-probes): `skCloudShellUtilF.glsl` `skShellStorm` keeps storm-anvil's rain/overshoot slab with interior-probes' chart signature, `tmin = 0` inside interiors, and the curtain/streak lookups through `skShellChart`; `skcloudshell.cpp` `paramRange` carries the `stormTop` extent.

### atmo/notation-sweep (merge `ebb33ef76d`)

A sweep over every registered module's notation, so the inventory is the provider's table.

- Skooma files: new `skatmonotationsweep.*`; changed `skatmodocument.cpp`, `skatmoeditor.cpp`, `skatmoimport.cpp`, `skatmoorbital.cpp`, `skatmoregistry.cpp`, `skatmosurround.cpp`, `skatmotypes.*`; tests `skatmonotationsweep_test.cpp` + `_landed.inc` (new), `skatmoimport_test.cpp`.
- Upstream files:
  - `indra/newview/CMakeLists.txt` (marked `<SK:Nexii>` block): `skatmonotationsweep` sources/headers and UNIT test `skatmonotationsweep`.

## Mini-map — SKNetMap (combat/minimap)

Landed on `develop` in two merges of the same branch: `b7f8571b51` (as `feature-nav/minimap`, round 1, 2026-10-05) and `5fcda1f319` (renamed `feature-combat/minimap`, round 2, 2026-10-07). Base `fix-nav/slope-grid`. Replaces the classic mini-map: navmesh floorplan, hover nametags, two-pane context menu, vehicles, and combat mode (damage parcel, SLMC ally factions, exact Moderate gate). Upstream edits are marked `<SK:Nexii>` unless stated.

### Files we own

- `sknetmap.*`, `skfloatermap.*`, `skfloatermapsettings.*`, `skmap{allies,allyresolver,geom,labels,markers,menu,objects,palette,parcelfacts,parcels,people,policy,raster,ribbons,scene,view}.*`, `sknavpolys.*`, `skpopuplayout.*`, `skpopuppanes.*`; changed `sknavmesh.*` (poly reads, `revision()`, demand switch).
- XUI: `floater_sk_map.xml`, `floater_sk_map_settings.xml`, `panel_sk_map_popup.xml`. Data: `app_settings/sk_slmc_factions.xml` (SLMC factions; land groups in, civilian groups out).
- Tests: `skmap{allies,geom,labels,markers,policy,raster,ribbons,view}_test.cpp`, `sknavpolys_test.cpp`, `skpopuplayout_test.cpp`.

### Injection sites

- `indra/newview/llviewerfloaterreg.cpp` (marked): `mini_map` now builds `SKFloaterMap` from `floater_sk_map.xml` (the classic line is kept commented); registers `sk_map_settings`; two includes.
- `indra/newview/llviewerparcelmgr.cpp` (marked): `processParcelProperties` hands replies carrying the map's own sequence id to `SKMapParcelFacts::handleParcelProperties` and returns, so the Moderate gate never reads the shared hover slot; one include.
- `indra/newview/alavataractions.h` / `.cpp` (marked): `ALAvatarActions::parcelEjectWithBan`, eject using the parcel facts the gate already holds instead of `selectParcelAt`, so Eject no longer replaces the user's parcel selection.
- `indra/newview/alderenderlist.h` / `.cpp` (marked): `ALDerenderList::addAvatar` and an `ALDerenderObject` constructor, derendering an avatar from the person menu without touching the selection.
- `indra/newview/llviewerobjectlist.h` (marked): `getMapObjects()` accessor for the map's object fallback layer.

### Unmarked additions

- `indra/newview/CMakeLists.txt`: the files above, UNIT tests `skmap*`, `sknavpolys`, `skpopuplayout`.
- `indra/newview/app_settings/settings_alchemy.xml`: the `SKMap*` keys in one block.
- `indra/newview/CMakeLists.txt` (marked, `combat/minimap-air-people`): `skmapmarkers.cpp` and `tests/skmapmarkers_test.cpp` in `newview_tests`, the TUT test ported to Catch2.
- `indra/newview/app_settings/settings_alchemy.xml` (marked, `combat/minimap-air-people`): `SKMapFloorChevronBand1`..`5`, the graded floor chevron band edges (2.5/6/12/24/48 m).
- `llnetmap.*` and `menu_mini_map.xml` stay in use by the People panel's Nearby map; `LLFloaterMap` / `floater_map.xml` are no longer registered but remain in the tree.

### Follow-ups: combat/minimap-camera and combat/minimap-section (2026-10-07)

- `combat/minimap-camera` (merges `8318d6d690` .. `645bf8b776`): the map follows the camera, bounded analysis box, tile carry-over, in-place re-bakes, 2048 m zoom, screen-constant ribbon widths, the agent floor ring. Skooma files only, plus `settings_alchemy.xml` (unmarked): `SKMapRotate` removed, `SKMapZoomMeters` comment.
- `combat/minimap-section` (merges `a2ca48a83e`, `b58cc13680`): walls drawn as a section cut through the census geometry (new `skmapsection.*`, `skmapsection_test.cpp`). Upstream files, unmarked registrations only: `indra/newview/CMakeLists.txt` (the `skmapsection` sources and its UNIT test), `indra/newview/app_settings/settings_alchemy.xml` (`SKMapSection`, since removed, `b5c7042d6c`).
- `combat/minimap-drop-floater` (merge `da6b00f0ba`): the classic floater retired. Deleted `llfloatermap.*` and every `floater_map.xml`. Marked single lines: `indra/newview/CMakeLists.txt` (`llfloatermap.cpp`/`.h` commented out), `llnetmap.h` (the `LLFloaterMap` forward declaration and friend), and the `llfloatermap.h` include in `llstartup.cpp`, `llviewerfloaterreg.cpp`, `llviewermenufile.cpp`, `llviewerwindow.cpp`, `llworldmapview.cpp`.
- `combat/minimap-drop-classic` (merge `46f6a015dc`): the People panel map and `LLNetMap` retired (the map shows allies only; the Nearby list still lists everyone). Deleted `llnetmap.*` and every `menu_mini_map.xml`. Marked: `CMakeLists.txt` (`llnetmap.cpp`/`.h`), `llpanelpeople.cpp/.h` (include, `mMiniMap`, its setup and `setSelected`), `llviewerobjectlist.cpp/.h` (include, `renderObjectsForMap` under `#if 0`), the `llnetmap.h` include in `lltoolgun.cpp`, the `LLNetMap` forward declaration in `llviewerregion.h` and `llworld.h`, and XML comments in `en/panel_people.xml` (Net Map Panel) and `en/menu_people_nearby_view.xml` (View Map). Unmarked: `view_map` removed from the ten localised `menu_people_nearby_view.xml`, the `net_map` row from `xui_notes.xml`. `rlvui.cpp`'s `findChild("Net Map Panel")` now finds nothing and is left alone; `NearbyListShowMap` stays in `settings.xml`.
- `combat/minimap-section-perf` (merge `5f0827fa14`): faster section cut (`skmapsection.*`, `skmapgeom.cpp`, new `skmapsection_bench.cpp`). Upstream file, unmarked registration only: `indra/newview/CMakeLists.txt` (the bench source and test).
- `combat/ml-map` (merge `5d0bae0a2d`, 2026-10-13): the Mouselook map (Automap), the mini-map drawn faintly over the world in mouselook through the same `SKNetMap`/composite (new `skmapml.*`; Automap tab in `floater_sk_map_settings.xml`). `indra/newview/llviewerwindow.cpp` (marked): include, `SKMapML::draw()` before the tool draw (skipped for tiled snapshots), `SKMapML::cleanup()` in `shutdownGL()` and `stopGL()`. `settings_alchemy.xml`: the `SKMapML*` keys in a marked block. `CMakeLists.txt`: `skmapml.cpp`/`.h` inside the existing marked block.


### Navmesh: feature-nav/solid-raster (B2, 2026-10-08)

- `feature-nav/solid-raster` (merge `4fbd3c5d1a`): WEATHER dropped as a Detour class, one solid raster. `indra/newview/app_settings/settings_alchemy.xml`: added `SKNavBuildRaster`, `SKNavOverlayRunoff` (marked `<SK:Nexii>` block); removed `SKNavBuildWeather`, `SKNavWeatherSlope`, `SKNavWeatherHeight`, `SKNavWeatherRadius`, `SKNavOverlayClass` (unmarked removals). All other edits are in `indra/sknavmesh` and `sk*` files.
- `fix-nav/airborne-clip`, `fix-nav/cut-perf`, `feature-nav/air-morton`, `feature-nav/space-polys-baseline`, `fix-nav/test-numbers` (2026-10-08): Skooma files only, no injections.


### Outline capture: feature-skoutline/capture (2026-10-11)

- `spike-skoutline/rtt-spike` was retired from develop (reverts `351f866b56`..`57f30a968a`); its injections are gone.
- `feature-skoutline/capture` (merge `694c68a28f`): vehicle outline glyphs rendered as GPU coverage into an atlas (`.outline-capture-buildout.md`, "Direction"). Own files `skoutlinecapture.*`, `skoutlinepolicy.*` (+ `tests/skoutlinepolicy_test.cpp`), `skfloateroutlinecapture.*`, `floater_sk_outline_capture.xml`, `shaders/class1/deferred/skOutlineResolveF.glsl`. Marked: `pipeline.h`/`.cpp` (`skRenderOutlineCoverage()` wrapping `renderShadow` with scoped save/restore; `sSKCoverageRender` turns face culling off and lowers the alpha-blend `minimum_alpha` during coverage; `SKOutlineCapture::cleanupGL()` at the end of `releaseGLBuffers`), `llviewercamera.h` (`CAMERA_SK_COVERAGE` after `CAMERA_WATER0`, so it never sets any-camera visibility in `llvieweroctree.cpp`), `llviewerdisplay.cpp` (`SKOutlineCapture::onFrame()` after the world, before the UI, not for snapshots), `llviewerfloaterreg.cpp` (`sk_outline_capture`), `shadowAlphaMaskF.glsl` / `pbrShadowAlphaBlendF.glsl` (`sk_coverage_cutoff` uniform replaces the stripe dither and 0.05 floor during coverage), `CMakeLists.txt` (sources, shader, `al_add_test` in an `<SK:Nexii>` block), `settings_alchemy.xml` (ten `SKOutlineCapture*` at the top of the map), `en/menu_viewer.xml` (Develop > Render Tests > Outline Capture Runtime, XML comment marker).

- `feature-skoutline/capture` crew layers (develop `e42d4ef0ea`): marked `llselectmgr.cpp` (include; `SKOutlineCapture::onObjectName(id, name)` in `processObjectPropertiesFamily`, so crew-attachment names feed the layer rule), `llspatialpartition.h` / `.cpp` (`<functional>` include; `LLCullResult::skEraseDrawInfos`, non-virtual, layout unchanged), `pipeline.h` / `.cpp` (`sk_coverage_skip` static and a filter after `stateSort` in `renderShadow` that drops thin-plane draw infos during coverage; `skRenderOutlineCoverage` takes the skip predicate, defaulted). Own files: `skoutlinepolicy.*` crew/plane/merge rules, `SKOutlineCaptureDumpFolder` in `settings_alchemy.xml`, `sk_count` in `skOutlineResolveF.glsl`.
- `feature-combat/minimap-glyphs` (into `feature-combat/minimap`, develop `7736e092dc`): the map draws captured vehicle glyphs. Only `sk*` files plus `settings_alchemy.xml` (`SKMapVehicleGlyphs` beside `SKMapLayerVehicles`).

### Navmesh: feature-nav/tile-stages (B3)

- `feature-nav/tile-stages` (B3, merge `007a0fbd37`): `indra/newview/app_settings/settings_alchemy.xml` — the duplicate `SKNavAirBudgetMB` removed (unmarked deletion), the remaining entry wrapped in a `<SK:Nexii>` comment block. Everything else in `indra/sknavmesh` and `sk*` files.
- `feature-nav/space-stamp` (B4, via `feature-navmesh`, develop `b10cecd4ce`): the Recast overlay port (`portfile.cmake`, `vcpkg.json`) gains `DT_VIRTUAL_QUERYFILTER` and a port-version bump; ours but unprefixed and unmarked. Otherwise `indra/sknavmesh` and `sk*` files only.
- `fix-nav/stamp-perf` (via `feature-navmesh`, develop `7065397133`): `indra/cmake/BootstrapVcpkg.cmake` — one `<SK:Nexii>` block plus a marked single line: the install stamp hashes every overlay port file.
- `feature-nav/air-overlay` (via `feature-navmesh`, develop `8fb47460a5`): `indra/newview/app_settings/settings_alchemy.xml` — a `<SK:Nexii>` block with `SKNavOverlayAir`, `SKNavOverlayAirSource`, `SKNavOverlayAirSlice`, `SKNavOverlayAirSliceCamera`, `SKNavOverlayAirBudget` (`Source`/`Slice`/`SliceCamera` since removed, `8ebc9cb749`); `indra/newview/CMakeLists.txt` — `sknavairoverlay.cpp` added (unmarked, like its sknav neighbours).
- `feature-nav/air-residency` (via `feature-navmesh`, develop `e6f765de14`): `indra/newview/app_settings/settings_alchemy.xml` — a `<SK:Nexii>` block with `SKNavAirSpill`, `SKNavAirDetailHoldSeconds`, `SKNavAirWholeCluster`.
- `fix-nav/review-stamp` (via `feature-navmesh`): `indra/newview/CMakeLists.txt` — trailing `# <SK:Nexii> navmesh: …` markers added to the existing `skcaptureexport.cpp/.h`, `skclusterprobes.cpp/.h`, `sknavairoverlay.cpp` entries (previously unmarked).
- `feature-combat/edge-chevrons` (via `feature-combat/minimap`, develop `9620ed08f6`): `indra/newview/app_settings/settings_alchemy.xml` — `SKMapEdgeChevrons` added unmarked next to the other `SKMap*` entries.
- `feature-combat/map-recorder` (via `feature-combat/minimap`, develop `9620ed08f6`): `indra/newview/app_settings/settings_alchemy.xml` — a `<SK:Nexii>` block with `SKMapRecord`, `SKMapRecordPath`, `SKMapRecordMaxMB`; `indra/newview/CMakeLists.txt` — `skmaprecorder.cpp/.h` added (unmarked, like its skmap neighbours).
- `feature-combat/minimap-capture` (via `feature-combat/minimap`, develop `a98488462d`): tactical overview and census-styled capture base. Own files `skoverviewcapture.*`, `skoverviewcensus.*`, `shaders/class1/skooma/overview*.glsl`. Marked: `pipeline.h` / `.cpp` (`skRenderOverview` on `gClipProgram`: terrain, ghost, census and section passes), `llviewerdisplay.cpp` (per-frame overview hook after the world), `llviewerwindow.cpp` (wheel sets `SKTacticalDistance` while the overview is on), `llviewerinput.cpp` (`tactical_overview` action, hold / tap-to-latch), `key_bindings.xml` (CapsLock in first and third person), `control_table_contents_camera.xml` (Tactical Overview row), `en/menu_viewer.xml` (Develop > Render Tests > Tactical Overview), `llwindow.h` (no-op virtual `skSetCapsLockCapture`), `llwindowwin32.h` / `.cpp` (`WH_KEYBOARD_LL` hook that swallows Caps Lock while the world has focus and forwards it), `llkeyboardwin32.cpp` (Caps Lock level follows the physical key while held), `CMakeLists.txt` (inside the marked map block), `settings_alchemy.xml` (`SKMapCaptureBase`, `SKOverview*`, `SKTactical*`).
- `feature-camera/zoom-to-cursor` (develop `5a84c29126`): alt-cam wheel zoom about the cursor. Marked: `llagentcamera.h` / `.cpp` (`cameraZoomToCursor`, called from `handleScrollWheel` before the classic alt-cam zoom), `llviewerwindow.h` / `.cpp` (`pickImmediate` takes `pick_focus`, from `feature-altcam-click-distance`, merged in), `skins/default/xui/en/panel_preferences_move_general.xml` (checkbox under Clicking on your avatar). Unmarked: `settings_alchemy.xml` (`SKCameraZoomToCursor`, after `ClickingAvatarKeepsCamera`).
- `feature-input/extended-fkeys` (develop `026c5fafc3`): F13–F24 bindable. Marked: `indra_constants.h` (`KEY_F13`..`KEY_F24` at 0xAD–0xB8), `llkeyboard.cpp` (names), `llkeyboardwin32.cpp` (`VK_F13`..`VK_F24`), `llkeyboardsdl.cpp` (SDL map; WindowsVK F16–F24), `llviewerinput.cpp` (no auto-repeat; remap range); unmarked `en/strings.xml` (F13–F24 key names). macOS reports up to F20 only.
- `fix-combat/minimap-tactical-live` (via `feature-combat/minimap`, develop `15f210105d`): `llviewerdisplay.cpp` gains a marked `SKOverviewCapture::onComposite()` call right after `renderFinalize()` in `render_ui()`; `pipeline.cpp` (`skRenderOverview` cut plane built from the view rotation/translation, marked block).
- `feature-combat/ots` bindings (develop `cbc2d95e66`): OTS rows moved from `control_table_contents_movement.xml` to `control_table_contents_camera.xml` (markers kept); marked `llfloaterpreference.cpp` (camera table listed in First Person) and `llkeyconflict.cpp` (camera actions no longer locked in first person; unbound by default).
- `fix-combat/minimap-tactical-markers` (via `feature-combat/minimap`, develop `9e33b28112`): marked `llviewerdisplay.cpp` line skipping `LLHUDObject::renderAll()` (name tags, hover text, chat bubbles) on frames the tactical overview is drawn. Otherwise `sk*` files.
- `fix-combat/minimap-tactical-menus` (via `feature-combat/minimap`): marked `llviewerwindow.cpp` in `handleAnyMouseClick` — a right-click the UI declines goes to `SKOverviewCapture::handleRightClick` before the tool manager. Otherwise `sk*` files.
- `fix-combat/minimap-tactical-noavatars` (via `feature-combat/minimap`): marked `pipeline.cpp` — `skRenderOverview` clears `RENDER_TYPE_AVATAR`; file-static `sSkOverviewCull` set around its cull/stateSort; `markVisible` skips attachment bridges while it is set. Animesh kept.
- `fix-combat/minimap-tactical-invisible` (via `feature-combat/minimap`): marked `pipeline.h` / `.cpp` — `skRenderOverview` takes the overview viewpoints and skips ghost draw infos of octree groups spanning > 64 m on two axes around a viewpoint (enclosures). Otherwise `sk*` files.
- `fix-combat/minimap-tactical-enclosure` (via `feature-combat/minimap`): removes the enclosure group filter and viewpoint plumbing from `pipeline.h` / `.cpp` that `fix-combat/minimap-tactical-invisible` added; that row is superseded.
- `fix-combat/minimap-tactical-ghostinvis` (via `feature-combat/minimap`): marked `pipeline.cpp` — every ghost pass of `skRenderOverview` skips draw infos whose texture is `TEXTURE_TRANSPARENT` or an invisiprim texture (`skoverview::transparentTexture`), logged once per texture.
- `fix-combat/minimap-tactical-ghostinvis` row superseded once `fix-combat/minimap-tactical-fps` lands (ghost layer removed).
- `fix-combat/minimap-tactical-fps` (via `feature-combat/minimap`): ghost passes removed from `skRenderOverview` (`pipeline.cpp`); marked gate in `llviewerdisplay.cpp` / `pipeline.cpp` skipping the main view cull, hero probes, sun shadows, impostors, geometry, lighting and post while the tactical overview covered the view last frame.
- `fix-combat/minimap-tactical-fps2` (via `feature-combat/minimap`): marked line in `llappviewer.cpp` skipping `LLReflectionMapManager::update()` while the tactical overview covered the view. Crease and cut-floor fixes: `sk*` files only.
- `fix-nav/census-without-navmesh` (via `feature-navmesh`, develop `c45751dfdb`): `settings_alchemy.xml` — `SKWorldCensusEnabled` (on) and `SKAirCellsEnabled` (on) added, `SKNavMeshEnabled` default off; marked `pipeline.cpp` census overlay no longer waits for `SKNavMesh::overlayActive()`.
- `fix-combat/minimap-capture-default` (develop `e194a1d0bb`): `SKMapCaptureBase` default on (unmarked value change in `settings_alchemy.xml`); the setting was later removed (`dd779ed388`, base is capture-only).
- `feature-combat/minimap-avatar-shapes` (via `feature-combat/minimap`): unmarked `indra/newview/CMakeLists.txt` lines adding `skcensusavatarshape.cpp` / `.h`. Avatar physics shapes in the tactical overview: `sk*` files only.
- `feature-mesh/havok-block` (develop `a1169f4cef`), physics_havok block, all marked `<SK:Nexii> physics_havok block`: `llprimitive/CMakeLists.txt` (`skmeshhavok.*`, its test); `newview/CMakeLists.txt` (`skmeshhavokshape.*`, beside `llmeshrepository`); `llmeshrepository.h` (include, `LLMeshHeader` physics_havok offset/size/in-cache and `FLAG_PHYSHAVOK`, repo-thread havok queues and fetch/received/failed, `LLMeshRepository::getPhysicsHavok` and its cache); `llmeshrepository.cpp` (include, thread-table rows, `SKMeshPhysicsHavokHandler`, request loop in `run()`, header in-cache detection, cache reservation, both `notifyLoadedMeshes` handoffs, `LLMeshHandlerBase::mTakeRequestedOnly` capped copy, `readCachedPhysicsMesh`); `llspatialpartition.cpp` (`renderPhysicsShape` draws the simulator's triangles for a decomposition without hulls). Own files: `llprimitive/skmeshhavok.*`, `llprimitive/tests/skmeshhavok_test.cpp`, `newview/skmeshhavokshape.*`.
- `spike-mesh/dump` (DO NOT LAND, develop `229d6899c9`, re-cut on `feature-mesh/havok-block`): marked whole-asset dump in `llmeshrepository.h/.cpp` (`mFullDumpQ`, `issueFullDumps`, `SKMeshFullDumpHandler`, queue push in `headerReceived`). Retired 2026-10-16 (revert `5e4e272a7b`): no longer in develop.
- Build fixes after the `next` merge `690ee90ccc` (develop, 2026-10-15): marked TUT guards in `llmath`, `llui`, `newview` and `llaudio` `CMakeLists.txt` (`if(COMMAND al_add_test)`, develop's TUT tests registered but not built until ported to Catch2); marked `setCanCollapse` in upstream's `llui/tests/alstudiofloater_test.cpp`; `skNudge` in `llviewerobject.cpp` (inside its existing motion block) passes `MOVED` only, as upstream dropped `SILHOUETTE`; develop's `ALNoiseHash` cases in `newview/tests/noise_test.cpp` ported to Catch2 (unmarked, develop-only test cases in upstream's file); merge-resolution placements of the atmo/surround outline, motion recorder, tactical-overview and overlay injections in `llselectmgr.cpp`, `llappviewer.cpp`, `llviewerdisplay.cpp` (see `V:\Scratch\mesh-havok-block\SYNC-REPORT.md`).
- `feature-nav/havok-census` (via `feature-navmesh`, develop `44ee57a6cb`): census uses the simulator's collision triangles (provenance `SIM`, census format v5): `sk*` files only, plus `sknavmesh/CMakeLists.txt` registering the `sknavmesh_tests` Catch2 binary (census io test). The trunk's reconciliation with develop's TUT guards (`3d6968f436`) leaves the nav-only map tests (`skmapgeom`, `skmapraster`, `skmapribbons`) in their own marked guard after `newview_tests`.
- `feature-combat/minimap-avatar-shapes` (via `feature-combat/minimap`): marked `llviewerobject.h` / `.cpp` — `LLViewerObject::skPredictedRotationRegion()`, the sim's last rotation extrapolated by its angular velocity, read by the minimap heading and the tactical overview's avatar shapes.
- `feature-modal-view` (develop `a639a27c1d`): the modal view foundation (`SKModalView`, `SKModalMode`), marked `<SK:Nexii> modal view`; it took over the world map's hooks (first landed in `1db5319883`/`bc49267e7f`), same sites, every gate `SKModalView::worldCovered()`. `newview/CMakeLists.txt` adds `skmodalview`, `skmodalviewmath` and the `skmodalviewmath` test; `llagentcamera.cpp`/`.h` lift camera constraints, refuse mouselook and add `skipCameraSmoothing()` (`isDisableCameraConstraints`, `getCameraMaxZoomDistance`, `changeCameraToMouselook`); `llagent.cpp` refuses movement and autopilot (`moveAt`/`moveAtNudge`/`moveLeft`/`moveLeftNudge`/`moveUp`/`moveYaw`/`movePitch`, `startAutoPilotGlobal`, `startFollowPilot`); `llappviewer.cpp` calls `SKModalView::shutdown` in `cleanup` and skips probe faces (one condition with the tactical overview's `worldHidden()`); `lltoolfocus.cpp` `pickCallback` → `SKModalView::pickGround` (top mode); `llviewerdisplay.cpp` adds `sk_display_modal`/`sSKModalFrame` (world object work kept) and the `display()` gate through `closeIfNeeded(leaving_world)`, and skips finalize, HUD, HUD attachments, hover text and 3D UI in a modal frame (finalize and hover text share their conditions with the tactical overview's `worldHidden()`/`tacticalShown()`; its composite is skipped in a modal frame); `llviewerinput.cpp` `scanKey`/`scanMouse` → `SKModalView::blocksAction`; `llviewerjoystick.cpp` `moveAvatar` returns, `moveFlycam` × `SKModalView::flycamScale`; `llviewermessage.cpp` `send_agent_update` sends `SKModalView::worldCameraForUpdate` while covered, otherwise the over-the-shoulder camera (one shared `camera_left`/`camera_up`); `llvieweraudio.cpp` `audio_update_listener` and `llvoicewebrtc.cpp` ear location use `SKModalView::worldCamera`; `llviewerwindow.cpp` `rawSnapshot`/`simpleSnapshot` refuse while covered; `llviewermenu.cpp` `handle_reset_view` (Escape) → `SKModalView::closeTop()`.
- `feature-worldmap` (develop `562f6607dc`, first `1db5319883`/`bc49267e7f`): 3D world map mode, an `SKModalMode` on the foundation above, marked `<SK:Nexii> 3D world map`. `newview/CMakeLists.txt` adds `skmapmode`, `skworldmapmath`, `skworldmapscene`, `skworldmaptiles` and the `skworldmapmath` test; `llviewermenu.cpp` (`skmapmode.h` include, `WorldMap3D.Toggle`/`IsOpen`) and `menu_viewer.xml` add the World menu toggle (Ctrl+Alt+M); new `panel_sk_world_map.xml`; RLVa checks in `skmapmode.cpp` only (no upstream edit).
- `fix-combat/minimap-tactical-altcam` (via `feature-combat/minimap`, develop `bb0e0b0019`): marked lines at the top of `LLToolCamera::handleMouseDown`/`handleHover`/`handleMouseUp` (`lltoolfocus.cpp`) route alt-cam input to the tactical overview while shown; marked call in `LLAgentCamera::resetView` (`llagentcamera.cpp`) resets the tactical camera. Coexists with `feature-worldmap`'s hooks (merged via `fix-combat/minimap-worldmap-merge`; tactical first).
- Same landing: `indra/newview/CMakeLists.txt` conflict resolved to develop's lists; the TUT `al_add_test(skoutlinepolicy …)` line removed in favour of its Catch2 entry in `newview_tests`.
- `fix-combat/minimap-tactical-altcam2` (develop `720e31e164`): the earlier tactical hooks in `LLToolCamera` are replaced by marked wrappers around its five `gAgentCamera` orbit/zoom/pan calls (`lltoolfocus.cpp`) that redirect to the tactical camera while the overview is shown.
- `fix-combat/minimap-capture-percut` (develop `451adeb7ae`): `SKMapCoverAhead` removed from `settings_alchemy.xml` (with its map-settings checkbox); `SKNavMesh::coverAt` removed. Otherwise `sk*` files.
- `fix-combat/minimap-tactical-skybox` (develop `8d3b99efbc`): `SKNavOverlayClusters` added to `settings_alchemy.xml`; nav console Clusters tab. Otherwise `sk*` files.
- `combat/minimap-tacmap-camera` (via `feature-combat/minimap`, develop `e12a6df3b1`): the overview's camera snaps rather than eases, and gains a battlefield mode (`SKTacticalCameraMode`, default 0; `SKTacticalBattlefieldNeighbours`). Own files `skfloatertacticaloptions.*`, `floater_sk_tactical_options.xml`. Marked: `llviewerfloaterreg.cpp` (include and `sk_tactical_options`, beside `sk_map_settings`), `en/menu_viewer.xml` (Tactical Overview moved from Develop > Render Tests into the Combat menu, with Tactical Overview Options... and Mini-map Options... opening `sk_map_settings`; the Combat block was merged by hand with the OTS items), `CMakeLists.txt` (sources beside `skfloatermapsettings`), `settings_alchemy.xml` (two `SKTactical*` in the marked block).
- `combat/minimap-tacmap-nametags` (via `feature-combat/minimap`, develop `bf4928936c`): close avatars on the tactical overview carry their in-world name tag instead of the marker label (`SKTacticalNametagPixelsPerMetre`, default 8). Marked: `llhudnametag.h` / `.cpp` (`drawFlat`, the tag drawn flat in UI pixels), `llvoavatar.cpp` (include; while `SKOverviewCapture::tacticalShown()`, the own tag ignores `RenderNameShowSelf` and the apparent-size fade is skipped), `settings_alchemy.xml` (`SKTacticalNametagPixelsPerMetre`, unmarked, beside the other `SKTactical*`).
- `combat/minimap-projectiles` (via `feature-combat/minimap`, develop `15680f761a`): projectiles gathered every frame from the whole object list (the map objects never held bullets) and drawn as fading streaks on the mini-map and tactical overview, plus speed-scaled trails for anything over 80 m/s on the overview. Only `sk*` files plus `settings_alchemy.xml`: `SKMapLayerProjectiles` now defaults on, and `SKMapProjectile*`/`SKMapTrail*` beside it in the existing unmarked mini-map run.
- `combat/minimap-tacmap-pan` (via `feature-combat/minimap`, develop `7c72ce9d59`): a left drag pans the tactical overview (grab the point under the cursor, keep it there); every overview pick (alt-click focus, right-click menu point, pan grab) casts the tactical camera's ray against the census as drawn (`SKOverviewCensus::raycast`), then terrain, then the agent's floor. Marked: `llviewerwindow.cpp` (`SKOverviewCapture::handleLeftClick` beside the existing right-click hook in `handleAnyMouseClick`).
- `feature-combat/minimap-dissolve-cut` (via `feature-combat/minimap`, develop `9c6de7ec4e`): `indra/newview/app_settings/settings_alchemy.xml` — `SKOverviewMaxAbove`, `SKTacticalDissolve`, `SKTacticalDissolveRadius`, `SKTacticalDissolveSoftness` added unmarked before `SKTacticalPitch`. Otherwise Skooma files only.
- `combat/minimap-cleanup` (via `feature-combat/minimap`, develop `e99db575b9`): mini-map base is capture-only. Marked removals: `llviewerobjectlist.cpp/.h` (`renderObjectsForMap`), `llviewerregion.cpp/.h` and `llviewerparceloverlay.cpp/.h` (`renderPropertyLinesOnMinimap`, `grid_2d_part_lines`). Unmarked: `settings.xml` (removed `MiniMapPrimMaxRadius`, `ShowMiniMapButton`, `MiniMapScale/Rotate/AutoCenter/ShowPropertyLines`), `settings_alchemy.xml` (removed classic `AlchemyMiniMap*`/`AlchemyMinimap*` and dead `SKMap*`; added layer, floorplan and region-view settings), `CMakeLists.txt` (removed `skmapcomposite.*`, `skmapobjects.*`).
- `combat/minimap-pan-fix` (via `feature-combat/minimap`, develop `327a1aa272`): the tactical pan's press no longer reads the per-frame composite flag. Only `sk*` files plus `settings_alchemy.xml`: `SKOverviewPanDebug` added unmarked before `SKTacticalPitch`.
- `combat/minimap-map-pan-tool` (via `feature-combat/minimap`, develop `1c2d15b09e`): `SKToolMapPan` (new `sktoolmappan.*`) pans both modal map views; `SKModalMode` gains `beginPan`/`pan`/`endPan`/`passesClicks`. Removed the `SKOverviewCapture::handleLeftClick` hook from `llviewerwindow.cpp` (right-click hook kept). Marked: `lltoolmgr.cpp` (include; `getCurrentTool` swaps the pie for `SKToolMapPan::substitute` while a modal view is on top), `CMakeLists.txt` (`sktoolmappan.cpp`/`.h` beside `skmodalview`).
- `combat/minimap-tacmap-interest` (via `feature-combat/minimap`, develop `1c2d15b09e`): while the tactical overview is shown, AgentUpdate carries its camera, Far is 1.5x the draw distance capped at 1024 m, and the region is put in 360 interest mode (restored on close). Marked: `llviewermessage.cpp` (`SKModalView::tacticalInterestFar` in `send_agent_update`). Unmarked: `settings_alchemy.xml` (`SKTacticalSendCamera`, `SKTacticalInterestFarScale`, `SKTacticalInterestFarCap`).
- `combat/minimap-outdoor-no-cut` (via `feature-combat/minimap`, develop `1c2d15b09e`): Skooma files only, plus unmarked `settings_alchemy.xml` (`SKOverviewCutReleaseSeconds`).
- `combat/minimap-tacmap-zoomout` (develop `fe38239089`): Battlefield tilt, zoomed-out cylinders, coded-person badges, quiet cut log. Only `sk*` files plus `settings_alchemy.xml`: `SKTacticalBattlefieldPitch` (before `SKTacticalPitch`) and `SKTacticalCylinderPixelsPerMetre`, `SKTacticalCylinderMinRadiusPx`, `SKTacticalCylinderMaxRadiusM` (before `SKTacticalNametagPixelsPerMetre`), unmarked.
- `combat/minimap-flat-markers` (develop `7a7f2e5e10`): zoomed-out cylinders replaced by the mini-map's flat markers. `settings_alchemy.xml`: the three `SKTacticalCylinder*` settings become `SKTacticalFlatMarkerPixelsPerMetre` (same place), unmarked.
- `combat/minimap-bullet-size` (develop `94ffb4bde0`): every physical munition up to 10 m (8 m/s+) and needles drawn as projectiles, bullets as streaks, others as footprints. Only `sk*` files plus unmarked `settings_alchemy.xml` with the `SKMapProjectile*` keys: `SKMapProjectileMinSpeed` 20 → 8, `SKMapProjectileMaxSize` replaced by `SKMapProjectileMaxLength`, new `SKMapProjectileBulletSize`, `SKMapProjectileBulletSpeed`, `SKMapProjectileMinFootprintPx`, `SKMapProjectileNeedleLength`, `SKMapProjectileNeedleThickness`.
- `combat/minimap-tacmap-wasd` (via `feature-combat/minimap`, develop `06628731f5`): while the tactical overview is shown, held movement bindings pan the map on the ground along the live camera heading (`SKOverviewCapture::handleKeyPan`/`applyKeyPan`). Marked: `llviewerinput.cpp` (`scanKey`, before the `SKModalView::blocksAction` check). Unmarked: `settings_alchemy.xml` (`SKTacticalKeyPanSpeed`, before `SKTacticalPitch`).
- `combat/minimap-tap-window` (via `feature-combat/minimap`, develop `4f63105d40`): Skooma files only, plus unmarked `settings_alchemy.xml` (`SKTacticalTapSeconds`).
- `combat/minimap-dissolve-gate` (via `feature-combat/minimap`, develop `9dbaca04ba`): Skooma files only, plus unmarked `settings_alchemy.xml` (`SKTacticalDissolveReach`).
- `mesh/physics-shape-types` (develop `7dceda85a7`): Render Metadata > Physics Shapes becomes a Cost / Types submenu (`SKPhysicsShapeTypes`). Marked: `menu_viewer.xml` (en submenu); `llviewermenu.cpp` (`SKTogglePhysicsShapes` / `SKCheckPhysicsShapes` and their registration); `llspatialpartition.cpp` (`renderPhysicsShape` draw colours through `skPhysicsShapeColor`, each line marked `Types colouring`). Unmarked: the other locales' `menu_viewer.xml` (`menu_item_check` -> `menu` for `Physics Shapes`), `settings_alchemy.xml` (`SKPhysicsShapeTypes`, before `AlchemyRenderWaterHextile`). Own: `skmeshhavokshape.*` (`SKPhysicsShapeSource`, `skPhysicsShapeColor`).
- `combat/minimap-objlist-zones` (via `feature-combat/minimap`, develop `6ff3f2e8a9`): profiler sub-zones in `LLViewerObjectList::update`. Marked: `llviewerobjectlist.cpp` (`vol - copy active`, `vol - fetch costs`, `vol - fetch physics flags`, `vol - frame stats`).
- `combat/minimap-physics-flags` (via `feature-combat/minimap`, develop `93c98627a3`): physics flags fetched from each object's own region, with refused objects not queued again (stopped the census re-queuing parts from other regions every frame). Marked: `llviewerobjectlist.cpp` (`sPhysicsFlagsRefused`, `fetchPhysicsFlags`, `fetchPhisicsFlagsCoro`, `updatePhysicsFlags`, `onPhysicsFlagsFetchFailure`), `llviewerobjectlist.h` (`fetchPhisicsFlagsCoro` signature).
- `nav/physics-cost` (via `feature-navmesh`, develop `9341208865`): Physics Shapes > Cost colours by the shape's own estimated physics cost instead of land impact. Marked: `llspatialpartition.cpp` (`renderPhysicsShape` ramp fraction from `skPhysicsShapeCostFraction(skPhysicsShapeCost(...))`; `ObjectCostHighThreshold` no longer read there). Own: `skphysicsshape.*` (`skPhysicsShapeCost`, `skPhysicsShapeCostFraction`).
- `combat/minimap-lookat-aim` (via `feature-combat/minimap`, develop `6297b520dc`): firing lines aim along the mouselook LookAt (else the body rotation), never the animated head; precise ones also drawn on the tactical overview. Marked: `llhudmanager.h` (`getEffects()` read-only getter). Own: `skmappeople.*` (`aimDirection`, `mouselookTargets`, `FiringLine::mPrecise`), `skoverviewcapture.cpp`.
- `spike-physics-cost-sample` (DO NOT LAND, develop `b4fd2b457c`): `llspatialpartition.cpp` `renderPhysicsShape` calls `skPhysicsCostSample`, which queues GetObjectCost and appends estimate vs server physics cost to `logs/sk-physics-cost.csv`. Retired 2026-10-20 (reverts `e0e9637510`, `df1ec61bac`): no longer in develop.
- `spike-prim-cost-sample` (DO NOT LAND, develop `a536aa29be`): `llspatialpartition.cpp` `renderPhysicsShape` calls `skPhysicsPrimCostSample`, which logs PRIM_MESH prims' parameters and per-LOD triangles against GetObjectCost to `logs/sk-prim-cost.csv`. Retired 2026-10-20 (revert `9ae41ed053`): no longer in develop; prims now share the triangle mesh cost.
- `fix-combat/minimap-cam` (via `feature-combat/minimap`, develop `54f1424f27`): camera warp, an animation that eases up, cuts out its cruise and brakes onto the target; the mini-map's Cam frames an avatar from a clear third-person view and warps there, the 3D world map warps in on open, a tapped tactical overview warps in (a hold snaps). Marked: `llagentcamera.h` (`startCameraWarp`, `mCameraWarping`, `mWarpProfile`, `skcamerawarp.h` include), `llagentcamera.cpp` (warp fraction in the `updateCamera` animation ahead of the over-the-shoulder follow, smoothing held off while warping, flag cleared in `startCameraAnimation`, `startCameraWarp`). Own: `skcamerawarp.h`, `skmapmenu.cpp` (`camOnAvatar`), `skmapmode.cpp`, `skoverviewcapture.cpp`.
- `combat/minimap-storey-cut` (via `feature-combat/minimap`, develop `01bb19b90e`): tactical overview storey cut. The roof pass counts any linkset over the feet as cover (a storey slab and its ceiling in separate linksets no longer reset the room); SHELTERED takes the ceiling cut and room; floor + 1.8 m only when the ceiling is unknown; agentFloor never more than 1 m below the feet; whereabouts, agentFloor and roomAt's start take the space-below stand-in only within 1 m (`airGapNear`); a linkset is never-cut only when every record is a void surround (sky build at Resdayn 3497). No markers: own files only, `skoverviewcensus.cpp` (`applyCut` roof pass, anchors), `skoverviewcapture.cpp` (`roomAt`, `updateCut`), `skmapplan.cpp` (`airGapNear`, `whereabouts`, `agentFloor`).
- `nav/small-mesh-convex` (fix, via `feature-navmesh`, develop `afd906745a`): meshes under 0.5 m on any axis collide as hulls. Marked: `llspatialpartition.cpp` `renderPhysicsShape` skips the physics mesh for them (`Small meshes collide as hulls`); the `physics_havok block` line gains `!skPhysicsMeshTooSmallForTriangles(volume)`. Own: `skphysicsshape.*` (`skPhysicsMeshTooSmallForTriangles`, gates in `skPhysicsShapeSimTriangles`, the geometry and the cost).
- `combat/minimap-quick-fixes` (via `feature-combat/minimap`, develop `c178326d57`): verdict timer, census log memory, worker region hash, mover retry, region-handle tactical key, room hash in the cut key, unknown-physics counter. Own files only.
- `combat/minimap-early-z` (via `feature-combat/minimap`, develop `9f32772d13`): overview programs built by the shader manager, clip distances, permutations, dirty-rect tactical repair, GPU spans, census group cull. Marked: `llviewershadermgr.cpp` (include `skoverviewcapture.h`; `SKOverviewCapture::loadShaders(mShaderLevel[SHADER_DEFERRED])` at the end of `loadShadersDeferred`, after the RLVa block; a failure leaves only the overview off), `llvertexbuffer.h` / `.cpp` (`multiDrawArrays`, `glMultiDrawArrays` over ranges of the bound buffer, beside `drawArrays`), `newview/CMakeLists.txt` (`skoverviewprograms.h` in the SK headers list; `tests/skoverviewshaders_test.cpp` and `tests/skoverviewshaders_gl_test.cpp` registrations, each line-marked). Own: `skoverview*`, `shaders/class1/skooma/overview*.glsl`, `tests/skoverviewshaders*_test.cpp`.
- `nav/room-graph` (via `feature-navmesh`, develop `310657b191`): rooms on the world view and the air/wind revision split. Own files only (`sknavroomgraph.*`, its test, `sknavrooms.*`, `sknavmesh.cpp`, `indra/sknavmesh/CMakeLists.txt`); the develop merge also gave the `view.rebuild` and `rooms.publish` recorder lines their air/wind and rooms fields.
- `combat/minimap-escape-warp` (via `feature-combat/minimap`, develop `0e5abf28e5`): Escape warps the camera home when it is over `SKCameraWarp::RESET_DISTANCE` from the avatar. Marked: `llagentcamera.cpp` (`resetView`, around `setFocusOnAvatar`). Own: `skcamerawarp.h` (`RESET_DISTANCE`).
- `combat/minimap-churn` (via `feature-combat/minimap`, develop `8b8fd270b1`): census stamp on a content hash with a delta in the snapshot, cut rooms from the room graph, overview caches on `airRevision`. Own files only (`skworldcensus.*`, `sknavcensusdelta.*`, `sknavcluster.*`, `tests/sknavclusterhash_test.cpp`, `skoverviewcensus.*`, `skoverviewcapture.*`, `indra/sknavmesh/CMakeLists.txt`); `llspatialpartition.cpp` arrived through its `feature-navmesh` merge, already indexed above.
- `combat/minimap-cut-table` (via `feature-combat/minimap`, develop `f2b5a8afeb`): per-linkset cut table for the tactical overview. Marked: `llshadermgr.h` / `.cpp` (reserved uniform `SK_OVERVIEW_CUT_TABLE` `"sk_cut_table"`, last before `END_RESERVED_UNIFORMS`; reserved storage block `SK_OVERVIEW_CUT_BLOCK` `{ "SKOverviewCutBlock", 1 }`, first entry, binding point 0), `altextureslot.h` / `.cpp` (`TT_BUFFER_TEXTURE` → `GL_TEXTURE_BUFFER`), `llgl.h` / `.cpp` (`LLGLManager::mMaxTextureBufferSize`, `GL_MAX_TEXTURE_BUFFER_SIZE` queried beside `GL_MAX_UNIFORM_BLOCK_SIZE`), `newview/CMakeLists.txt` (`skoverviewcuttable.cpp/.h` in the SK lists, unmarked; `tests/skoverviewcuttable_test.cpp` and `tests/skoverviewcuttable_gl_test.cpp` registrations and the two `UNIT` entries, line-marked; the GL binary's `UNIT` list reflowed one file a line by gersemi). Own: `skoverviewcuttable.*`, `skoverviewcensus.*`, `skoverviewcapture.cpp`, `skoverviewprograms.h`, `shaders/class1/skooma/overviewGeomV.glsl`, `tests/skoverview*_test.cpp`. The storage binding points are now full in `llglslshader_gl_test.cpp` (ours + 7 test blocks): the next product block takes one test block out.
- `combat/minimap-cover` (via `feature-combat/minimap`, develop `d43a179343`): verdicts by clipping against colliding neighbours, visible-only census records, census export v6. Own files only (`sknavinvisible.*`, `sknavtypes.h`, `sknavcensusio.*`, `sknavcluster.cpp`, `sknavfeatures.cpp`, `skworldcensus.*`, `skoverviewcensus.*`, `skphysicsshape.*`, `sknavmesh.cpp`, tests).
- `nav/census-incremental` (via `feature-navmesh`, develop `0865ee2601`): census dirty-set scan, rooms change list, live air byte counts, shared tops. Marked: `llviewerobjectlist.cpp` (include `skworldcensus.h`; `SKWorldCensus::noteObjectUpdate` as the last statement of `processUpdateCore`; `SKWorldCensus::noteObjectGone` in `killObject` before `markDead`). Own: `skworldcensus.*`, `sknavmesh.*`, `sknavrooms.*`, `indra/sknavmesh` (`sknavairdistil.*`, `sknavskymap.*`, `sknavlayout.*`, `sknavtilecache.cpp`, tests). The develop merge kept the recorder lines and `bumpRevision` at the ten air-revision sites beside the change-list notes, moved the `tile.drop` recorder line ahead of the retire (it read a moved-from pointer), and widened `airReach()` with the live `requestAir` boxes. Develop-only `145578d488` moved four of the mini-map trunk's `sknavenclosure_test.cpp` cases onto `TileTops`.
- `combat/minimap-linkset-buffers` (via `feature-combat/minimap`, develop `63df43dbd7`): census slot pages per region and class, stable per-root linkset ids, compute-built draw commands. Marked: `llvertexbuffer.h` / `.cpp` (`multiDrawArraysIndirect`, `glMultiDrawArraysIndirect` of an `ALGPUBuffer`'s commands over the bound buffer, range-checked, beside early-z's `multiDrawArrays`), `llrender.h` / `.cpp` (`bindDrawIndirect`, `GL_DRAW_INDIRECT_BUFFER` through the cached indirect binding), `newview/CMakeLists.txt` (`skoverviewslots.cpp/.h` in the SK lists, unmarked; `tests/skoverviewslots_test.cpp` and `tests/skoverviewslots_gl_test.cpp` registrations and the two `UNIT skoverviewslots.cpp` entries, line-marked). Unmarked: `settings_alchemy.xml` (`SKOverviewPageVertices`, U32 3145728, beside the other `SKOverview*`). Own: `skoverviewslots.*`, `skoverviewcensus.*`, `skoverviewcapture.*`, `skoverviewprograms.h`, `shaders/class1/skooma/overviewCommandsC.glsl`, `tests/skoverviewslots*_test.cpp`.
- `combat/minimap-id-readback` (via `feature-combat/minimap`, develop `ed11c4e608`): the tactical cut's room, column, cover and feet answers from GPU probes, picks from the shown frame's depth. Marked: `newview/CMakeLists.txt` (`skoverviewprobes.cpp/.h` in the SK lists, unmarked; `tests/skoverviewprobes_test.cpp` and `tests/skoverviewprobes_gl_test.cpp` registrations and the two `UNIT skoverviewprobes.cpp` entries, line-marked). Unmarked: `settings_alchemy.xml` (`SKOverviewProbeBudget` U32 8, `SKOverviewProbeQueries` U32 256, `SKOverviewProbeDump` Boolean 0 not persisted, beside the other `SKOverview*`). Own: `skoverviewprobes.*`, `skoverviewcensus.*`, `skoverviewcapture.*`, `skoverviewprograms.h`, `shaders/class1/skooma/overviewGeomV.glsl` / `overviewGeomF.glsl`, `tests/skoverviewprobes*_test.cpp`, `tests/skoverviewshaders*_test.cpp`. No llrender edit: `LLVertexBuffer::sGLRenderBuffer` can name a buffer whose attribute pointers were never set up after `_unmapBuffer`, a VBO-pool miss (which binds the new buffer) and a recycled pool name, so `setBuffer`'s same-buffer skip draws from the old pointers; the probes `LLVertexBuffer::unbind()` before drawing, as `LLRender::genBuffer` does. A one-line `_unmapBuffer` fix was tried and the GL test still crashed on the pool-miss path; the proper fix is an llrender follow-up.
- `ui/game-chat` (via `ui`, main `35fb5f2a26`): game-style local chat window replacing the nearby toasts and chat bar while `SkoomaLocalChatModern` is on. Marked: `alchatbar.h` / `.cpp` (send, keystroke and channel-label code as shared statics), `llfloaterimnearbychathandler.cpp` (`processChat` hands the message to `SKFloaterLocalChat` and returns before the toasts), `llviewerinput.cpp`, `llviewerwindow.cpp` (Enter, `/` and letter keys open its input), `llfloaterimnearbychat.h` (`getMessageArchive()` for the backfill), `llviewerfloaterreg.cpp` (registration), `newview/CMakeLists.txt`, `settings_alchemy.xml` (`SkoomaLocalChatModern`, `SkoomaLocalChatStyle`, `SkoomaLocalChatFadeDelay`, `SkoomaLocalChatFadeTime`, before `AlchemyChatHistoryStyle`). Own: `skfloaterlocalchat.*`, `floater_sk_local_chat.xml`, `menu_sk_local_chat.xml`.
- `ui/resize-handle-hover` (via `ui`, main `2f93619111`): a floater that draws no background shows its resize grip only while hovered, focused or resizing. Marked: `llresizehandle.cpp` (one block in `draw`). The grip stays grabbable while hidden. Applies to every background-less floater, upstream ones included.
- `ui/game-chat` round 2 (main `1462603293`): `llfloater.cpp` is untouched; the wheel fix lives in `SKFloaterLocalChat::handleScrollWheel`. New setting `SkoomaLocalChatHoverGrace` beside the other `SkoomaLocalChat*`.
- `ui/game-chat` link previews (main `bc26ebad05`): official SL links only (`skchatunfurl.*` allowlist, `skchatunfurlcache.*`). Marked: `newview/CMakeLists.txt` (`tests/skchatunfurl_test.cpp` after `llvocache_test.cpp`, `skchatunfurl.cpp` UNIT after `llviewercontrollistener.cpp`, line-marked; the SK source/header list entries unmarked), `llviewerinput.cpp` / `llviewerwindow.cpp` now call `SKFloaterLocalChat::shouldTakeChat()` (Conversations open → old path). Unmarked: `settings_alchemy.xml` (`SkoomaLocalChatUnfurl`, beside the other `SkoomaLocalChat*`).
- `render/gl-sync` (via `render`, main `f558e2f1db`): outline capture off raw GL. Marked: `llglslshader.h` / `.cpp` (hashed `uniform1f` and `uniform4iv` setters for Skooma programs whose uniform names are not reserved), `llimagegl.h` / `.cpp` (`LLImageGL::setSwizzle` for any texture; `setTextureSwizzle`'s deprecated-format path goes through it), `pipeline.cpp` (`sk_coverage_cutoff_name` and its `uniform1f` where the shadow alpha programs bind, replacing the shadow pass's own lookups). Own: `skoutlinecapture.cpp` (ALQueryPool, ALQuerySpan, viewport/scissor trackers, `gGL.readPixels`).
- `ui/floater-lock` (via `ui`, main `c35f3cfbb0`): lock toggle for background-less floaters that opt in with `can_lock`. Marked: `llfloater.h` / `.cpp` (`BUTTON_LOCK` last in `EFloaterButton`, `can_lock` param, `isLocked`/`setLocked`, `virtual hitsContentAt`, background click/hover/wheel/tooltip pass-through while locked, no drag/resize while locked; `virtual pickCollapseEdge` so a floater can opt out of edge folding — the local chat does), `llfloaterreg.h` / `.cpp` (`declareLockControl`, per-floater `floater_lock_<name>` setting). Unmarked: `skins/default/xui/en/strings.xml` (`BUTTON_LOCK`, `BUTTON_UNLOCK` beside `BUTTON_HELP`). Layout change to `LLFloater`: full rebuild. Opt-ins: `floater_sk_local_chat.xml`, `floater_sk_map.xml` (via `combat/minimap-lock`, which also keeps main's guarded `al_add_test(skoutlinepolicy …)` block in `newview/CMakeLists.txt`).
- `ui/smooth-scroll` (via `ui`, main `8abf470a67`): precise wheel deltas and glide. Marked: `llscrolldelta.h` (`mPixels`, `mLines`, `mIsPrecise`, `mPage`, `mPhase`, `lines()`), `llwindow.h`, `llwindowwin32.cpp` (lines/chars per notch from the OS, `WM_SETTINGCHANGE`, precise-stream guess), `llwindowsdl.cpp` + macOS NSEvent monitor, `llviewerwindow.cpp` (sensitivity on the new fields), `llscrollbar.*`, `llscrollcontainer.*`, `lltextbase.cpp`, `llscrolllistctrl.*`, `text/altextview.*`, `widgets/altabstrip.*`, `llmath`/`llui` CMake lists, two llui tests (`SkoomaScrollSmooth` off in a settings scope). Unmarked: `settings_alchemy.xml` (seven `SkoomaScroll*` beside `MouseWheelScrollSensitivity`). Own: `llmath/skwarpprofile.h`, `llui/skscrollanimator.*`, tests. Layout changes: full rebuild. Pending: `skcamerawarp.h` forwarder (`V:\Scratch\smooth-scroll\skcamerawarp.h`) for the combat/camera side.
- `ui/game-chat` emoji: marked `lltexteditor.h` / `.cpp` (pre-key hook, public `getCursorPosition`, shortcode popup switch), `llemojihelper.cpp` (closes silently — all chats). Own: `skemojicomplete.*` (CMake SK list after `skchatunfurlcache`).
- `ui/mu-pose-emoticons` (via `ui`, main `b41d92bc8c`): the `:` MU* pose (`EnableMUPoseChat`) skips text emoticons and emoji shortcodes. Marked: `llfloaterimsessiontab.cpp` (`sk_starts_with_emoticon` before `applyMUPose`, its call in the condition, and `:'` posing only before a letter). Applies to nearby chat, the chat bar, the game-style chat and IMs.
