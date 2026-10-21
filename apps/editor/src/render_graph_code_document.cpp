#include "internal/render_graph_code_document.h"

#include "internal/render_graph_document.h"

namespace ludus::editor
{
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
