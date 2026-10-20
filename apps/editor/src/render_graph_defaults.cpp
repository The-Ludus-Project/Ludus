#include "internal/render_graph_defaults.h"

#include <QDir>
#include <QFileInfo>
#include <QUtf8StringView>

namespace ludus::editor
{
namespace
{
// Absolute UTF-8 build-tree path from CMake (apps/editor/CMakeLists.txt); empty without it.
#if defined(LUDUS_EDITOR_DEFAULT_RENDER_GRAPH_RUNTIME)
constexpr QUtf8StringView DEFAULT_RUNTIME = LUDUS_EDITOR_DEFAULT_RENDER_GRAPH_RUNTIME;
#else
constexpr QUtf8StringView DEFAULT_RUNTIME;
#endif
} // namespace

QString ResolveProjectRenderGraph(const QString& projectRoot)
{
    if (projectRoot.isEmpty() == true)
    {
        return {};
    }
    const QDir graphs(QDir(projectRoot).filePath(QStringLiteral("graphs")));
    const QString main = graphs.filePath(QStringLiteral("Main/") + DEFAULT_RENDER_GRAPH_ROOT);
    if (QFileInfo::exists(main) == true)
    {
        return main;
    }
    const QFileInfoList directories =
        graphs.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo& directory : directories)
    {
        const QString candidate = QDir(directory.absoluteFilePath()).filePath(DEFAULT_RENDER_GRAPH_ROOT);
        if (QFileInfo::exists(candidate) == true)
        {
            return candidate;
        }
    }
    return {};
}
QString GetDefaultRenderGraphSource()
{
    return QStringLiteral(":/render-graph/default");
}

QString GetDefaultRenderGraphRuntime()
{
    // A build-tree path is offered only while it still exists on this machine.
    const QString path = DEFAULT_RUNTIME.toString();
    return path.isEmpty() == false && QFileInfo::exists(path) == true ? path : QString();
}
} // namespace ludus::editor
