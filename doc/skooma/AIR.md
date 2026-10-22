# Air cells — index

## What it is

Skooma's model of the empty space above solids. Each 32 m tile keeps, per 0.125 m column, every open
gap from the top of one solid to the bottom of the next (or open sky), distilled into a nesting
quadtree of leaves 0.125–2 m in Morton order. A per-mesh pass floods that air for enclosure (sky
openness S, depth D, indoorness e, bucket OUTDOORS / SHELTERED / INDOORS), cuts it into spaces at
portals, and derives wind slabs and room probes. With the census (`SKWorldCensusEnabled`, default on) it is the always-on base
(`SKAirCellsEnabled`, default on); the Recast/Detour polygons are optional (`SKNavMeshEnabled`, default
off) and being phased out. `skinteriorair*` and `skatmoinfoviewsair.cpp` are Atmo's interior haze and
wind/weather debug views, not air cells, despite the names.

## Where it lives

| library | files | what |
|---|---|---|
| `sknavmesh` | `sknavair.h/.cpp` | `AirGap`, `AirGrid`, `extractAir` (off an `rcHeightfield`), clearance, sliver folding |
| | `sknavairdistil.h/.cpp` | `AirLeaf`, `TileAir`, `airKey`, distil, seams, tops, `buildTileAir`, `spanGaps`, `upsampleAir`, pack |
| | `sknavairfine.h/.cpp` | 0.0625 m fine air over narrow openings; experiment B7, failed, not used |
| | `sknavenclosure.h/.cpp` | S/D/e floods, buckets, `GapEnclosure`, `CoarseAir` |
| | `sknavspaces.h/.cpp` | spaces and portals (DOOR/DOORWAY/ARCH/OPENING/STAIR) cut from the air |
| | `sknavroomgraph.h/.cpp` | rooms: a mesh's member spaces joined across DOOR/OPENING portals (`buildRoomGraph`) |
| | `sknavspaceproducts.*`, `sknavspacefilter.*` | walls/storeys/sheet by space; Detour filter by space (navmesh-only) |
| | `sknavskymap.h`, `sknavwind*`, `sknavroomsschedule.h`, `sknavtilecache.*` | region sky map, wind slabs, pass scheduling, disk cache |
| `newview` | `sknavmesh.h/.cpp` | `SKNavMesh`: tiles, air radius, budget/eviction, spill, sky map, `requestAir` |
| | `sknavrooms.h/.cpp` | `SKNavRooms` per-mesh pass; `SKNavWorldView` (reads), `SKNavWind` |
| | `sknavroomprobes.*` | automatic reflection probes per INDOORS/SHELTERED space |
| | `sknavairoverlay.cpp` | `SKNavOverlayAir` debug drawing |

## Data model

- `AirGap` (8 B): `mFloor`/`mCeiling` in height units (0.125 m, 13 bits, `AIR_HEIGHT_MAX` 8191;
  `AIR_CEILING_OPEN` when nothing above), flags `TOP` (walkable floor), `OPEN`, `OWN_SKY`, `FLIGHT`, two
  area bits, `AVATAR_FLOOR`; `mClear` lateral clearance in 0.125 m capped at 2 m; `mSlopeX/Y` so a merged
  floor is a plane (`airFloorAt`).
- `AirLeaf` (12 B): square of 2^level cells (level 0–4, `AIR_MAX_LEVEL`), `mFirstGap`/`mGapCount`,
  `mParent` back-offset. Leaves nest: a column's gaps are the union up the parent chain (`airColumn`).
  Merging is gap by gap, so flat floors stay coarse over varied ground below.
- `TileAir`: leaves in `airKey` (Morton, x even bits) order, coarsest first at a shared origin;
  `findLeafByKey` finds a cell's finest leaf. Also four `SeamStrip`s (level-0 gaps per edge, for
  stitching), solid tops on 1 m cells (sky map), sparse `mFaceOffsets` (true wall faces from census
  triangles), and an `AirSerial` that keys caches instead of addresses.
- Tops (`nav/census-incremental`, 2026-10-21): the 1 m solid tops are a `TileTops` (origin, cell, unit, intervals)
  shared by pointer between the detail air, the coarse air an evicted tile keeps and the sky map; `TileAir::mTops` is
  never null (`noTops()`). On-disk layout unchanged, `TILE_PRODUCT_VERSION` 19.
- Far tiles keep only coarse air: 1 m cells (`COARSE_AIR_M`), upsampled for passes that need the fine frame.
- Enclosure: `GapEnclosure` parallel to the tile's gaps (space + 1, S, e, D in 0.25 m, bucket, sealed /
  canopy flags, cover, side openness O); `CoarseAir` keeps it per 1 m cell when detail is evicted.
- Spaces: unlabelled ids stable within a mesh, joined by portals; read as `SKNavWorldView::SpaceId`.
- Rooms (`nav/room-graph`, 2026-10-20): `sknav::RoomGraph` per mesh, built from the spaces product. Members are
  INDOORS/SHELTERED spaces with a building, not roof/flight/frozen; they join across same-storey DOOR or OPENING
  portals (sill slack climb + 0.125 m), never STAIR; a space whose floor band is over 1.5 m (`mRampBandM`) is a
  room of its own. `Room`: `mId` (least member id), `mHash` (moves only with members, rasters or band), `mSpaces`,
  `mPortals`, band, box, cell mask (`covers`). Per mesh: a building over two clusters is two rooms.

## Build and lifecycle

- Meshes are census clusters (`SKNavMesh::reconcile` on a census snapshot), tiled on the 32 m lattice.
  Visible-only census records (`mCollides` false: phantom, volume detect, shapeless) are dropped at the cluster
  filter, so they never reach a tile, cut, feature or probe.
  Each tile is a job on the navmesh pool: census triangles → soup → Recast raster → filters → air;
  with `SKNavMeshEnabled` off the polygon stage, stamps and held rasters are skipped (`e1943b911a`).
- Only a mover (repeated or sustained bursts within two minutes) waits `SKNavSettleSeconds` (4 s); a
  one-off move is landscape at once, and a door leaf at rest `DOORWAY_ESTABLISHED_SECONDS` (10 s) is
  held shut in the census while it swings.
- Eager detail air within `SKNavAirRadius` (300 m, `inAirRadius`, `sknavmesh.cpp:245`) of the camera and
  over the clusters the agent is in (`mHomeBoxes`, `SKNavAirWholeCluster`); consumers widen it with
  `requestAir(who, box)` held for `SKNavAirDetailHoldSeconds`. Outside, tiles build coarse air only
  (`sknavmesh.cpp:1643`).
- Memory: `SKNavAirBudgetMB` evicts farthest detail (keeping coarse tops); settled meshes spill detail
  air and held rasters to the disk cache bit for bit (`SKNavAirSpill`; `requestDetail` brings them back).
  Cache key: params, terrain samples, record keys, a hash of the sources, `TILE_PRODUCT_VERSION` (19).
- Live air bytes: `gAirLiveBytes` counts every finished `TileAir` alive (its own bytes), `gAirTopsBytes` every
  `TileTops`; `evictAir` holds the budget against the live count. `statsLine`: `air <n> tiles <scan> MB, live <n> MB,
  tops <t> MB (<e> evicted)`.
- Products still pin evicted detail (`TileEnclosure::mAir`, `EnclosureInput::Tile::mAir`, `RoomsTile::mAir`) until
  the next rooms pass lands, so the live count bounds the total by evicting more far tiles, not by freeing sooner.
- `sknav::recordSignature` (`sknavlayout`) is a wrapping sum of the records' signatures: order free, and two equal
  records no longer cancel. The disk content key never read it.
- Rooms change list: every site that moves a tile's air notes an `AirChange` (evicted, spilled, lost, reloaded, mesh
  gone, dropped, published) beside `bumpRevision`. `SKNavRooms::update` drains it and walks only those meshes, plus
  a full walk when the census feature stamp moves, every 30 s (WARNS `rooms full walk found N meshes the change list
  missed`) and when 65536 notes go unread. `post` reads `airSnapshot` (a copy of the mesh's air pointers), never the
  navmesh's tiles.
- `airReach()` takes the reach once a pass (camera, agent, home boxes, live `requestAir` boxes), so it agrees with
  the per-tile `inAirRadius`.
- Rooms pass (`SKNavRooms::update`): per mesh, own thread, settles `SKNavRoomsSettleMs` (100 ms; wind
  slabs `SKNavWindSettleMs`, 1 s), 20 ms chunks,
  generation cancel; an old layout always beats none (`sknavroomsschedule.h`). Repairs enclosure and
  spaces incrementally from the last pass; only what moved settles again. The room graph is built in the
  chunk the spaces pass finishes in (Tracy `sknr - room graph`) and published with the spaces.
- View revisions: `airRevision()` moves for enclosure, spaces or rooms, `windRevision()` for wind slabs only
  (`mViewAirDirty`/`mViewWindDirty`); `revision()` = air. Key air caches on `airRevision()`.
- Edits rebuild the whole tile; the local re-raster (`nav/air-local`, B6) is built but held back.

## Queries

`SKNavMesh::instance().view()` → `SKNavWorldViewPtr` (readable on any thread once taken):
`enclosureAt(pos, Enclosure&)`, `airGapAt(pos, SpaceId&, Enclosure&)` (falls back to the room under a
solid), `spaceAt`, `space(id)`, `spacesIn`/`portalsIn`/`portalsOf`, `gapsAt(xy, fn)`, `topOfWorld`,
`raster(box, res, channel)`, `sheet(box, res)`, `meshes()`, `roomOf(SpaceId)`, `roomAt(pos, RoomId*)`,
`room(RoomId)`, `roomCellOrigin` (null for outdoor, roof, flight spaces). Also `SKNavMesh::skyOpenAt(pos, reach)`,
`requestAir`, `requestDetail`, `detailResident`, `settlingIn`, `SKNavRooms::wind()` →
`SKNavWind::sampleAt/slab`. Polygon-side reads (`spaceAt` on `SKNavMesh`, `storeys`, `wallRunsIn`,
`findPath`) need `SKNavMeshEnabled`.

## Consumers

| consumer | reads |
|---|---|
| mini-map whereabouts (`skmapplan.cpp:1868`) | `airGapAt` at the feet: space and bucket; falls back to `SKNavMesh::spaceAt` |
| mini-map plan (`skmapplan.cpp:1329+`) | view meshes for sheet/storeys; `requestDetail` for wall runs (which read tile air) |
| tactical overview (`skoverviewcapture.cpp`) | `cutHeights` floor cut; `roomAt` reads one start gap (`airGapAt` at floor + 0.3) and then the room graph (`roomOf`, `roomCellOrigin`), no flood (`combat/minimap-churn`, 2026-10-21); held columns and rooms keyed on `airRevision()`; `outdoorsAt` (no cut, dissolve) from `enclosureAt` at the feet |
| tactical overview cover (`skoverviewcensus.cpp`, `sknavinvisible.cpp`, `combat/minimap-cover`) | no air: census records only. Each invisible part gets PROXY, EXTRA, BARRIER or UNKNOWN_PHYSICS by clipping it against its colliding neighbours |
| annotator (`sknavannotator.cpp`) | view meshes, spaces, portals |
| reflection probes (`sknavroomprobes`, via `llreflectionmapmanager.cpp:439`) | spaces and portals, INDOORS/SHELTERED |
| overlay / exports (`sknavairoverlay.cpp`, `skfloaternavmesh.cpp`) | tile air, enclosure, stamp, face offsets |

Atmo's acoustics, clouds, precipitation and surface code call their own `wind()`, not `SKNavWind`.

## Settings and debug

`SKWorldCensusEnabled`, `SKAirCellsEnabled`, `SKNavMeshEnabled`, `SKNavAirRadius`, `SKNavAirWholeCluster`,
`SKNavAirMinGap`, `SKNavAirBudgetMB`, `SKNavAirSpill`, `SKNavAirDetailHoldSeconds`, `SKNavSettleSeconds`,
`SKNavRoomsSettleMs`, `SKNavWindSettleMs`, `SKNavRoomProbes`, `SKNavRoomProbesMax`. Overlay
`SKNavOverlayAir` (off, leaves, gaps, space, stamp, clearance, face offsets, indoorness) with
`SKNavOverlayAirBudget`; coarse tiles draw dimmed and framed, and the enclosure combo and the
source/slice settings are gone (`8ebc9cb749`). The navmesh
floater shows `statsLine` (air tiles, bytes, evicted, spilled), "mark under camera" and the exports.

## Coupling to the navmesh

The air comes from the Recast tile raster stage; it builds without the polygon stage but not without Recast.

- Types: `extractAir` and `airTops` take `rcHeightfield` (`sknavair.h:35`, `sknavair.cpp:30,61`,
  `sknavairdistil.cpp:793`); `buildTileAir` takes one (`sknavairdistil.h`, `.cpp:838`); `spanGaps` /
  `leafSpanGaps` map air onto `rcCompactHeightfield` spans (`.cpp:901,1048`).
- Raster: census triangles go through `rcRasterizeTriangles` (`sknavbuild.cpp:554,572`), Recast's
  span filters (`:1375-1384`), bridges and underground culling, into AVATAR's single solid raster
  (`:1754`); the air is read from it inside `buildTile` (`:2305` padded + face offsets, `:2357`
  `buildTileAir`, `:2411` `spanGaps` into `TileRaster::mSpanGap`). The gaps' walkable flag and area bits
  are AVATAR's Recast areas and slope partition; flights come from the polygon side's stair records.
  Older docs say "off the WEATHER heightfield"; WEATHER was folded into this one raster in B2.
- Scheduling and storage: tiles, air radius, eviction, spill, sky map and `requestAir` live in
  `SKNavMesh` (`sknavmesh.h:424-431`, `:592`); tile products and cache are keyed with
  `DT_NAVMESH_VERSION` (`sknavtilecache.cpp:837`). `SKNavRooms::update` reads `SKNavMesh::mMeshes`,
  `mRevision` and `mSkyMap` directly (`sknavrooms.cpp:684,991`).
- Navmesh-only, can go with it: `sknavspacefilter`, poly stamping, storeys/wall runs over polys,
  `SKNavMesh::spaceAt` fallback in whereabouts.

To survive removing Recast, per the owner's rule (build air from census triangles, not tile rasters):
a column rasteriser of our own over census triangles (gap list per column, the walkable test and area
codes we need, without Recast filters/erosion); `extractAir`/`buildTileAir`/`airTops` taking that
instead of `rcHeightfield`; stairs/flights and face offsets fed from the census, not Recast polygons;
tile scheduling, residency, sky map and the cache moved out of `SKNavMesh` into an air owner (cache
key without `DT_NAVMESH_VERSION`); `SKNavRooms` reading that owner; whereabouts without the poly
fallback; wall runs and storeys rebased on air and spaces.

## Read in this order

| doc | what | status |
|---|---|---|
| `NAVMESH.md` | branch history and rulings for every air branch | current index |
| `.navmesh-rooms-plan.md` | enclosure, spaces, portals, wind, probes design | v2 2026-09-25; WEATHER-raster wording stale |
| `.air-cell-rules-review.md` | review of the cell rules | read for rationale |
| `.nav-air-distil-brief.md`, `.nav-air-morton-brief.md` | quadtree distil, Morton order | built; gap-merge/leaf-cap superseded parts |
| `.nav-air-cache-brief.md`, `.nav-air-residency-brief.md` | content key, disk spill | built |
| `.nav-enclosure-brief.md`, `.enclosure-incremental.md` | enclosure pass, incremental repair | built |
| `.nav-rooms-brief.md`, `.nav-space-*`, `.nav-spaces-perf-brief.md` | spaces, stamp, products, perf | built; stamp/products navmesh-only |
| `.nav-space-polys-plan.md` | the B0–B7 restructure | plan, done except B6 held, B7 failed |
| `.nav-air-local-brief.md`, `.nav-air-fine-brief.md` | local re-raster, fine air | held back / failed |
| `.nav-air-overlay-brief.md` | overlay modes | built |
| `.navmesh-rooms-consumers.md`, `MINIMAP.md` | who reads rooms and enclosure | partly stale; consumer table above is current |

## Open questions

- The census-triangle column rasteriser: its walkable rule and areas without Recast's filters, and
  whether results must match today's air byte for byte or only within tolerance.
- Where flights/stairs come from once Recast polygons go.
- Whether local re-rasterising (B6) returns once the raster is ours.
- Whether air keeps the 32 m tile/cluster unit or moves to a region lattice independent of meshes.
