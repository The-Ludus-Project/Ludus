#include <ludus/foundation/math/addressed_random.hpp>

#include "internal/addressed_random.hpp"

#include <array>
#include <thread>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation::math;
using ludus::foundation::core::usize;

TEST_CASE("Philox4x32-10 matches Random123 known answers", "[math][random]")
{
    // Random123 tests/kat_vectors, revision 9545ff6413f258be2f04c1d319d99aaef7521150.
    // https://github.com/DEShawResearch/random123/blob/9545ff6413f258be2f04c1d319d99aaef7521150/tests/kat_vectors
    struct Vector
    {
        RandomBlock Counter;
        RandomKey Key;
        RandomBlock Expected;
    };
    const Vector vectors[] = {
        {{{0, 0, 0, 0}}, {0}, {{0x6627e8d5u, 0xe169c58du, 0xbc57ac4cu, 0x9b00dbd8u}}},
        {{{0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu}},
         {0xffffffffffffffffULL},
         {{0x408f276du, 0x41c83b0eu, 0xa20bc7c6u, 0x6d5451fdu}}},
        {{{0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u}},
         {0x299f31d0a4093822ULL},
         {{0xd16cfe09u, 0x94fdccebu, 0x5001e420u, 0x24126ea1u}}},
    };
    for (const Vector& vector : vectors)
    {
        const RandomBlock actual = internal::Philox4x32(vector.Counter, vector.Key);
        for (usize lane = 0; lane < 4; ++lane)
        {
            REQUIRE(actual.Values[lane] == vector.Expected.Values[lane]);
        }
    }
}

TEST_CASE("Addressed random v1 freezes derivation, packing and public mappings", "[math][random]")
{
    RandomKey key;
    REQUIRE(TryMakeRandomKey(42, 7, key) == MathStatus::Success);
    REQUIRE(key.Value == 0xccf635ee9e9e2fa4ULL);
    const RandomAddress address{0x0123456789abcdefULL, 99, 1234};
    REQUIRE(SampleUInt32(key, address) == 0x942d2d40u);
    REQUIRE(SampleFloat01(key, address) == 0.5788143277168274f);
    uint32 ticket = 0;
    REQUIRE(TrySampleBounded(key, address, 1000, ticket) == MathStatus::Success);
    REQUIRE(ticket == 578);
    // This address rejects attempts 0 and 1, then accepts attempt 2. Freeze
    // retry packing as well as the attempt-zero sequence.
    REQUIRE(SampleUInt32(key, {}) == 0xa3a6ca03u);
    REQUIRE(TrySampleBounded(key, {}, 0x80000001u, ticket) == MathStatus::Success);
    REQUIRE(ticket == 0x3c0cb600u);
    RandomBlock block;
    REQUIRE(TrySampleBlock(key, {address.Scope, address.Event, 1232}, block) == MathStatus::Success);
    const uint32 expected[] = {0x991146b3u, 0x1aa17390u, 0x942d2d40u, 0x487e565du};
    for (usize lane = 0; lane < 4; ++lane)
    {
        REQUIRE(block.Values[lane] == expected[lane]);
        REQUIRE(SampleUInt32(key, {address.Scope, address.Event, static_cast<uint16>(1232u + lane)}) == expected[lane]);
    }
    REQUIRE(RandomKeyDerivationVersion == 1);
    REQUIRE(RandomAddressLayoutVersion == 1);
    REQUIRE(RandomBoundedMappingVersion == 1);
    REQUIRE(RandomMaxDimension == 65535);
    REQUIRE(RandomBoundedAttemptCount == 65536);
}

TEST_CASE("Checked randomness boundaries preserve outputs", "[math][random]")
{
    RandomKey key{123};
    REQUIRE(TryMakeRandomKey(42, 0, key) == MathStatus::InvalidArgument);
    REQUIRE(key.Value == 123);
    REQUIRE(TryMakeRandomKey(0xffffffffffffffffULL, 0xffffffffu, key) == MathStatus::Success);

    RandomAddress address{7, 8, 9};
    REQUIRE(TryMakeRandomAddress({ .Scope = 1, .Event = 0x100000000ULL }, address) == MathStatus::OutOfRange);
    REQUIRE(TryMakeRandomAddress({ .Scope = 1, .Dimension = 65536 }, address) == MathStatus::OutOfRange);
    REQUIRE(address.Scope == 7);
    REQUIRE(address.Event == 8);
    REQUIRE(address.Dimension == 9);
    REQUIRE(TryMakeRandomAddress(
                {
                    .Scope = 0xffffffffffffffffULL,
                    .Event = 0xffffffffu,
                    .Dimension = 65535,
                },
                address) == MathStatus::Success);
    const RandomBlock packed = internal::PackRandomCounter(address, 65535);
    REQUIRE(packed.Values[0] == 0xffffffffu);
    REQUIRE(packed.Values[1] == 0xffffffffu);
    REQUIRE(packed.Values[2] == 0xffffffffu);
    REQUIRE(packed.Values[3] == 0xffff3fffu);

    RandomBlock block{{1, 2, 3, 4}};
    REQUIRE(TrySampleBlock(key, address, block) == MathStatus::InvalidArgument);
    REQUIRE(block.Values[0] == 1);
    REQUIRE(block.Values[1] == 2);
    REQUIRE(block.Values[2] == 3);
    REQUIRE(block.Values[3] == 4);
    address.Dimension = 65532;
    REQUIRE(TrySampleBlock(key, address, block) == MathStatus::Success);
    for (usize lane = 0; lane < 4; ++lane)
    {
        address.Dimension = static_cast<uint16>(65532u + lane);
        REQUIRE(SampleUInt32(key, address) == block.Values[lane]);
    }
    uint32 out = 123;
    REQUIRE(TrySampleBounded(key, address, 0, out) == MathStatus::InvalidArgument);
    REQUIRE(out == 123);
    PreparedBound32 bound;
    REQUIRE(TryPrepareBound32(17, bound) == MathStatus::Success);
    REQUIRE(TryPrepareBound32(0, bound) == MathStatus::InvalidArgument);
    REQUIRE(bound.Bound() == 17);
    REQUIRE(bound.Threshold() == 1);
}

TEST_CASE("Float mapping includes zero and excludes one", "[math][random]")
{
    REQUIRE(internal::RandomFloat01(0) == 0.0f);
    REQUIRE(internal::RandomFloat01(255) == 0.0f);
    REQUIRE(internal::RandomFloat01(256) == 0x1p-24f);
    REQUIRE(internal::RandomFloat01(0xffffffffu) == 1.0f - 0x1p-24f);
}

TEST_CASE("Prepared and direct bounds agree at integer boundaries", "[math][random]")
{
    const uint32 bounds[] = {1u, 2u, 3u, 65535u, 65536u, 65537u, 0x7fffffffu, 0x80000000u, 0x80000001u, 0xffffffffu};
    for (uint32 bound : bounds)
    {
        PreparedBound32 prepared;
        REQUIRE(TryPrepareBound32(bound, prepared) == MathStatus::Success);
        for (uint32 event = 0; event < 64; ++event)
        {
            uint32 direct = 0;
            uint32 cached = 0;
            const RandomAddress address{0xffffffffffffffffULL, event, 65535};
            REQUIRE(TrySampleBounded({0xffffffffffffffffULL}, address, bound, direct) == MathStatus::Success);
            REQUIRE(TrySampleBounded({0xffffffffffffffffULL}, address, prepared, cached) == MathStatus::Success);
            REQUIRE(direct == cached);
            REQUIRE(direct < bound);
        }
    }
}

TEST_CASE("Multiply-high rejection handles retries, last attempt and exhaustion", "[math][random]")
{
    PreparedBound32 bound;
    REQUIRE(TryPrepareBound32(0x80000001u, bound) == MathStatus::Success);
    uint32 calls = 0;
    uint32 out = 123;
    const MathStatus lastAttemptStatus = internal::SampleBounded(bound, out, [&calls](uint16 attempt) noexcept {
        ++calls;
        return attempt == 65535 ? 1u : 0u;
    });
    REQUIRE(lastAttemptStatus == MathStatus::Success);
    REQUIRE(calls == 65536);
    REQUIRE(out == 0);
    calls = 0;
    out = 123;
    const MathStatus exhaustedStatus = internal::SampleBounded(bound, out, [&calls](uint16) noexcept {
        ++calls;
        return 0u;
    });
    REQUIRE(exhaustedStatus == MathStatus::OutOfRange);
    REQUIRE(calls == 65536);
    REQUIRE(out == 123);
    calls = 0;
    const MathStatus unitBoundStatus = internal::SampleBounded(PreparedBound32{}, out, [&calls](uint16) noexcept {
        ++calls;
        return 0xffffffffu;
    });
    REQUIRE(unitBoundStatus == MathStatus::Success);
    REQUIRE(calls == 1);
    REQUIRE(out == 0);
}

TEST_CASE("Every bound in a reduced eight-bit mapper has exact equal accepted mass", "[math][random]")
{
    // Enumerate the actual production multiply-high mapper by lifting the
    // 8-bit bound into its 32-bit domain. For r in [0,255], b in [1,255],
    // bound=b*2^24 gives threshold=((256-b)%b)*2^24 and output=floor(r*b/256).
    // Exactly floor(256/b) accepted source words land on each output. This
    // oracle counts preimages; a histogram of PRNG draws would prove less.
    for (uint32 smallBound = 1; smallBound < 256; ++smallBound)
    {
        PreparedBound32 bound;
        REQUIRE(TryPrepareBound32(smallBound << 24u, bound) == MathStatus::Success);
        std::array<uint32, 256> counts{};
        for (uint32 source = 0; source < 256; ++source)
        {
            uint32 out = 0xffffffffu;
            // A rejected source then uses raw=1 (always accepted for these
            // bounds). Count only first-attempt acceptance.
            bool retried = false;
            REQUIRE(internal::SampleBounded(bound, out, [source, &retried](uint16 attempt) noexcept {
                        retried = attempt != 0;
                        return attempt == 0 ? source : 1u;
                    }) == MathStatus::Success);
            if (retried == false)
            {
                ++counts[static_cast<usize>(out)];
            }
        }
        for (usize bucket = 0; bucket < smallBound; ++bucket)
        {
            REQUIRE(counts[bucket] == 256u / smallBound);
        }
    }
}

TEST_CASE("Event samples survive worker count, task order and unrelated draws", "[math][random]")
{
    RandomKey key;
    RandomKey presentation;
    REQUIRE(TryMakeRandomKey(42, 7, key) == MathStatus::Success);
    REQUIRE(TryMakeRandomKey(42, 8, presentation) == MathStatus::Success);
    REQUIRE(key.Value != presentation.Value);
    constexpr usize count = 64;
    struct Result
    {
        uint32 Raw = 0;
        uint32 Bounded = 0;
        MathStatus Status = MathStatus::InvalidArgument;
    };
    auto evaluate = [key](usize entity) noexcept {
        const RandomAddress address{static_cast<uint64>(entity) + 0x123400000000ULL, 99, 1234};
        Result result;
        result.Raw = SampleUInt32(key, address);
        result.Status = TrySampleBounded(key, address, 0x80000001u, result.Bounded);
        return result;
    };
    std::array<Result, count> expected{};
    for (usize entity = 0; entity < count; ++entity)
    {
        expected[entity] = evaluate(entity);
    }
    for (usize workers = 1; workers <= 4; ++workers)
    {
        std::array<Result, count> actual{};
        {
            std::array<std::thread, 4> threads;
            for (usize worker = 0; worker < workers; ++worker)
            {
                threads[worker] = std::thread([&, worker] {
                    for (usize task = worker; task < count; task += workers)
                    {
                        const usize entity = (task * 37u) % count;
                        (void)SampleUInt32(presentation, {static_cast<uint64>(task), 5, 6});
                        (void)SampleUInt32(key, {static_cast<uint64>(entity), 100, 1235});
                        actual[entity] = evaluate(entity);
                    }
                });
            }
            for (usize worker = 0; worker < workers; ++worker)
            {
                threads[worker].join();
            }
        } // Join before examining disjoint writes.
        for (usize entity = 0; entity < count; ++entity)
        {
            REQUIRE(actual[entity].Status == MathStatus::Success);
            REQUIRE(actual[entity].Raw == expected[entity].Raw);
            REQUIRE(actual[entity].Bounded == expected[entity].Bounded);
        }
    }
}
