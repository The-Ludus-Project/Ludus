#include "internal/world.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <utility>
using namespace ludus::world_demo;
using ludus::foundation::time::Duration;
using ludus::foundation::time::FrameSample;
using ludus::foundation::time::NANOSECONDS_PER_SECOND;
using ludus::foundation::time::Timestamp;
using ludus::foundation::time::TimeStatus;
namespace ludus::world_demo::internal
{
struct WorldTestAccess final
{
    static void SetTick(GameWorld& inoutWorld, uint64 tick) noexcept
    {
        inoutWorld.mTick = tick;
    }
};
} // namespace ludus::world_demo::internal
namespace
{
constexpr uint64 MILLISECOND = NANOSECONDS_PER_SECOND / 1000;
// A long-uptime epoch: integer intervals must stay exact far from zero.
constexpr uint64 EPOCH = uint64{1} << 62U;
Timestamp At(uint64 nanoseconds)
{
    return {EPOCH + nanoseconds};
}
uint64 GetAccountedTime(const TickDriver& driver)
{
    return driver.GetAcceptedTime().Nanoseconds + driver.GetClampedTime().Nanoseconds +
           driver.GetExcludedTime().Nanoseconds;
}
GameWorld Build(uint32 capacity = 48)
{
    Level level;
    REQUIRE(ReadLevel(ExampleLevel(), level).Error == LevelError::None);
    GameWorld world;
    REQUIRE(world.Prepare(level, capacity) == Status::Success);
    while (world.IsBuilt() == false)
    {
        REQUIRE(world.BuildNext() == Status::Success);
    }
    return world;
}
void Load(Session& session)
{
    REQUIRE(session.RequestLoad(ExampleLevel()).Error == LevelError::None);
    while (session.GetLoadStage() != LoadStage::Ready)
    {
        REQUIRE(session.PollLoad() == Status::Success);
    }
    REQUIRE(session.Activate());
}
struct TickAccounting final
{
    uint64 Ticks = 0;
    uint64 Phase = 0;
    uint64 DiscardedTicks = 0;
    uint64 AcceptedNanoseconds = 0;
};
// Drive a fresh world from a baseline at zero to `span`, asking `getFrameLength` for each frame.
template <typename FrameLength>
TickAccounting DriveFrames(uint64 span, FrameLength getFrameLength)
{
    GameWorld world = Build();
    TickDriver driver;
    REQUIRE(driver.Advance(world, At(0), {}) == Status::Success);
    uint64 now = 0;
    uint64 frame = 0;
    while (now < span)
    {
        const uint64 length = getFrameLength(frame);
        REQUIRE(length > 0);
        now = length < span - now ? now + length : span;
        REQUIRE(driver.Advance(world, At(now), {}) == Status::Success);
        REQUIRE(driver.GetTickPhase() < PHASE_UNITS_PER_TICK);
        ++frame;
    }
    REQUIRE(driver.GetClampedTime() == Duration{});
    return
    {
        .Ticks = world.GetTick(),
        .Phase = driver.GetTickPhase(),
        .DiscardedTicks = driver.GetDiscardedTicks(),
        .AcceptedNanoseconds = driver.GetAcceptedTime().Nanoseconds,
    };
}
// Deterministic irregular frame lengths in [1 ns, 50 ms]: short enough that no whole tick is discarded.
struct IrregularFrames final
{
    uint64 State = 0;
    uint64 operator()(uint64 /*frame*/) noexcept
    {
        State = State * 6364136223846793005ULL + 1442695040888963407ULL;
        return (State >> 33U) % (50 * MILLISECOND) + 1;
    }
};
// Accepted phase equals run ticks plus discarded ticks plus the carried remainder, when no reset dropped phase.
void RequirePhaseConservation(const TickAccounting& accounting, uint64 ticksPerSecond = TICKS_PER_SECOND)
{
    REQUIRE(accounting.AcceptedNanoseconds * ticksPerSecond ==
            (accounting.Ticks + accounting.DiscardedTicks) * PHASE_UNITS_PER_TICK + accounting.Phase);
}
Recipe ExtraGuard()
{
    Level level;
    REQUIRE(ReadLevel(ExampleLevel(), level).Error == LevelError::None);
    Recipe recipe = level.Entities[1];
    recipe.Id = { .Text = "extra-guard" };
    return recipe;
}
} // namespace
TEST_CASE("Level codec round-trips canonically and owns forward references", "[world-demo]")
{
    Level level;
    REQUIRE(ReadLevel(ExampleLevel(), level).Error == LevelError::None);
    ludus::foundation::core::Array<char> json;
    REQUIRE(WriteLevel(level, json));
    REQUIRE(std::string_view(json.GetData(), json.GetSize()).find("\n  \"version\": 1") != std::string_view::npos);
    Level copy;
    REQUIRE(ReadLevel({json.GetData(), json.GetSize()}, copy).Error == LevelError::None);
    ludus::foundation::core::Array<char> second;
    REQUIRE(WriteLevel(copy, second));
    REQUIRE(std::string_view(json.GetData(), json.GetSize()) == std::string_view(second.GetData(), second.GetSize()));
    auto world = Build();
    REQUIRE(world.Enemies().GetValues()[0].Target == world.FindAuthored("player-start"));
    REQUIRE(world.Registry().GetCreationSequence(world.FindAuthored("exit-a")) == 1);
}
TEST_CASE("Malformed content leaves the supplied definition unchanged", "[world-demo]")
{
    Level level;
    REQUIRE(ReadLevel(ExampleLevel(), level).Error == LevelError::None);
    const auto seed = level.Seed;
    for (auto source : {"{}", "{\"version\":2}", "{\"version\":1,\"version\":1}", "{\"unknown\":0}"})
    {
        REQUIRE(ReadLevel(source, level).Error != LevelError::None);
        REQUIRE(level.Seed == seed);
    }
    std::string invalid(ExampleLevel());
    invalid.replace(invalid.find("player-start", invalid.find("target")), 12, "missing");
    REQUIRE(ReadLevel(invalid, level).Error == LevelError::InvalidReference);
    invalid = ExampleLevel();
    invalid.replace(invalid.find("\"speed\": 4"), 10, "\"speed\":1e99");
    REQUIRE(ReadLevel(invalid, level).Error != LevelError::None);
    level.Entities[0].Position.X = std::numeric_limits<float32>::quiet_NaN();
    REQUIRE(ValidateLevel(level) != LevelError::None);
}
TEST_CASE("Candidate cancellation, failure, supersession and restart preserve ownership", "[world-demo]")
{
    Session session;
    Load(session);
    const auto old = session.World().FindAuthored("player-start");
    const auto hash = session.World().GetReplayHash();
    REQUIRE(session.RequestLoad("{}").Error != LevelError::None);
    REQUIRE(session.World().Registry().IsAlive(old));
    REQUIRE(session.World().GetReplayHash() == hash);
    REQUIRE(session.RequestLoad(ExampleLevel()).Error == LevelError::None);
    REQUIRE(session.PollLoad() == Status::Success);
    const auto transition = session.GetTransition();
    session.CancelLoad();
    REQUIRE_FALSE(session.Activate());
    REQUIRE(session.World().Registry().IsAlive(old));
    Load(session);
    REQUIRE(session.GetTransition() > transition);
    REQUIRE_FALSE(session.World().Registry().IsAlive(old));
    REQUIRE(session.World().GetReplayHash() == hash);
}
TEST_CASE("Input edges survive zero ticks, catch-up consumes once, and suspension gates held controls", "[world-demo]")
{
    InputBridge input;
    input.Sample({ .Held = true, .Pressed = true });
    input.Sample({ .Released = true });
    const auto tap = input.Consume();
    REQUIRE(tap.Pressed);
    REQUIRE(tap.Released);
    REQUIRE_FALSE(tap.Held);
    REQUIRE_FALSE(input.Consume().Pressed);
    input.Sample({ .Axis = {1, 0}, .Held = true });
    input.Cancel();
    input.Sample({ .Axis = {1, 0}, .Held = true, .Pressed = true });
    REQUIRE(input.Consume().Cancelled);
    REQUIRE_FALSE(input.Consume().Pressed);
    input.Sample({});
    input.Sample({ .Pressed = true });
    REQUIRE(input.Consume().Pressed);
    auto world = Build();
    TickDriver driver;
    REQUIRE(driver.Advance(world, At(0), { .Pressed = true }) == Status::Success);
    REQUIRE(world.GetTick() == 0);
    REQUIRE(driver.Advance(world, At(100 * MILLISECOND), {}) == Status::Success);
    REQUIRE(world.GetTick() == 4);
    // 100 ms is six ticks of debt: four run, two whole ticks are discarded and nothing remains.
    REQUIRE(driver.GetDiscardedTicks() == 2);
    REQUIRE(driver.GetTickPhase() == 0);
    REQUIRE(driver.GetAlpha() == 0);
    driver.SetMode(Mode::Paused);
    REQUIRE(driver.Advance(world, At(8 * NANOSECONDS_PER_SECOND), {}) == Status::Success);
    REQUIRE(world.GetTick() == 4);
    REQUIRE(driver.Step(world) == Status::Success);
    REQUIRE(world.GetTick() == 5);
    driver.SetMode(Mode::Playing);
    REQUIRE(driver.Advance(world, At(16 * NANOSECONDS_PER_SECOND), {}, false) == Status::Success);
    REQUIRE(world.GetTick() == 5);
}
TEST_CASE("Frame integration: startup is a zero baseline and accepted time is exact", "[world-demo][frame]")
{
    GameWorld world = Build();
    TickDriver driver;
    REQUIRE(driver.Advance(world, At(0), {}) == Status::Success);
    REQUIRE(driver.GetFrameSample().IsBaseline);
    REQUIRE(driver.GetFrameSample().Elapsed == Duration{});
    REQUIRE(world.GetTick() == 0);
    // Uneven integer partitions of one second at a large epoch accumulate without loss.
    uint64 previous = 0;
    for (uint64 frame = 1; frame <= 60; ++frame)
    {
        const uint64 next = frame * NANOSECONDS_PER_SECOND / 60;
        REQUIRE(driver.Advance(world, At(next), {}) == Status::Success);
        REQUIRE(driver.GetFrameSample().Elapsed == Duration{next - previous});
        previous = next;
    }
    REQUIRE(driver.GetAcceptedTime() == Duration{NANOSECONDS_PER_SECOND});
    REQUIRE(driver.GetClampedTime() == Duration{});
    REQUIRE(driver.GetExcludedTime() == Duration{});
    REQUIRE(driver.GetDiscardedTicks() == 0);
    REQUIRE(world.GetTick() == 60);
    REQUIRE(driver.GetTickPhase() == 0);
    // Equal readings are valid zero-length frames.
    const uint64 tick = world.GetTick();
    REQUIRE(driver.Advance(world, At(previous), {}) == Status::Success);
    REQUIRE(driver.GetFrameSample().Status == TimeStatus::Ok);
    REQUIRE(driver.GetFrameSample().Elapsed == Duration{});
    REQUIRE(world.GetTick() == tick);
}
TEST_CASE("Frame integration: a stall is clamped once and its loss stays visible", "[world-demo][frame]")
{
    GameWorld world = Build();
    TickDriver driver;
    REQUIRE(driver.Advance(world, At(0), {}) == Status::Success);
    REQUIRE(driver.Advance(world, At(2 * NANOSECONDS_PER_SECOND), {}) == Status::Success);
    const FrameSample& stall = driver.GetFrameSample();
    REQUIRE(stall.Elapsed == MAXIMUM_FRAME_DELTA);
    REQUIRE(stall.Discarded == Duration{2 * NANOSECONDS_PER_SECOND - MAXIMUM_FRAME_DELTA.Nanoseconds});
    // The FrameClock is the only clamp: all 250 ms became debt; the catch-up budget ran four ticks.
    REQUIRE(driver.GetAcceptedTime() == MAXIMUM_FRAME_DELTA);
    REQUIRE(driver.GetClampedTime() == stall.Discarded);
    REQUIRE(world.GetTick() == 4);
    // The clamp and the catch-up trim are reported separately: 15 ticks of debt, four run, 11 discarded.
    REQUIRE(driver.GetDiscardedTicks() == 11);
    REQUIRE(driver.GetTickPhase() == 0);
    // The stall advanced the baseline: the next frame measures from the stalled sample.
    REQUIRE(driver.Advance(world, At(2 * NANOSECONDS_PER_SECOND + 10 * MILLISECOND), {}) == Status::Success);
    REQUIRE(driver.GetFrameSample().Elapsed == Duration{10 * MILLISECOND});
    REQUIRE(driver.GetFrameSample().Discarded == Duration{});
    REQUIRE(GetAccountedTime(driver) == 2 * NANOSECONDS_PER_SECOND + 10 * MILLISECOND);
}
TEST_CASE("Frame integration: pause and resume never catch up paused time", "[world-demo][frame]")
{
    GameWorld world = Build();
    TickDriver driver;
    REQUIRE(driver.Advance(world, At(0), {}) == Status::Success);
    REQUIRE(driver.Advance(world, At(50 * MILLISECOND), {}) == Status::Success);
    const uint64 tick = world.GetTick();
    REQUIRE(tick == 3);
    driver.SetMode(Mode::Paused);
    for (uint64 frame = 1; frame <= 3; ++frame)
    {
        REQUIRE(driver.Advance(world, At(50 * MILLISECOND + frame * NANOSECONDS_PER_SECOND), {}) == Status::Success);
        REQUIRE(driver.GetFrameSample().Elapsed == Duration{});
        REQUIRE(world.GetTick() == tick);
        REQUIRE(driver.GetAlpha() == 1);
    }
    // Resuming drops the baseline; the first Playing frame contributes nothing.
    driver.SetMode(Mode::Playing);
    REQUIRE(driver.Advance(world, At(4 * NANOSECONDS_PER_SECOND), {}) == Status::Success);
    REQUIRE(driver.GetFrameSample().IsBaseline);
    REQUIRE(world.GetTick() == tick);
    REQUIRE(driver.Advance(world, At(4 * NANOSECONDS_PER_SECOND + 20 * MILLISECOND), {}) == Status::Success);
    REQUIRE(driver.GetFrameSample().Elapsed == Duration{20 * MILLISECOND});
    REQUIRE(world.GetTick() == tick + 1);
    REQUIRE(driver.GetAcceptedTime() == Duration{70 * MILLISECOND});
    REQUIRE(driver.GetClampedTime() == Duration{});
    REQUIRE(driver.GetExcludedTime() == Duration{4 * NANOSECONDS_PER_SECOND - 50 * MILLISECOND});
    REQUIRE(GetAccountedTime(driver) == 4 * NANOSECONDS_PER_SECOND + 20 * MILLISECOND);
    // A faulted or paused session discards time identically; Step advances exactly one tick.
    driver.SetMode(Mode::Paused);
    REQUIRE(driver.Step(world) == Status::Success);
    REQUIRE(world.GetTick() == tick + 2);
}
TEST_CASE("Frame integration: visibility suspension and interruption exclude hidden time", "[world-demo][frame]")
{
    GameWorld world = Build();
    TickDriver driver;
    REQUIRE(driver.Advance(world, At(0), {}) == Status::Success);
    REQUIRE(driver.Advance(world, At(10 * MILLISECOND), {}) == Status::Success);
    // Hidden callbacks reset the baseline and pending edges.
    REQUIRE(driver.Advance(world, At(20 * MILLISECOND), { .Pressed = true }, false) == Status::Success);
    REQUIRE(driver.Advance(world, At(30 * NANOSECONDS_PER_SECOND), {}, false) == Status::Success);
    const uint64 tick = world.GetTick();
    REQUIRE(driver.Advance(world, At(31 * NANOSECONDS_PER_SECOND), {}) == Status::Success);
    REQUIRE(driver.GetFrameSample().IsBaseline);
    REQUIRE(world.GetTick() == tick);
    // A browser stops callbacks while hidden; the queued input reset interrupts the next interval.
    driver.Interrupt();
    REQUIRE(driver.Advance(world, At(90 * NANOSECONDS_PER_SECOND), {}) == Status::Success);
    REQUIRE(driver.GetFrameSample().IsBaseline);
    REQUIRE(driver.GetFrameSample().Discarded == Duration{});
    REQUIRE(world.GetTick() == tick);
    REQUIRE(driver.GetMode() == Mode::Playing);
    REQUIRE(driver.GetAcceptedTime() == Duration{10 * MILLISECOND});
    REQUIRE(driver.GetClampedTime() == Duration{});
    REQUIRE(GetAccountedTime(driver) == 90 * NANOSECONDS_PER_SECOND);
}
TEST_CASE("Frame integration: session replacement starts from a fresh baseline", "[world-demo][frame]")
{
    Session session;
    Load(session);
    REQUIRE(session.Driver.Advance(session.World(), At(0), {}) == Status::Success);
    REQUIRE(session.Driver.Advance(session.World(), At(100 * MILLISECOND), {}) == Status::Success);
    REQUIRE(session.World().GetTick() == 4);
    session.Driver.SetMode(Mode::Paused);
    // A long load: frames keep arriving, the replacement activates and resets the driver.
    REQUIRE(session.RequestLoad(ExampleLevel()).Error == LevelError::None);
    uint64 frame = 1;
    while (session.GetLoadStage() != LoadStage::Ready)
    {
        REQUIRE(session.PollLoad() == Status::Success);
        REQUIRE(session.Driver.Advance(session.World(), At(100 * MILLISECOND + frame * NANOSECONDS_PER_SECOND), {}) ==
                Status::Success);
        ++frame;
    }
    REQUIRE(session.Activate());
    REQUIRE(session.Driver.GetMode() == Mode::Playing);
    REQUIRE(session.World().GetTick() == 0);
    REQUIRE(session.Driver.Advance(session.World(), At(100 * MILLISECOND + frame * NANOSECONDS_PER_SECOND), {}) ==
            Status::Success);
    REQUIRE(session.Driver.GetFrameSample().IsBaseline);
    REQUIRE(session.World().GetTick() == 0);
    REQUIRE(session.Driver.GetAcceptedTime() == Duration{100 * MILLISECOND});
    REQUIRE(GetAccountedTime(session.Driver) == 100 * MILLISECOND + frame * NANOSECONDS_PER_SECOND);
}
TEST_CASE("Frame integration: clock regression adds no time and cannot rebase below the high-water mark",
          "[world-demo][frame]")
{
    GameWorld world = Build();
    TickDriver driver;
    REQUIRE(driver.Advance(world, At(0), {}) == Status::Success);
    REQUIRE(driver.Advance(world, At(50 * MILLISECOND), {}) == Status::Success);
    const uint64 tick = world.GetTick();
    REQUIRE(driver.Advance(world, At(20 * MILLISECOND), {}) == Status::Success);
    REQUIRE(driver.GetFrameSample().Status == TimeStatus::ClockRegression);
    REQUIRE(driver.GetFrameSample().Elapsed == Duration{});
    REQUIRE(driver.GetClockRegressions() == 1);
    REQUIRE(world.GetTick() == tick);
    // Recovery measures from the high-water mark, never re-counting the regressed span.
    REQUIRE(driver.Advance(world, At(60 * MILLISECOND), {}) == Status::Success);
    REQUIRE(driver.GetFrameSample().Status == TimeStatus::Ok);
    REQUIRE(driver.GetFrameSample().Elapsed == Duration{10 * MILLISECOND});
    // After a lifecycle reset, a regressed sample must not become the new baseline.
    driver.Interrupt();
    REQUIRE(driver.Advance(world, At(40 * MILLISECOND), {}) == Status::Success);
    REQUIRE(driver.GetFrameSample().Status == TimeStatus::ClockRegression);
    REQUIRE_FALSE(driver.GetFrameSample().IsBaseline);
    REQUIRE(driver.GetClockRegressions() == 2);
    REQUIRE(driver.Advance(world, At(80 * MILLISECOND), {}) == Status::Success);
    REQUIRE(driver.GetFrameSample().IsBaseline);
    REQUIRE(driver.Advance(world, At(90 * MILLISECOND), {}) == Status::Success);
    REQUIRE(driver.GetFrameSample().Elapsed == Duration{10 * MILLISECOND});
    REQUIRE(driver.GetAcceptedTime() == Duration{70 * MILLISECOND});
    REQUIRE(GetAccountedTime(driver) == 90 * MILLISECOND);
}
TEST_CASE("Integer driver: tick and remainder accounting is independent of frame partitioning", "[world-demo][frame]")
{
    // 1.234567891 s at 60 Hz is 74.07407346 ticks: 74 ticks and 74,073,460 phase units remain.
    constexpr uint64 SPAN = 1'234'567'891;
    constexpr uint64 EXPECTED_TICKS = SPAN * TICKS_PER_SECOND / PHASE_UNITS_PER_TICK;
    constexpr uint64 EXPECTED_PHASE = SPAN * TICKS_PER_SECOND % PHASE_UNITS_PER_TICK;
    STATIC_REQUIRE(EXPECTED_TICKS == 74);
    STATIC_REQUIRE(EXPECTED_PHASE == 74'073'460);
    const TickAccounting partitions[] = {
        DriveFrames(SPAN, [](uint64 /*frame*/) { return MILLISECOND; }),
        DriveFrames(SPAN, [](uint64 /*frame*/) { return uint64{16'666'667}; }),
        DriveFrames(SPAN, [](uint64 /*frame*/) { return uint64{6'944'444}; }),
        DriveFrames(SPAN, [](uint64 /*frame*/) { return 50 * MILLISECOND; }),
        DriveFrames(SPAN, [](uint64 frame) { return frame % 2 == 0 ? uint64{1} : 49 * MILLISECOND; }),
        DriveFrames(SPAN, IrregularFrames{ .State = 1 }),
        DriveFrames(SPAN, IrregularFrames{ .State = 0x5eedULL }),
    };
    for (const TickAccounting& accounting : partitions)
    {
        REQUIRE(accounting.AcceptedNanoseconds == SPAN);
        REQUIRE(accounting.Ticks == EXPECTED_TICKS);
        REQUIRE(accounting.Phase == EXPECTED_PHASE);
        REQUIRE(accounting.DiscardedTicks == 0);
        RequirePhaseConservation(accounting);
    }
}
TEST_CASE("Integer driver: one second at 60 Hz is exactly 60 ticks", "[world-demo][frame]")
{
    const TickAccounting partitions[] = {
        DriveFrames(NANOSECONDS_PER_SECOND, [](uint64 /*frame*/) { return MILLISECOND; }),
        DriveFrames(NANOSECONDS_PER_SECOND, [](uint64 /*frame*/) { return uint64{16'666'666}; }),
        DriveFrames(NANOSECONDS_PER_SECOND, [](uint64 /*frame*/) { return uint64{16'666'667}; }),
        DriveFrames(NANOSECONDS_PER_SECOND, [](uint64 /*frame*/) { return uint64{6'944'445}; }),
        DriveFrames(NANOSECONDS_PER_SECOND, [](uint64 /*frame*/) { return uint64{33'333'333}; }),
        DriveFrames(NANOSECONDS_PER_SECOND, [](uint64 /*frame*/) { return 50 * MILLISECOND; }),
        DriveFrames(NANOSECONDS_PER_SECOND, IrregularFrames{ .State = 60 }),
    };
    for (const TickAccounting& accounting : partitions)
    {
        REQUIRE(accounting.Ticks == TICKS_PER_SECOND);
        REQUIRE(accounting.Phase == 0);
        REQUIRE(accounting.DiscardedTicks == 0);
    }
    // 25 ms is exactly 1.5 ticks: one runs and the remaining half tick is half of the interpolation interval.
    GameWorld world = Build();
    TickDriver driver;
    REQUIRE(driver.Advance(world, At(0), {}) == Status::Success);
    REQUIRE(driver.GetAlpha() == 0);
    REQUIRE(driver.Advance(world, At(25 * MILLISECOND), {}) == Status::Success);
    REQUIRE(world.GetTick() == 1);
    REQUIRE(driver.GetTickPhase() == PHASE_UNITS_PER_TICK / 2);
    REQUIRE(driver.GetAlpha() == 0.5F);
}
TEST_CASE("Integer driver: frame clamp and catch-up loss are explicit and separate", "[world-demo][frame]")
{
    GameWorld world = Build();
    TickDriver driver;
    REQUIRE(driver.Advance(world, At(0), {}) == Status::Success);
    // 2 s stall: 1.75 s clamped by the FrameClock; 250 ms is 15 ticks, four run and 11 are discarded.
    REQUIRE(driver.Advance(world, At(2 * NANOSECONDS_PER_SECOND), {}) == Status::Success);
    REQUIRE(driver.GetClampedTime() == Duration{2 * NANOSECONDS_PER_SECOND - MAXIMUM_FRAME_DELTA.Nanoseconds});
    REQUIRE(world.GetTick() == MAXIMUM_CATCH_UP_TICKS);
    REQUIRE(driver.GetDiscardedTicks() == 11);
    // 70 ms is 4.2 ticks: within budget, so nothing is discarded and 0.2 tick carries.
    uint64 now = 2 * NANOSECONDS_PER_SECOND + 70 * MILLISECOND;
    REQUIRE(driver.Advance(world, At(now), {}) == Status::Success);
    REQUIRE(world.GetTick() == 8);
    REQUIRE(driver.GetDiscardedTicks() == 11);
    REQUIRE(driver.GetTickPhase() == PHASE_UNITS_PER_TICK / 5);
    // 100 ms plus the carried 0.2 tick is 6.2 ticks: four run, two whole ticks are discarded, 0.2 carries.
    now += 100 * MILLISECOND;
    REQUIRE(driver.Advance(world, At(now), {}) == Status::Success);
    REQUIRE(world.GetTick() == 12);
    REQUIRE(driver.GetDiscardedTicks() == 13);
    REQUIRE(driver.GetTickPhase() == PHASE_UNITS_PER_TICK / 5);
    REQUIRE(driver.GetClampedTime() == Duration{2 * NANOSECONDS_PER_SECOND - MAXIMUM_FRAME_DELTA.Nanoseconds});
    RequirePhaseConservation(
    {
        .Ticks = world.GetTick(),
        .Phase = driver.GetTickPhase(),
        .DiscardedTicks = driver.GetDiscardedTicks(),
        .AcceptedNanoseconds = driver.GetAcceptedTime().Nanoseconds,
    });
    REQUIRE(GetAccountedTime(driver) == now);
    // A smaller catch-up budget discards more whole ticks of the same debt.
    TickDriver single;
    REQUIRE(single.Configure({ .MaximumCatchUpTicks = 1 }) == Status::Success);
    GameWorld other = Build();
    REQUIRE(single.Advance(other, At(0), {}) == Status::Success);
    REQUIRE(single.Advance(other, At(100 * MILLISECOND), {}) == Status::Success);
    REQUIRE(other.GetTick() == 1);
    REQUIRE(single.GetDiscardedTicks() == 5);
    // The largest carried remainder still interpolates strictly below one.
    TickDriver slow;
    REQUIRE(slow.Configure({ .TicksPerSecond = 1, .MaximumFrameDelta = Duration{NANOSECONDS_PER_SECOND} }) ==
            Status::Success);
    GameWorld third = Build();
    REQUIRE(slow.Advance(third, At(0), {}) == Status::Success);
    REQUIRE(slow.Advance(third, At(PHASE_UNITS_PER_TICK - 1), {}) == Status::Success);
    REQUIRE(third.GetTick() == 0);
    REQUIRE(slow.GetTickPhase() == PHASE_UNITS_PER_TICK - 1);
    REQUIRE(slow.GetAlpha() < 1);
    REQUIRE(slow.GetAlpha() > 0.99F);
}
TEST_CASE("Integer driver: configuration rejects zero rates, zero budgets and phase overflow", "[world-demo][frame]")
{
    TickDriver driver;
    REQUIRE(driver.GetPolicy().TicksPerSecond == TICKS_PER_SECOND);
    REQUIRE(driver.GetPolicy().MaximumFrameDelta == MAXIMUM_FRAME_DELTA);
    REQUIRE(driver.GetPolicy().MaximumCatchUpTicks == MAXIMUM_CATCH_UP_TICKS);
    REQUIRE(driver.Configure({}) == Status::Success);
    REQUIRE(driver.Configure({ .TicksPerSecond = 0 }) == Status::InvalidConfiguration);
    REQUIRE(driver.Configure({ .MaximumCatchUpTicks = 0 }) == Status::InvalidConfiguration);
    // MaximumFrameDelta * TicksPerSecond overflows.
    constexpr uint64 MAXIMUM = std::numeric_limits<uint64>::max();
    REQUIRE(driver.Configure({ .TicksPerSecond = MAXIMUM / MAXIMUM_FRAME_DELTA.Nanoseconds + 1 }) ==
            Status::InvalidConfiguration);
    // The product fits, but adding the largest carried remainder does not.
    constexpr uint64 LARGEST_RATE = MAXIMUM - (PHASE_UNITS_PER_TICK - 1);
    const Duration nanosecond{1};
    REQUIRE(driver.Configure({ .TicksPerSecond = LARGEST_RATE + 1, .MaximumFrameDelta = nanosecond }) ==
            Status::InvalidConfiguration);
    // Every failure preserved the defaults.
    REQUIRE(driver.GetPolicy().TicksPerSecond == TICKS_PER_SECOND);
    REQUIRE(driver.GetPolicy().MaximumFrameDelta == MAXIMUM_FRAME_DELTA);
    REQUIRE(driver.GetPolicy().MaximumCatchUpTicks == MAXIMUM_CATCH_UP_TICKS);
    // The exact bound is accepted and sufficient: repeated clamped frames never overflow the phase.
    REQUIRE(driver.Configure({ .TicksPerSecond = LARGEST_RATE, .MaximumFrameDelta = nanosecond }) == Status::Success);
    GameWorld world = Build();
    REQUIRE(driver.Advance(world, At(0), {}) == Status::Success);
    REQUIRE(driver.Advance(world, At(1), {}) == Status::Success);
    REQUIRE(world.GetTick() == MAXIMUM_CATCH_UP_TICKS);
    REQUIRE(driver.GetDiscardedTicks() == LARGEST_RATE / PHASE_UNITS_PER_TICK - MAXIMUM_CATCH_UP_TICKS);
    REQUIRE(driver.GetTickPhase() == LARGEST_RATE % PHASE_UNITS_PER_TICK);
    REQUIRE(driver.Advance(world, At(2 * MILLISECOND), {}) == Status::Success);
    REQUIRE(driver.GetClampedTime() == Duration{2 * MILLISECOND - 2});
    REQUIRE(world.GetTick() == uint64{2} * MAXIMUM_CATCH_UP_TICKS);
    REQUIRE(driver.GetMode() == Mode::Playing);
    // Configure drops the carried phase and the frame baseline.
    REQUIRE(driver.Configure({}) == Status::Success);
    REQUIRE(driver.GetTickPhase() == 0);
    REQUIRE(driver.Advance(world, At(3 * MILLISECOND), {}) == Status::Success);
    REQUIRE(driver.GetFrameSample().IsBaseline);
}
TEST_CASE("Integer driver: a failed or exhausted tick is not completed and stops the session", "[world-demo][frame]")
{
    // An invalid tick input faults the world before it publishes a tick.
    GameWorld faulted = Build();
    TickDriver driver;
    REQUIRE(driver.Advance(faulted, At(0), {}) == Status::Success);
    const TickInput invalid{ .Axis = {std::numeric_limits<float32>::quiet_NaN(), 0} };
    REQUIRE(driver.Advance(faulted, At(20 * MILLISECOND), invalid) == Status::InvalidConfiguration);
    REQUIRE(faulted.GetTick() == 0);
    REQUIRE(driver.GetMode() == Mode::Faulted);
    REQUIRE(driver.GetTickPhase() == 0);
    // The last representable tick completes; the next one exhausts the tick identity.
    GameWorld world = Build();
    ludus::world_demo::internal::WorldTestAccess::SetTick(world, std::numeric_limits<uint64>::max() - 1);
    TickDriver exhausted;
    REQUIRE(exhausted.Advance(world, At(0), {}) == Status::Success);
    REQUIRE(exhausted.Advance(world, At(40 * MILLISECOND), {}) == Status::IdentityExhausted);
    REQUIRE(world.GetTick() == std::numeric_limits<uint64>::max());
    REQUIRE(world.GetPhase() == Phase::Faulted);
    REQUIRE(exhausted.GetMode() == Mode::Faulted);
    REQUIRE(exhausted.GetTickPhase() == 0);
    REQUIRE(exhausted.GetAlpha() == 1);
    // The stopped session runs no more ticks and cannot single-step.
    REQUIRE(exhausted.Advance(world, At(100 * MILLISECOND), {}) == Status::Success);
    REQUIRE(world.GetTick() == std::numeric_limits<uint64>::max());
    REQUIRE(exhausted.Step(world) == Status::InvalidPhase);
}
TEST_CASE("Deferred spawn and destruction publish complete bundles and stale handles fail", "[world-demo]")
{
    auto world = Build();
    const auto old = world.FindAuthored("guard-a");
    REQUIRE(world.QueueDestroy(old) == Status::Success);
    REQUIRE(world.QueueDestroy(old) == Status::Success);
    REQUIRE(world.Registry().IsAlive(old));
    REQUIRE_FALSE(world.Registry().IsActive(old));
    uint64 request = 0;
    REQUIRE(world.QueueSpawn(ExtraGuard(), request) == Status::Success);
    REQUIRE(world.RunTick({}) == Status::Success);
    REQUIRE_FALSE(world.Registry().IsAlive(old));
    REQUIRE(world.Transforms().Find(old) == nullptr);
    REQUIRE(world.GetSpawnOutcomes().size() == 1);
    const auto spawned = world.GetSpawnOutcomes()[0].Entity;
    REQUIRE(world.Registry().IsActive(spawned));
    REQUIRE(world.HealthValues().Find(spawned)->Current == 2);
    REQUIRE(world.Transforms().Find(spawned)->Previous.X == world.Transforms().Find(spawned)->Current.X);
    REQUIRE(world.GetOutbox().size() == 1);
    REQUIRE(world.GetOutbox()[0].Kind == EventKind::Spawn);
    REQUIRE(world.RunTick({}) == Status::Success);
    REQUIRE(world.GetOutbox().size() == 1);
    world.ConsumeOutbox();
    REQUIRE(world.GetOutbox().empty());
}
TEST_CASE("Camera extraction uses draw interpolation and never changes authoritative state", "[world-demo][camera]")
{
    auto world = Build();
    const auto player = world.FindAuthored("player-start");
    REQUIRE(world.RunTick({ .Axis = {1, 0} }) == Status::Success);
    const auto hash = world.GetReplayHash();
    const auto* transform = world.Transforms().Find(player);
    for (const auto alpha : {0.0F, 0.25F, 1.0F})
    {
        RenderFrame fixed;
        RenderFrame follow;
        REQUIRE(world.Extract(alpha, fixed));
        REQUIRE(world.Extract(alpha, follow, true, 123));
        REQUIRE(fixed.Camera.Pose.Position.X == 0);
        REQUIRE(fixed.Camera.Lens.VerticalSpan == 14);
        REQUIRE(fixed.Camera.Lens.MinimumVisibleWidth == 24);
        const auto expected = transform->Previous.X + (transform->Current.X - transform->Previous.X) * alpha;
        REQUIRE(follow.Camera.Pose.Position.X == static_cast<float64>(expected));
        REQUIRE(follow.Camera.PresentationSequence == 123);
        REQUIRE(follow.Camera.CurrentTick == world.GetTick());
        REQUIRE(follow.Camera.DiscontinuityRevision == world.Registry().GetWorld());
        REQUIRE(follow.CameraTrace.Published.Pose.Position == follow.Camera.Pose.Position);
        REQUIRE(world.GetReplayHash() == hash);
    }
    // Pause/single-step extraction uses exact completed tick alpha, with no camera time debt.
    TickDriver driver;
    driver.SetMode(Mode::Paused);
    REQUIRE(driver.Step(world) == Status::Success);
    RenderFrame frame;
    REQUIRE(world.Extract(driver.GetAlpha(), frame, true));
    REQUIRE(frame.Camera.Pose.Position.X == static_cast<float64>(world.Transforms().Find(player)->Current.X));
    REQUIRE(world.QueueDestroy(player) == Status::Success);
    REQUIRE(world.RunTick({}) == Status::Success);
    const auto accepted = frame.Camera.Pose.Position;
    REQUIRE_FALSE(world.Extract(1, frame, true));
    REQUIRE(frame.Camera.Pose.Position == accepted);
    REQUIRE(world.Extract(1, frame)); // Explicit game fallback to authored fixed.
}

TEST_CASE("Camera follow shares fixed tick presentation cadence across rates and resume", "[world-demo][camera]")
{
    for (const uint64 rate : {30U, 60U, 120U, 240U})
    {
        auto world = Build();
        TickDriver driver;
        const auto player = world.FindAuthored("player-start");
        RenderFrame frame;
        for (uint64 sequence = 0; sequence < rate; ++sequence)
        {
            const Timestamp now = At(sequence * NANOSECONDS_PER_SECOND / rate);
            REQUIRE(driver.Advance(world, now, { .Axis = {1, 0} }) == Status::Success);
            const auto hash = world.GetReplayHash();
            const auto alpha = driver.GetAlpha();
            const auto* transform = world.Transforms().Find(player);
            REQUIRE(world.Extract(alpha, frame, true, sequence));
            const auto expected = transform->Previous.X + (transform->Current.X - transform->Previous.X) * alpha;
            REQUIRE(frame.Camera.Pose.Position.X == static_cast<float64>(expected));
            REQUIRE(world.GetReplayHash() == hash);
        }
        // Suspension clears clock debt; extraction has no independent camera debt.
        REQUIRE(driver.Advance(world, At(20 * NANOSECONDS_PER_SECOND), {}, false) == Status::Success);
        REQUIRE(driver.Advance(world, At(20 * NANOSECONDS_PER_SECOND), {}) == Status::Success);
        REQUIRE(driver.GetFrameSample().IsBaseline);
        REQUIRE(world.Extract(driver.GetAlpha(), frame, true));
        const auto* transform = world.Transforms().Find(player);
        const auto expected =
            transform->Previous.X + (transform->Current.X - transform->Previous.X) * driver.GetAlpha();
        REQUIRE(frame.Camera.Pose.Position.X == static_cast<float64>(expected));
    }
}

TEST_CASE("Commit preflight faults without publishing spawns or presentation", "[world-demo]")
{
    auto world = Build(3);
    RenderFrame frame;
    REQUIRE(world.Extract(1, frame));
    const auto old = world.FindAuthored("guard-a");
    REQUIRE(world.QueueDestroy(old) == Status::Success);
    uint64 request = 0;
    REQUIRE(world.QueueSpawn(ExtraGuard(), request) == Status::Success);
    REQUIRE(world.RunTick({}) == Status::CapacityExceeded);
    REQUIRE(world.GetTick() == 0);
    REQUIRE(world.GetPhase() == Phase::Faulted);
    REQUIRE(world.GetTraceRing().back().LastPhase == Phase::Commit);
    REQUIRE(world.GetTraceRing().back().Result == Status::CapacityExceeded);
    REQUIRE(world.Registry().IsAlive(old));
    REQUIRE(world.GetOutbox().empty());
    REQUIRE_FALSE(world.Extract(1, frame));
    // The retained frame still holds the room floor, the floor collision box, the three entities and the
    // room's placements.
    REQUIRE(frame.Count == ROOM_FLOOR_DRAWS + 4 + world.GetPlacements().size());
}
TEST_CASE("Gameplay command overflow retains failure context without publishing", "[world-demo]")
{
    Level level;
    REQUIRE(ReadLevel(ExampleLevel(), level).Error == LevelError::None);
    level.Entities[1].Position = {-5.5F, 0};
    level.Entities[1].Health = 1;
    GameWorld world;
    REQUIRE(world.Prepare(level) == Status::Success);
    while (world.IsBuilt() == false)
    {
        REQUIRE(world.BuildNext() == Status::Success);
    }
    uint64 request = 0;
    for (usize index = 0; index < 32; ++index)
    {
        REQUIRE(world.QueueSpawn(ExtraGuard(), request) == Status::Success);
    }
    REQUIRE(world.RunTick({ .Pressed = true }) == Status::CapacityExceeded);
    REQUIRE(world.GetPhase() == Phase::Faulted);
    REQUIRE(world.GetTraceRing()[0].LastPhase == Phase::Gameplay);
    REQUIRE(world.GetTraceRing()[0].Commands == 32);
    REQUIRE(world.GetTraceRing()[0].Result == Status::CapacityExceeded);
    REQUIRE(world.GetOutbox().empty());
    REQUIRE(world.Transforms().GetOwners().size() == 3);
}

TEST_CASE("A failed tick cannot publish a newly reached level outcome", "[world-demo]")
{
    Level level;
    REQUIRE(ReadLevel(ExampleLevel(), level).Error == LevelError::None);
    level.Entities[2].Position = level.Entities[0].Position;
    GameWorld world;
    REQUIRE(world.Prepare(level, 3) == Status::Success);
    while (world.IsBuilt() == false)
    {
        REQUIRE(world.BuildNext() == Status::Success);
    }
    uint64 request = 0;
    REQUIRE(world.QueueSpawn(ExtraGuard(), request) == Status::Success);
    REQUIRE(world.RunTick({}) == Status::CapacityExceeded);
    REQUIRE(world.GetPhase() == Phase::Faulted);
    REQUIRE(world.GetNextLevel().View().empty());
    REQUIRE(world.GetOutbox().empty());
}

TEST_CASE("Owned completions reject old world, request and revision values", "[world-demo]")
{
    auto world = Build();
    const auto player = world.FindAuthored("player-start");
    Completion old;
    REQUIRE(world.BeginRequest(player, old) == Status::Success);
    Completion foreign;
    REQUIRE(world.BeginRequest(player, foreign) == Status::Success);
    ++foreign.Entity.World;
    Completion current;
    REQUIRE(world.BeginRequest(player, current) == Status::Success);
    REQUIRE(world.SubmitCompletion(old) == Status::Success);
    REQUIRE(world.SubmitCompletion(foreign) == Status::InvalidEntity);
    --foreign.Entity.World;
    foreign.State = CompletionState::Cancelled;
    REQUIRE(world.SubmitCompletion(foreign) == Status::Success);
    REQUIRE(world.RunTick({}) == Status::Success);
    REQUIRE(world.GetStaleCompletions() == 3);
    current.Velocity = {6, 0};
    REQUIRE(world.SubmitCompletion(current) == Status::Success);
    const auto before = world.Transforms().Find(player)->Current.X;
    REQUIRE(world.RunTick({}) == Status::Success);
    REQUIRE(world.Transforms().Find(player)->Current.X > before);
    REQUIRE(world.SubmitCompletion(current) == Status::InvalidConfiguration);
    Completion leases[16];
    for (auto& ticket : leases)
    {
        REQUIRE(world.BeginRequest(player, ticket) == Status::Success);
    }
    Completion rejected;
    REQUIRE(world.BeginRequest(player, rejected) == Status::CapacityExceeded);
    for (auto& ticket : leases)
    {
        ticket.State = CompletionState::Cancelled;
        REQUIRE(world.SubmitCompletion(ticket) == Status::Success);
    }
    REQUIRE(world.RunTick({}) == Status::Success);
    REQUIRE(world.BeginRequest(player, rejected) == Status::Success);
}
TEST_CASE("Same-build replay hashes exclude world identity and rendering", "[world-demo]")
{
    auto first = Build();
    auto second = Build();
    REQUIRE(first.Registry().GetWorld() != second.Registry().GetWorld());
    for (usize tick = 0; tick < 120; ++tick)
    {
        const TickInput input{ .Axis = {tick < 60 ? 1.0F : -1.0F, 0}, .Pressed = tick % 30 == 0 };
        REQUIRE(first.RunTick(input) == Status::Success);
        REQUIRE(second.RunTick(input) == Status::Success);
        REQUIRE(first.GetReplayHash() == second.GetReplayHash());
        RenderFrame frame;
        REQUIRE(first.Extract(0.5F, frame));
        REQUIRE(first.GetReplayHash() == second.GetReplayHash());
    }
}

TEST_CASE("Replay boundaries include runtime colliders and outstanding tickets", "[world-demo]")
{
    auto first = Build();
    auto second = Build();
    auto recipe = ExtraGuard();
    uint64 request = 0;
    REQUIRE(first.QueueSpawn(recipe, request) == Status::Success);
    REQUIRE(first.GetReplayHash() == 0);
    recipe.HalfExtent.X *= 2;
    REQUIRE(second.QueueSpawn(recipe, request) == Status::Success);
    REQUIRE(first.RunTick({}) == Status::Success);
    REQUIRE(second.RunTick({}) == Status::Success);
    REQUIRE(first.GetReplayHash() != second.GetReplayHash());
    auto third = Build();
    auto fourth = Build();
    Completion ticket;
    REQUIRE(third.BeginRequest(third.FindAuthored("player-start"), ticket) == Status::Success);
    REQUIRE(fourth.BeginRequest(fourth.FindAuthored("player-start"), ticket) == Status::Success);
    REQUIRE(third.GetReplayHash() == fourth.GetReplayHash());
    REQUIRE(fourth.SubmitCompletion(ticket) == Status::Success);
    REQUIRE(fourth.GetReplayHash() == 0);
}

TEST_CASE("Compiled definitions run the same gameplay with enter/stay/exit triggers", "[world-demo]")
{
    Level level
    {
        .Id = { .Text = "first-room" },
        .Seed = 42,
        .Minimum = {-12, -7},
        .Maximum = {12, 7},
        .Camera = {},
        .VerticalExtent = 14,
        .Entities =
            {
                {
                    .Id = { .Text = "player-start" },
                    .Type = Kind::Player,
                    .Position = {0, 0},
                    .Speed = 4,
                    .Health = 3,
                    .Sprite = { .Text = "player" },
                },
                {
                    .Id = { .Text = "guard-a" },
                    .Type = Kind::Enemy,
                    .Position = {0.8F, 0},
                    .Speed = 2,
                    .Health = 1,
                    .Sprite = { .Text = "guard" },
                    .Target = { .Text = "player-start" },
                },
                {
                    .Id = { .Text = "exit-a" },
                    .Type = Kind::Exit,
                    .Position = {0, 0},
                    .HalfExtent = {.5F, 1},
                    .NextLevel = { .Text = "second-room" },
                },
            },
        .EntityCount = 3,
    };
    GameWorld world;
    REQUIRE(world.Prepare(level) == Status::Success);
    while (world.IsBuilt() == false)
    {
        REQUIRE(world.BuildNext() == Status::Success);
    }
    const auto guard = world.FindAuthored("guard-a");
    const auto player = world.FindAuthored("player-start");
    REQUIRE(world.RunTick({ .Pressed = true }) == Status::Success);
    REQUIRE_FALSE(world.Registry().IsAlive(guard));
    REQUIRE(world.HealthValues().Find(player)->Current == 3);
    REQUIRE(world.GetContacts().size() == 1);
    REQUIRE(world.GetContacts()[0].Kind == ContactPhase::Enter);
    REQUIRE(world.GetNextLevel().View() == "second-room");
    REQUIRE(world.GetOutbox().size() == 3);
    world.ConsumeOutbox();
    REQUIRE(world.RunTick({}) == Status::Success);
    REQUIRE(world.GetContacts()[0].Kind == ContactPhase::Stay);
    REQUIRE(world.GetOutbox().empty());
    for (usize tick = 0; tick < 20; ++tick)
    {
        REQUIRE(world.RunTick({ .Axis = {1, 0} }) == Status::Success);
    }
    REQUIRE(world.Transforms().Find(player)->Current.X > 1);
    REQUIRE(world.GetOutbox().empty());
}
TEST_CASE("Swept static collision stops crossing a thin floor", "[world-demo]")
{
    auto world = Build();
    const auto player = world.FindAuthored("player-start");
    for (usize tick = 0; tick < 100; ++tick)
    {
        REQUIRE(world.RunTick({ .Axis = {0, -1} }) == Status::Success);
    }
    REQUIRE(world.Transforms().Find(player)->Current.Y == Catch::Approx(-3.1F));
}

TEST_CASE("World authoring decoder preserves outputs and rejects incompatible sample recipes",
          "[world-demo][world-authoring]")
{
    Level level;
    REQUIRE(ReadWorldLevel(ExampleLevel(), level).Error == LevelError::None);
    const auto originalSeed = level.Seed;
    const auto source = std::string(ExampleLevel());
    for (const auto& replacement : {std::pair{"\"ludus.world-demo.v1\"", "\"unknown.schema\""},
                                    std::pair{"\"scale_x\": 1", "\"scale_x\": 0"},
                                    std::pair{"\"seed\": \"42\"", "\"seed\": \"18446744073709551616\""},
                                    std::pair{"\"target\": \"player-start\"", "\"target\": \"exit-a\""},
                                    std::pair{"\"speed\": 4", "\"speed\": 4, \"speed\": 5"},
                                    std::pair{"\"half_x\": 10", "\"unknown\": 10"}})
    {
        auto invalid = source;
        const auto position = invalid.find(replacement.first);
        REQUIRE(position != std::string::npos);
        invalid.replace(position, std::string_view(replacement.first).size(), replacement.second);
        CHECK(ReadWorldLevel(invalid, level).Error != LevelError::None);
        CHECK(level.Seed == originalSeed);
        CHECK(level.EntityCount == 3);
    }
    CHECK(ReadWorldLevel(std::string(65537, ' '), level).Error == LevelError::Capacity);
    CHECK(level.Seed == originalSeed);
    REQUIRE(ReadWorldLevel(BuiltinLevel("second-room"), level).Error == LevelError::None);
    CHECK(level.Id.View() == "second-room");
}
