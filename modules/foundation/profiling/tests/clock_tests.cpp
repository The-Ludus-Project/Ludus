#include <ludus/foundation/profiling/clock.hpp>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation::profiling;
namespace threading = ludus::foundation::threading;
using ludus::foundation::uint64;

// Gate G3: the shared clock must be monotonic, non-decreasing, and produce
// integer-nanosecond ticks (never floating-point seconds — §5 rejects R's
// single-precision-seconds timestamps).

TEST_CASE("clock is non-decreasing on one thread", "[profiling][clock]")
{
    uint64 previous = NowTicks();
    for (int i = 0; i < 10000; ++i)
    {
        const uint64 now = NowTicks();
        REQUIRE(now >= previous);
        previous = now;
    }
}

TEST_CASE("clock advances over observable work", "[profiling][clock]")
{
    const uint64 start = NowTicks();
    // Busy-ish spin so at least one nanosecond elapses on any real clock.
    volatile uint64 sink = 0;
    for (int i = 0; i < 1'000'000; ++i)
    {
        sink = sink + static_cast<uint64>(i);
    }
    const uint64 end = NowTicks();
    CHECK(end > start);
}

TEST_CASE("ticks are nanoseconds for the steady_clock backend", "[profiling][clock]")
{
    // The MVP backend reports 1 ns per tick (ticks already are nanoseconds).
    CHECK(TicksPerNanosecondDenominator() == 1);
}

TEST_CASE("clock is non-decreasing across threads reading concurrently", "[profiling][clock]")
{
    // Not a cross-core-ordering proof (that is gate C7); this only asserts each
    // thread sees a monotonic sequence and no read faults under contention.
    constexpr ludus::foundation::usize THREAD_COUNT = 4;
    ludus::foundation::StaticArray<threading::Thread, THREAD_COUNT> threads;
    for (threading::Thread& thread : threads)
    {
        REQUIRE(thread.Start([] {
            uint64 previous = NowTicks();
            for (int i = 0; i < 20000; ++i)
            {
                const uint64 now = NowTicks();
                REQUIRE(now >= previous);
                previous = now;
            }
        }) == threading::ThreadStatus::Ok);
    }
    for (threading::Thread& thread : threads)
    {
        REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
    }
}

TEST_CASE("profiling shares FoundationTime epoch and units", "[profiling][clock]")
{
    const auto before = ludus::foundation::time::Now();
    const auto profile = NowTicks();
    const auto after = ludus::foundation::time::Now();
    CHECK(profile >= before.Nanoseconds);
    CHECK(profile <= after.Nanoseconds);
}
