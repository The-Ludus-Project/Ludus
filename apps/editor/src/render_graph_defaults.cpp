#include "internal/render_graph_defaults.h"

#include <QFileInfo>

namespace ludus::editor
{
namespace
{
#if defined(LUDUS_EDITOR_DEFAULT_RENDER_GRAPH_RUNTIME)
const QString DEFAULT_RUNTIME = QStringLiteral(LUDUS_EDITOR_DEFAULT_RENDER_GRAPH_RUNTIME);
const QString EDITOR_HELPER = QStringLiteral(LUDUS_EDITOR_SHADER_HELPER);
const QString EDITOR_COMPILER = QStringLiteral(LUDUS_EDITOR_SLANG_COMPILER);
const QString EDITOR_VALIDATOR = QStringLiteral(LUDUS_EDITOR_SPIRV_VALIDATOR);
const QString EDITOR_SPIRV_CROSS = QStringLiteral(LUDUS_EDITOR_SPIRV_CROSS);
#else
const QString DEFAULT_RUNTIME;
const QString EDITOR_HELPER;
const QString EDITOR_COMPILER;
const QString EDITOR_VALIDATOR;
const QString EDITOR_SPIRV_CROSS;
#endif

/// A build-tree path is offered only while it still exists on this machine.
QString GetExistingPath(const QString& path)
{
    return path.isEmpty() == false && QFileInfo::exists(path) == true ? path : QString();
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
