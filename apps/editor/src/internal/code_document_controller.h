#pragma once

#include "internal/code_document.h"
#include "internal/code_editor.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>

namespace ludus::editor
{
/// Owned capture for background services; borrows neither document nor widget.
struct CodeTextCapture final
{
    CodeDocumentIdentity Identity;
    QByteArray Text;
    foundation::usize LimitBytes = 0;
};

/// Bridges committed Qt input to one authoritative document history. Editor and
/// host outlive this object. GUI thread only; no file/process ownership.
class CodeDocumentController final : public QObject
{
    Q_OBJECT
public:
    explicit CodeDocumentController(CodeEditor& inoutEditor, QObject* parentOrNull = nullptr);
    ~CodeDocumentController() override;
    void Bind(CodeDocumentHost* hostOrNull);
    void Synchronize(bool shouldRestoreView = false);
    void BreakUndoGroup();
    [[nodiscard]] bool IsBound() const noexcept;
    [[nodiscard]] CodeTextCapture Capture() const;
    [[nodiscard]] CodeTextSelection CaptureSelection() const;
    void RestoreSelection(CodeTextSelection selection);
    /// Exact-identity publication. One history group; never writes a file.
    [[nodiscard]] CodeEditStatus
    ApplyFormattedText(const CodeDocumentIdentity& expected, const QByteArray& text, CodeTextSelection selection);

Q_SIGNALS:
    void TextAccepted();
    void BindingChanged();
    void Failed(const QString& message);

private:
    void captureSelectionChange();
    void beginOperation(CodeEditKind kind);
    void endOperation();
    void captureChange(foundation::int32 position, foundation::int32 removed, foundation::int32 added);
    void commitPending();
    void updateHistoryAvailability();
    [[nodiscard]] bool canMerge(const CodeTextEdit& edit) const;
    CodeEditor& mEditor;
    CodeDocumentHost* mHostOrNull = nullptr;
    CodeDocumentIdentity mIdentity;
    QByteArray mBytes;
    QByteArray mPendingBytes;
    CodeTextSelection mBefore;
    CodeTextSelection mLastSelection;
    CodeEditStatus mPendingStatus = CodeEditStatus::Ok;
    CodeEditKind mKind = CodeEditKind::Command;
    CodeEditKind mLastKind = CodeEditKind::Command;
    foundation::int32 mOperationDepth = 0;
    foundation::usize mLastStartBytes = 0;
    foundation::usize mLastInsertedBytes = 0;
    bool mHasPending = false;
    bool mIsSynchronizing = false;
    bool mHasTextGroup = false;
    bool mHasCrLf = false;
    bool mHasTrailingWhitespace = false;
    QElapsedTimer mTypingClock;
};
} // namespace ludus::editor
