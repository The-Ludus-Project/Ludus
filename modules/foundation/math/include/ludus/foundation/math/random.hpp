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
/// Do not dump the struct (its padding is not a wire format); TryEncodeRandomState in random_checkpoint.hpp writes
/// the explicit 20-byte little-endian payload.
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

/// @brief Version of RandomState's fields and their meaning; RandomState::Version and RandomStream::TryRestore
/// accept only this value. Store it in the owner's replay/content manifest.
inline constexpr uint32 RANDOM_STREAM_STATE_VERSION = 1;

/// @brief Version of the named stream setup convention implemented by TryMakeRandomStream.
///
/// Changing the seed or selector formula requires a new version and a new function; existing explicit
/// TryReseed schemes are unaffected by this convention.
inline constexpr uint32 RANDOM_STREAM_SETUP_VERSION = 1;

/// @brief Persistent identity of one mutable random owner: a manifest domain and a stable scope within it.
///
/// Domain is a nonzero ID from a checked-in manifest (see random_domains.hpp). Scope is a stable gameplay or content
/// identity that survives reconstruction, such as a saved creation ID or a persistent room ID; runtime handles,
/// array positions, pointers and thread or job indices are unsuitable. The same pair names the same owner in a
/// checkpoint (random_checkpoint.hpp).
struct RandomOwner
{
    /// Nonzero manifest domain ID.
    uint32 Domain = 0;
    /// Stable owner identity within the domain.
    uint64 Scope = 0;

    /// Memberwise equality.
    [[nodiscard]] constexpr bool operator==(const RandomOwner&) const noexcept = default;
};

/// @brief Seed a sequential stream with the named domain/scope setup convention, version 1.
///
/// Derives `domainKey = Mix64(rootSeed + (Domain + 1) * 0x9e3779b97f4a7c15)` (the same key as TryMakeRandomKey),
/// then reseeds with `seed = Mix64(domainKey + Scope)` and `selector = Domain`, using wrapping uint64 arithmetic.
/// Scopes in one domain share an increment and start at different phases; that is an organizational convention,
/// not a guarantee of disjoint or statistically independent streams. Use addressed sampling when independent
/// parallel identity is required. See "Stable keys, domains, and identities" in docs/architecture/randomness.md.
/// @param rootSeed The world's recorded root seed; zero is a valid deterministic seed.
/// @param owner The stream's persistent domain and scope.
/// @param outStream Receives the seeded stream on success; unchanged on failure.
/// @return Success, or InvalidArgument when owner.Domain is zero.
[[nodiscard]] MathStatus TryMakeRandomStream(uint64 rootSeed, RandomOwner owner, RandomStream& outStream) noexcept;
} // namespace ludus::foundation::math
