#pragma once

#include "internal/render_graph_recovery_store.h"

#include <QByteArray>
#include <QString>
#include <QTimer>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Throttled local checkpoint/review surface. Borrows the authoritative document,
/// which must outlive it. Checkpoints never publish or build sources. GUI thread only.
class RenderGraphRecovery final : public QWidget
{
    Q_OBJECT
public:
    explicit RenderGraphRecovery(RenderGraphDocument& inoutDocument, QWidget* parentOrNull = nullptr);
    /// Observe a new revision/binding and schedule a checkpoint without restarting the
    /// timer on each keystroke. Clean states remove only this session's/consumed checkpoints.
    void Update();
    /// Update whether the host permits restoration (for example, no running build).
    void SetCanRestore(bool canRestore);
    /// Flush before document replacement/destruction. Explicit discard suppresses that flush.
    void Leave();
    /// Respect an explicit Discard decision. If another owner's close prompt is cancelled,
    /// the still-live draft resumes checkpointing on the next timer tick or edit.
    void DiscardCurrent();
    /// Immediate checkpoint for lifecycle boundaries; false leaves the last checkpoint intact.
    /// Force refresh after a failed/partial Save, whose saved-byte baseline may change
    /// without advancing the draft revision.
    [[nodiscard]] bool CheckpointNow(bool shouldForce = false);
    /// Explicit restoration, one history entry. Failure preserves draft and checkpoint.
    /// An optional digest binds the operation to the exact bytes shown in the review.
    [[nodiscard]] bool Restore(const QString& path, const QByteArray& expectedDigest = {});
    /// Open the checkpoint chooser, read-only comparison, Restore/Save Copy/Discard controls.
    void Review();
Q_SIGNALS:
    /// Host job availability changed while a recovery review is open.
    void AvailabilityChanged();
    /// A checkpoint became the current dirty draft; the host refreshes its normal views/history.
    void Restored();

private:
    void refreshStatus();
    void removeOwned();
    RenderGraphDocument& mDocument;
    RenderGraphRecoveryStore mStore;
    QTimer mTimer;
    QLabel* mStatus;
    QPushButton* mReview;
    QString mBinding;
    QString mOwnPath;
    QByteArray mOwnDigest;
    QString mConsumedPath;
    QByteArray mConsumedDigest;
    QString mError;
    foundation::uint64 mObservedRevision = 0;
    foundation::uint64 mCheckpointRevision = 0;
    bool mIsDiscardPending = false;
    bool mCanRestore = true;
};
} // namespace ludus::editor
