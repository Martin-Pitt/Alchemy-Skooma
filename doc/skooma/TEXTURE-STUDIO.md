# Texture Studio — index

## What it is

An in-viewer node-graph tool for content creators. Graphs are built from GLSL SDF shapes (Pixel Spirit Deck style), repeat and mirror ops (Arcanigen) and Blender-style texture nodes. They bake to SL textures or a GLTF PBR set, preview live on a prim through local bitmaps, and upload in one click.

## Read in this order

| doc | what | status |
|---|---|---|
| [.proctex-spec.md](.proctex-spec.md) | Spec and branch plan: architecture §3, nodes §4, bake §5, apply/upload §6, UI §8, plan §10, open questions §11 | draft 2026-10-21, unreviewed |

## Decisions at a glance

- Hybrid evaluation: per-pixel chains are generated as one fragment shader, and buffer nodes (blur, height→normal, warp) cut the graph into render targets. Params are uniforms, so a slider drag never recompiles.
- Blender's types, socket colours and implicit conversions, plus an SDF type.
- Tileable by default (periodic noise, wrap sampling).
- Pixel Spirit Deck's `lib/*.glsl` are BSD-2 and usable, but we write our own on the same maths (ruling 2026-10-21). LYGIA (non-commercial) is a reference only. Material Maker (MIT) may be borrowed from with attribution.
- Core `indra/sktexgraph` is free of UI and tested headless; the GL, the floater and the canvas live in newview.
