#pragma once

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
    /// Reject stale identities, invalid UTF-8 boundaries and capacity failures
    /// without changing text/history. Successful edits advance the owner revision.
    [[nodiscard]] virtual CodeEditStatus ApplyEdit(const CodeDocumentIdentity& expected,
                                                   const CodeTextEdit& edit,
                                                   CodeTextSelection before,
                                                   CodeTextSelection after,
                                                   bool shouldMerge) = 0;
    virtual void BreakUndoGroup() noexcept = 0;
    [[nodiscard]] virtual bool CanUndo() const noexcept = 0;
    [[nodiscard]] virtual bool CanRedo() const noexcept = 0;
};
} // namespace ludus::editor
