#pragma once

// Thanks to Jason Page, "MultiStream-The Art of Writing a Next-Gen Audio Engine",
// Game Programming Gems 7 (2008), section 4.2, pp. 305-319, for keeping related
// commands in one atomic batch. Thanks also to Guy Somberg, "Thread-Safe Command
// Buffer", Game Audio Programming 4: Principles and Practices, chapter 12,
// pp. 238-261, for publication/retirement ownership and receiver progress.
// These are design influences: Ludus retains fixed SPSC rings with immutable
// publication, not Page's SPU/DMA path or Somberg's three-buffer implementation.
// Reviewed source scope: docs/architecture/audio-research.md.

// Fixed-capacity single-producer / single-consumer ring for audio command
// batches and debug snapshots, per .kiro/specs/audio/design.md section 4 and 11.
//
// Contract:
//  - One producer thread, one consumer thread. No MPSC, no CAS retry loop, no
//    spinning in rendering, no hidden lock fallback.
//  - Payload bytes are written before the producer publishes with a release
//    store; the consumer acquires publication before reading. A slot cannot be
//    overwritten until the producer observes the consumer's release.
//  - Counters are uint32 and verified lock-free; capacity is kept below half the
//    counter space so unsigned distance arithmetic is unambiguous. Counter wrap
//    is safe for queue arithmetic only (never for identity generations).
//
// This container owns no heap: storage is an embedded fixed array. Enqueue is
// batch-aware: the producer reserves space for a whole batch and publishes it
// with a single release update after writing every record (never splitting a
// batch across the publication boundary).

#include <ludus/foundation/base/types.h>

#include <atomic>
#include <span>

namespace ludus::audio::internal
{
using ludus::foundation::uint32;
using ludus::foundation::usize;

// Capacity must be a power of two and <= 2^31 so masking and distance math are
// well defined. The record type must be trivially copyable (plain command/
// snapshot records).
template <typename RecordType, usize Capacity>
class SpscRing final
{
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(Capacity <= (1U << 31), "Capacity must stay below half the counter space");

public:
    SpscRing() noexcept = default;

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;

    [[nodiscard]] static constexpr usize GetCapacity() noexcept
    {
        return Capacity;
    }

    // Producer: number of free slots available right now. Bounded unsigned
    // distance (write - read) stays below Capacity.
    [[nodiscard]] uint32 FreeSlots() const noexcept
    {
        const uint32 write = mWrite.load(std::memory_order_relaxed);
        const uint32 read = mRead.load(std::memory_order_acquire);
        return static_cast<uint32>(Capacity) - (write - read);
    }

    // Consumer: number of published records available right now.
    [[nodiscard]] uint32 Available() const noexcept
    {
        const uint32 write = mWrite.load(std::memory_order_acquire);
        const uint32 read = mRead.load(std::memory_order_relaxed);
        return write - read;
    }

    [[nodiscard]] uint32 HighWater() const noexcept
    {
        return mHighWater.load(std::memory_order_relaxed);
    }

    // Producer: publish a whole batch atomically. Returns false (publishing
    // nothing) if the batch does not fit; the caller leaves its accounting
    // unchanged. The records are copied into the ring, then a single release
    // store on the write counter publishes them.
    [[nodiscard]] bool TryPublishBatch(std::span<const RecordType> records) noexcept
    {
        const uint32 count = static_cast<uint32>(records.size());
        if (count == 0)
        {
            return true;
        }
        if (count > Capacity)
        {
            return false;
        }
        const uint32 write = mWrite.load(std::memory_order_relaxed);
        const uint32 read = mRead.load(std::memory_order_acquire);
        const uint32 used = write - read;
        if (static_cast<uint32>(Capacity) - used < count)
        {
            return false;
        }
        for (uint32 i = 0; i < count; ++i)
        {
            mSlots[(write + i) & Mask()] = records[i];
        }
        const uint32 newWrite = write + count;
        mWrite.store(newWrite, std::memory_order_release);

        const uint32 depth = newWrite - read;
        uint32 hw = mHighWater.load(std::memory_order_relaxed);
        while (depth > hw && mHighWater.compare_exchange_weak(hw, depth, std::memory_order_relaxed) == false)
        {
            // hw refreshed by the failed exchange; retry only on the producer
            // thread's own high-water bookkeeping, never on the hot queue path.
        }
        return true;
    }

    // Consumer: peek the next published record without consuming it. Returns
    // false if nothing is available.
    [[nodiscard]] bool Peek(RecordType& out) const noexcept
    {
        const uint32 write = mWrite.load(std::memory_order_acquire);
        const uint32 read = mRead.load(std::memory_order_relaxed);
        if (write == read)
        {
            return false;
        }
        out = mSlots[read & Mask()];
        return true;
    }

    // Consumer: consume up to `max` published records into `out`. Returns the
    // number consumed. Releases the read counter once at the end so the producer
    // can reuse those slots.
    [[nodiscard]] uint32 ConsumeUpTo(RecordType* out, uint32 max) noexcept
    {
        const uint32 write = mWrite.load(std::memory_order_acquire);
        const uint32 read = mRead.load(std::memory_order_relaxed);
        uint32 avail = write - read;
        if (avail > max)
        {
            avail = max;
        }
        for (uint32 i = 0; i < avail; ++i)
        {
            out[i] = mSlots[(read + i) & Mask()];
        }
        if (avail != 0)
        {
            mRead.store(read + avail, std::memory_order_release);
        }
        return avail;
    }

    // Consumer: advance the read counter by one (used with Peek).
    void ConsumeOne() noexcept
    {
        const uint32 read = mRead.load(std::memory_order_relaxed);
        mRead.store(read + 1, std::memory_order_release);
    }

private:
    // Index mask (Capacity is a power of two). A constexpr function rather than a
    // static data member so no static initializer appears in this header
    // (bugprone-dynamic-static-initializers under the web tidy pass).
    [[nodiscard]] static constexpr uint32 Mask() noexcept
    {
        return static_cast<uint32>(Capacity) - 1U;
    }

    RecordType mSlots[Capacity] = {};
    std::atomic<uint32> mWrite{0};
    std::atomic<uint32> mRead{0};
    std::atomic<uint32> mHighWater{0};

    static_assert(std::atomic<uint32>::is_always_lock_free,
                  "SPSC ring requires lock-free uint32 atomics (verified native + wasm)");
};

} // namespace ludus::audio::internal
