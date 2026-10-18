#pragma once

// Checked strong/weak count transitions for the shared controller
// (docs/architecture/smart-pointers.md, "Controller state and reclamation").
// Extends the release/fence pattern of FoundationStrings' private
// reference_count.hpp with zero-rejecting promotion. Every transition is a
// CAS, so a concurrent saturation cannot be undone by an unchecked fetch_sub.
// The Count parameter exists so tests can saturate a reduced-width count; the
// controller instantiates uint64 only.
#include <ludus/foundation/base/types.h>

#include <atomic>

namespace ludus::foundation::detail
{
enum class CountChange : uint8
{
    Changed,     // Ordinary increment or decrement.
    Saturated,   // This increment reached the maximum: the count is now pinned.
    Pinned,      // The count was already pinned and no longer changes.
    ReachedZero, // This decrement released the last hold.
    Rejected,    // Promotion found zero: the object has been released permanently.
};

// Maximum representable count, which pins the count forever.
template <typename Count>
[[nodiscard]] constexpr Count GetPinnedCount() noexcept
{
    return static_cast<Count>(~Count{0});
}

// Adds one hold while the caller already owns one, so the count is nonzero.
// Relaxed: the caller's existing hold already protects the controller.
template <typename Count>
[[nodiscard]] CountChange RetainCount(std::atomic<Count>& inoutCount) noexcept
{
    constexpr Count PINNED = GetPinnedCount<Count>();
    Count old = inoutCount.load(std::memory_order_relaxed);
    while (old != PINNED)
    {
        const Count next = static_cast<Count>(old + 1);
        if (inoutCount.compare_exchange_weak(old, next, std::memory_order_relaxed, std::memory_order_relaxed) == true)
        {
            return next == PINNED ? CountChange::Saturated : CountChange::Changed;
        }
    }
    return CountChange::Pinned;
}

// Removes one hold. Release on success publishes this owner's prior accesses;
// the acquire fence on 1 -> 0 makes every other owner's accesses visible
// before the caller destroys or frees.
template <typename Count>
[[nodiscard]] CountChange ReleaseCount(std::atomic<Count>& inoutCount) noexcept
{
    constexpr Count PINNED = GetPinnedCount<Count>();
    Count old = inoutCount.load(std::memory_order_relaxed);
    while (old != PINNED)
    {
        if (inoutCount.compare_exchange_weak(old,
                                             static_cast<Count>(old - 1),
                                             std::memory_order_release,
                                             std::memory_order_relaxed) == true)
        {
            if (old == 1)
            {
                std::atomic_thread_fence(std::memory_order_acquire);
                return CountChange::ReachedZero;
            }
            return CountChange::Changed;
        }
    }
    return CountChange::Pinned;
}

// Adds one strong hold only while the count is nonzero; the successful CAS is
// the promotion's linearization point. Zero is permanent, so a promotion can
// never revive a released object. A pinned count is never released, so
// promotion succeeds without an increment after an acquire observation.
template <typename Count>
[[nodiscard]] CountChange TryPromoteCount(std::atomic<Count>& inoutCount) noexcept
{
    constexpr Count PINNED = GetPinnedCount<Count>();
    Count old = inoutCount.load(std::memory_order_relaxed);
    while (true)
    {
        if (old == 0)
        {
            return CountChange::Rejected;
        }
        if (old == PINNED)
        {
            std::atomic_thread_fence(std::memory_order_acquire);
            return CountChange::Pinned;
        }
        const Count next = static_cast<Count>(old + 1);
        if (inoutCount.compare_exchange_weak(old, next, std::memory_order_acquire, std::memory_order_relaxed) == true)
        {
            return next == PINNED ? CountChange::Saturated : CountChange::Changed;
        }
    }
}
} // namespace ludus::foundation::detail
