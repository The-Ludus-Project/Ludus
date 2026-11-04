#pragma once

#include "internal/code_document.h"
#include "internal/document_history.h"

#include <QByteArray>

namespace ludus::editor
{
/// Draft-backed source host for the Scripts work area's text editor. It owns the
/// current UTF-8 draft and a bounded undo history over the seam, so the Scripts
/// work area edits through the shared CodeEditor/CodeDocumentController instead
/// of a bare QPlainTextEdit. Save, reload and cooking stay with the work area
/// and the controller; this host never touches a file. GUI thread only.
///
/// Unlike the file host, the script host's baseline is set explicitly by the
/// work area (from disk on open and after a save), so IsDirty reflects the
/// unsaved-draft rule the S5 tests assert.
class ScriptCodeDocument final : public CodeDocumentHost
{
public:
    ScriptCodeDocument() = default;

    /// Load bytes as the draft and the saved baseline, clearing history. Used on
    /// open and reload; the new content is the clean baseline.
    void SetBaseline(const QByteArray& bytes);
    /// Replace the draft without creating history, used when the work area has
    /// already recorded the change (for example a graph-cell edit) and only needs
    /// the shared text projection to follow. Dirty is recomputed against baseline.
    void SetDraft(const QByteArray& bytes);

    [[nodiscard]] const QByteArray& GetDraft() const noexcept
    {
        return mDraft;
    }
    [[nodiscard]] const QByteArray& GetBaseline() const noexcept
    {
        return mBaseline;
    }
    [[nodiscard]] bool IsDirty() const noexcept
    {
        return mDraft != mBaseline;
    }

    [[nodiscard]] CodeDocumentSnapshot GetSnapshot() const noexcept override;
    [[nodiscard]] CodeEditStatus ApplyEdit(const CodeDocumentIdentity& expected,
                                           const CodeTextEdit& edit,
                                           CodeTextSelection before,
                                           CodeTextSelection after,
                                           bool shouldMerge) override;
    void BreakUndoGroup() noexcept override;
    [[nodiscard]] bool CanUndo() const noexcept override;
    [[nodiscard]] bool CanRedo() const noexcept override;

    /// Undo/redo one step, returning whether the draft changed.
    [[nodiscard]] bool Undo();
    [[nodiscard]] bool Redo();

private:
    [[nodiscard]] CodeEditStatus
    applyByteEdit(const CodeTextEdit& edit, CodeTextSelection before, CodeTextSelection after, bool shouldMerge);
    QByteArray mBaseline;
    QByteArray mDraft;
    DocumentHistory<QByteArray> mHistory;
    bool mHasMergeableGroup = false;
    foundation::uint64 mBinding = 1;
};
} // namespace ludus::editor
