#pragma once

// Render-graph build settings (private editor header, not installed).
//
// Builds normally resolve the shader helper and compilers from the project's selected
// SDK and the editor's prepared tools. These settings are the advanced manual overrides
// and the optional native shader worker, edited in Tools > Options and applied by
// RendererWorkspace to the next build. See
// docs/development/render-graph-authoring.md#build-settings.

#include <QString>

namespace ludus::editor
{
/// Shader tool paths of one render-graph build. An empty path means "not set".
struct RenderGraphBuildTools final
{
    /// Directory of the SDK shader helper scripts (`--helper`).
    QString Helper;
    /// Slang compiler executable (`--compiler`).
    QString Compiler;
    /// SPIR-V validator executable (`--validator`), needed for SPIR-V and GLSL profiles.
    QString Validator;
    /// SPIRV-Cross executable (`--spirv-cross`), needed for the GLSL profile.
    QString SpirvCross;

    [[nodiscard]] bool operator==(const RenderGraphBuildTools&) const = default;
};

/// User build settings that RendererWorkspace applies to every later build.
struct RenderGraphBuildSettings final
{
    /// True to pass Tools instead of resolving them from the project's selected SDK.
    /// An override needs at least Tools.Helper and Tools.Compiler.
    bool ShouldOverrideTools = false;
    /// Manual tool paths; ignored unless ShouldOverrideTools is true.
    RenderGraphBuildTools Tools;
    /// Existing `ludus-shader-worker` socket, or empty for the offline compiler path.
    /// It is an editor preference, kept across projects.
    QString WorkerSocket;

    [[nodiscard]] bool operator==(const RenderGraphBuildSettings&) const = default;
};
} // namespace ludus::editor
