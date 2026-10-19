#include "internal/render_graph_source_watch.h"
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_document.h"

#include <QDir>
#include <QFileInfo>

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 DEBOUNCE_MILLISECONDS = 250;
}
RenderGraphSourceWatch::RenderGraphSourceWatch(QObject* parentOrNull) : QObject(parentOrNull)
{
    mDebounce.setSingleShot(true);
    mDebounce.setInterval(DEBOUNCE_MILLISECONDS);
    connect(&mDebounce, &QTimer::timeout, this, &RenderGraphSourceWatch::SourcesChanged);
#if !defined(Q_OS_WASM)
    connect(&mWatcher, &QFileSystemWatcher::fileChanged, this, [this]() { mDebounce.start(); });
    connect(&mWatcher, &QFileSystemWatcher::directoryChanged, this, [this]() { mDebounce.start(); });
#endif
}
void RenderGraphSourceWatch::SetDocument(const RenderGraphDocument& document, const QString& projectRoot)
{
#if !defined(Q_OS_WASM)
    const QStringList previous = mWatcher.files() + mWatcher.directories();
    if (previous.isEmpty() == false)
    {
        mWatcher.removePaths(previous);
    }
    QStringList paths;
    if (projectRoot.isEmpty() == false)
    {
        paths.append(projectRoot);
        const QString graphs = QDir(projectRoot).filePath(QStringLiteral("graphs"));
        if (QFileInfo(graphs).isDir() == true)
        {
            paths.append(graphs);
        }
        const QString root = ResolveProjectRenderGraph(projectRoot);
        if (root.isEmpty() == false)
        {
            paths.append(root);
            paths.append(QFileInfo(root).absolutePath());
        }
    }
    if (document.IsLoaded() == true && document.IsBuiltIn() == false)
    {
        paths.append(document.GetPackageRoot());
    }
    for (const QString& file : document.IsBuiltIn() == false ? document.GetFiles() : QStringList{})
    {
        const QFileInfo information(QDir(document.GetPackageRoot()).filePath(file));
        if (information.isSymLink() == false)
        {
            if (information.exists() == true)
            {
                paths.append(information.absoluteFilePath());
            }
            paths.append(information.absolutePath());
        }
    }
    paths.removeDuplicates();
    if (paths.isEmpty() == false)
    {
        mWatcher.addPaths(paths);
    }
#else
    static_cast<void>(document);
    static_cast<void>(projectRoot);
#endif
}
} // namespace ludus::editor
