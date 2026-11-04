#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::editor
{
/// Identifies one source binding and its owner's monotonic content revision.
struct CodeDocumentIdentity final
{
    foundation::uint64 Epoch = 0;
    foundation::uint64 Document = 0;
    foundation::uint64 Revision = 0;
    [[nodiscard]] bool operator==(const CodeDocumentIdentity& other) const = default;
};

/// UTF-8 byte boundaries, including selection direction; never Qt cursor units.
struct CodeTextSelection final
{
    foundation::usize AnchorBytes = 0;
    foundation::usize CaretBytes = 0;
};

/// A single contiguous replacement. The owner copies Inserted before returning.
struct CodeTextEdit final
{
    foundation::usize StartBytes = 0;
    foundation::usize RemovedBytes = 0;
    foundation::BorrowedView<const char> Inserted;
};

/// Identifies one workspace edit shared across the files it touches (CE11). Zero
/// means a single-document edit with no workspace transaction. The id is opaque
/// and compared for equality only; the registry assigns it.
struct CodeTransactionId final
{
    foundation::uint64 Value = 0;
    [[nodiscard]] bool operator==(const CodeTransactionId& other) const = default;
    [[nodiscard]] LUDUS_INLINE bool IsSet() const noexcept
    {
        return Value != 0;
    }
};

/// The whole cursor set in document order: the primary selection first, then the
/// secondary selections ascending by caret (CE06). An empty set leaves the view's
/// current selection untouched when the edit set is applied. Storage is borrowed
/// by the caller until the host returns, exactly like CodeTextEdit::Inserted; the
/// host copies the selections it keeps for history before returning.
struct CodeSelectionSet final
{
    foundation::BorrowedView<const CodeTextSelection> Selections;
    [[nodiscard]] LUDUS_INLINE bool IsEmpty() const noexcept
    {
        return Selections.IsEmpty();
    }
};

/// An ordered set of non-overlapping replacements applied as one revision (CE05).
/// Every multi-position edit (multi-cursor typing, Replace All, formatting,
/// completion with additional edits, a per-file slice of a workspace edit) reaches
/// a host as one of these. The host validates the whole set before changing
/// anything -- ascending StartBytes, no overlap between adjacent edits, UTF-8
/// scalar boundaries, and the capacity of the whole set -- and either applies all
/// edits or rejects all of them, recording at most one undo step that restores
/// both the text and the Before/After selection sets. A single edit still reaches
/// the host through ApplyEdit, a thin wrapper that forwards a one-element set.
struct CodeEditSet final
{
    foundation::BorrowedView<const CodeTextEdit> Edits; ///< Ascending StartBytes, no overlap.
    CodeSelectionSet Before;
    CodeSelectionSet After;
    CodeTransactionId Transaction; ///< Zero, or shared by every file of one workspace edit.
};

enum class CodeEditStatus : foundation::uint8
{
    Ok,
    Conflict,
    Capacity,
    InvalidText,
    Unavailable,
};

/// Text storage is borrowed until the next owner mutation; GUI-thread use only.
struct CodeDocumentSnapshot final
{
    CodeDocumentIdentity Identity;
    foundation::BorrowedView<const char> Text;
    foundation::usize LimitBytes = 0;
    bool IsDirty = false;
};

/// Private, language-neutral seam to the authoritative document/history owner.
/// Binding lifetime outlives the controller; none of these operations writes files.
class CodeDocumentHost
{
public:
    virtual ~CodeDocumentHost() = default;
    [[nodiscard]] virtual CodeDocumentSnapshot GetSnapshot() const noexcept = 0;
    /// Apply an ordered, non-overlapping edit set as one revision (CE05). Reject a
    /// stale identity, an unordered or overlapping set, invalid UTF-8 boundaries
    /// and capacity failures without changing text, history or selections. On Ok
    /// the whole set has applied as one undo step that restores the Before/After
    /// selection sets, and the owner revision has advanced. The host copies any
    /// borrowed bytes and selections it keeps before returning.
    [[nodiscard]] virtual CodeEditStatus
    ApplyEdits(const CodeDocumentIdentity& expected, const CodeEditSet& edits, bool shouldMerge) = 0;
    /// Apply a single contiguous replacement. The default forwards a one-element
    /// edit set to ApplyEdits, so a host only implements ApplyEdits; it stays
    /// virtual for the few call sites that still build one edit directly.
    [[nodiscard]] virtual CodeEditStatus ApplyEdit(const CodeDocumentIdentity& expected,
                                                   const CodeTextEdit& edit,
                                                   CodeTextSelection before,
                                                   CodeTextSelection after,
                                                   bool shouldMerge)
    {
        const CodeTextEdit edits[] = {edit};
        const CodeTextSelection beforeSelections[] = {before};
        const CodeTextSelection afterSelections[] = {after};
        const CodeEditSet set =
        {
            .Edits = {edits, 1},
            .Before = {{beforeSelections, 1}},
            .After = {{afterSelections, 1}},
            .Transaction = {},
        };
        return ApplyEdits(expected, set, shouldMerge);
    }
    virtual void BreakUndoGroup() noexcept = 0;
    [[nodiscard]] virtual bool CanUndo() const noexcept = 0;
    [[nodiscard]] virtual bool CanRedo() const noexcept = 0;
};
} // namespace ludus::editor
