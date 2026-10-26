# Inventory Explorer — index

## What it is

A second inventory floater beside the existing one, built from the best of Finder, Windows 11 File Explorer and Thunar:
tabs, a button path bar with subfolder menus, a sidebar (Favorites, Inventory, Marketplace, Library), and Columns as the
default view, with Icons, List and a full-floater Gallery. Branch `feature-ui/inventory-explorer` (from `develop`, lands
into `ui`).

## Read in this order

| doc | what | status |
|---|---|---|
| [.inventory-explorer.md](.inventory-explorer.md) | Spec: rules §2, data §3, layout §4, sidebar §5, views §6, interaction §7, path bar and search §8, details §9, viewer mapping §10, branch plan §11, open questions §12 | draft 2026-10-26, unreviewed |

Prototype: `V:\Scratch\web\inventory-explorer\` (the reference for every behaviour; `#hash` demos per feature).

## Decisions at a glance

- Nothing new stored about inventory: only server fields (AIS) and what can be worked out from them. UI preferences are settings.
- Favourites are the AIS favourite flag; a texture's display aspect is a trailing `(w:h)` in its description.
- Columns is the default view; the sidebar is its first column. A click selects, double-click or → opens; a ghost column previews a folder, a details column shows an item.
- The view never jumps: strip and column scrolls are kept across redraws, and spare columns hold the strip's width.
- Default sort: Acquired, newest first; folders before items.
- Actions go through the existing inventory bridges and menus, so they match the Inventory floater.
