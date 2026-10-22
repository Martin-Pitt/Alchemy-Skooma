# XUI layout reference

How to lay out a floater or panel without launching the viewer. XUI has **no
reflow, no auto-height, no ellipsis and no overflow warning** — every rectangle
is a number you wrote, and a wrong number draws over the row beneath it in
silence. Read this before touching `indra/newview/skins/*/xui/`.

Companion docs: `doc/LIGHTBOX.md` §5 (worked height math for the Lightbox
accordion), `indra/newview/skins/default/xui/en/fonts.xml` (font registry, whose
header comment is the reference for families/sizes).

## 1. The coordinate model

- `layout="topleft"` on essentially everything (8483 uses vs 78 `bottomleft`).
  It inherits from the parent, but **write it on every widget** — that is the
  house style and it makes a widget safe to move between parents.
- Under `topleft`, `top` counts **down** from the parent's top; `bottom` still
  names the lower edge. `height` and `top` together set the rect; you normally
  give `top` (or `top_pad`) plus `height`.
- **Negative = measured from the far edge.** `right="-8"` is 8px in from the
  parent's right; `left="8"` is 8px in from its left. `right="-1"`/`right="-2"`
  is the idiom for "fill to the edge minus the border".
- Coordinates are in UI-scaled logical pixels, not device pixels. Never write a
  number derived from your monitor.

### Relative placement — what each `*_pad` / `*_delta` chains from

All of these are relative to **the previously declared sibling** (`LLView`'s
`default_rect`, `llview.cpp:2620-2752`), not to the parent and not to the
nearest visually-above widget:

| Attribute | Meaning |
|---|---|
| `top_pad="N"` | my top = previous sibling's **bottom** − N (gap of N) |
| `top_delta="N"` | my top = previous sibling's **top** + N |
| `bottom_delta="N"` | my bottom = previous sibling's bottom + N |
| `left_pad="N"` | my left = previous sibling's **right** + N (gap of N) |
| `left_delta="N"` | my left = previous sibling's **left** + N |
| none of the above | implicit `top_pad = 4` (`VPAD`), same left |

Consequences that bite:

- **Side-by-side widgets break the chain.** Three swatches all at `top="8"`
  leave the chain pointing at the *last one declared*. The next row's `top_pad`
  measures from that one's bottom, so a bank of N widgets contributes **one**
  widget's height to the stack, not N.
- **The chain follows declaration order, not visual order.** Reordering
  elements in the file silently moves everything after them.
- A widget that hangs below its row's nominal height extends the chain. A 16px
  slider with an 18px reset button next to it pitches **26px**, not 25, if the
  button was declared last.
- Prefer absolute `top=` at the start of each visual group and `top_pad=` within
  it. Long `top_pad` chains are the single biggest source of layout drift.

## 2. Text height — the thing that goes wrong

Line height is **not** a constant you can derive from XUI. It comes from the
loaded face's FreeType metrics (`LLFontGL::getLineHeight()` =
`ceil(ascender + descender)`, `llfontgl.cpp:1143`), at `FontScreenDPI` (default
96), for whichever family the user picked in preferences. Do not compute it.
Design to the conventions below, which hold across every bundled face.

Named point sizes (global table, `fonts.xml`; some families override per-family
to keep visual parity): `Small` 9, `Medium` 10, `Large` 11, `Huge` 16,
`Monospace` 10.

**Per-line box, `SansSerifSmall` (the default for `text`, `button`, `check_box`,
`combo_box`, `slider`): 16px.** That is the dominant height in the tree (578 of
1859 `<text>` elements) and it carries ~1px of slack over the drawn line.

| Content | `height` |
|---|---|
| One line of label text | `16` |
| One line, `SansSerif`/`Medium` | `18` |
| One line, `SansSerifLarge` | `20` |
| N wrapped lines at `SansSerifSmall` | `16 * N` (+ `2 * v_pad` if you set one) |
| Section heading (bold, with air under it) | `18`–`20` |

Rules:

1. **A one-line `text` needs `height="16"`, not `height="30"` "for safety".**
   Extra height is dead space that pushes everything below it; too little height
   does not scroll, ellipse or clip — it *draws over the next row*.
2. **Wrapping is off by default.** `<text>` has `wrap` unset (false) and
   `use_ellipses="false"`. A long string in a one-line box runs past its right
   edge and over its neighbours. For a paragraph you must set
   `word_wrap="true"` **and** give `height = 16 * lines`.
3. **Count the lines yourself.** At `SansSerifSmall`, roughly **70 characters**
   fit per line across a ~360px column. Divide, round up, add one line of
   margin for German and Russian — the `xui/de` and `xui/ru` overrides inherit
   your rect unless they override it, and translations run 20-35% longer.
4. `<text>` default `v_pad="0"`, `h_pad="0"`. `<text_editor>` defaults
   `h_pad="6" v_pad="4"` (`simple_text_editor.xml`), so its first usable line
   starts 4px down — a 3-line editor is `16*3 + 8 = 56`, not 48.
5. `text_editor` defaults `word_wrap="false"` and `allow_scroll="true"`, so it
   *does* scroll rather than overdraw. `text` defaults `allow_scroll="false"`.
6. `max_length` defaults to **255** on `text_editor`/`textbase` and 4096 on
   `text`. Longer content is truncated, not scrolled.
7. When the string is genuinely unknown at design time (a name, a server
   message), either use `expandable_text`, or call
   `LLTextBox::reshapeToFitText()` from `postBuild()` and give the siblings
   below it `follows` that tracks the move. There is no declarative option.

## 3. Standard widget heights

Measured over all 632 `en/` XUI files; the modal value is the house style.

| Widget | Height | Notes |
|---|---|---|
| `text` | **16** | one line, `SansSerifSmall` |
| `check_box` | **16** | label baseline is fixed at `bottom="3"` inside the box |
| `slider` | **16** | 320 uses, 247 at 16 |
| `icon` | **16** | match the adjacent text row |
| `button` | **23** | the widget default (`button.xml`); 20 and 18 for dense/toolbar rows |
| `combo_box` | **23** | the widget default; 18/20 in dense rows |
| `line_editor` | **20** | 23 when paired with a 23px button |
| `spinner` | **20** | 19 also common |
| `filter_editor` / `search_editor` | **23** / **22** | |
| `color_swatch` | **24** | small; 80+ for a picker patch |
| `progress_bar` | **16** | |
| `texture_picker` | **103** | the standard thumbnail; 80 small, 115 large |
| `view_border` | **0** | the divider idiom: `height="0" bevel_style="none"` draws one rule |
| `accordion_tab` | inner panel + **29** | see §5 |
| floater header | **25** | `header_height`, `header_vpad="5"` |

Horizontal gaps: `left_pad="5"` between related controls, `10` between groups.
Vertical: `top_pad="5"` within a group, `10` between groups. Both are the modal
values in the tree. Content inset from a panel edge: `left="8"` (or `10`) and
`right="-8"`.

Common floater sizes: 300×400, 400×500, 500×300. Nothing forces these; they are
just what users expect a viewer window to be.

## 4. Widths and clipping

- **Widths never reflow and overflow clips silently, with no scrollbar.**
- Usable inner width of a floater ≈ `width − 28` (border + resize margin +
  the conventional 8px insets). Subtract **15** more (`UIScrollbarSize`)
  whenever a scroll container or accordion can show a vertical scrollbar.
- **Size a button row for the floater's `min_width`, not its default width.** A
  row laid out against a 460px default loses its last button the moment the user
  narrows the floater to 420.
- **A button's `width` must fit its own label.** No reflow, no ellipsis — the
  label is cut. An `image_overlay` eats ~18px before the text starts. Prefer
  wording the viewer already uses over a longer phrase you invent.
- `min_width` is the only declarative way to widen an *existing* user's floater
  (`LLFloater::applyRectControl` prefers the saved rect over the XUI width), and
  it force-widens everyone permanently. Design to the current width instead.
- Sliders: `label_width` reserves the label column; `text_width` reserves the
  value box. **Any slider with `max_val` below 1.0 or ≤ 0 must set an explicit
  `text_width`** (56 fits a signed 4-decimal value) or `LLSliderCtrl` sizes the
  box from `log10(max_val)` and truncates the number.

## 5. Containers

### `accordion_tab`

`height` **must** equal the inner panel's height **+ 29** (25px header +
`padding_top` 2 + `padding_bottom` 2). The tab's rect *is* its expanded height;
`fit_panel="true"` squeezes the panel into what remains. An undersized tab clips
the bottom rows and the overflow draws over the sections below — panels do not
clip their children.

Inner panel height = walk the `top_pad` chain to the **last widget's bottom**,
then add 8 for the bottom inset.

### `layout_stack` / `layout_panel`

The stack distributes along one axis; the other axis is the parent's.

- `auto_resize` defaults **true**, `user_resize` defaults **false**.
- `min_dim` / `max_dim` default −1 (unset). `min_width`/`min_height` are
  synonyms; the stack reads whichever matches its `orientation`.
- A fixed-size pane needs `auto_resize="false"` **and** a `height` (or `width`)
  — auto-resize ignores your declared size.
- `border_size` (alias `drag_handle_gap`) defaults to `UIResizeBarHeight` = **3**
  and is inserted *between* panels, so N panels cost `3 * (N-1)` extra.
- A `layout_panel` with `visible="false"` collapses to 0 and the others absorb
  its space; that is the correct way to hide a pane, not `height="0"`.

### `follows`

Sets which parent edges the widget is pinned to on resize. `left|top` (the
default for a fixed widget), `left|top|right` for anything that should stretch
horizontally, `all` for a pane that fills. **A widget whose parent can resize
and that has only `left|top` will sit still while everything around it moves.**

## 6. Check it without launching — tooling

`V:\Scratch\xui\` holds a resolver that ports the C++ rect arithmetic
(`llview.cpp:2620-2790`, `llui.cpp:637-700`), verified against §5's worked
example and clean on all four `panel_lightbox_*.xml`. Stdlib Python, no venv.

```
python V:/Scratch/xui/xui_dump.py <file>              # resolved rects, top-down
python V:/Scratch/xui/xui_lint.py <file>              # findings
python V:/Scratch/xui/xui_lint.py --changed           # git-dirty XUI only
python V:/Scratch/xui/xui_lint.py <file> --fix        # accordion_tab heights
```

`xui_dump.py` is the one to reach for when a layout looks wrong: it prints each
widget's `bot`, which is the number the next sibling's `top_pad` chains from.

`xui_lint.py` errors on the three things that are wrong on their own terms —
`accordion-tab-height` (≠ panel + 29), `panel-too-short` (a child bottoms out
past the declared height), `slider-text-width` (§4) — warns on resolved
overflow, and hints on string-width estimates. Exit status 1 on any error, so
it drops into a hook. See `V:\Scratch\xui\README.md` for the full check list and
for what it deliberately skips (layout_stack / tab_container / scroll container
children, and any box whose height was never pinned).

`--fix` only rewrites `accordion_tab` heights, because that number is derived
rather than chosen. Everything else is a judgement call and is left to you.

Without access to `V:\`, the manual version: walk each panel's children in
declaration order, track `top = prev_bottom + top_pad` (or the absolute `top`),
and assert panel `height` ≥ lowest child bottom + 8, `accordion_tab` `height`
== panel + 29, and no child's right edge past parent width − 8 (− 15 more if
scrollable). Then check the narrowest real case: the floater at its `min_width`
**with** the vertical scrollbar visible.

## 7. Skooma conventions

- Skooma XUI is `floater_sk_*.xml` / `panel_sk_*.xml` with widget names `sk_*`,
  following Alchemy's `floater_al_*.xml` (e.g. `floater_sk_map.xml`,
  `floater_sk_tactical_options.xml`, `panel_sk_atmo_*.xml`).
- Edits to inherited (LL/FS/AL) XUI files are wrapped in
  `<!-- <SK:Nexii> ... --> ... <!-- </SK:Nexii> -->` and recorded in
  [`INJECTIONS.md`](INJECTIONS.md). This applies on every branch, `feature-*`
  included -- a marker tells the next reader of the *merged* tree what is ours,
  which a diff against `next` cannot once the work has landed (see
  `CLAUDE.local.md`).
- English strings live in `xui/en/`. Do not hand-edit the other locales; they
  inherit your rects, which is why §2 rule 3 matters.
