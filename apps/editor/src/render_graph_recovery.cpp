#include "internal/render_graph_recovery.h"

#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 CHECKPOINT_INTERVAL_MILLISECONDS = 3000;
constexpr foundation::int32 REVIEW_WIDTH = 1000;
constexpr foundation::int32 REVIEW_HEIGHT = 650;

QString GetRecoveryDirectory()
{
    const QString configured = qEnvironmentVariable("LUDUS_EDITOR_RECOVERY_DIRECTORY");
    if (configured.isEmpty() == false)
    {
        return configured;
    }
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return root.isEmpty() == false ? QDir(root).filePath(QStringLiteral("source-recovery")) : QString{};
}
} // namespace

RenderGraphRecovery::RenderGraphRecovery(RenderGraphDocument& inoutDocument, QWidget* parentOrNull)
    : QWidget(parentOrNull), mDocument(inoutDocument), mStore(GetRecoveryDirectory()), mStatus(new QLabel(this)),
      mReview(new QPushButton(QStringLiteral("Review Recovery..."), this))
{
    setObjectName(QStringLiteral("renderGraphRecovery"));
    QHBoxLayout* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    mStatus->setObjectName(QStringLiteral("renderGraphRecoveryStatus"));
    mStatus->setTextFormat(Qt::PlainText);
    mStatus->setWordWrap(false);
    mStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    row->addWidget(mStatus, 1);
    mReview->setObjectName(QStringLiteral("renderGraphReviewRecovery"));
    row->addWidget(mReview);
    mTimer.setSingleShot(false);
    mTimer.setInterval(CHECKPOINT_INTERVAL_MILLISECONDS);
    connect(&mTimer, &QTimer::timeout, this, [this]() {
        if (mIsDiscardPending == true &&
            (QApplication::activeModalWidget() != nullptr || window()->isVisible() == false))
        {
            return;
        }
        mIsDiscardPending = false;
        static_cast<void>(CheckpointNow());
    });
    connect(mReview, &QPushButton::clicked, this, &RenderGraphRecovery::Review);
    Update();
}

void RenderGraphRecovery::SetCanRestore(bool canRestore)
{
    if (mCanRestore != canRestore)
    {
        mCanRestore = canRestore;
        Q_EMIT AvailabilityChanged();
    }
}

void RenderGraphRecovery::Update()
{
    const QString binding = mStore.GetPath(mDocument);
    if (binding != mBinding)
    {
        mTimer.stop();
        mBinding = binding;
        mOwnPath.clear();
        mOwnDigest.clear();
        mConsumedPath.clear();
        mConsumedDigest.clear();
        mCheckpointRevision = 0;
        mObservedRevision = 0;
        mError.clear();
        mIsDiscardPending = false;
    }
    if (mDocument.GetRevision() != mObservedRevision)
    {
        mObservedRevision = mDocument.GetRevision();
        mIsDiscardPending = false;
    }
    if (mDocument.IsDirty() == false)
    {
        mTimer.stop();
        removeOwned();
    }
    else if (mTimer.isActive() == false)
    {
        mTimer.start();
    }
    refreshStatus();
}

void RenderGraphRecovery::removeOwned()
{
    QString errors;
    QString error;
    if (mOwnPath.isEmpty() == false)
    {
        if (mStore.Remove(mOwnPath, mOwnDigest, error) == true)
        {
            mOwnPath.clear();
            mOwnDigest.clear();
        }
        else
        {
            errors = error;
        }
    }
    if (mConsumedPath.isEmpty() == false)
    {
        if (mStore.Remove(mConsumedPath, mConsumedDigest, error) == true)
        {
            mConsumedPath.clear();
            mConsumedDigest.clear();
        }
        else
        {
            errors += QLatin1Char(' ') + error;
        }
    }
    mError = errors;
    mCheckpointRevision = 0;
}

void RenderGraphRecovery::Leave()
{
    mTimer.stop();
    if (mIsDiscardPending == false)
    {
        static_cast<void>(CheckpointNow());
    }
    mTimer.stop();
    mStore.BeginBinding();
    mBinding.clear();
    mOwnPath.clear();
    mOwnDigest.clear();
    mConsumedPath.clear();
    mConsumedDigest.clear();
    mCheckpointRevision = 0;
}

void RenderGraphRecovery::DiscardCurrent()
{
    removeOwned();
    mIsDiscardPending = true;
    // Wait out other owners' close prompts. An accepted close hides/destroys or
    // rebinds this owner; a cancelled close resumes protection in the visible window.
    mTimer.start();
    refreshStatus();
}

bool RenderGraphRecovery::CheckpointNow(bool shouldForce)
{
    if (mBinding != mStore.GetPath(mDocument))
    {
        Update();
    }
    if (mDocument.IsLoaded() == false || mIsDiscardPending == true)
    {
        mTimer.stop();
        return true;
    }
    if (mDocument.IsDirty() == false)
    {
        mTimer.stop();
        removeOwned();
        refreshStatus();
        return mError.isEmpty();
    }
    const QFileInfo existing(mOwnPath);
    if (shouldForce == false && mCheckpointRevision == mDocument.GetRevision() && existing.isFile() == true &&
        existing.isSymLink() == false)
    {
        return true;
    }
    if (mStore.Write(mDocument, mError) == false)
    {
        refreshStatus();
        return false;
    }
    RenderGraphRecoveryCheckpoint checkpoint;
    const QString path = mStore.GetPath(mDocument);
    if (mStore.Read(path, mDocument, checkpoint, mError) == false)
    {
        refreshStatus();
        return false;
    }
    mOwnPath = path;
    mOwnDigest = checkpoint.Digest;
    mCheckpointRevision = mDocument.GetRevision();
    refreshStatus();
    return true;
}

void RenderGraphRecovery::refreshStatus()
{
    QString status;
    if (mError.isEmpty() == false)
    {
        status = mError;
    }
    else if (mDocument.IsLoaded() == false)
    {
        status = QStringLiteral("Recovery: open a graph to protect its draft.");
    }
    else if (mDocument.IsDirty() == false)
    {
        status = QStringLiteral("Recovery: no unsaved edits.");
    }
    else if (mCheckpointRevision == mDocument.GetRevision())
    {
        status = QStringLiteral("Local recovery checkpoint saved. Save publishes project files.");
    }
    else
    {
        status = QStringLiteral("Unsaved edits; recovery checkpoint pending. Save publishes project files.");
    }
#if defined(Q_OS_WASM)
    status = QStringLiteral("Browser recovery is session-only. Download/export before closing or reloading.") +
             (mError.isEmpty() == false ? QLatin1Char(' ') + mError : QString{});
#endif
    QStringList pending = mStore.Find(mDocument);
    pending.removeAll(mConsumedPath);
    mReview->setEnabled(pending.isEmpty() == false || mDocument.IsDirty() == true);
    if (pending.isEmpty() == false)
    {
        status = QStringLiteral("%1 recovery checkpoint(s) available. ").arg(pending.size()) + status;
    }
    mStatus->setText(status.simplified());
    mStatus->setToolTip(status + QLatin1Char('\n') + mStore.GetPath(mDocument));
}

bool RenderGraphRecovery::Restore(const QString& path, const QByteArray& expectedDigest)
{
    RenderGraphRecoveryCheckpoint checkpoint;
    if (mCanRestore == false || mDocument.IsDirty() == true)
    {
        mError = QStringLiteral("Finish the running job and Save or discard current edits before restoring recovery.");
        refreshStatus();
        return false;
    }
    if (mStore.Read(path, mDocument, checkpoint, mError) == false)
    {
        refreshStatus();
        return false;
    }
    if (expectedDigest.isEmpty() == false && checkpoint.Digest != expectedDigest)
    {
        mError = QStringLiteral("The checkpoint changed. Select it again to review its current contents.");
        refreshStatus();
        return false;
    }
    const RenderGraphDocumentStatus status =
        mDocument.RestoreRecoverySnapshot(checkpoint.Draft, checkpoint.SavedHashes, checkpoint.IsBuiltIn);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mError = QStringLiteral("The saved baseline or disk files changed, or recovery exceeds package limits. "
                                "Nothing was replaced. Review the recovered files and use Save Copy to keep them.");
        refreshStatus();
        return false;
    }
    mConsumedPath = path;
    mConsumedDigest = checkpoint.Digest;
    mError.clear();
    Q_EMIT Restored();
    Update();
    return true;
}

void RenderGraphRecovery::Review()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("renderGraphRecoveryDialog"));
    dialog.setWindowTitle(QStringLiteral("Review source recovery"));
    dialog.resize(REVIEW_WIDTH, REVIEW_HEIGHT);
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    RenderGraphRecoveryCheckpoint currentDraft;
    if (mDocument.IsDirty() == true)
    {
        currentDraft.Draft = mDocument.CaptureRecoverySnapshot(RenderGraphSource::Draft);
        currentDraft.IsBuiltIn = mDocument.IsBuiltIn();
        for (const QString& file : mDocument.GetFiles(RenderGraphSource::Saved))
        {
            currentDraft.SavedHashes.insert(
                file,
                QCryptographicHash::hash(mDocument.GetSaved(file), QCryptographicHash::Sha256).toHex());
        }
    }
    QComboBox* checkpoints = new QComboBox(&dialog);
    if (currentDraft.Draft.Files.isEmpty() == false)
    {
        checkpoints->addItem(QStringLiteral("Current draft (copy only)"), QStringLiteral("current-draft"));
    }
    checkpoints->setObjectName(QStringLiteral("recoveryCheckpoints"));
    for (const QString& path : mStore.Find(mDocument))
    {
        checkpoints->addItem(QFileInfo(path).lastModified().toString(Qt::ISODate) + QStringLiteral(" · ") +
                                 QFileInfo(path).fileName(),
                             path);
    }
    checkpoints->setMinimumContentsLength(1);
    checkpoints->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    checkpoints->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    layout->addWidget(checkpoints);
    QComboBox* files = new QComboBox(&dialog);
    files->setObjectName(QStringLiteral("recoveryFiles"));
    files->setMinimumContentsLength(1);
    files->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    layout->addWidget(files);
    QLabel* message = new QLabel(
        QStringLiteral(
            "Recovered text is read-only here. Restore creates one undoable draft edit; Save remains explicit."),
        &dialog);
    message->setObjectName(QStringLiteral("recoveryReviewMessage"));
    message->setWordWrap(true);
    message->setTextFormat(Qt::PlainText);
    message->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(message);
    QSplitter* split = new QSplitter(&dialog);
    QPlainTextEdit* recovered = new QPlainTextEdit(split);
    recovered->setReadOnly(true);
    recovered->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    recovered->setAccessibleName(QStringLiteral("Recovered source"));
    QPlainTextEdit* current = new QPlainTextEdit(split);
    current->setReadOnly(true);
    current->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    current->setAccessibleName(QStringLiteral("Current source"));
    layout->addWidget(split, 1);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QPushButton* restore = buttons->addButton(QStringLiteral("Restore Draft"), QDialogButtonBox::ActionRole);
    QPushButton* copy = buttons->addButton(QStringLiteral("Save Copy..."), QDialogButtonBox::ActionRole);
    QPushButton* discard = buttons->addButton(QStringLiteral("Discard Checkpoint"), QDialogButtonBox::DestructiveRole);
    restore->setObjectName(QStringLiteral("restoreRecoveryDraft"));
    copy->setObjectName(QStringLiteral("saveRecoveryCopy"));
    discard->setObjectName(QStringLiteral("discardRecoveryCheckpoint"));
    layout->addWidget(buttons);
    RenderGraphRecoveryCheckpoint checkpoint;
    connect(this, &RenderGraphRecovery::AvailabilityChanged, &dialog, [&]() {
        restore->setEnabled(mCanRestore == true && mDocument.IsDirty() == false &&
                            checkpoint.Digest.isEmpty() == false);
    });
    const auto showFile = [&]() {
        const QString file = files->currentData().toString();
        copy->setEnabled(checkpoint.Draft.Files.contains(file));
        recovered->setPlainText(checkpoint.Draft.Files.contains(file) == true
                                    ? QString::fromUtf8(checkpoint.Draft.Files.value(file))
                                    : QStringLiteral("File removed in this checkpoint."));
        current->setPlainText(QString::fromUtf8(mDocument.GetDraft(file)));
    };
    const auto load = [&]() {
        QString error;
        const bool isCurrent = checkpoints->currentData().toString() == QStringLiteral("current-draft");
        const bool isValid = isCurrent == true ||
                             mStore.Read(checkpoints->currentData().toString(), mDocument, checkpoint, error) == true;
        if (isCurrent == true)
        {
            checkpoint = currentDraft;
        }
        restore->setEnabled(isValid == true && isCurrent == false && mCanRestore == true &&
                            mDocument.IsDirty() == false);
        copy->setEnabled(isValid);
        discard->setEnabled(isValid == true && isCurrent == false);
        files->clear();
        if (isValid == true)
        {
            QStringList names = checkpoint.Draft.Files.keys() + checkpoint.SavedHashes.keys();
            names.removeDuplicates();
            for (const QString& name : names)
            {
                const bool isRemoved = checkpoint.Draft.Files.contains(name) == false;
                const bool hasChanged =
                    isRemoved == true ||
                    QCryptographicHash::hash(checkpoint.Draft.Files.value(name), QCryptographicHash::Sha256).toHex() !=
                        checkpoint.SavedHashes.value(name);
                files->addItem(name + (isRemoved == true    ? QStringLiteral(" (removed)")
                                       : hasChanged == true ? QStringLiteral(" (changed)")
                                                            : QString{}),
                               name);
            }
            message->setText(
                QStringLiteral("Left: recovered source. Right: current draft. Restore requires the unchanged saved "
                               "baseline; Save Copy keeps recovered bytes in a new file."));
        }
        else
        {
            checkpoint = {};
            message->setText(error);
        }
        showFile();
    };
    connect(checkpoints, &QComboBox::currentIndexChanged, &dialog, load);
    connect(files, &QComboBox::currentIndexChanged, &dialog, showFile);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(restore, &QPushButton::clicked, &dialog, [&]() {
        if (Restore(checkpoints->currentData().toString(), checkpoint.Digest) == true)
        {
            dialog.accept();
        }
        else
        {
            message->setText(mError);
        }
    });
    connect(discard, &QPushButton::clicked, &dialog, [&]() {
        QString error;
        if (mStore.Remove(checkpoints->currentData().toString(), checkpoint.Digest, error) == true)
        {
            checkpoints->removeItem(checkpoints->currentIndex());
            refreshStatus();
        }
        else
        {
            message->setText(error);
        }
    });
    connect(copy, &QPushButton::clicked, &dialog, [&]() {
        const QByteArray bytes = checkpoint.Draft.Files.value(files->currentData().toString());
        const QString name = QFileInfo(files->currentData().toString()).completeBaseName() +
                             QStringLiteral(".recovered.") + QFileInfo(files->currentData().toString()).suffix();
#if defined(Q_OS_WASM)
        QFileDialog::saveFileContent(bytes, name);
#else
        const QString path = QFileDialog::getSaveFileName(&dialog, QStringLiteral("Save recovered copy to a new file"), name);
        if (path.isEmpty() == false)
        {
            QString error;
            message->setText(WriteRecoveryCopy(bytes, path, error) == true
                                 ? QStringLiteral("Recovered copy saved to %1. Existing files were not overwritten.").arg(path)
                                 : error);
        }
#endif
    });
    load();
    static_cast<void>(dialog.exec());
    // Callbacks borrow the reviewed snapshot and lambdas above; disconnect before
    // those locals are destroyed and the dialog tears down its child controls.
    for (QObject* control : {static_cast<QObject*>(checkpoints),
                             static_cast<QObject*>(files),
                             static_cast<QObject*>(buttons),
                             static_cast<QObject*>(restore),
                             static_cast<QObject*>(copy),
                             static_cast<QObject*>(discard)})
    {
        disconnect(control, nullptr, &dialog, nullptr);
    }
    disconnect(this, nullptr, &dialog, nullptr);
    refreshStatus();
}
} // namespace ludus::editor
