#include "internal/render_graph_code_document.h"

#include "internal/code_edit_set.h"
#include "internal/code_text.h"
#include "internal/render_graph_document.h"

namespace ludus::editor
{
using foundation::BorrowedView;
using foundation::usize;
RenderGraphCodeDocument::RenderGraphCodeDocument(RenderGraphDocument& inoutDocument) : mDocument(inoutDocument) {}

void RenderGraphCodeDocument::Bind(const QString& file, foundation::uint64 epoch)
{
    if (mFile != file || mEpoch != epoch)
    {
        BreakUndoGroup();
        mFile = file;
        mEpoch = epoch;
        ++mBinding;
    }
}

void RenderGraphCodeDocument::ResetBinding()
{
    BreakUndoGroup();
    mFile.clear();
    ++mBinding;
}

CodeDocumentSnapshot RenderGraphCodeDocument::GetSnapshot() const noexcept
{
    return {{mEpoch, mBinding, mDocument.GetRevision()},
            mDocument.GetDraftView(mFile),
            static_cast<foundation::usize>(RenderGraphDocument::MAX_FILE_BYTES),
            mDocument.IsDirty()};
}

CodeEditStatus RenderGraphCodeDocument::ApplyEdits(const CodeDocumentIdentity& expected,
                                                   const CodeEditSet& edits,
                                                   bool shouldMerge)
{
    if (expected != GetSnapshot().Identity)
    {
        return CodeEditStatus::Conflict;
    }
    // Validate the whole set against the package's current draft, all or nothing,
    // using the same rules as the owning hosts.
    const BorrowedView<const char> original = mDocument.GetDraftView(mFile);
    QByteArray result;
    const CodeEditStatus status =
        ValidateAndApplyEditSet(original, static_cast<usize>(RenderGraphDocument::MAX_FILE_BYTES), edits, result);
    if (status != CodeEditStatus::Ok)
    {
        return status;
    }
    // Forward one combined replacement so the package records a single undo step
    // (CE05). The result buffer must outlive the forwarded borrowed edit.
    const QByteArray before(original.GetDataOrNull(), static_cast<qsizetype>(original.GetSize()));
    const CodeTextEdit combined = MakeCodeTextEdit(before, result);
    if (combined.RemovedBytes == 0 && combined.Inserted.IsEmpty() == true)
    {
        return CodeEditStatus::Ok;
    }
    // The selection set is byte offsets into the whole document; the package's
    // single-cursor history uses the primary (first) Before/After selection.
    const CodeTextSelection beforeSelection =
        edits.Before.Selections.IsEmpty() == false ? edits.Before.Selections[0] : CodeTextSelection{};
    const CodeTextSelection afterSelection =
        edits.After.Selections.IsEmpty() == false ? edits.After.Selections[0] : CodeTextSelection{};
    return mDocument.ApplyTextEdit(mFile, expected.Revision, combined, beforeSelection, afterSelection, shouldMerge);
}

CodeEditStatus RenderGraphCodeDocument::ApplyEdit(const CodeDocumentIdentity& expected,
                                                  const CodeTextEdit& edit,
                                                  CodeTextSelection before,
                                                  CodeTextSelection after,
                                                  bool shouldMerge)
{
    if (expected != GetSnapshot().Identity)
    {
        return CodeEditStatus::Conflict;
    }
    return mDocument.ApplyTextEdit(mFile, expected.Revision, edit, before, after, shouldMerge);
}

void RenderGraphCodeDocument::BreakUndoGroup() noexcept
{
    mDocument.BreakTextGroup();
}

bool RenderGraphCodeDocument::CanUndo() const noexcept
{
    return mDocument.CanUndo();
}

bool RenderGraphCodeDocument::CanRedo() const noexcept
{
    return mDocument.CanRedo();
}
} // namespace ludus::editor
