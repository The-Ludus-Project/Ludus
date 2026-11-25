#pragma once

// Private SplitMix64 finalizer shared by key derivation and named stream setup.
//
// With thanks to Sebastiano Vigna, "splitmix64.c" (2015, public domain):
// https://prng.di.unimi.it/splitmix64.c
// We adapt only its finalizer to mix identifiers, not the stateful generator;
// this permutation supplies neither entropy nor secrecy. The domain-seed and
// stream-setup formulas are Ludus's own contracts
// (docs/architecture/randomness.md#stable-keys-domains-and-identities).

#include <ludus/foundation/base/core.h>

namespace ludus::foundation::math::internal
{
/// Odd 64-bit golden-ratio multiplier of the domain-key derivation v1.
inline constexpr core::uint64 RANDOM_DOMAIN_WEYL = 0x9e3779b97f4a7c15ULL;

/// SplitMix64 finalizer multipliers and xor-shift distances, in application order.
inline constexpr core::uint64 MIX64_FIRST_MULTIPLIER = 0xbf58476d1ce4e5b9ULL;
inline constexpr core::uint64 MIX64_SECOND_MULTIPLIER = 0x94d049bb133111ebULL;
inline constexpr core::uint32 MIX64_FIRST_SHIFT = 30u;
inline constexpr core::uint32 MIX64_SECOND_SHIFT = 27u;
inline constexpr core::uint32 MIX64_FINAL_SHIFT = 31u;

/// SplitMix64 finalizer; a bijection on uint64 with wrapping arithmetic.
[[nodiscard]] constexpr core::uint64 Mix64(core::uint64 value) noexcept
{
    value = (value ^ (value >> MIX64_FIRST_SHIFT)) * MIX64_FIRST_MULTIPLIER;
    value = (value ^ (value >> MIX64_SECOND_SHIFT)) * MIX64_SECOND_MULTIPLIER;
    return value ^ (value >> MIX64_FINAL_SHIFT);
}

/// Domain key v1: Mix64(rootSeed + (domainId + 1) * W). The caller rejects domain zero.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): documented root seed / domain ID order.
[[nodiscard]] constexpr core::uint64 DeriveDomainKey(core::uint64 rootSeed, core::uint32 domainId) noexcept
{
    return Mix64(rootSeed + (static_cast<core::uint64>(domainId) + 1u) * RANDOM_DOMAIN_WEYL);
}
} // namespace ludus::foundation::math::internal
