#pragma once

#include <QLatin1String>
#include <QString>

namespace ludus::editor
{
/// Root graph of the engine default and of a project's customized copy.
inline constexpr QLatin1String DEFAULT_RENDER_GRAPH_ROOT("main.lurender.json");
/// Project-relative package directory that a customized default renderer is saved to, in the
/// project's graphs folder beside graphs made with New Graph. A project whose
/// `graphs/Main/main.lurender.json` exists opens that graph instead of the default.
inline constexpr QLatin1String PROJECT_RENDER_GRAPH_DIRECTORY("graphs/Main");

// The engine-owned default render graph that lets the Render Graph document work without setup (see
// docs/development/render-graph-authoring.md#start-customizing-the-renderer). The editor
// embeds its authored sources and, in native preview builds, its prebuilt runtime
// generation. Nothing here reads or writes project files.

/// Read-only Qt resource directory holding the default graph's authored sources.
[[nodiscard]] QString GetDefaultRenderGraphSource();

/// Absolute path of the prebuilt default `.lurgrt`, or empty when this editor was built
/// without the native preview or the build tree no longer holds the file.
[[nodiscard]] QString GetDefaultRenderGraphRuntime();
} // namespace ludus::editor
