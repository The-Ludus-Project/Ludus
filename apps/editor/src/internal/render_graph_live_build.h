#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

namespace ludus::editor
{
class RenderGraphDocument;
/// Stable source/configuration digest; layout-only graph edits are excluded.
[[nodiscard]] QByteArray ComputeRenderGraphBuildIdentity(const RenderGraphDocument& document,
                                                         const QString& configuration);
/// Debounce and supersession for live draft builds. Owns no documents, files,
/// compiler or viewport. The workspace owns admission and result publication.
class RenderGraphLiveBuild final : public QObject
{
    Q_OBJECT
public:
    explicit RenderGraphLiveBuild(QObject* parentOrNull = nullptr);
    void SetEnabled(bool isEnabled);
    void SetBusy(bool isBusy);
    void Schedule(const QByteArray& identity);
    void MarkRequested(const QByteArray& identity);
    void Reset();
Q_SIGNALS:
    void BuildRequested();

private:
    void schedule();
    QTimer mTimer;
    QByteArray mCurrentIdentity;
    QByteArray mLastRequestedIdentity;
    bool mIsEnabled = false;
    bool mIsBusy = false;
};
} // namespace ludus::editor
