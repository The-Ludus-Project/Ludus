#pragma once

#include <ludus/foundation/base/types.h>

#include <optional>

// Thanks to Robert Nystrom, Game Programming Patterns (2014), "Command",
// "Undo and Redo": reversible before/after operations. This private Qt-free
// history adds bounded storage, checked heads and monotonic revisions; saved
// content remains owned by the document. No filesystem effect is an Undo item.
// https://gameprogrammingpatterns.com/command.html
// See docs/architecture/editor-gui-systems.md, Documents and edit transactions.
namespace ludus::editor
{
/// Name an edit shows when its owner gives none: Undo/Redo labels read "Undo Edit".
inline constexpr const char* UNNAMED_EDIT_NAME = "Edit";

/// Bounded linear undo history of before/after values. Every entry carries a readable command name (for
/// example "Rename Entity") that the editor shows in Undo/Redo labels, undo feedback and the History panel.
/// Names are pointers to strings with static storage duration, so naming costs one pointer store per commit.
template <typename T, foundation::usize CAPACITY = 64>
class DocumentHistory
{
    static_assert(CAPACITY > 0);

public:
    // T supplies equality and exception-free value copying. History storage
    // itself allocates nothing; payload ownership belongs to T's adapter.
    /// @param name Readable command name with static storage duration; a merged edit keeps its group's name.
    [[nodiscard]] bool
    Commit(const T& before, const T& after, bool shouldMerge = false, const char* name = UNNAMED_EDIT_NAME) noexcept
    {
        if (before == after || Revision_ == ~foundation::uint64{0})
        {
            return false;
        }
        auto* previous = Cursor_ != 0 ? &Entries_[Cursor_ - 1] : nullptr;
        if (shouldMerge && MergeAllowed_ && Cursor_ == Count_ && previous != nullptr && previous->has_value() &&
            (*previous)->After == before)
        {
            auto& entry = *previous;
            if (entry.has_value() == false)
            {
                return false;
            }
            entry->After = after;
            MergeAllowed_ = (entry->Before == after) == false;
            if (MergeAllowed_ == false)
            {
                // Removing a cancelled group must not expose an older field's
                // entry to coalescing by the next edit in this field.
                entry.reset();
                --Cursor_;
                --Count_;
            }
        }
        else
        {
            for (auto index = Cursor_; index < Count_; ++index)
            {
                Entries_[index].reset();
            }
            Count_ = Cursor_;
            if (Count_ == CAPACITY)
            {
                for (foundation::usize index = 1; index < CAPACITY; ++index)
                {
                    Entries_[index - 1] = Entries_[index];
                }
                --Count_;
                --Cursor_;
            }
            Entries_[Count_++] = Entry{before, after, name != nullptr ? name : UNNAMED_EDIT_NAME};
            Cursor_ = Count_;
            MergeAllowed_ = true;
        }
        ++Revision_;
        return true;
    }

    [[nodiscard]] bool CanUndo() const noexcept
    {
        return Cursor_ != 0 && Revision_ != ~foundation::uint64{0};
    }
    [[nodiscard]] bool CanRedo() const noexcept
    {
        return Cursor_ < Count_ && Revision_ != ~foundation::uint64{0};
    }
    // Reject a mismatched current head without changing content or history.
    [[nodiscard]] bool Undo(T& current) noexcept
    {
        if (CanUndo() == false)
        {
            return false;
        }
        const auto& entry = Entries_[Cursor_ - 1];
        if (entry.has_value() == false || (current == entry->After) == false)
        {
            return false;
        }
        current = entry->Before;
        --Cursor_;
        ++Revision_;
        BreakGroup();
        return true;
    }
    [[nodiscard]] bool Redo(T& current) noexcept
    {
        if (CanRedo() == false)
        {
            return false;
        }
        const auto& entry = Entries_[Cursor_];
        if (entry.has_value() == false || (current == entry->Before) == false)
        {
            return false;
        }
        current = entry->After;
        ++Cursor_;
        ++Revision_;
        BreakGroup();
        return true;
    }
    /// Move to history position `cursor` (0 is before the oldest retained entry, GetCount() after the newest)
    /// by undoing or redoing every step in between. All-or-nothing: a mismatched head, a position past
    /// GetCount() or a missing entry leaves `inoutCurrent` and the history unchanged and returns false.
    /// Jumping to the current position succeeds without a revision change.
    [[nodiscard]] bool JumpTo(T& inoutCurrent, foundation::usize cursor) noexcept
    {
        if (cursor > Count_ || Revision_ == ~foundation::uint64{0})
        {
            return false;
        }
        if (cursor == Cursor_)
        {
            return true;
        }
        const T* head = &inoutCurrent;
        foundation::usize position = Cursor_;
        while (position != cursor)
        {
            const bool isUndo = cursor < position;
            const std::optional<Entry>& entry = Entries_[isUndo == true ? position - 1 : position];
            if (entry.has_value() == false || (*head == (isUndo == true ? entry->After : entry->Before)) == false)
            {
                return false;
            }
            head = isUndo == true ? &entry->Before : &entry->After;
            position = isUndo == true ? position - 1 : position + 1;
        }
        inoutCurrent = *head;
        Cursor_ = cursor;
        ++Revision_;
        BreakGroup();
        return true;
    }
    /// Retained entries, at most CAPACITY.
    [[nodiscard]] foundation::usize GetCount() const noexcept
    {
        return Count_;
    }
    /// Entries currently applied: Undo acts on entry GetCursor() - 1 and Redo on entry GetCursor().
    [[nodiscard]] foundation::usize GetCursor() const noexcept
    {
        return Cursor_;
    }
    /// Command name of retained entry `index` (oldest first), or UNNAMED_EDIT_NAME outside [0, GetCount()).
    [[nodiscard]] const char* GetName(foundation::usize index) const noexcept
    {
        if (index >= Count_)
        {
            return UNNAMED_EDIT_NAME;
        }
        const std::optional<Entry>& entry = Entries_[index];
        return entry.has_value() == true ? entry->Name : UNNAMED_EDIT_NAME;
    }
    /// Name of the edit Undo would revert, or nullptr when nothing can be undone.
    [[nodiscard]] const char* GetUndoNameOrNull() const noexcept
    {
        return CanUndo() == true ? GetName(Cursor_ - 1) : nullptr;
    }
    /// Name of the edit Redo would reapply, or nullptr when nothing can be redone.
    [[nodiscard]] const char* GetRedoNameOrNull() const noexcept
    {
        return CanRedo() == true ? GetName(Cursor_) : nullptr;
    }
    void BreakGroup() noexcept
    {
        MergeAllowed_ = false;
    }
    // Only a new document identity may reset the monotonic revision.
    void Reset() noexcept
    {
        for (auto& entry : Entries_)
        {
            entry.reset();
        }
        Cursor_ = Count_ = 0;
        Revision_ = 0;
        BreakGroup();
    }
    [[nodiscard]] foundation::uint64 Revision() const noexcept
    {
        return Revision_;
    }

private:
    struct Entry
    {
        T Before;
        T After;
        const char* Name;
    };
    std::optional<Entry> Entries_[CAPACITY];
    foundation::usize Cursor_ = 0;
    foundation::usize Count_ = 0;
    foundation::uint64 Revision_ = 0;
    bool MergeAllowed_ = false;
};
} // namespace ludus::editor
