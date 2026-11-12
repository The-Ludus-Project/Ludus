#pragma once

#include "internal/code_document.h"
#include "internal/code_editor.h"
#include "internal/code_multi_cursor.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
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

/// Bridges committed Qt input to one authoritative document history. The editor
/// and any bound host must be alive during use. GUI thread only.
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
    /// Restore a whole selection set recorded with an undo/redo step (CE05). The
    /// primary (first) selection becomes the single Qt cursor; the rest become
    /// the view's secondary cursors through its multi-cursor layer (CE06). An
    /// empty set leaves the view's selection untouched. Byte offsets are mapped
    /// to Qt positions in one ascending pass with AdvanceCodeByteOffset, so a
    /// hundred-cursor set costs one walk of the text, not one walk per cursor.
    void RestoreSelectionSet(foundation::BorrowedView<const CodeTextSelection> selections);
    /// Publish formatter/completion text with an exact identity. One history group;
    /// never writes a file. Invalid/stale text preserves the accepted draft.
    [[nodiscard]] CodeEditStatus
    ApplyServiceText(const CodeDocumentIdentity& expected, const QByteArray& text, CodeTextSelection selection);

    /// Apply an ordered, non-overlapping edit set to the bound host as one undo
    /// step (CE05): formatting, Replace All, completion with additional edits and
    /// the per-file slice of a workspace edit all publish through here. The host
    /// validates and applies the whole set or rejects it whole; on Ok the view
    /// re-synchronizes, the recorded after-selection is restored and the scroll is
    /// preserved. A stale identity or invalid set keeps the accepted draft.
    /// `expected` is the identity the caller built the set against; it must match
    /// the bound host, so a set computed against older text is rejected whole.
    [[nodiscard]] CodeEditStatus ApplyEditSet(const CodeDocumentIdentity& expected, const CodeEditSet& edits);

    /// Apply one multi-cursor text operation as a single CodeEditSet (CE06). The
    /// regions arrive in document order (ascending Start) in the view's UTF-16
    /// coordinates, one per cursor; exactly one is flagged primary. Every Start
    /// and End is mapped to a UTF-8 byte offset in one ascending pass with
    /// AdvanceCodeByteOffset, so the mapping is one walk of the text regardless of
    /// the cursor count. On Ok the whole set has applied as one undo step, the
    /// view has re-synchronized with the single Qt cursor on the primary region's
    /// resulting caret, and outSecondaryCursors holds the resulting caret of each
    /// non-primary region (UTF-16, document order) for the view's overlay. A
    /// stale identity, an unmappable position or a rejected set keeps the accepted
    /// draft and leaves outSecondaryCursors unchanged.
    [[nodiscard]] CodeEditStatus ApplyMultiCursorEdit(const QList<CodeRegionEdit>& regionsInDocumentOrder,
                                                      QList<CodeSecondaryCursor>& outSecondaryCursors);

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
    foundation::int32 mProjectionRevision = 0;
    bool mIsSynchronizing = false;
    bool mHasTextGroup = false;
    bool mHasCrLf = false;
    bool mHasTrailingWhitespace = false;
    bool mIsEditorAlive = true;
    QElapsedTimer mTypingClock;
};
} // namespace ludus::editor
