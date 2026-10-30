#pragma once

// Editor icon system (docs/architecture/editor-art-direction.md, Iconography).
//
// Icons are monochrome outline drawings on a 24 unit grid, shipped as the Lucide SVG files under
// apps/editor/icons (ISC licence, see apps/editor/icons/LICENSE). Each file is parsed once, on first use,
// into a QPainterPath; no SVG is parsed while painting. A QIconEngine rasterises that path with the
// application palette (the palette is built from the design tokens, so icons follow the theme and the
// high-contrast variants) and keeps the pixmaps in one bounded cache keyed by icon, size, device pixel ratio,
// mode and colour.

#include <ludus/foundation/base/types.h>

#include <QIcon>
#include <QPainterPath>
#include <QString>
#include <QStringView>

class QByteArray;

namespace ludus::editor
{
/// Every icon the Editor draws. Names follow the concept, not the picture.
enum class EditorIcon : foundation::uint8
{
    OpenProject,
    Save,
    Play,
    Build,
    Run,
    Stop,
    Folder,
    ProjectSettings,
    World,
    RenderGraph,
    Code,
    Audio,
    Content,
    Configuration,
    Error,
    Warning,
    Information,
    Output,
    Timing,
    Profiling,
    LiveInspector,
    RayPaths,
    Validate,
    Preview,
    LivePreview,
    Cancel,
};

/// Number of icons in EditorIcon.
inline constexpr foundation::usize EDITOR_ICON_COUNT = 26;

/// Side of the grid every icon is drawn on, in SVG units.
inline constexpr qreal EDITOR_ICON_GRID = 24.0;

/// Stroke width on EDITOR_ICON_GRID, in SVG units.
inline constexpr qreal EDITOR_ICON_STROKE_WIDTH = 1.5;

/// Logical side, in pixels, of toolbar and list icons.
inline constexpr int EDITOR_ICON_SIZE = 20;

/// Parse SVG path data (the `d` attribute) into outPath. Supports M, L, H, V, C, S, Q, T, A and Z in both cases.
/// Returns false and leaves outPath empty when the data is malformed.
[[nodiscard]] bool TryParseIconPathData(QStringView pathData, QPainterPath& outPath);

/// Parse a Lucide style SVG document (24 by 24 viewBox with path, circle and rect elements) into outPath.
/// Returns false and leaves outPath empty when the document is malformed or uses anything else.
[[nodiscard]] bool TryParseIconSvg(const QByteArray& document, QPainterPath& outPath);

/// The outline of icon on EDITOR_ICON_GRID, or an empty path when its resource cannot be parsed.
[[nodiscard]] const QPainterPath& GetEditorIconOutline(EditorIcon icon);

/// A QIcon that draws icon in the current palette: ButtonText for normal use and the disabled ButtonText when
/// disabled. The icon is cached; calling again returns the same engine data.
[[nodiscard]] QIcon GetEditorIcon(EditorIcon icon);

/// Number of rasterised pixmaps currently cached across all icons.
[[nodiscard]] int GetEditorIconCacheCount() noexcept;

/// Largest value GetEditorIconCacheCount can reach before the cache is cleared.
[[nodiscard]] int GetEditorIconCacheLimit() noexcept;

/// Drop every cached pixmap. Pixmaps are rebuilt on demand.
void ClearEditorIconCache();
} // namespace ludus::editor
