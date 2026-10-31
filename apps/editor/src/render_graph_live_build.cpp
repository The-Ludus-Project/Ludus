#include "internal/render_graph_live_build.h"
#include "internal/render_graph_document.h"

#include <ludus/foundation/base/types.h>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 DEBOUNCE_MILLISECONDS = 250;
}
QByteArray ComputeRenderGraphBuildIdentity(const RenderGraphDocument& document,
                                           const QString& configuration,
                                           RenderGraphSource source)
{
    QCryptographicHash identity(QCryptographicHash::Sha256);
    // Where the sources live is part of the build: a project copy of the engine default has the
    // same bytes as the prebuilt default but must still be cooked into its own package.
    const QByteArray location =
        (document.IsBuiltIn() == true ? QByteArrayLiteral("built-in:") : QByteArrayLiteral("project:")) +
        document.GetPackageRoot().toUtf8() + QByteArrayLiteral("/") + document.GetRootGraph().toUtf8();
    identity.addData(QByteArray::number(location.size()));
    identity.addData(QByteArrayLiteral(":"));
    identity.addData(location);
    const QByteArray configurationText = configuration.toUtf8();
    identity.addData(QByteArray::number(configurationText.size()));
    identity.addData(QByteArrayLiteral(":"));
    identity.addData(configurationText);
    for (const QString& file : document.GetFiles(source))
    {
        QByteArray bytes = source == RenderGraphSource::Saved ? document.GetSaved(file) : document.GetDraft(file);
        if (file.endsWith(QStringLiteral(".lurender.json")) == true)
        {
            const QJsonDocument parsed = QJsonDocument::fromJson(bytes);
            if (parsed.isObject() == true)
            {
                QJsonObject graph = parsed.object();
                QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
                for (qsizetype i = 0; i < configurations.size(); ++i)
                {
                    QJsonObject selected = configurations[i].toObject();
                    selected.remove(QStringLiteral("layout"));
                    configurations[i] = selected;
                }
                graph.insert(QStringLiteral("configurations"), configurations);
                bytes = QJsonDocument(graph).toJson(QJsonDocument::Compact);
            }
        }
        const QByteArray path = file.toUtf8();
        identity.addData(QByteArray::number(path.size()));
        identity.addData(QByteArrayLiteral(":"));
        identity.addData(path);
        identity.addData(QByteArray::number(bytes.size()));
        identity.addData(QByteArrayLiteral(":"));
        identity.addData(bytes);
    }
    return identity.result();
}
RenderGraphLiveBuild::RenderGraphLiveBuild(QObject* parentOrNull) : QObject(parentOrNull)
{
    mTimer.setSingleShot(true);
    mTimer.setInterval(DEBOUNCE_MILLISECONDS);
    connect(&mTimer, &QTimer::timeout, this, [this]() {
        if (mIsEnabled == false || mCurrentIdentity == mLastRequestedIdentity)
        {
            return;
        }
        if (mIsBusy == true)
        {
            mTimer.start();
            return;
        }
        Q_EMIT BuildRequested();
    });
}
void RenderGraphLiveBuild::SetEnabled(bool isEnabled)
{
    mIsEnabled = isEnabled;
    if (isEnabled == false)
    {
        mTimer.stop();
    }
    else
    {
        schedule();
    }
}
void RenderGraphLiveBuild::SetBusy(bool isBusy)
{
    mIsBusy = isBusy;
    if (isBusy == false)
    {
        schedule();
    }
}
void RenderGraphLiveBuild::Schedule(const QByteArray& identity, bool hasDebouncedSource)
{
    mCurrentIdentity = identity;
    mHasDebouncedSource = hasDebouncedSource;
    schedule();
}
void RenderGraphLiveBuild::MarkRequested(const QByteArray& identity)
{
    mLastRequestedIdentity = identity;
}
void RenderGraphLiveBuild::Reset()
{
    mTimer.stop();
    mCurrentIdentity.clear();
    mLastRequestedIdentity.clear();
    mHasDebouncedSource = false;
}
void RenderGraphLiveBuild::schedule()
{
    if (mIsEnabled == true && mCurrentIdentity.isEmpty() == false && mCurrentIdentity != mLastRequestedIdentity)
    {
        mTimer.start(mHasDebouncedSource == true ? 0 : DEBOUNCE_MILLISECONDS);
    }
}
} // namespace ludus::editor
