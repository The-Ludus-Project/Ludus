#pragma once

// Opt-in bounded keyboard trace and reproducible fixtures (K13).
//
// InputDebugTrace is a fixed ring that, when attached to an InputSystem, records
// each accepted/ignored record and each reset boundary with order, source, key,
// transition, ingestion sequence, native timestamp (when present), step
// assignment, and map version. It also tracks ring wrap/truncation and
// gameplay-loss counts SEPARATELY. Disabled tracing (no trace attached) is a
// predictable branch with no formatting and no cost beyond a null check.
//
// A trace that has wrapped, or that recorded gameplay overflow loss, is
// explicitly INCOMPLETE and must not be presented as an exact replay. Copies for
// inspection are caller-owned and taken outside callbacks. No filesystem work
// occurs on any ingestion/step path.

#include <ludus/foundation/base/types.h>
#include <ludus/input/key.h>
#include <ludus/input/keyboard_event.h>

#include <span>

namespace ludus::input
{
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;

inline constexpr usize TRACE_CAPACITY = 256;

/// What a trace entry captured.
enum class TraceKind : uint8
{
    Transition,
    IgnoredTransition, // repeat/duplicate: visible but not a gameplay edge
    ResetBoundary,
    Overflow, // the pending gameplay ring overflowed at this point
};

struct TraceEntry final
{
    TraceKind Kind = TraceKind::Transition;
    RecordSource Source = RecordSource::Synthetic;
    KeyTransition Transition = KeyTransition::Down;
    Key PhysicalKey = Key::Unknown;
    ResetReason Reset = ResetReason::None;
    bool HasNativeTime = false;
    uint32 NativeTimeMs = 0;
    uint32 MapVersion = 0;
    uint64 Sequence = 0;
    uint64 StepId = 0; ///< step this entry was assigned to (0 before first step)
};

/// Fixed-capacity, main-thread diagnostic ring; wrapped or lossy traces are incomplete for replay.
class InputDebugTrace final
{
public:
    InputDebugTrace() noexcept = default;

    /// @brief Record one entry.
    ///
    /// Called by InputSystem on the main thread; cheap, bounded, no allocation, no formatting. Overflow wraps
    /// the ring and marks truncation.
    void Record(const TraceEntry& entry) noexcept;

    /// @brief Borrow the valid entries in chronological order from internal scratch storage.
    ///
    /// The next View or destruction invalidates this view. Take it outside
    /// callbacks and copy the entries into caller-owned storage for retention.
    [[nodiscard]] std::span<const TraceEntry> View() const noexcept;

    [[nodiscard]] bool HasWrapped() const noexcept
    {
        return mWrapped;
    }
    /// @brief Count of entries dropped by ring wrap (trace loss), SEPARATE from gameplay
    /// overflow loss (which is counted as Overflow entries / InputCounters).
    [[nodiscard]] uint64 GetTruncatedCount() const noexcept
    {
        return mTruncated;
    }
    [[nodiscard]] uint64 GetTotalRecorded() const noexcept
    {
        return mTotal;
    }

    /// @brief A wrapped trace, or one that saw gameplay overflow, cannot be replayed as
    /// an exact capture.
    [[nodiscard]] bool IsCompleteForReplay() const noexcept
    {
        return mWrapped == false && mSawOverflow == false;
    }

    void Clear() noexcept;

private:
    TraceEntry mEntries[TRACE_CAPACITY] = {};
    usize mHead = 0; // next write index
    usize mSize = 0; // valid entries, <= TRACE_CAPACITY
    bool mWrapped = false;
    bool mSawOverflow = false;
    uint64 mTruncated = 0;
    uint64 mTotal = 0;

    // Scratch for View() so it can present a chronological, contiguous span
    // without allocating. Only the first mSize entries are meaningful.
    mutable TraceEntry mLinear[TRACE_CAPACITY] = {};
};
} // namespace ludus::input
