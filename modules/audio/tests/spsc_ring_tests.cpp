// SPSC command ring contract tests (design section 4; tasks A1 gate rows
// "Full command ring / oversized batch", "Producer update flood / consumer
// paused mid-read"). These exercise the production ring type directly.

#include <ludus/foundation/threading/thread.hpp>

#include "internal/spsc_ring.hpp"

#include <catch2/catch_test_macros.hpp>

namespace threading = ludus::foundation::threading;

using ludus::audio::internal::SpscRing;
using ludus::foundation::uint32;

namespace
{
struct Rec final
{
    uint32 A = 0;
    uint32 B = 0;
};
} // namespace

TEST_CASE("SPSC ring publishes and consumes whole batches", "[audio][spsc]")
{
    SpscRing<Rec, 8> ring;
    REQUIRE(ring.Available() == 0);
    REQUIRE(ring.FreeSlots() == 8);

    Rec batch[3] = {{1, 10}, {2, 20}, {3, 30}};
    REQUIRE(ring.TryPublishBatch(std::span<const Rec>(batch, 3)));
    REQUIRE(ring.Available() == 3);
    REQUIRE(ring.FreeSlots() == 5);

    Rec out[3] = {};
    REQUIRE(ring.ConsumeUpTo(out, 3) == 3);
    REQUIRE(out[0].A == 1);
    REQUIRE(out[1].B == 20);
    REQUIRE(out[2].A == 3);
    REQUIRE(ring.Available() == 0);
}

TEST_CASE("SPSC ring rejects an oversized or non-fitting batch atomically", "[audio][spsc]")
{
    SpscRing<Rec, 4> ring;
    Rec tooBig[5] = {};
    REQUIRE_FALSE(ring.TryPublishBatch(std::span<const Rec>(tooBig, 5)));
    REQUIRE(ring.Available() == 0);

    Rec fill[3] = {{1, 1}, {2, 2}, {3, 3}};
    REQUIRE(ring.TryPublishBatch(std::span<const Rec>(fill, 3)));
    // Only one free slot: a 2-record batch must be refused wholesale.
    Rec two[2] = {{9, 9}, {8, 8}};
    REQUIRE_FALSE(ring.TryPublishBatch(std::span<const Rec>(two, 2)));
    REQUIRE(ring.Available() == 3); // unchanged
}

TEST_CASE("SPSC ring tracks high-water depth", "[audio][spsc]")
{
    SpscRing<Rec, 8> ring;
    Rec batch[5] = {};
    REQUIRE(ring.TryPublishBatch(std::span<const Rec>(batch, 5)));
    REQUIRE(ring.HighWater() == 5);
    Rec out[5] = {};
    REQUIRE(ring.ConsumeUpTo(out, 5) == 5);
    Rec more[2] = {};
    REQUIRE(ring.TryPublishBatch(std::span<const Rec>(more, 2)));
    REQUIRE(ring.HighWater() == 5); // high-water does not decrease
}

TEST_CASE("SPSC ring survives a producer flood with a slow consumer", "[audio][spsc]")
{
    // Published records are immutable until consumed; the producer cannot
    // retract or overwrite them, and continuous traffic does not starve the
    // consumer. One producer thread, one consumer thread.
    constexpr uint32 kTotal = 100000;
    SpscRing<Rec, 1024> ring;

    threading::Thread producer;
    REQUIRE(producer.Start([&] {
        uint32 next = 0;
        while (next < kTotal)
        {
            Rec r{next, next * 2};
            if (ring.TryPublishBatch(std::span<const Rec>(&r, 1)))
            {
                ++next;
            }
            // else: full; retry (producer-side only; never spins in rendering).
        }
    }) == threading::ThreadStatus::Ok);

    uint32 received = 0;
    uint32 expected = 0;
    bool ordered = true;
    while (received < kTotal)
    {
        Rec out[64] = {};
        const uint32 n = ring.ConsumeUpTo(out, 64);
        for (uint32 i = 0; i < n; ++i)
        {
            if (out[i].A != expected || out[i].B != expected * 2)
            {
                ordered = false;
            }
            ++expected;
        }
        received += n;
    }
    REQUIRE(producer.Join() == threading::ThreadStatus::Ok);

    REQUIRE(ordered);
    REQUIRE(received == kTotal);
}
