#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/logging/level.hpp>

#include <atomic>
#include <cstring>
#include <string_view>

namespace ludus::foundation::logging::internal
{

// Bounded multi-producer / single-consumer queue of OWNED log records
// (design.md section 7; requirements R24/R35/R36/R37/R38).
//
// Thanks to Dmitry Vyukov, "Bounded MPMC queue", 1024cores, algorithm/code:
// https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
// Adopt the per-cell sequence and ticket-claim publication protocol. Ludus uses
// embedded owned log records, bounded producer retries/drop accounting and one
// consumer without a dequeue CAS; it retains the stalled-producer limitation.
// Author-hosted page: https://sites.google.com/site/1024cores/home/lock-free-algorithms/queues/bounded-mpmc-queue
//
// This is a Vyukov-style bounded queue: each cell carries a `Sequence`
// atomic that encodes whose turn it is. A producer only claims a ticket by a
// SUCCESSFUL compare-exchange on the enqueue position, and only after it has
// confirmed the target cell is free for that ticket. Therefore:
//   * A failed enqueue does NOT consume a ticket and does NOT create a hole
//     (requirements R38): the enqueue position only moves on success.
//   * A producer that has claimed a ticket but is preempted before publishing
//     leaves the cell's Sequence unadvanced; the consumer simply sees "not ready"
//     and waits/So times out. The slot is NEVER reclaimed from under it
//     (requirements R35 — the explicit adversarial case).
//   * Publication uses release; consumption uses acquire; a cell is reusable only
//     after the consumer advances its Sequence (no reuse before release).
//
// Not wait-free: a paused producer between claim and publish stalls the consumer
// at that position; the consumer polls with a deadline rather than reclaiming.

struct QueuedRecord
{
    static constexpr usize kMessageCap = 1024; // prototype slot payload (spec CD3)

    uint64 Sequence = 0;
    uint64 MonotonicTicks = 0;
    uint64 NativeThreadId = 0;
    uint32 ThreadId = 0;
    uint32 CategoryId = 0;
    uint32 Line = 0;
    LogLevel Level = LogLevel::Info;
    bool Truncated = false;

    // Category name and file are process-lifetime borrowed views (constant
    // categories, __FILE__ literals), so storing the view is safe across the
    // queue. The thread NAME, however, lives in producer thread-local storage and
    // would DANGLE once that producer thread exits before the worker renders the
    // record — so it is COPIED into the slot (requirements R24; a real producer-
    // lifetime hazard found and fixed under ASan). The message bytes are copied
    // likewise.
    std::string_view CategoryName;
    std::string_view File;

    static constexpr usize kThreadNameCap = 32;
    char ThreadName[kThreadNameCap] = {};
    uint16 ThreadNameLen = 0;

    uint16 MessageLen = 0;
    char Message[kMessageCap] = {};

    void SetMessage(std::string_view m) noexcept
    {
        const usize n = m.size() < kMessageCap ? m.size() : kMessageCap;
        std::memcpy(Message, m.data(), n);
        MessageLen = static_cast<uint16>(n);
        if (n < m.size())
        {
            Truncated = true;
        }
    }

    void SetThreadName(std::string_view m) noexcept
    {
        const usize n = m.size() < kThreadNameCap ? m.size() : kThreadNameCap;
        std::memcpy(ThreadName, m.data(), n);
        ThreadNameLen = static_cast<uint16>(n);
    }

    [[nodiscard]] std::string_view ThreadNameView() const noexcept
    {
        return std::string_view(ThreadName, ThreadNameLen);
    }
};

#if defined(LUDUS_COMPILER_MSVC)
#    pragma warning(push)
// The queue cursors deliberately occupy separate cache lines.
#    pragma warning(disable : 4324)
#endif
template <usize Capacity>
class MpscQueue
{
public:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

    MpscQueue() noexcept
    {
        for (usize i = 0; i < Capacity; ++i)
        {
            mCells[i].Sequence.store(i, std::memory_order_relaxed);
        }
        mEnqueuePos.store(0, std::memory_order_relaxed);
        mDequeuePos.store(0, std::memory_order_relaxed);
    }

    // Producer: attempt to enqueue by copying from `src`. Nonblocking, bounded
    // retries. Returns true on success; false if the queue is full or contention
    // was not resolved within the retry budget (reported as a drop). Never
    // overwrites a cell owned by another producer or the consumer.
    [[nodiscard]] bool TryEnqueue(const QueuedRecord& src) noexcept
    {
        constexpr int kMaxRetries = 16; // bounded; contention exhaustion => drop
        for (int attempt = 0; attempt < kMaxRetries; ++attempt)
        {
            uint64 pos = mEnqueuePos.load(std::memory_order_relaxed);
            Cell& cell = mCells[pos & (Capacity - 1)];
            const uint64 seq = cell.Sequence.load(std::memory_order_acquire);
            const int64 diff = static_cast<int64>(seq) - static_cast<int64>(pos);

            if (diff == 0)
            {
                // Cell is free for this ticket. Claim it.
                if (mEnqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    cell.Record = src;
                    // Publish: advance the cell sequence so the consumer can take
                    // it (release makes the payload visible before the sequence).
                    cell.Sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
                // Lost the race; retry.
            }
            else if (diff < 0)
            {
                // Cell still owned by a not-yet-drained earlier occupant: full.
                mDropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            // diff > 0: another producer advanced; reload and retry.
        }
        mDropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Consumer: single-consumer dequeue. Returns true and fills `out` if a record
    // is ready; false if the queue is empty or the next producer has claimed a
    // ticket but not published yet (the paused-producer case — we wait, never
    // reclaim).
    [[nodiscard]] bool TryDequeue(QueuedRecord& out) noexcept
    {
        uint64 pos = mDequeuePos.load(std::memory_order_relaxed);
        Cell& cell = mCells[pos & (Capacity - 1)];
        const uint64 seq = cell.Sequence.load(std::memory_order_acquire);
        const int64 diff = static_cast<int64>(seq) - static_cast<int64>(pos + 1);

        if (diff == 0)
        {
            out = cell.Record;
            mDequeuePos.store(pos + 1, std::memory_order_relaxed);
            // Free the cell for the next generation (release).
            cell.Sequence.store(pos + Capacity, std::memory_order_release);
            return true;
        }
        // diff < 0: not published yet (empty or producer mid-publish). Wait.
        return false;
    }

    // True if any producer has claimed a ticket at or beyond the consumer cursor
    // (i.e. there is pending work, possibly mid-publish). Lets flush distinguish
    // "idle" from "waiting on a producer".
    [[nodiscard]] bool HasPending() const noexcept
    {
        return mEnqueuePos.load(std::memory_order_acquire) > mDequeuePos.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64 EnqueuePos() const noexcept
    {
        return mEnqueuePos.load(std::memory_order_acquire);
    }
    [[nodiscard]] uint64 DequeuePos() const noexcept
    {
        return mDequeuePos.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64 Dropped() const noexcept
    {
        return mDropped.load(std::memory_order_relaxed);
    }

private:
    struct Cell
    {
        std::atomic<uint64> Sequence{0};
        QueuedRecord Record;
    };

    alignas(64) std::atomic<uint64> mEnqueuePos{0};
    alignas(64) std::atomic<uint64> mDequeuePos{0};
    alignas(64) std::atomic<uint64> mDropped{0};
    Cell mCells[Capacity];
};
#if defined(LUDUS_COMPILER_MSVC)
#    pragma warning(pop)
#endif

} // namespace ludus::foundation::logging::internal
