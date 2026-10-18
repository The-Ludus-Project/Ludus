#pragma once

// Session and per-slot generation bookkeeping, per .kiro/specs/audio/design.md
// section 3. Handles carry Session/Slot/Generation (uint32). Zero session or
// generation is invalid. Session IDs are process-unique and never reused, even
// across different AudioSystem instances. Per-slot generations increment without
// wrapping; an exhausted slot is retired until a fresh session. Generation and
// session counters are identity counters, so (unlike the queue arithmetic) their
// wrap is never treated as safe.

#include <ludus/foundation/base/types.h>

#include <atomic>

namespace ludus::audio::internal
{
using ludus::foundation::uint32;

// Process-wide, monotonically increasing session source. Starts at 1 so zero is
// always an invalid session. SequenceExhausted when it would wrap.
class SessionSource final
{
public:
    // Returns 0 on exhaustion (the caller rejects initialization).
    [[nodiscard]] static uint32 Next() noexcept
    {
        static std::atomic<uint32> sCounter{0};
        uint32 prev = sCounter.load(std::memory_order_relaxed);
        uint32 next = 0;
        do
        {
            if (prev == 0xFFFFFFFFU)
            {
                return 0; // exhausted
            }
            next = prev + 1U;
        } while (sCounter.compare_exchange_weak(prev, next, std::memory_order_relaxed) == false);
        return next;
    }
};

// Per-slot generation. Nonzero generations identify a live reservation. 0 means
// "free slot, no live generation". The high sentinel marks an exhausted slot.
struct SlotGeneration final
{
    uint32 Value = 0;

    [[nodiscard]] bool IsExhausted() const noexcept
    {
        return Value == kExhausted;
    }

    // Advance to the next live generation. Returns false if the slot is now
    // exhausted (its generation space is used up and it must stay retired until
    // a fresh session).
    [[nodiscard]] bool Advance() noexcept
    {
        if (Value >= kExhausted - 1U)
        {
            Value = kExhausted;
            return false;
        }
        ++Value;
        return true;
    }

    static constexpr uint32 kExhausted = 0xFFFFFFFFU;
};

} // namespace ludus::audio::internal
