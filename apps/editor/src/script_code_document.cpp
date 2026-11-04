#include "internal/script_code_document.h"

#include "internal/code_edit_set.h"

#include <utility>

// Draft-backed source host for the Scripts work area (S5). It carries the draft
// and a bounded undo history through the shared document seam, so script text is
// edited in a CodeEditor through a CodeDocumentController, while Save, reload and
// cooking stay with the work area. No file I/O, no exceptions.

namespace ludus::editor
{
using foundation::BorrowedView;
using foundation::usize;

namespace
{
/// The largest script draft the host accepts, matching the work area's bound.
constexpr usize MAX_SCRIPT_DRAFT_BYTES = 131072;
} // namespace

void ScriptCodeDocument::SetBaseline(const QByteArray& bytes)
{
    mBaseline = bytes;
    mDraft = bytes;
    mHistory.Reset();
    mHasMergeableGroup = false;
    ++mBinding;
}

void ScriptCodeDocument::MarkSaved() noexcept
{
    mBaseline = mDraft;
    mHistory.BreakGroup();
    mHasMergeableGroup = false;
}

void ScriptCodeDocument::SetDraft(const QByteArray& bytes)
{
    mDraft = bytes;
    mHistory.BreakGroup();
    mHasMergeableGroup = false;
    ++mBinding;
}

CodeDocumentSnapshot ScriptCodeDocument::GetSnapshot() const noexcept
{
    return {{0, mBinding, mHistory.Revision()},
            {mDraft.constData(), static_cast<usize>(mDraft.size())},
            MAX_SCRIPT_DRAFT_BYTES,
            IsDirty()};
}

CodeEditStatus
ScriptCodeDocument::ApplyEdits(const CodeDocumentIdentity& expected, const CodeEditSet& edits, bool shouldMerge)
{
    if (expected != GetSnapshot().Identity)
    {
        return CodeEditStatus::Conflict;
    }
    return applyByteEdits(edits, shouldMerge);
}

CodeEditStatus ScriptCodeDocument::applyByteEdits(const CodeEditSet& edits, bool shouldMerge)
{
    const BorrowedView<const char> original{mDraft.constData(), static_cast<usize>(mDraft.size())};
    QByteArray candidate;
    const CodeEditStatus status = ValidateAndApplyEditSet(original, MAX_SCRIPT_DRAFT_BYTES, edits, candidate);
    if (status != CodeEditStatus::Ok)
    {
        return status;
    }
    if (candidate == mDraft)
    {
        return CodeEditStatus::Ok;
    }
    const QByteArray previous = mDraft;
    const bool canMerge = shouldMerge == true && mHasMergeableGroup == true;
    if (mHistory.Commit(previous, candidate, canMerge) == false)
    {
        mDraft = std::move(candidate);
        mHasMergeableGroup = false;
        return CodeEditStatus::Ok;
    }
    mDraft = std::move(candidate);
    mHasMergeableGroup = true;
    return CodeEditStatus::Ok;
}

void ScriptCodeDocument::BreakUndoGroup() noexcept
{
    mHasMergeableGroup = false;
    mHistory.BreakGroup();
}

bool ScriptCodeDocument::CanUndo() const noexcept
{
    return mHistory.CanUndo();
}

bool ScriptCodeDocument::CanRedo() const noexcept
{
    return mHistory.CanRedo();
}

bool ScriptCodeDocument::Undo()
{
    BreakUndoGroup();
    return mHistory.Undo(mDraft);
}

bool ScriptCodeDocument::Redo()
{
    BreakUndoGroup();
    return mHistory.Redo(mDraft);
}
} // namespace ludus::editor
