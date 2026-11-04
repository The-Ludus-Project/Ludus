#pragma once

// Caller-owned PCG32 (XSH-RR) random stream (design §11).
//
// This is an opt-in, explicitly-seeded, non-cryptographic simulation PRNG. There
// is no global generator, no time-based or automatic reseeding, no thread-local
// stream and no cryptographic promise. Gameplay assigns stable stream selectors.
//
// Provenance: the PCG algorithm and the seeding sequence are adapted from Melissa
// O'Neill's PCG reference "pcg-c-basic" (pcg_basic.c; https://www.pcg-random.org/),
// which is distributed under the Apache License 2.0. Only the small integer
// algorithm (XSH-RR output, LCG step, two-step seeding) is reimplemented here in
// Ludus style; no source file is copied. See modules/foundation/math/README.md
// for the attribution and the frozen known-answer sequence.
//
// The integer sequence and saved state have a BIT-EXACT contract for the same
// logical stream, seed and call order. The [0,1) float mapping is also fixed and
// versioned. Replay can still differ if a different number of rejection draws is
// consumed (e.g. a different bound) — ownership of the logical call order is the
// caller's.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/status.hpp>

namespace ludus::foundation::math
{
using core::float32;
using core::uint32;
using core::uint64;

/// @brief Serialisable stream state. version==1 for v1.
///
/// Byte order on disk is the owner's responsibility (do not dump struct padding).
struct RandomState
{
    uint32 Version = 1;
    uint64 State = 0;
    uint64 Increment = 0;
};

/// @brief Caller-owned, noncryptographic PCG32 stream with explicit logical call order.
///
/// Equal seed, selector and call order produce the versioned bit-exact sequence.
/// No global state, entropy acquisition or automatic reseeding. See RandomState
/// for explicit replay snapshots and docs/decisions/0010-foundation-math-module.md for conventions.
class RandomStream
{
public:
    /// @brief Default construction uses a DETERMINISTIC seed=0/selector=0 (not entropy).
    ///
    /// mState/mIncrement have default member initializers; seedInternal overwrites
    /// them with the documented two-step PCG seeding for seed=0, selector=0.
    constexpr RandomStream() noexcept
    {
        seedInternal(0, 0);
    }

    /// @brief Reseed to the documented two-step PCG sequence. selector must be <= 0x7fffffffffffffff (a higher
    /// selector would alias its high bit and is rejected).
    ///
    /// Returns false and leaves the stream unchanged on an invalid selector. seed is any uint64.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the (seed, selector) pair is the documented PCG API order.
    [[nodiscard]] bool TryReseed(uint64 seed, uint64 selector) noexcept
    {
        if (selector > 0x7fffffffffffffffULL)
        {
            return false;
        }
        seedInternal(seed, selector);
        return true;
    }

    /// Next raw uint32 (XSH-RR output). Always advances the stream.
    [[nodiscard]] uint32 NextUInt32() noexcept;

    /// @brief Unbiased integer in [0, bound) via rejection sampling. bound must be > 0; an invalid bound leaves
    /// BOTH the output and the stream unchanged and returns InvalidArgument.
    ///
    /// The rejection loop has small expected work but NO hard worst-case iteration bound — never call it from a
    /// deadline-bounded infrastructure callback.
    [[nodiscard]] MathStatus TryNextBounded(uint32 bound, uint32& out) noexcept;

    /// @brief Float in [0,1): high 24 bits of one draw times exactly 2^-24.
    ///
    /// Includes 0, excludes 1, with a fixed single draw. Always advances the stream.
    [[nodiscard]] float32 NextFloat01() noexcept;

    /// @brief Save / restore.
    ///
    /// GetState captures a versioned snapshot. TryRestore rejects a mismatched version or an even increment
    /// (PCG increments are always odd) WITHOUT changing the stream, returning InvalidArgument.
    [[nodiscard]] RandomState GetState() const noexcept
    {
        return RandomState{1, mState, mIncrement};
    }
    /// Restore a version-1 snapshot with an odd increment; rejection leaves the stream unchanged.
    [[nodiscard]] MathStatus TryRestore(const RandomState& state) noexcept
    {
        if (state.Version != 1 || (state.Increment & 1ULL) == 0ULL)
        {
            return MathStatus::InvalidArgument;
        }
        mState = state.State;
        mIncrement = state.Increment;
        return MathStatus::Success;
    }

private:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the (seed, selector) pair is the documented PCG API order.
    LUDUS_INLINE constexpr void seedInternal(uint64 seed, uint64 selector) noexcept
    {
        // Reference sequence: state=0, inc=(selector<<1)|1, step, +seed, step.
        mState = 0;
        mIncrement = (selector << 1u) | 1u;
        stepState();
        mState += seed;
        stepState();
    }
    LUDUS_INLINE constexpr void stepState() noexcept
    {
        constexpr uint64 PCG_MULTIPLIER = 6364136223846793005ULL;
        mState = mState * PCG_MULTIPLIER + mIncrement;
    }

    uint64 mState = 0;
    uint64 mIncrement = 0;
};
} // namespace ludus::foundation::math
