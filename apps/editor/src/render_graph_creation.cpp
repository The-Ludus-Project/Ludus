#include "internal/render_graph_creation.h"

#include "internal/render_graph_template_registry.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace ludus::editor
{
using namespace foundation;

RenderGraphCreation::RenderGraphCreation(QObject* parentOrNull) : QObject(parentOrNull)
{
    connect(&mJob, &RenderGraphJob::Finished, this, [this](const RenderGraphJobResult& result) {
        if (result.Request.Request != mRequest)
        {
            Q_EMIT Finished();
            return;
        }
        if (result.IsSuccessful == true)
        {
            Q_EMIT Created(mRootGraph, result.Request.ProjectEpoch);
        }
        else
        {
            Q_EMIT Failed(result.Message);
        }
        Q_EMIT Finished();
    });
}

bool RenderGraphCreation::Start(const ToolingPaths& tooling,
                                const QString& projectRoot,
                                const QString& name,
                                uint64 epoch,
                                QString& outError,
                                const QString& templateIdentity)
{
    if (mJob.IsActive() == true)
    {
        outError = QStringLiteral("Wait for graph creation to finish.");
        return false;
    }
    if (RenderGraphTemplateRegistry::GetBuiltIn().HasTemplate(RenderGraphTemplateKind::Graph, templateIdentity) ==
        false)
    {
        outError = QStringLiteral("Select a built-in render graph template.");
        return false;
    }
    const QString project = QFileInfo(projectRoot).canonicalFilePath();
    const QRegularExpression identifier(QStringLiteral("\\A[A-Za-z][A-Za-z0-9_-]{0,62}\\z"));
    if (project.isEmpty() == true || QFileInfo(project).isDir() == false)
    {
        outError = QStringLiteral("Open a game project before creating a render graph.");
        return false;
    }
    if (identifier.match(name).hasMatch() == false)
    {
        outError = QStringLiteral("Use a name beginning with a letter, followed by letters, numbers, - or _ "
                                  "(up to 63 characters).");
        return false;
    }
    const QString graphs = QDir(project).filePath(QStringLiteral("graphs"));
    if (QFileInfo(graphs).isSymLink() == true ||
        (QFileInfo(graphs).exists() == true && QFileInfo(graphs).isDir() == false))
    {
        outError = QStringLiteral("The project's graphs folder must be a local directory, not a link or file.");
        return false;
    }
    const QString destination = QDir(graphs).filePath(name);
    if (QFileInfo(destination).exists() == true || QFileInfo(destination).isSymLink() == true)
    {
        outError =
            QStringLiteral("A graph folder named %1 already exists. Open its graph or choose another name.").arg(name);
        return false;
    }
    RenderGraphJobRequest request;
    request.Request = ++mRequest;
    request.ProjectEpoch = epoch;
    request.Action = QStringLiteral("new");
    request.Arguments = {destination,
                         QStringLiteral("--output"),
                         QStringLiteral("offscreen"),
                         QStringLiteral("--renderer-static"),
                         QStringLiteral("--template"),
                         templateIdentity,
                         QStringLiteral("--profiles")};
#if defined(Q_OS_MACOS)
    request.Arguments += {QStringLiteral("metal"), QStringLiteral("wgsl")};
#else
    request.Arguments += {QStringLiteral("spirv"), QStringLiteral("wgsl")};
#endif
    mRootGraph = QDir(destination).filePath(QStringLiteral("main.lurender.json"));
    if (mJob.Start(tooling, request) == false)
    {
        outError = QStringLiteral("Graph creation is unavailable. Check the editor's Python tooling.");
        return false;
    }
    outError.clear();
    return true;
}

void RenderGraphCreation::Cancel()
{
    ++mRequest;
    mJob.Cancel();
}

bool RenderGraphCreation::IsActive() const noexcept
{
    return mJob.IsActive();
}
} // namespace ludus::editor
