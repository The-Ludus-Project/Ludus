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
QByteArray ComputeRenderGraphBuildIdentity(const RenderGraphDocument& document, const QString& configuration)
{
    QCryptographicHash identity(QCryptographicHash::Sha256);
    const QByteArray configurationText = configuration.toUtf8();
    identity.addData(QByteArray::number(configurationText.size()));
    identity.addData(QByteArrayLiteral(":"));
    identity.addData(configurationText);
    for (const QString& file : document.GetFiles())
    {
        QByteArray bytes = document.GetDraft(file);
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
void RenderGraphLiveBuild::Schedule(const QByteArray& identity)
{
    mCurrentIdentity = identity;
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
}
void RenderGraphLiveBuild::schedule()
{
    if (mIsEnabled == true && mCurrentIdentity.isEmpty() == false && mCurrentIdentity != mLastRequestedIdentity)
    {
        mTimer.start();
    }
}
} // namespace ludus::editor
