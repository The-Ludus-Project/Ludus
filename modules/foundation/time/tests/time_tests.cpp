#include <ludus/foundation/time/time.hpp>
#include <ludus/foundation/time/timers.hpp>

#include <array>
#include <atomic>
#include <thread>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::foundation::time;

static_assert(sizeof(Duration) == sizeof(uint64));
static_assert(sizeof(Timestamp) == sizeof(uint64));
static_assert(std::is_convertible_v<Duration, Timestamp> == false);
static_assert(std::is_convertible_v<Timestamp, Duration> == false);
static_assert(noexcept(Now()));
static_assert(std::is_trivially_copyable_v<Stopwatch>);
static_assert(std::is_trivially_copyable_v<FrameClock>);

TEST_CASE("time arithmetic checks boundaries and preserves failed outputs", "[time]")
{
    constexpr uint64 maxValue = ~uint64{0};
    Duration duration{17};
    CHECK(TryAdd(Duration{maxValue - 1}, Duration{1}, duration) == TimeStatus::Ok);
    CHECK(duration.Nanoseconds == maxValue);
    CHECK(TryAdd(duration, Duration{1}, duration) == TimeStatus::Overflow);
    CHECK(duration.Nanoseconds == maxValue);
    CHECK(TryAdd(duration, Duration{}, duration) == TimeStatus::Ok);

    Timestamp timestamp{19};
    CHECK(TryAdd(Timestamp{maxValue}, Duration{1}, timestamp) == TimeStatus::Overflow);
    CHECK(timestamp.Nanoseconds == 19);
    CHECK(TryAdd(Timestamp{maxValue - 3}, Duration{3}, timestamp) == TimeStatus::Ok);
    CHECK(timestamp.Nanoseconds == maxValue);
    CHECK(TryElapsed(timestamp, Timestamp{0}, duration) == TimeStatus::ClockRegression);
    CHECK(duration.Nanoseconds == maxValue);
    CHECK(TryElapsed(Timestamp{0}, timestamp, duration) == TimeStatus::Ok);
    CHECK(duration.Nanoseconds == maxValue);
    CHECK(TryElapsed(timestamp, timestamp, duration) == TimeStatus::Ok);
    CHECK(duration.Nanoseconds == 0);
}

TEST_CASE("short intervals retain precision at long uptime", "[time]")
{
    // An absolute double-nanosecond epoch cannot distinguish these samples.
    constexpr uint64 origin = uint64{1} << 63;
    Duration interval;
    REQUIRE(TryElapsed(Timestamp{origin}, Timestamp{origin + 1}, interval) == TimeStatus::Ok);
    CHECK(interval.Nanoseconds == 1);
    CHECK(ToSeconds(interval) == 1e-9);
    CHECK(ToSeconds(Duration{1'500'000'000}) == 1.5);
}

TEST_CASE("stopwatches pause independently and exclude paused time", "[time]")
{
    Stopwatch gameplay;
    Stopwatch ui;
    Duration elapsed{23};
    CHECK(gameplay.Read(Timestamp{}, elapsed) == TimeStatus::InvalidState);
    CHECK(elapsed.Nanoseconds == 23);
    gameplay.Start(Timestamp{0});
    ui.Start(Timestamp{0});
    REQUIRE(gameplay.Pause(Timestamp{10}) == TimeStatus::Ok);
    CHECK(gameplay.GetState() == StopwatchState::Paused);
    REQUIRE(gameplay.Read(Timestamp{1000}, elapsed) == TimeStatus::Ok);
    CHECK(elapsed.Nanoseconds == 10);
    REQUIRE(ui.Read(Timestamp{1000}, elapsed) == TimeStatus::Ok);
    CHECK(elapsed.Nanoseconds == 1000);
    REQUIRE(gameplay.Resume(Timestamp{1000}) == TimeStatus::Ok);
    REQUIRE(gameplay.Read(Timestamp{1005}, elapsed) == TimeStatus::Ok);
    CHECK(elapsed.Nanoseconds == 15);
    REQUIRE(gameplay.Pause(Timestamp{1005}) == TimeStatus::Ok);
    REQUIRE(gameplay.Resume(Timestamp{2000}) == TimeStatus::Ok);
    REQUIRE(gameplay.Read(Timestamp{2007}, elapsed) == TimeStatus::Ok);
    CHECK(elapsed.Nanoseconds == 22);
    gameplay.Start(Timestamp{3000});
    REQUIRE(gameplay.Read(Timestamp{3000}, elapsed) == TimeStatus::Ok);
    CHECK(elapsed.Nanoseconds == 0);
    gameplay.Reset();
    CHECK(gameplay.GetState() == StopwatchState::Stopped);
}

TEST_CASE("failed stopwatch transitions preserve inspectable state", "[time]")
{
    Stopwatch timer;
    Duration elapsed{41};
    CHECK(timer.Pause(Timestamp{}) == TimeStatus::InvalidState);
    CHECK(timer.Resume(Timestamp{}) == TimeStatus::InvalidState);
    timer.Start(Timestamp{100});
    CHECK(timer.Resume(Timestamp{101}) == TimeStatus::InvalidState);
    CHECK(timer.Read(Timestamp{99}, elapsed) == TimeStatus::ClockRegression);
    CHECK(elapsed.Nanoseconds == 41);
    CHECK(timer.Pause(Timestamp{99}) == TimeStatus::ClockRegression);
    CHECK(timer.GetState() == StopwatchState::Running);
    REQUIRE(timer.Pause(Timestamp{110}) == TimeStatus::Ok);
    CHECK(timer.Pause(Timestamp{111}) == TimeStatus::InvalidState);
    CHECK(timer.Resume(Timestamp{109}) == TimeStatus::ClockRegression);
    CHECK(timer.GetState() == StopwatchState::Paused);
    REQUIRE(timer.Read(Timestamp{1000}, elapsed) == TimeStatus::Ok);
    CHECK(elapsed.Nanoseconds == 10);
    REQUIRE(timer.Resume(Timestamp{110}) == TimeStatus::Ok);
    REQUIRE(timer.Read(Timestamp{110}, elapsed) == TimeStatus::Ok);
    CHECK(elapsed.Nanoseconds == 10);
}

TEST_CASE("deadline zero cancellation equality and overflow are explicit", "[time]")
{
    Deadline deadline;
    CHECK_FALSE(deadline.IsArmed());
    CHECK_FALSE(deadline.IsExpired(Timestamp{~uint64{0}}));
    REQUIRE(deadline.Arm(Timestamp{}, Duration{}) == TimeStatus::Ok);
    CHECK(deadline.IsExpired(Timestamp{}));
    REQUIRE(deadline.Arm(Timestamp{100}, Duration{20}) == TimeStatus::Ok);
    CHECK_FALSE(deadline.IsExpired(Timestamp{119}));
    CHECK(deadline.IsExpired(Timestamp{120}));
    CHECK(deadline.IsArmed());
    CHECK(deadline.Arm(Timestamp{~uint64{0}}, Duration{1}) == TimeStatus::Overflow);
    CHECK(deadline.GetDue().Nanoseconds == 120);
    CHECK(deadline.IsArmed());
    deadline.Cancel();
    CHECK_FALSE(deadline.IsExpired(Timestamp{120}));
    CHECK(deadline.Arm(Timestamp{~uint64{0}}, Duration{1}) == TimeStatus::Overflow);
    CHECK_FALSE(deadline.IsArmed());
}

TEST_CASE("frame clock reports stalls without hiding or carrying discarded time", "[time]")
{
    FrameClock clock{Duration{250}};
    const auto first = clock.Sample(Timestamp{100});
    CHECK(first.IsBaseline);
    CHECK(first.Elapsed.Nanoseconds == 0);
    CHECK(first.Discarded.Nanoseconds == 0);
    const auto equal = clock.Sample(Timestamp{100});
    CHECK_FALSE(equal.IsBaseline);
    CHECK(equal.Status == TimeStatus::Ok);
    CHECK(equal.Elapsed.Nanoseconds == 0);
    const auto boundary = clock.Sample(Timestamp{350});
    CHECK(boundary.Elapsed.Nanoseconds == 250);
    CHECK(boundary.Discarded.Nanoseconds == 0);
    const auto stall = clock.Sample(Timestamp{1351});
    CHECK(stall.Elapsed.Nanoseconds == 250);
    CHECK(stall.Discarded.Nanoseconds == 751);
    CHECK(stall.Elapsed.Nanoseconds + stall.Discarded.Nanoseconds == 1001);
    CHECK(clock.Sample(Timestamp{1360}).Elapsed.Nanoseconds == 9);
    clock.Reset();
    CHECK(clock.Sample(Timestamp{1'000'000}).IsBaseline);
    CHECK(clock.Sample(Timestamp{1'000'001}).Elapsed.Nanoseconds == 1);
}

TEST_CASE("frame clock rejects regressions without double-counting recovery", "[time]")
{
    FrameClock clock{Duration{100}};
    (void)clock.Sample(Timestamp{1000});
    const auto reversed = clock.Sample(Timestamp{900});
    CHECK(reversed.Status == TimeStatus::ClockRegression);
    CHECK(reversed.Elapsed.Nanoseconds == 0);
    CHECK(clock.Sample(Timestamp{950}).Status == TimeStatus::ClockRegression);
    CHECK(clock.Sample(Timestamp{1001}).Elapsed.Nanoseconds == 1);
    FrameClock frozen{Duration{0}};
    (void)frozen.Sample(Timestamp{0});
    const auto sample = frozen.Sample(Timestamp{~uint64{0}});
    CHECK(sample.Elapsed.Nanoseconds == 0);
    CHECK(sample.Discarded.Nanoseconds == ~uint64{0});
}

TEST_CASE("production clock is monotonic under concurrent readers", "[time][clock]")
{
    // Workers report through an atomic; Catch assertions stay on the test thread.
    std::atomic<bool> valid{true};
    std::array<std::thread, 4> threads;
    for (auto& thread : threads)
    {
        thread = std::thread([&valid] {
            auto previous = Now();
            for (usize i = 0; i < 10'000; ++i)
            {
                const auto now = Now();
                if (now.Nanoseconds < previous.Nanoseconds)
                {
                    valid.store(false, std::memory_order_relaxed);
                }
                previous = now;
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    CHECK(valid.load(std::memory_order_relaxed));
}
