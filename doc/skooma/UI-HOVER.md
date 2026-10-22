# Hover context and overlays

How a hovered widget tells the rest of the viewer what it is, and how anything draws
in the world (3D), anchored to a world point (2.5D) or on the screen (2D) in response.
Branch `feature-ui/hover`. Files: `indra/newview/skhover.{h,cpp}`, `skoverlay.{h,cpp}`.

## 1. What upstream already gives you

- `LLViewerWindow::updateUI` builds the set of views under the cursor every frame and
  fires `onMouseEnter` / `onMouseLeave` on the views that changed. Containers get it too,
  not just the leaf. Top ctrl and mouse capture are respected.
- Every `LLUICtrl` exposes those as `setMouseEnterCallback` / `setMouseLeaveCallback`.
  Code only, no XUI param.
- `handleHover(x, y, mask)` on any `LLView` gives continuous position inside the widget.
- Tooltips take custom panels through `LLToolTip::Params::create_callback`, but only at
  the cursor. Use a tooltip when the feedback belongs next to the mouse; use this doc
  when it belongs somewhere else.
- When no UI view took the hover, `LLToolPie` picks the world every frame:
  `LLSelectMgr::instance().getHoverObjects()` / `getHoverNode()`.

What upstream did not give you: a readable hovered set, a channel from the hovered
widget to a distant consumer, and a registration point for overlays. That is these two files.

## 2. `SKHover` — who is hovered, and what they say

```cpp
#include "skhover.h"

SKHover::leaf()                 // deepest LLView under the cursor, nullptr in the world
SKHover::isHovered(view)        // view or an ancestor-chain member is under the cursor
SKHover::overUI()               // false → read LLSelectMgr's hover pick instead
```

A hovered widget **publishes a context**: a `kind` string consumers filter on, and an
LLSD payload they interpret. The one-liner, at `postBuild`:

```cpp
SKHover::bind(getChild<LLUICtrl>("row_sun_azimuth"), "atmo.param",
              [this] { return LLSD().with("param", "sun_azimuth").with("track", mTrack); });
```

`bind` publishes on mouse enter (the lambda runs then, so it sees current state) and
retracts on leave. Manual form for widgets that change what they mean as the mouse moves
inside them (a timeline, a list): call `SKHover::publish(this, kind, data)` from
`handleHover`, and nothing else. Contexts are dropped automatically when the source leaves
the hover set or is deleted; `retract(view)` only ends one early.

Consumers:

```cpp
// poll, from any draw() or per-frame update
if (const SKHoverContext* c = SKHover::context("atmo.param")) { ... c->data ... }

// or subscribe; fires on every change, including the clear (kind empty)
mHoverConn = SKHover::listen([this](const SKHoverContext& c) { ... });
```

Rules:
- The deepest publisher wins. A row inside a publishing panel overrides the panel.
- `kind` is a dotted namespace: `atmo.param`, `nav.room`, `inventory.item`, `object`.
  Add new kinds freely; list them in §5 so consumers can find them.
- Payloads are data, not pointers. Put UUIDs, names and numbers in the LLSD, and resolve
  them on the consumer side, so a consumer never dereferences a widget that has gone.
- `listen` connections must be stored and die with the subscriber (`boost::signals2::
  scoped_connection` as a member).

## 3. `SKOverlays` — drawing 3D, 2.5D and 2D

Three layers, each one call site in the frame. Register a drawer; hold the handle.

| Layer | When | Coordinates | Use for |
|---|---|---|---|
| `World` | After `LLPipeline::renderDebug`, before post-processing. Skipped for probe captures and impostors | World, camera matrices set, scene depth buffer live | Highlight boxes, paths, volumes, ghost objects (3D) |
| `ScreenUnderUI` | Start of `render_ui_2d`, before floaters draw | Ortho pixels, y up, UI-scaled | Labels, brackets, arrows pinned to a world point (2.5D), HUD-style readouts (2D) |
| `ScreenOverUI` | After the UI has drawn | Same | Cursor-attached ghosts, drag previews, anything that must beat floaters |

```cpp
#include "skoverlay.h"

class MyFloater : public LLFloater
{
    SKOverlayHandle mOverlay;       // removes itself when the floater dies
    bool postBuild() override
    {
        mOverlay = SKOverlayHandle(SKOverlayLayer::ScreenUnderUI, "my label",
                                   [this] { drawLabel(); });
        return true;
    }
};
```

Drawer contract:
- Draw only when you have something to draw; the registry checks `any(layer)` and calls
  every drawer in the layer each frame. An empty drawer costs one function call.
- Leave GL state as you found it. Scope your state: `LLGLDepthTest`, `LLGLEnable blend`,
  `gGL.pushMatrix()` / `popMatrix()`, bind and unbind your shader.
- A drawer may remove itself or another during the pass; the registry tolerates it.
- Drawers run before `LLFontGL::sCurOrigin` is reset, so text is fine in the screen layers.

### 3D (`World`)

Same boilerplate the navmesh overlay uses:

```cpp
gDebugProgram.bind();                      // or gUIProgram for textured quads
LLGLEnable blend(GL_BLEND);
LLGLDepthTest depth(GL_TRUE, GL_FALSE);    // test against the scene, do not write
gGL.color4f(...); gGL.begin(LLRender::LINES); ... gGL.end();
// "seen through walls" trick: draw again with LLGLDepthTest(GL_TRUE, GL_FALSE, GL_GREATER) at lower alpha
gDebugProgram.unbind();
```

Positions are agent-space (`LLViewerObject::getRenderPosition()`, `getPositionAgent()`),
not region or global.

### 2.5D (`ScreenUnderUI` + projection)

```cpp
S32 x, y;
if (SKOverlays::projectToScreen(pos_agent, x, y))   // false when behind the camera
{
    // off-screen points are returned unclamped; clamp yourself or draw an edge arrow
    gl_rect_2d(x - 4, y + 4, x + 4, y - 4, colour);
    LLFontGL::getFontSansSerif()->renderUTF8(label, 0, x + 8, y, colour, LLFontGL::LEFT, LLFontGL::BASELINE);
}
```

`LLViewerCamera::projectPosAgentToScreenEdge` gives the clamped edge point if you want the
arrow. Depth against the scene is not available in 2D; if the anchor must hide behind
walls, do the occlusion test in the `World` layer and stash the result for the 2D drawer.

### 2D (`ScreenOverUI` / `ScreenUnderUI`)

Plain `gl_rect_2d`, `gl_line_2d`, `LLFontGL`, `LLUIImage::draw`. Screen coordinates are the
same as any widget's `calcScreenRect()`, so a ghost can be drawn exactly over a widget in
another floater. `LLUI::getInstance()->getMousePositionScreen(&x, &y)` for cursor-attached.

## 4. Recipes

**Ghost keyframes on a timeline while a property row is hovered.** Row: `SKHover::bind(row,
"atmo.param", …)`. Timeline widget: in `draw()`, `context("atmo.param")` → look up that
parameter's keyframes → draw them at reduced alpha. No overlay needed: the consumer is a
widget and draws in its own `draw()`. This is what Soapstorm polled rects for.

**Highlight the in-world object a list row names.** Row payload carries the UUID. A
`World` drawer owned by the floater reads `context("object")`, `gObjectList.findObject`,
draws the bounding box. Hover leaves → context clears → drawer draws nothing.

**Label every room while the rooms panel is hovered.** `ScreenUnderUI` drawer; project each
room centre; skip when `!SKHover::context("nav.rooms")`.

**Hover in the world lighting up a widget.** `SKHover::overUI()` false → read
`LLSelectMgr::instance().getHoverObjects()` in the widget's `draw()`. Nothing to publish;
the pick is already global.

## 5. Kinds in use

| kind | payload | publisher | consumers |
|---|---|---|---|
| _(none yet)_ | | | |

Add a row when you introduce one.

## 6. Injection sites

Four single-line markers, all `// <SK:Nexii>`: the `SKHover::update` call after the hover
set swap in `LLViewerWindow::updateUI`; `SKOverlays::draw(World)` after `renderDebug()`
in `LLPipeline::renderGeomPostDeferred`'s debug block; the two screen-layer calls in
`render_ui_2d` (`llviewerdisplay.cpp`). Listed in [INJECTIONS.md](INJECTIONS.md).
