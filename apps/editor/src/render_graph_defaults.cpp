#include "internal/render_graph_defaults.h"

#include <QFileInfo>
#include <QUtf8StringView>

namespace ludus::editor
{
namespace
{
// Absolute UTF-8 build-tree paths from CMake (apps/editor/CMakeLists.txt); empty without them.
#if defined(LUDUS_EDITOR_DEFAULT_RENDER_GRAPH_RUNTIME)
constexpr QUtf8StringView DEFAULT_RUNTIME = LUDUS_EDITOR_DEFAULT_RENDER_GRAPH_RUNTIME;
constexpr QUtf8StringView EDITOR_HELPER = LUDUS_EDITOR_SHADER_HELPER;
constexpr QUtf8StringView EDITOR_COMPILER = LUDUS_EDITOR_SLANG_COMPILER;
constexpr QUtf8StringView EDITOR_VALIDATOR = LUDUS_EDITOR_SPIRV_VALIDATOR;
constexpr QUtf8StringView EDITOR_SPIRV_CROSS = LUDUS_EDITOR_SPIRV_CROSS;
#else
constexpr QUtf8StringView DEFAULT_RUNTIME;
constexpr QUtf8StringView EDITOR_HELPER;
constexpr QUtf8StringView EDITOR_COMPILER;
constexpr QUtf8StringView EDITOR_VALIDATOR;
constexpr QUtf8StringView EDITOR_SPIRV_CROSS;
#endif

/// A build-tree path is offered only while it still exists on this machine.
QString GetExistingPath(QUtf8StringView path)
{
    const QString text = path.toString();
    return text.isEmpty() == false && QFileInfo::exists(text) == true ? text : QString();
}

QString Choose(const QString& manual, const QString& fallback)
{
    return manual.trimmed().isEmpty() == false ? manual.trimmed() : fallback;
}
} // namespace

QString GetDefaultRenderGraphSource()
{
    return QStringLiteral(":/render-graph/default");
}

QString GetDefaultRenderGraphRuntime()
{
    return GetExistingPath(DEFAULT_RUNTIME);
}

RenderGraphBuildTools GetEditorBuildTools()
{
    return
    {
        .Helper = GetExistingPath(EDITOR_HELPER),
        .Compiler = GetExistingPath(EDITOR_COMPILER),
        .Validator = GetExistingPath(EDITOR_VALIDATOR),
        .SpirvCross = GetExistingPath(EDITOR_SPIRV_CROSS),
    };
}

RenderGraphBuildTools ResolveBuildTools(const RenderGraphBuildTools& overrides, const RenderGraphBuildTools& defaults)
{
    return
    {
        .Helper = Choose(overrides.Helper, defaults.Helper),
        .Compiler = Choose(overrides.Compiler, defaults.Compiler),
        .Validator = Choose(overrides.Validator, defaults.Validator),
        .SpirvCross = Choose(overrides.SpirvCross, defaults.SpirvCross),
    };
}
} // namespace ludus::editor
