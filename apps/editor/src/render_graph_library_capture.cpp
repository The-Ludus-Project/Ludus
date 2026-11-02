#include "internal/render_graph_library_capture.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace ludus::editor
{
QString RenderGraphCaptureTarget::GetLabel() const
{
    return QStringLiteral("%1#%2.%3").arg(Library, Configuration, Output);
}

QVector<RenderGraphCaptureTarget> ListCaptureTargets(const RenderGraphSourceBytes& files, const QString& rootGraph)
{
    QVector<RenderGraphCaptureTarget> targets;
    for (RenderGraphSourceBytes::const_iterator file = files.constBegin(); file != files.constEnd(); ++file)
    {
        if (file.key() == rootGraph || file.key().endsWith(QStringLiteral(".lurender.json")) == false)
        {
            continue;
        }
        const QJsonObject graph = QJsonDocument::fromJson(file.value()).object();
        for (const QJsonValueConstRef configuration : graph.value(QStringLiteral("configurations")).toArray())
        {
            const QJsonObject record = configuration.toObject();
            for (const QJsonValueConstRef output : record.value(QStringLiteral("outputs")).toArray())
            {
                if (output.toObject().value(QStringLiteral("type")).toString() == QStringLiteral("ColorTexture"))
                {
                    targets.append({file.key(),
                                    record.value(QStringLiteral("id")).toString(),
                                    output.toObject().value(QStringLiteral("id")).toString()});
                }
            }
        }
    }
    return targets;
}

RenderGraphLibraryCapture::RenderGraphLibraryCapture(QObject* parentOrNull) : QObject(parentOrNull) {}

void RenderGraphLibraryCapture::Request(const QByteArray& generationIdentity, const RenderGraphCaptureTarget& target)
{
    if (isCurrent(generationIdentity, target) == true &&
        (mState == LibraryCaptureState::Building || mState == LibraryCaptureState::Ready))
    {
        return;
    }
    mGenerationIdentity = generationIdentity;
    mTarget = target;
    mPackagePath.clear();
    mProfilePath.clear();
    mError.clear();
    mState = LibraryCaptureState::Building;
    Q_EMIT Changed();
    // The workspace may answer synchronously, for a cached library capture or a rejected job.
    Q_EMIT BuildRequested(generationIdentity, target);
}

void RenderGraphLibraryCapture::AcceptBuild(const QByteArray& generationIdentity,
                                            const RenderGraphCaptureTarget& target,
                                            const QString& packagePath,
                                            const QString& profilePath)
{
    if (isCurrent(generationIdentity, target) == false || mState != LibraryCaptureState::Building)
    {
        return;
    }
    mPackagePath = packagePath;
    mProfilePath = profilePath;
    mState = LibraryCaptureState::Ready;
    Q_EMIT Changed();
}

void RenderGraphLibraryCapture::RejectBuild(const QByteArray& generationIdentity,
                                            const RenderGraphCaptureTarget& target,
                                            const QString& reason)
{
    if (isCurrent(generationIdentity, target) == false || mState != LibraryCaptureState::Building)
    {
        return;
    }
    mError = reason;
    mState = LibraryCaptureState::Failed;
    Q_EMIT Changed();
}

bool RenderGraphLibraryCapture::IsReadyFor(const QByteArray& generationIdentity,
                                           const RenderGraphCaptureTarget& target) const noexcept
{
    return mState == LibraryCaptureState::Ready && isCurrent(generationIdentity, target) == true;
}

bool RenderGraphLibraryCapture::isCurrent(const QByteArray& generationIdentity,
                                          const RenderGraphCaptureTarget& target) const noexcept
{
    return generationIdentity == mGenerationIdentity && target == mTarget;
}
} // namespace ludus::editor
