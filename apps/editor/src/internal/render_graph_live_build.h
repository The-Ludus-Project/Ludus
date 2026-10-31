#pragma once

#include "internal/render_graph_source.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

namespace ludus::editor
{
class RenderGraphDocument;
/// Stable source/configuration digest; layout-only graph edits are excluded. The package location
/// and built-in state are included, so identical sources in another package still build.
[[nodiscard]] QByteArray ComputeRenderGraphBuildIdentity(const RenderGraphDocument& document,
                                                         const QString& configuration,
                                                         RenderGraphSource source = RenderGraphSource::Draft);
/// Debounce and supersession for saved-source preview builds. Owns no documents, files,
/// compiler or viewport. The workspace owns admission and result publication.
class RenderGraphLiveBuild final : public QObject
{
    Q_OBJECT
public:
    explicit RenderGraphLiveBuild(QObject* parentOrNull = nullptr);
    void SetEnabled(bool isEnabled);
    void SetBusy(bool isBusy);
    /// Already debounced file notifications queue a build on the next event turn.
    void Schedule(const QByteArray& identity, bool hasDebouncedSource = false);
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
    bool mHasDebouncedSource = false;
};
} // namespace ludus::editor
