#include <ludus/foundation/math/addressed_random.hpp>

#include <ludus/foundation/base/checked_integer.hpp>

#include "internal/addressed_random.hpp"

namespace ludus::foundation::math
{
namespace
{
// With thanks to Sebastiano Vigna, "splitmix64.c" (2015, public domain):
// https://prng.di.unimi.it/splitmix64.c
// We adapt only its finalizer to mix identifiers, not the stateful generator;
// this permutation supplies neither entropy nor secrecy. The domain-seed
// formula is Ludus's own contract (docs/architecture/randomness.md).
uint64 Mix64(uint64 value) noexcept
{
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31u);
}
} // namespace

namespace internal
{
RandomBlock Philox4x32(RandomBlock counter, RandomKey key) noexcept
{
    // Salmon et al. SC11, Philox4x32-10. Constants/round order checked against
    // Random123 include/Random123/philox.h at revision
    // 9545ff6413f258be2f04c1d319d99aaef7521150 (BSD-3-Clause notice in
    // cmake/sdk/THIRD_PARTY_NOTICES.md). No macros or SIMD backend are imported.
    uint32 key0 = static_cast<uint32>(key.Value);
    uint32 key1 = static_cast<uint32>(key.Value >> 32u);
    for (uint32 round = 0; round < 10u; ++round)
    {
        const uint64 product0 = 0xd2511f53ULL * counter.Values[0];
        const uint64 product1 = 0xcd9e8d57ULL * counter.Values[2];
        counter = RandomBlock{{static_cast<uint32>(product1 >> 32u) ^ counter.Values[1] ^ key0,
                               static_cast<uint32>(product1),
                               static_cast<uint32>(product0 >> 32u) ^ counter.Values[3] ^ key1,
                               static_cast<uint32>(product0)}};
        key0 += 0x9e3779b9u;
        key1 += 0xbb67ae85u;
    }
    return counter;
}

RandomBlock PackRandomCounter(RandomAddress address, uint16 attempt) noexcept
{
    // Lane occupies the low two dimension bits. Group occupies bits [0,13];
    // bits [14,15] stay zero, retries occupy [16,31]. This packing is injective
    // across (scope,event,dimension,attempt) when lane is included.
    return RandomBlock{{static_cast<uint32>(address.Scope),
                        static_cast<uint32>(address.Scope >> 32u),
                        address.Event,
                        (static_cast<uint32>(address.Dimension) >> 2u) | (static_cast<uint32>(attempt) << 16u)}};
}

float32 RandomFloat01(uint32 bits) noexcept
{
    return static_cast<float32>(bits >> 8u) * (1.0f / 16777216.0f);
}
} // namespace internal

MathStatus TryMakeRandomKey(uint64 rootSeed, uint32 domainId, RandomKey& out) noexcept
{
    if (domainId == 0u)
    {
        return MathStatus::InvalidArgument;
    }
    out.Value = Mix64(rootSeed + (static_cast<uint64>(domainId) + 1u) * 0x9e3779b97f4a7c15ULL);
    return MathStatus::Success;
}

MathStatus TryMakeRandomAddress(RandomAddressInput input, RandomAddress& out) noexcept
{
    RandomAddress candidate{input.Scope, 0, 0};
    if (core::TryIntegerCast(input.Event, candidate.Event) == false ||
        core::TryIntegerCast(input.Dimension, candidate.Dimension) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = candidate;
    return MathStatus::Success;
}

uint32 SampleUInt32(RandomKey key, RandomAddress address) noexcept
{
    const core::usize lane = address.Dimension & 3u;
    return internal::Philox4x32(internal::PackRandomCounter(address, 0), key).Values[lane];
}

float32 SampleFloat01(RandomKey key, RandomAddress address) noexcept
{
    return internal::RandomFloat01(SampleUInt32(key, address));
}

MathStatus TrySampleBlock(RandomKey key, RandomAddress address, RandomBlock& out) noexcept
{
    if ((address.Dimension & 3u) != 0u)
    {
        return MathStatus::InvalidArgument;
    }
    out = internal::Philox4x32(internal::PackRandomCounter(address, 0), key);
    return MathStatus::Success;
}

MathStatus TryPrepareBound32(uint32 bound, PreparedBound32& out) noexcept
{
    if (bound == 0u)
    {
        return MathStatus::InvalidArgument;
    }
    out.mBound = bound;
    out.mThreshold = (0u - bound) % bound;
    return MathStatus::Success;
}

MathStatus TrySampleBounded(RandomKey key, RandomAddress address, uint32 bound, uint32& out) noexcept
{
    PreparedBound32 prepared;
    const MathStatus status = TryPrepareBound32(bound, prepared);
    if (IsSuccess(status) == false)
    {
        return status;
    }
    return TrySampleBounded(key, address, prepared, out);
}

MathStatus TrySampleBounded(RandomKey key, RandomAddress address, PreparedBound32 bound, uint32& out) noexcept
{
    // A private, inlined seam permits forcing rejection and exhaustion in tests;
    // production has no indirect call or runtime test callback.
    return internal::SampleBounded(bound, out, [key, address](uint16 attempt) noexcept {
        const core::usize lane = address.Dimension & 3u;
        return internal::Philox4x32(internal::PackRandomCounter(address, attempt), key).Values[lane];
    });
}
} // namespace ludus::foundation::math
