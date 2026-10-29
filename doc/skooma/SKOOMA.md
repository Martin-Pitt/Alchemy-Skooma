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

Grouped by area. The trunk a group's branches land through is named in its heading where one exists.

### Render (trunk `render`)

| Feature | Branch | What it does | Index | Credit |
|---|---|---|---|---|
| **Flat prim sides** | `feature-render/prim-split` (landed, parked as `backup/`) | Untwisted box and prism prims are drawn as flat quads, two triangles a side, without the old edge splits that only served per-vertex lighting (a box goes from 108 to 12 triangles at the highest LOD). Twisted prims keep the splits, and a LOD change that builds the same mesh skips the rebuild. Develop > Render > Render Tests > Legacy Prim Edge Splits brings the splits back for comparison | [INJECTIONS.md](INJECTIONS.md) | Nexii |
| **Squeeze** *(planned)* | — | Textures stay BC7-compressed in VRAM, about a quarter of the memory, encoded in the background. A from-scratch rewrite of Soapstorm's Squeeze, using the same encoder: Basis Universal's `bc7f`, vendored unmodified (Apache 2.0) | — | Nexii |

### Atmosphere

| Feature | Branch | What it does | Index | Credit |
|---|---|---|---|---|
| **Atmo Magic** *(paused)* | `backup/atmo-magic` | 1) Parameters over time instead of assets over time. A per-parcel document in which every parameter is: a) constant, b) keyframes for that one parameter, or c) driver, a function of time supplied by a module. It sits on top of EEP as a modifier and follows EEP's own edit > local > experience > parcel > region precedence; switched off, you see plain EEP. 2) With this foundation, modules can plug into that one parameter table, grouped under: Space (system, orbital, celestial), Atmosphere (sky, volumetric clouds), Weather (weather, wind, precipitation, events), Surface (surface, water, surround) and Effects (screenfx, acoustics) | [ATMO.md](ATMO.md) | Nexii |

### Camera (trunk `camera`)

| Feature | Branch | What it does | Index | Credit |
|---|---|---|---|---|
| **Alt-cam zoom to cursor** | `feature-camera/zoom-to-cursor` | In alt-cam the mouse wheel zooms toward or away from the point under the cursor, which stays fixed on screen, so the focus slides onto what you zoom into and orbiting then pivots around it. Picks as far as alt-click does (`altcam-click-distance`: the far plane, open water, the void). Sky, HUDs and appearance mode keep the classic zoom; debug setting `SKCameraZoomToCursor` off restores it everywhere | [INJECTIONS.md](INJECTIONS.md) | Nexii |

### Maps and combat (trunk `combat`)

| Feature | Branch | What it does | Index | Credit |
|---|---|---|---|---|
| **Minimap** | `feature-combat/minimap` | A full redesign of the minimap, drawn from the overview capture with census meshes and a floor cut from the air cells; optional layers for stairs, doors, ladders, elevators and contours. A faint Automap shows it over the world in mouselook. Settings live in their own floater | [MINIMAP.md](MINIMAP.md) | Nexii |
| **Tactical overview** ("tacmap") | `feature-combat/minimap` | A modal view that replaces the world with a Battlefield-style tilt-shift overview of where you are. The world is drawn in a data-viz style (census meshes over a ghost of the real geometry, colour by relative height, outlines, no textures or lighting), with a floor cut for interiors that dissolves round people close up, nametags, projectiles, and vehicles as captured outlines. Alt-cam, flycam, WASD and a pan tool move around it | [MINIMAP.md](MINIMAP.md) | Nexii |
| **3D worldmap** (map mode landed; tabs and impostors planned) | `feature-worldmap` | A modal map that replaces the world view: zoom out, Dyson Sphere Program style, from your region through its neighbours to the whole grid, seen in 3D like Google Maps with a light tilt-shift and an overlay showing where people are. Alt-cam or fly-cam around it; entering and leaving restore each camera, and a home key re-centres on you. Tabs for search, the Destination Guide in 3D, and your own places: a searchable visit history (like Google Maps Timeline) with pins and suggestions. Regions you visit are kept as relightable impostors | [WORLDMAP.md](WORLDMAP.md) | Nexii |
| **Over-the-shoulder mouselook** | `feature-combat/ots` | M opens mouselook with the camera behind the shoulder and the avatar drawn; scrolling in still gives first person. Shots converge on the crosshair: the camera and the body rotation sent to the region both aim from the eye at what the crosshair raycast hits, so guns using `llGetCameraRot` or `llGetRot` land where you aim. Options under the Combat menu | [COMBAT.md](COMBAT.md) | squeedledorf (Soapstorm) |
| **Combat render lockout** | `feature-combat/lockout` | Against wallhacks: in mouselook (the shoulder view too) and for 5 s after, debug views that see through walls are off (render metadata, beacons, look-at, point-at), and wireframe, highlight transparent, hidden selection and hidden world geometry are forced back, your own settings restored when it lifts. Mouselook's offscreen arrows, crosshair name and position/health text are removed | [COMBAT.md](COMBAT.md) | Nexii |
| **Combat** *(planned)* | — | Kill Feed, Detective Mode, Damage Numbers, hit markers and minimap deaths from the combat log (via the LSL Bridge) | [COMBAT.md](COMBAT.md) | Kill Feed, hit markers: squeedledorf (Soapstorm); rest: Nexii |

### UI and chat (trunk `ui`)

| Feature | Branch | What it does | Index | Credit |
|---|---|---|---|---|
| **Game-style local chat** | `feature-ui/game-chat` + `feature-ui/chat-object-glyphs` | Replaces the nearby toasts and chat bar (`SkoomaLocalChatModern`) with a chromeless MMO-style chat: Cards (default) or Minimal style; messages fade, hover or typing shows history with a fading backdrop and an edge-hover border; per-pixel scroll-edge mask, overlay scroll bar, jump-to-present pill, Discord-style day dividers and timestamps; growing multi-line input with emoji button and a `:shortcode` autocomplete list (`SKEmojiComplete`); typing line with out-of-range typers (20–30 m); collapses into the shared bar with its unread count and who is typing; stays up beside Conversations (hiding is `SkoomaLocalChatHideForConversations`); messages held longer for their words and never gone before the one above; a settings floater from the cog; avatar pictures and captured side/plan outlines for object and vehicle speakers; previews of SL places and listings, lsl.dev/slua.dev, YouTube and GitHub repos, issues and pull requests with their state (`skchatunfurl`; resident and group previews off); lockable | — | Nexii |
| **MU\* pose emoticons** | `pr/mu-pose-emoticons` (merged upstream, #579) | The `:` pose shorthand (`EnableMUPoseChat`) leaves text emoticons (`:o`, `:D`, `:'(`) and emoji shortcodes (`:smile:`) as typed, in every chat | — | Nexii |
| **Reopen IMs** | `feature-ui/reopen-ims` | The IM and group chats open when you quit come back in Conversations at the next login (`SkoomaReopenIMSessions`, per account; ad-hoc conferences, muted senders and RLVa-blocked IMs are skipped). Saved only by a clean quit, not when the viewer crashes or is stopped from the debugger; saving on each change waits for Rye's settings upgrade, which will save settings incrementally | [INJECTIONS.md](INJECTIONS.md) | Nexii |
| **Inventory Explorer** (spec) | `feature-ui/inventory-explorer` | A second inventory floater in the style of Finder, File Explorer and Thunar: tabs, a path bar with subfolder menus, a sidebar of Favorites, Inventory, Marketplace and Library, Columns as the default view with ghost and details columns, Icons, List, and a full-floater Gallery with zoom. Nothing new is stored about inventory | [INVENTORY-EXPLORER.md](INVENTORY-EXPLORER.md) | Nexii |
| **Avatar menu teleports** | `feature-ui/avatar-menu-teleports` | Offer Teleport and Request Teleport sit under Teleport To in the in-world right-click menu of an avatar and of its attachments | [INJECTIONS.md](INJECTIONS.md) | Nexii |
| **Mesh uploader pivot** | `feature-ui/mesh-uploader-pivot` | Place an unrigged mesh's origin, and so the axis it turns about, anywhere in its bounding box, so a door hinges on its edge without hidden geometry. Upload options tab, in percent of the half-extent or metres from the centre; the uploader shifts the stored position domains, not the vertices. The sim keeps only analyzed hulls where the pivot puts them, so the pivot is held at the centre while a physics model is set but not analyzed, and for rigged mesh or several models; with no physics model it warns the object collides centred (fine for phantom or None). The preview's Origin display option draws the axes at the pivot | [INJECTIONS.md](INJECTIONS.md) | Nexii |

### Objects and motion

| Feature | Branch | What it does | Index | Credit |
|---|---|---|---|---|
| **Object motion updates** | `feature-object-motion-updates` | Two fixes to how moving objects follow their updates. Clamped velocity recovery: velocities the terse update clamps to ±128 m/s (bullets, fast vehicles) are recovered, so they no longer snap back along their path (`SKMotionRecoverClampedVelocity`). Smooth update corrections: a moving object's position error at each update is bled out over a fraction of a second instead of snapping; large jumps still snap (`SKMotionSmoothCorrections`). Both under Develop > Network | [MOTION.md](MOTION.md) | Nexii |
| **Ghosted object cleanup** | `workaround-ghosted-physical-objects` | Temporary physical objects (bullets, thrown objects) that the viewer strands at a region border or that stop without being told to are hidden at once, sound included, then the region is asked whether they still exist; one still there a second later comes back. Judged by distance (speed × silence ≥ 5 m), not time, and never while the circuit is stalled. A workaround until the simulator sends their kills reliably | [INJECTIONS.md](INJECTIONS.md) | Nexii |

### Audio

| Feature | Branch | What it does | Index | Credit |
|---|---|---|---|---|
| **Loudness Normalization** | `feature-audio/loudness-normalization` | BS.1770 loudness is measured while the sound decodes, so clips play back at a consistent level | [INJECTIONS.md](INJECTIONS.md) | Nexii |
| **Long sound upload** *(planned)* | — | Sounds longer than the grid's clip limit are split into numbered parts with click-free edges instead of being rejected, and loudness is normalised on upload, measured across all parts. Idea from Soapstorm | — | Nexii |

### Input

| Feature | Branch | What it does | Index | Credit |
|---|---|---|---|---|
| **F13–F24** | `feature-input/extended-fkeys` | The extra function keys can be bound like F1–F12, on Windows and SDL | [INJECTIONS.md](INJECTIONS.md) | Nexii |

## Wishlist

Ideas not yet committed to.

| Idea | Note |
|---|---|
| **HUD anti-aliasing** | Smooth the edges of HUD attachments. Rye plans MSAA for the HUD layer upstream (2026-10-03), which replaces Soapstorm's supersampling; wait for it to land in `develop` |
| **Dynamic near clip** | Adjust the near clip plane as the camera moves. Skooma's depth setup differs from Soapstorm's focus-scaled near clip, so only the dynamic adjustment carries over |
| **Procedural mesh tools** | In-viewer generators for useful meshes: roads, streets and pavements, and 3D terrain tiles that look good, with erosion, cliffs and similar features |
| **Silhouette textures for creators** | Expose silhouette capture as a tool, so content creators can generate outline textures of weapons, vehicles and other objects for their own HUDs and UI |
| **Bridge link unfurls** | Local chat previews for non-SL links fetched by the LSL bridge's `llHTTPRequest`, so the site sees the simulator, never the resident's IP (only owner/region headers, which chat range already reveals). Title and description only (16 KB body cap; images only from SL hosts), queued under the script HTTP throttles, cached, per-domain cooldown, opt-out. Second fetch path beside the SL-domain unfurls (`skchatunfurl`). Needs the bridge foundation first |
| **Procedural textures** | An in-viewer tool for content creators to generate textures from node graphs, in the style of [Arcanigen](https://arcanigen.thatrobhuman.com/), built from GLSL SDF shapes ([Pixel Spirit Deck](https://github.com/patriciogonzalezvivo/PixelSpiritDeck/)) and Blender-style nodes. Spec: [TEXTURE-STUDIO.md](TEXTURE-STUDIO.md) |
| **Combat mouselook** | [secondlife/viewer#1443](https://github.com/secondlife/viewer/issues/1443): in mouselook, turn your own avatar's torso toward where you aim, as an opt-in procedural animation seen only by you, so held weapons stay on screen, and fix animated child prims lagging behind. An optional, adjustable camera at the eyes with your own head hidden. Turn the camera and head only, never BodyRotation, which steers movement |
| **Touch pointer capture** | [Feedback: touch pointer capture](https://feedback.secondlife.com/scripting-features/p/touch-pointer-capture): once a script captures a touch, it keeps receiving touch events while the pointer leaves the object, as browsers' pointer capture works. This allows sliders that work past their edges and drag and drop with ghost previews. Proposed: `llCaptureTouch`, `llReleaseTouchCapture` and `llHasTouchCapture`, with modes for world or screen coordinates, confirm by tap, and passthrough raycasts. Escape always releases, and a cursor or indicator shows while capture is on. Needs server LSL support; the viewer side is continuing to send touch updates during a capture |
| **Accessibility** (trunk `accessibility`) | A new Accessibility tab in Preferences built to the [Game Accessibility Guidelines](https://gameaccessibilityguidelines.com/), with features hanging off an `accessibility` trunk. Screen reader support first. Later, on-demand navmesh pathfinding built off the census (the Recast navmesh comes back, but only built when asked for) so a resident can find their way round the world: a much better autopilot; a consumer of the foundations, never a replacement. Notes: `.accessibility-plan.md` |
| **Community emotes** | Skooma's own emote provider, modelled on 7TV but with no third party at runtime. A group's set is a notecard of emotes (name, sheet UUID, cell, first frame, frames, fps) over shared texture sheets (up to 2048, retina cells, gutters, no mips, UI boost). It is found by a `🫪:<base64url notecard UUID>` tag (27 bytes) in the parcel description (local chat, by the parcel's group) and the group charter (group chat). The notecard is read through the LSL bridge and cached by UUID, and a floater edits it and re-tags after a save. Per-emote frame cap with a static fallback. Design and rejected options: `.community-emotes.md` |
| **Linkset export** | Export a prim linkset to mesh or a file (`.obj`), like Firestorm's export, for full-perm linksets you created yourself |


## Foundations

Shared systems the features above build on. More features will be added on top of them. Each is a `foundation-*` branch cut from `develop` (or from the foundation below it), which branches of any trunk may merge.

The world foundations stack in processing order, each built on the one before and never calling its consumers: **physics shapes** → **census** (the physics world) → **air cells** (processing of the census) → **rooms and spaces** (processing of the air cells). Each is its own library (`skcensus`, `skair`, `skrooms`), so it can be built, tested and benchmarked alone. The Recast navmesh was dropped 2026-10-22; its useful parts moved into these layers. Plan: `.foundations-plan.md`.

| Foundation | What it provides | Used by | Index | Credit |
|---|---|---|---|---|
| **Physics shapes** (`foundation-world/physics-shapes`, built) | What an object's physics shape is, decided in one place: a mesh's `physics_havok` section is fetched and read, so the simulator's own collision triangles are known, not just the uploaded shape, with hulls, decompositions and prim shapes as the fallbacks. The physics shape display draws through it, coloured by cost or by where the shape came from | Census; physics shape display | [INJECTIONS.md](INJECTIONS.md) | Nexii |
| **Census** (`foundation-world/census`) | The physics world: a record of every solid object near the agent and camera, from its physics shape, plus terrain, grouped into structures (clusters). It classifies doors, ladders, elevators and vehicles, and keeps cached objects past the draw distance as boxes. It runs off the main thread in small slices. Library `skcensus` | Air cells; mini-map and tactical overview (census meshes) | [AIR.md](AIR.md) | Nexii |
| **Air cells** (`foundation-world/air`) | The empty space above each solid, built off the census and stored as a quadtree of 0.125–2 m cells, with wall face offsets. Each cell knows whether it is open to the sky, sheltered or indoors. Built eagerly out to 300 m. Library `skair`, rasterising census triangles into columns of its own | Rooms and spaces; tactical overview (floor cut, dissolve); mini-map (whereabouts, roof occupancy); Atmo weather and acoustics (later) | [AIR.md](AIR.md) | Nexii |
| **Rooms and spaces** (`foundation-world/rooms`) | The air cells processed into enclosures, rooms, spaces and portals (doors, doorways, arches, openings), with walls, floors, wind and probe placement; stairs detected from the air (new). Answers "is this person indoors, and on which floor?" Library `skrooms` | Reflection probes; tactical overview; mini-map | [AIR.md](AIR.md) | Nexii |
| **Overview capture** (`skoverviewcapture`, part of `combat/minimap`) | An orthographic G-buffer render of the region and its neighbours in a data-viz style: colour by relative height, depth and normal outlines, no albedo or lighting. Census meshes, drawn from GPU slot pages, sit over a ghost of the real geometry; the floor is cut per indoor occupant from the air cells and dissolves round people close up; moving objects are patched in rather than recaptured. | Mini-map and tactical overview (base map) | [MINIMAP.md](MINIMAP.md) | Nexii |
| **Modal view** (`foundation-modal-view`, landed) | A shared way for a view to take over from the world. It hides the world (render, probes, finalize, hover text) while the world keeps updating, snapshots and restores the camera, and routes the existing alt-cam and flycam to the mode, with a pan tool. The avatar stays safe: no movement, autopilot reports failure, no mouselook or snapshots, RLVa camera rules honoured. The sim, sound and voice stay at the avatar. `skmodalview` | Tactical overview; 3D worldmap | [WORLDMAP.md](WORLDMAP.md) | Nexii |
| **Outline capture** (`foundation-render/outline-capture`, landed) | Renders an object as a clean outline image in plan, side or front view, with a policy for which captures to keep and a cache of the best one | Mini-map and tactical overview (vehicle outlines); local chat (object and vehicle icons); Kill Feed and other combat UI (weapon outlines, planned) | — | Nexii |
| **Hover context & overlays** (`foundation-ui/hover`, landed) | A hovered widget publishes what it is, and anything can read it. Registered drawers show 3D, world-anchored 2.5D or 2D overlays in response | Atmo editor (ghost keyframes); Mini-map; Combat | [UI-HOVER.md](UI-HOVER.md) | Nexii |
| **Floater lock** (`foundation-ui/floater-lock`) | A lock button for background-less floaters that opt in (`can_lock`): no drag or resize, no backdrop, clicks on empty space pass through to the world | Local chat, minimap (`combat/minimap-lock`) | — | Nexii |
| **Resize grip on hover** (`foundation-ui/resize-handle-hover`) | Floaters that draw no background show their resize grip only while hovered, focused or resizing | Local chat, minimap | — | Nexii |
| **Collapse** (`foundation-ui/collapse`) | Collapse replaces minimize: a floater folds against the nearest screen edge as its title bar (turned into a strip on the sides) instead of going to the minimized row. Any floater can show live content in its collapsed bar through `setCollapsedStatus` (a bold count and a dim status line); chromeless floaters collapse into the same bar | Every floater; local chat, mini-map | [INJECTIONS.md](INJECTIONS.md) | Nexii |
| **Smooth scrolling** (`foundation-ui/scrolling`) | Platform-equivalent scroll distance (Windows lines per notch, 100 px at defaults) and Chrome-style glide across scroll containers, lists, text and code editors and tabs; precise trackpad deltas with OS momentum; warp mode from the camera warp for experiments (`SkoomaScrollSmoothMode`); `SkoomaScroll` off restores upstream wheel handling; `smooth_scroll` param opts a bar or text view out of the glide (code editor off) | Local chat, text views, code editor, tab strip, lists | [INJECTIONS.md](INJECTIONS.md) | Nexii |
| **Overlay scroll bar** (`foundation-ui/overlay-scrollbar`) | `LLScrollbar` `overlay` style: a thin rounded thumb over the content that widens on hover, fades when idle, and shows briefly when scrollable content first appears (macOS style). Clicking, dragging and paging are the normal bar's. `LLScrollContainer` `overlay_scrollbars`; `SkoomaScrollbarOverlayEverywhere` for all containers | Local chat | [INJECTIONS.md](INJECTIONS.md) | Nexii |
| **Thin scrollbars** (`foundation-ui/thin-scrollbars`, builds on Smooth scrolling) | `LLScrollbar` `thin` style matching Chrome's thin bar: a 10 px track, a 6 px pill thumb inset 2 px, small triangle arrows at each end that step half a wheel notch through the smooth-scroll path; hover and pressed colours; skin colours `ScrollbarThin*Color`. `LLScrollContainer` `thin_scrollbars` (10 px gutter); `SkoomaScrollbarThinEverywhere` for all containers (startup). Overlay wins when both are set | Inventory Explorer | [INJECTIONS.md](INJECTIONS.md) | Nexii |
| **UI saturation** (`foundation-ui/saturation`) | Any view can draw desaturated: `ALUISaturation` scopes set one value on `LLRender`, uploaded to the bound program on change and on bind; `uiF`/`solidcolorF` grey to Rec.709 luminance in gamma-2 light below 1, text, images and emoji alike. Owner idea: a later UI text contrast control as a second field of the same state (dark-mode UI assumed; input fields are light in LL's default skin, an open question) | Local chat fading history | [INJECTIONS.md](INJECTIONS.md) | Nexii |
| **Drag feedback** (`foundation-ui/drag-feedback`) | `LLToolDragAndDrop` feedback for every drag in the viewer: a ghost of what is dragged (stacked icons, count badge; a single item with a picture as a thumbnail; links as the original's icon with a link badge), a verdict chip with the action or refusal reason (inventory rules report it, RLVa refusals stay quiet), per-frame target marks on folder rows and Gallery tiles, world targets outlined through `ALSelectionOutline`, a static Ctrl/Shift/Alt hint line; accordion headers spring after `FolderAutoOpenDelay`. Fixes: copy cursor, tooltip on capture loss, calling-card `@share`, outfit editor and Link Replace acceptance. `SkoomaDragFeedbackClassic` restores the old look. Owns no scrolling | Inventory Explorer; every inventory, contents, people and world drop | [INJECTIONS.md](INJECTIONS.md), [.drag-feedback.md](.drag-feedback.md) | Nexii |
| **Audio preview control** (`foundation-audio/preview-control`) | llaudio: start a sound partway (`LLAudioSource::play(uuid, offset)`), seek and read the position of a playing source, `canSeek()`, and `stopTriggeredSounds` to stop our own copy of an in-world trigger (others still hear it). OpenAL, FMOD Studio and FAudio all seek | Inventory Explorer's sound player | [INJECTIONS.md](INJECTIONS.md) | Nexii |
| **Warp** (`foundation-warp`) | SKWarp (`skwarpprofile.h`, llmath): a move that ramps up, cuts out its cruise and brakes onto the target, with retargeting mid-trip, in the caller's units | Camera warps (mini-map Cam, world map, Escape reset); smooth scrolling | — | Nexii |
| **LSL Bridge** *(planned)* | A script bridge for data only LSL can reach: combat log listening, notecard fetching, and more | Combat | — | Nexii |
| **Local objects** (`render/local-objects`, planned) | Better support for viewer-local objects: the far flag (drawn past the draw distance out to the 8 km reach under the infinite projection), a purple selection outline and related polish. Sits between the far plane work and Atmo's surround | Atmo surround | — | Nexii |

## Fixes

Small `fix-*` branches cut from `develop`, each a PR in waiting.

| Fix | What it fixes | Status |
|---|---|---|
| **Normals debug mask** (`fix-normals-debug-mask`) | Crash when the normals debug overlay meets a face without normals | landed |
| **Parcel panel disconnect** (`fix-panelland-disconnect`) | Crash when the parcel panel refreshes after the world is gone | landed |
| **Tracy memory option** (`fix-tracy/memory-option`) | Tracy allocation tracking is a build option, off by default, since it balloons the viewer's memory | landed |
| **All hover tips** (`fix-all-hover-tips`) | `ShowAllObjectHoverTip` is honoured for non-interactive objects | landed |
| **Chat focus** (`fix-chat/focus`) | The chat bar takes focus when Conversations is closed | landed |


## Working on Skooma

| Doc | For |
|---|---|
| [DEVELOPING.md](DEVELOPING.md) | Humans: setup, branches, daily loop |
| [WORKFLOW.md](WORKFLOW.md) | Terse reference: `develop` (upstream) / `main` / `feature-*`, `git skooma` commands |
| [UPSTREAM.md](UPSTREAM.md) | Parts that can go to Alchemy on their own, and what each depends on |
| [INJECTIONS.md](INJECTIONS.md) | Every `<SK:Nexii>` edit to upstream files, grouped by feature |
| [UI-LAYOUT.md](UI-LAYOUT.md) | XUI layout rules for floaters and panels |
| [UI-HOVER.md](UI-HOVER.md) | Hover feedback across the UI and overlays in the world |
