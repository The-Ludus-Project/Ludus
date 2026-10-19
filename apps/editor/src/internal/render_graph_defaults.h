#pragma once

#include <QString>

namespace ludus::editor
{
/// Shader tools for a render-graph build. Empty entries are not passed to the CLI.
struct RenderGraphBuildTools final
{
    QString Helper;     ///< Complete SDK shader helper directory (`<sdk>/lib/cmake/Ludus/shaders`).
    QString Compiler;   ///< Pinned slangc executable.
    QString Validator;  ///< Pinned spirv-val; required only for SPIR-V/GLSL profiles.
    QString SpirvCross; ///< Verified SPIRV-Cross; required only for the GLSL profile.
};

/// Root graph of the engine default and of a project's customized copy.
inline const QString DEFAULT_RENDER_GRAPH_ROOT = QStringLiteral("main.lurender.json");
/// Project-relative package directory that a customized default renderer is saved to. A
/// project whose `renderer/main.lurender.json` exists uses that graph instead of the default.
inline const QString PROJECT_RENDER_GRAPH_DIRECTORY = QStringLiteral("renderer");

// Defaults that let the Renderer tab work without setup (see
// docs/development/render-graph-authoring.md#start-customizing-the-renderer).
// The editor embeds the engine-owned default render graph's authored sources and, in
// native preview builds, its prebuilt runtime generation plus the shader tools that
// built it. Nothing here reads or writes project files.

/// Read-only Qt resource directory holding the default graph's authored sources.
[[nodiscard]] QString GetDefaultRenderGraphSource();

/// Absolute path of the prebuilt default `.lurgrt`, or empty when this editor was built
/// without the native preview or the build tree no longer holds the file.
[[nodiscard]] QString GetDefaultRenderGraphRuntime();

/// Tools the editor build used for its own render graphs. Each entry is empty when this
/// build has none (no native preview, or the path no longer exists on this machine).
[[nodiscard]] RenderGraphBuildTools GetEditorBuildTools();

/// Per-entry choice: a non-empty manual override wins, otherwise the default is used.
[[nodiscard]] RenderGraphBuildTools ResolveBuildTools(const RenderGraphBuildTools& overrides,
                                                      const RenderGraphBuildTools& defaults);
} // namespace ludus::editor
