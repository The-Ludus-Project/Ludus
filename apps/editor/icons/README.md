# Editor icons

Outline icons for the Editor toolbars, Project Explorer and Error List, taken unchanged from
[Lucide](https://lucide.dev) 1.62.0 (commit `2ca268e74888d3506b3319fb8f94aaaaf70ceca9` of
[lucide-icons/lucide](https://github.com/lucide-icons/lucide)). Lucide is ISC licensed; some of its drawings derive
from Feather (MIT). `LICENSE` is Lucide's licence file, which carries both notices, and must ship with the icons.

The files are Qt resources under `:/icons`. `src/editor_icons.cpp` parses each one once, on first use, into a
`QPainterPath` on a 24 unit grid and draws it with a 1.5 unit round stroke in the palette's button text colour, so no SVG
is parsed while painting and no Qt SVG module is needed. The parser supports the `path`, `circle` and `rect` elements
and the M, L, H, V, C, S, Q, T, A and Z path commands that these icons use; a new icon that needs anything else fails the
icon tests.

To add an icon: copy its file from the pinned Lucide commit into this folder, add it to `EditorIcon` and to
`ICON_DESCRIPTORS` in `src/editor_icons.cpp`, and keep `EDITOR_ICON_COUNT` in step. Engine concepts that Lucide has no
drawing for (entity, probe, cook) are drawn in house on the same grid and stroke, and are listed here when added.
