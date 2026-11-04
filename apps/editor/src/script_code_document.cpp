#include "internal/script_code_document.h"

#include <ludus/foundation/strings/status.hpp>
#include <ludus/foundation/strings/utf8.hpp>

// Draft-backed source host for the Scripts work area (S5). It carries the draft
// and a bounded undo history through the shared document seam, so script text is
// edited in a CodeEditor through a CodeDocumentController, while Save, reload and
// cooking stay with the work area. No file I/O, no exceptions.

namespace ludus::editor
{
using foundation::BorrowedView;
using foundation::isize;
using foundation::StringStatus;
using foundation::usize;
using foundation::Utf8View;
using foundation::ValidateUtf8;

namespace
{
/// The largest script draft the host accepts, matching the work area's bound.
constexpr usize MAX_SCRIPT_DRAFT_BYTES = 131072;

[[nodiscard]] bool isValidPrefix(BorrowedView<const char> bytes, usize count) noexcept
{
    if (count == 0)
    {
        return true;
    }
    Utf8View proof;
    usize errorOffset = 0;
    return ValidateUtf8({bytes.GetDataOrNull(), count}, proof, errorOffset) == StringStatus::Ok;
}
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

CodeEditStatus ScriptCodeDocument::ApplyEdit(const CodeDocumentIdentity& expected,
                                             const CodeTextEdit& edit,
                                             CodeTextSelection before,
                                             CodeTextSelection after,
                                             bool shouldMerge)
{
    if (expected != GetSnapshot().Identity)
    {
        return CodeEditStatus::Conflict;
    }
    return applyByteEdit(edit, before, after, shouldMerge);
}

CodeEditStatus ScriptCodeDocument::applyByteEdit(const CodeTextEdit& edit,
                                                 CodeTextSelection before,
                                                 CodeTextSelection after,
                                                 bool shouldMerge)
{
    const BorrowedView<const char> original{mDraft.constData(), static_cast<usize>(mDraft.size())};
    if (edit.Inserted.IsValid() == false || edit.StartBytes > original.GetSize() ||
        edit.RemovedBytes > original.GetSize() - edit.StartBytes)
    {
        return CodeEditStatus::InvalidText;
    }
    const usize retainedBytes = original.GetSize() - edit.RemovedBytes;
    if (retainedBytes > MAX_SCRIPT_DRAFT_BYTES || edit.Inserted.GetSize() > MAX_SCRIPT_DRAFT_BYTES - retainedBytes)
    {
        return CodeEditStatus::Capacity;
    }
    if (isValidPrefix(original, edit.StartBytes) == false ||
        isValidPrefix(original, edit.StartBytes + edit.RemovedBytes) == false ||
        isValidPrefix(edit.Inserted, edit.Inserted.GetSize()) == false || before.AnchorBytes > original.GetSize() ||
        before.CaretBytes > original.GetSize())
    {
        return CodeEditStatus::InvalidText;
    }
    QByteArray candidate = mDraft;
    candidate.replace(static_cast<isize>(edit.StartBytes),
                      static_cast<isize>(edit.RemovedBytes),
                      edit.Inserted.GetDataOrNull(),
                      static_cast<isize>(edit.Inserted.GetSize()));
    const BorrowedView<const char> result{candidate.constData(), static_cast<usize>(candidate.size())};
    if (isValidPrefix(result, after.AnchorBytes) == false || isValidPrefix(result, after.CaretBytes) == false)
    {
        return CodeEditStatus::InvalidText;
    }
    if (candidate == mDraft)
    {
        return CodeEditStatus::Ok;
    }
    const QByteArray previous = mDraft;
    const bool canMerge = shouldMerge == true && mHasMergeableGroup == true;
    if (mHistory.Commit(previous, candidate, canMerge) == false)
    {
        mDraft = candidate;
        mHasMergeableGroup = false;
        return CodeEditStatus::Ok;
    }
    mDraft = candidate;
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
