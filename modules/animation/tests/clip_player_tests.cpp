// Clip player control: activation, half-open event intervals, reverse playback, loop wraps,
// exact endpoints, seeks, capacity rejection, and control independence from visual evaluation.

#include "clip_fixtures.hpp"

#include <ludus/animation/clip_player.hpp>
#include <ludus/animation/pose.hpp>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <limits>

using namespace ludus::animation;
using namespace ludus::animation::test;
namespace foundation = ludus::foundation;

namespace
{
/// Room for every event of a few ordinary ticks.
constexpr usize TICK_EVENT_CAPACITY = 16;

struct PlayerFixture final
{
    Skeleton ChainSkeleton;
    ClipSource Source;
    Clip ClampClip;
    Clip LoopClip;
};

PlayerFixture MakePlayerFixture()
{
    PlayerFixture fixture
    {
        .ChainSkeleton = PrepareSkeleton(MakeChainJoints(), CHAIN_AUTHORING_ID),
        .Source = MakeClipSource(),
        .ClampClip = {},
        .LoopClip = {},
    };
    fixture.ClampClip = PrepareClip(MakeClipPackage(fixture.Source, ClipWrapMode::Clamp), fixture.ChainSkeleton);
    fixture.LoopClip = PrepareClip(MakeClipPackage(fixture.Source, ClipWrapMode::Loop), fixture.ChainSkeleton);
    return fixture;
}

/// Advance and return the delivered events' identities and cycles.
struct DeliveredEvent final
{
    uint64 EventId = 0;
    int64 Cycle = 0;
    ClipTraversalDirection Direction = ClipTraversalDirection::Forward;
};

foundation::Array<DeliveredEvent>
Advance(const Clip& clip, float64 deltaSeconds, ClipPlayer& inoutPlayer, ClipAdvanceResult& outResult)
{
    foundation::Array<ClipEvent> buffer = MakeEventBuffer(TICK_EVENT_CAPACITY);
    REQUIRE(TryAdvanceClipPlayer(clip, deltaSeconds, MakeEventView(buffer), inoutPlayer, outResult) ==
            AnimationStatus::Success);
    foundation::Array<DeliveredEvent> delivered;
    for (usize index = 0; index < outResult.EventCount; ++index)
    {
        const ClipEvent& event = buffer[index];
        REQUIRE(event.InstanceId == inoutPlayer.GetInstanceId());
        REQUIRE(event.ActivationId == inoutPlayer.GetActivationId());
        REQUIRE(delivered.TryAdd({ .EventId = event.EventId, .Cycle = event.Cycle, .Direction = event.Direction }) ==
                true);
    }
    return delivered;
}

void RequireIds(const foundation::Array<DeliveredEvent>& delivered,
                std::initializer_list<uint64> expected,
                ClipTraversalDirection direction)
{
    REQUIRE(delivered.GetSize() == expected.size());
    usize index = 0;
    for (const uint64 eventId : expected)
    {
        REQUIRE(delivered[index].EventId == eventId);
        REQUIRE(delivered[index].Direction == direction);
        ++index;
    }
}

bool AreSamePlayers(const ClipPlayer& left, const ClipPlayer& right)
{
    return left.IsActive() == right.IsActive() && left.IsPaused() == right.IsPaused() &&
           left.IsCompleted() == right.IsCompleted() && left.GetTimeSeconds() == right.GetTimeSeconds() &&
           left.GetPreviousTimeSeconds() == right.GetPreviousTimeSeconds() && left.GetCycle() == right.GetCycle() &&
           left.GetRate() == right.GetRate();
}
} // namespace

TEST_CASE("Activation validates its identities, start time and rate", "[animation][player]")
{
    const PlayerFixture fixture = MakePlayerFixture();
    ClipPlayer player = ActivatePlayer(fixture.LoopClip, 0.5, 1);
    const ClipActivation valid{ .InstanceId = 3, .ActivationId = 4, .StartTimeSeconds = 0.25, .Rate = -2 };
    ClipActivation activation = valid;
    activation.InstanceId = 0;
    REQUIRE(TryActivateClipPlayer(fixture.LoopClip, activation, player) == AnimationStatus::InvalidArgument);
    activation = valid;
    activation.ActivationId = 0;
    REQUIRE(TryActivateClipPlayer(fixture.LoopClip, activation, player) == AnimationStatus::InvalidArgument);
    activation = valid;
    activation.StartTimeSeconds = 1.5;
    REQUIRE(TryActivateClipPlayer(fixture.LoopClip, activation, player) == AnimationStatus::InvalidArgument);
    activation = valid;
    activation.Rate = std::numeric_limits<float64>::infinity();
    REQUIRE(TryActivateClipPlayer(fixture.LoopClip, activation, player) == AnimationStatus::InvalidArgument);
    REQUIRE(player.GetTimeSeconds() == 0.5);

    REQUIRE(TryActivateClipPlayer(fixture.LoopClip, valid, player) == AnimationStatus::Success);
    REQUIRE(player.IsActive() == true);
    REQUIRE(player.GetInstanceId() == 3);
    REQUIRE(player.GetActivationId() == 4);
    REQUIRE(player.GetRate() == -2);
    REQUIRE(player.GetClipFingerprint() == fixture.LoopClip.GetFingerprint());

    // A loop maps its duration to the start of the cycle.
    activation = valid;
    activation.StartTimeSeconds = CLIP_DURATION_SECONDS;
    REQUIRE(TryActivateClipPlayer(fixture.LoopClip, activation, player) == AnimationStatus::Success);
    REQUIRE(player.GetTimeSeconds() == 0);
    REQUIRE(TryActivateClipPlayer(fixture.ClampClip, activation, player) == AnimationStatus::Success);
    REQUIRE(player.GetTimeSeconds() == CLIP_DURATION_SECONDS);
}

TEST_CASE("Forward playback delivers events in half-open (old, new] intervals", "[animation][player][events]")
{
    const PlayerFixture fixture = MakePlayerFixture();
    ClipPlayer player = ActivatePlayer(fixture.ClampClip, 0, 1);
    ClipAdvanceResult result{};

    // Activation is not a traversal: the time-zero event does not fire on entry.
    RequireIds(Advance(fixture.ClampClip, 0.25, player, result), {EVENT_QUARTER}, ClipTraversalDirection::Forward);
    REQUIRE(player.GetPreviousTimeSeconds() == 0);
    REQUIRE(player.GetTimeSeconds() == 0.25);
    // The event exactly at the old position is not delivered again.
    RequireIds(Advance(fixture.ClampClip, 0.25, player, result), {EVENT_HALF}, ClipTraversalDirection::Forward);
    RequireIds(Advance(fixture.ClampClip, 0.1, player, result), {}, ClipTraversalDirection::Forward);
    REQUIRE(result.HasCompleted == false);
}

TEST_CASE("A one-shot clip clamps, fires its endpoint event once and latches completion", "[animation][player][events]")
{
    const PlayerFixture fixture = MakePlayerFixture();
    ClipPlayer player = ActivatePlayer(fixture.ClampClip, 0.75, 1);
    ClipAdvanceResult result{};
    RequireIds(Advance(fixture.ClampClip, 10, player, result), {EVENT_END}, ClipTraversalDirection::Forward);
    REQUIRE(result.HasCompleted == true);
    REQUIRE(result.CyclesCrossed == 0);
    REQUIRE(player.IsCompleted() == true);
    REQUIRE(player.GetTimeSeconds() == CLIP_DURATION_SECONDS);

    // Completion is reported once; later steps neither move nor deliver.
    RequireIds(Advance(fixture.ClampClip, 1, player, result), {}, ClipTraversalDirection::Forward);
    REQUIRE(result.HasCompleted == false);
    REQUIRE(player.GetTimeSeconds() == CLIP_DURATION_SECONDS);
    REQUIRE(IsNear(result.RootDelta, RootTransform{}, 0));

    // Starting exactly on the endpoint completes on the first forward step without events.
    player = ActivatePlayer(fixture.ClampClip, CLIP_DURATION_SECONDS, 1);
    RequireIds(Advance(fixture.ClampClip, 0.1, player, result), {}, ClipTraversalDirection::Forward);
    REQUIRE(result.HasCompleted == true);

    // A zero step is not a traversal and does not complete.
    player = ActivatePlayer(fixture.ClampClip, CLIP_DURATION_SECONDS, 1);
    RequireIds(Advance(fixture.ClampClip, 0, player, result), {}, ClipTraversalDirection::Forward);
    REQUIRE(player.IsCompleted() == false);
}

TEST_CASE("Reverse playback delivers reversible events in [new, old)", "[animation][player][events]")
{
    const PlayerFixture fixture = MakePlayerFixture();
    ClipPlayer player = ActivatePlayer(fixture.ClampClip, CLIP_DURATION_SECONDS, -1);
    ClipAdvanceResult result{};
    // The endpoint is the old position and is excluded; the half event lies on the new position.
    RequireIds(Advance(fixture.ClampClip, 0.5, player, result), {EVENT_HALF}, ClipTraversalDirection::Reverse);
    // The forward-only quarter event is skipped; the start event is reversible.
    RequireIds(Advance(fixture.ClampClip, 0.75, player, result), {EVENT_START}, ClipTraversalDirection::Reverse);
    REQUIRE(result.HasCompleted == true);
    REQUIRE(player.GetTimeSeconds() == 0);
    REQUIRE(player.IsCompleted() == true);
}

TEST_CASE("Loop crossings split into per-cycle intervals with the new cycle identity", "[animation][player][loop]")
{
    const PlayerFixture fixture = MakePlayerFixture();
    ClipPlayer player = ActivatePlayer(fixture.LoopClip, 0.75, 1);
    ClipAdvanceResult result{};

    // Landing exactly on the wrap enters cycle 1 at time 0 and fires both loop-start aliases there.
    foundation::Array<DeliveredEvent> delivered = Advance(fixture.LoopClip, 0.25, player, result);
    RequireIds(delivered, {EVENT_START, EVENT_END}, ClipTraversalDirection::Forward);
    REQUIRE(delivered[0].Cycle == 1);
    REQUIRE(delivered[1].Cycle == 1);
    REQUIRE(result.CyclesCrossed == 1);
    REQUIRE(player.GetCycle() == 1);
    REQUIRE(player.GetTimeSeconds() == 0);

    // Leaving the wrap point does not repeat them.
    RequireIds(Advance(fixture.LoopClip, 0.3, player, result), {EVENT_QUARTER}, ClipTraversalDirection::Forward);

    // Reverse across the wrap: loop-start events belong to cycle 1, delivered in descending order.
    player = ActivatePlayer(fixture.LoopClip, 0, 1);
    Advance(fixture.LoopClip, 1.25, player, result);
    REQUIRE(player.GetCycle() == 1);
    REQUIRE(player.TrySetRate(-1) == AnimationStatus::Success);
    delivered = Advance(fixture.LoopClip, 0.5, player, result);
    RequireIds(delivered, {EVENT_END, EVENT_START}, ClipTraversalDirection::Reverse);
    REQUIRE(delivered[0].Cycle == 1);
    REQUIRE(result.CyclesCrossed == -1);
    REQUIRE(player.GetCycle() == 0);
    REQUIRE(player.GetTimeSeconds() == 0.75);
}

TEST_CASE("A large loop wrap delivers every crossing in order or rejects the whole step", "[animation][player][loop]")
{
    const PlayerFixture fixture = MakePlayerFixture();
    ClipPlayer player = ActivatePlayer(fixture.LoopClip, 0.75, 1);
    const ClipPlayer before = player;
    constexpr int64 CROSSED = 1001;
    // Cycle 0 after 0.75: nothing; 1000 full cycles of 4 events; cycle 1001 up to 0.25: 3 events.
    constexpr usize EXPECTED_EVENTS = 1000 * CLIP_EVENT_COUNT + 3;

    foundation::Array<ClipEvent> tooSmall = MakeEventBuffer(EXPECTED_EVENTS - 1);
    ClipAdvanceResult result{ .EventCount = 99 };
    REQUIRE(TryAdvanceClipPlayer(fixture.LoopClip, 1000.5, MakeEventView(tooSmall), player, result) ==
            AnimationStatus::EventCapacityExceeded);
    REQUIRE(AreSamePlayers(player, before) == true);
    REQUIRE(result.EventCount == 99);

    foundation::Array<ClipEvent> buffer = MakeEventBuffer(EXPECTED_EVENTS);
    REQUIRE(TryAdvanceClipPlayer(fixture.LoopClip, 1000.5, MakeEventView(buffer), player, result) ==
            AnimationStatus::Success);
    REQUIRE(result.EventCount == EXPECTED_EVENTS);
    REQUIRE(result.CyclesCrossed == CROSSED);
    REQUIRE(player.GetCycle() == CROSSED);
    REQUIRE(player.GetTimeSeconds() == 0.25);
    REQUIRE(buffer[0].Cycle == 1);
    REQUIRE(buffer[0].EventId == EVENT_START);
    REQUIRE(buffer[3].EventId == EVENT_HALF);
    REQUIRE(buffer[4].Cycle == 2);
    REQUIRE(buffer[EXPECTED_EVENTS - 1].Cycle == CROSSED);
    REQUIRE(buffer[EXPECTED_EVENTS - 1].EventId == EVENT_QUARTER);
    for (usize index = 1; index < EXPECTED_EVENTS; ++index)
    {
        REQUIRE(buffer[index].Cycle >= buffer[index - 1].Cycle);
    }
}

TEST_CASE("Event-free loops cross many cycles arithmetically and reject overflow", "[animation][player][loop]")
{
    const PlayerFixture fixture = MakePlayerFixture();
    ClipPackage package = MakeClipPackage(fixture.Source, ClipWrapMode::Loop);
    package.Events = {};
    const Clip clip = PrepareClip(package, fixture.ChainSkeleton);
    ClipPlayer player = ActivatePlayer(clip, 0.5, 1);
    ClipAdvanceResult result{};
    constexpr float64 BILLION = 1e9;
    REQUIRE(TryAdvanceClipPlayer(clip, BILLION, {}, player, result) == AnimationStatus::Success);
    REQUIRE(player.GetCycle() == static_cast<int64>(BILLION));
    REQUIRE(player.GetTimeSeconds() == 0.5);
    REQUIRE(result.EventCount == 0);

    const ClipPlayer before = player;
    REQUIRE(TryAdvanceClipPlayer(clip, static_cast<float64>(MAX_CLIP_TRAVERSAL_CYCLES) * 2, {}, player, result) ==
            AnimationStatus::OutOfRange);
    REQUIRE(AreSamePlayers(player, before) == true);
    REQUIRE(player.TrySetRate(std::numeric_limits<float64>::max()) == AnimationStatus::Success);
    REQUIRE(TryAdvanceClipPlayer(clip, BILLION, {}, player, result) == AnimationStatus::OutOfRange);
    REQUIRE(player.GetCycle() == before.GetCycle());
}

TEST_CASE("Seeks scrub without events or traverse intentionally", "[animation][player][seek]")
{
    const PlayerFixture fixture = MakePlayerFixture();
    ClipPlayer player = ActivatePlayer(fixture.ClampClip, 0.1, 1);
    ClipAdvanceResult result{};
    foundation::Array<ClipEvent> buffer = MakeEventBuffer(TICK_EVENT_CAPACITY);

    REQUIRE(TrySeekClipPlayer(fixture.ClampClip, 0.875, ClipSeekPolicy::Scrub, MakeEventView(buffer), player, result) ==
            AnimationStatus::Success);
    REQUIRE(result.EventCount == 0);
    REQUIRE(IsNear(result.RootDelta, RootTransform{}, 0));
    REQUIRE(player.GetTimeSeconds() == 0.875);

    REQUIRE(
        TrySeekClipPlayer(fixture.ClampClip, 0.3, ClipSeekPolicy::Traverse, MakeEventView(buffer), player, result) ==
        AnimationStatus::Success);
    REQUIRE(result.EventCount == 1);
    REQUIRE(buffer[0].EventId == EVENT_HALF);
    REQUIRE(buffer[0].Direction == ClipTraversalDirection::Reverse);
    REQUIRE(player.GetTimeSeconds() == 0.3);

    // A completed one-shot is reopened by a seek.
    Advance(fixture.ClampClip, 5, player, result);
    REQUIRE(player.IsCompleted() == true);
    REQUIRE(TrySeekClipPlayer(fixture.ClampClip, 0, ClipSeekPolicy::Scrub, MakeEventView(buffer), player, result) ==
            AnimationStatus::Success);
    REQUIRE(player.IsCompleted() == false);
    RequireIds(Advance(fixture.ClampClip, 0.25, player, result), {EVENT_QUARTER}, ClipTraversalDirection::Forward);

    // Traversing a loop to its duration enters the next cycle at its start.
    ClipPlayer looping = ActivatePlayer(fixture.LoopClip, 0.6, 1);
    REQUIRE(TrySeekClipPlayer(fixture.LoopClip,
                              CLIP_DURATION_SECONDS,
                              ClipSeekPolicy::Traverse,
                              MakeEventView(buffer),
                              looping,
                              result) == AnimationStatus::Success);
    REQUIRE(looping.GetCycle() == 1);
    REQUIRE(looping.GetTimeSeconds() == 0);
    REQUIRE(result.EventCount == 2);
    REQUIRE(buffer[0].Cycle == 1);

    REQUIRE(TrySeekClipPlayer(fixture.LoopClip, 2, ClipSeekPolicy::Scrub, MakeEventView(buffer), looping, result) ==
            AnimationStatus::InvalidArgument);
    REQUIRE(TrySeekClipPlayer(fixture.LoopClip,
                              0.5,
                              static_cast<ClipSeekPolicy>(9),
                              MakeEventView(buffer),
                              looping,
                              result) == AnimationStatus::InvalidArgument);
}

TEST_CASE("Pause, rate, stop and clip identity guard advances", "[animation][player]")
{
    const PlayerFixture fixture = MakePlayerFixture();
    ClipPlayer player = ActivatePlayer(fixture.LoopClip, 0.125, 1);
    ClipAdvanceResult result{};
    player.Pause();
    REQUIRE(player.IsPaused() == true);
    RequireIds(Advance(fixture.LoopClip, 0.5, player, result), {}, ClipTraversalDirection::Forward);
    REQUIRE(player.GetTimeSeconds() == 0.125);
    player.Resume();

    REQUIRE(player.TrySetRate(2) == AnimationStatus::Success);
    RequireIds(Advance(fixture.LoopClip, 0.125, player, result), {EVENT_QUARTER}, ClipTraversalDirection::Forward);
    REQUIRE(player.GetTimeSeconds() == 0.375);
    REQUIRE(player.TrySetRate(std::numeric_limits<float64>::quiet_NaN()) == AnimationStatus::InvalidArgument);
    REQUIRE(player.GetRate() == 2);

    foundation::Array<ClipEvent> buffer = MakeEventBuffer(TICK_EVENT_CAPACITY);
    REQUIRE(TryAdvanceClipPlayer(fixture.LoopClip, -0.1, MakeEventView(buffer), player, result) ==
            AnimationStatus::InvalidArgument);
    REQUIRE(TryAdvanceClipPlayer(fixture.ClampClip, 0.1, MakeEventView(buffer), player, result) ==
            AnimationStatus::IncompatibleClip);

    player.Stop();
    REQUIRE(player.IsActive() == false);
    REQUIRE(TryAdvanceClipPlayer(fixture.LoopClip, 0.1, MakeEventView(buffer), player, result) ==
            AnimationStatus::NotActive);
    REQUIRE(TrySeekClipPlayer(fixture.LoopClip, 0.1, ClipSeekPolicy::Scrub, MakeEventView(buffer), player, result) ==
            AnimationStatus::NotActive);
    REQUIRE(player.TrySetRate(1) == AnimationStatus::NotActive);
    const ClipPlayer never;
    REQUIRE(never.IsActive() == false);
}

TEST_CASE("Control, events and root motion agree with visual evaluation on or off", "[animation][player][visibility]")
{
    const PlayerFixture fixture = MakePlayerFixture();
    ClipPlayer visible = ActivatePlayer(fixture.LoopClip, 0.2, 1);
    ClipPlayer invisible = visible;
    LocalPose pose;
    REQUIRE(TryPrepareLocalPose(fixture.ChainSkeleton, pose) == AnimationStatus::Success);

    constexpr usize TICK_COUNT = 240;
    constexpr float64 TICK_SECONDS = 1.0 / 60.0;
    foundation::Array<ClipEvent> visibleEvents = MakeEventBuffer(TICK_EVENT_CAPACITY);
    foundation::Array<ClipEvent> invisibleEvents = MakeEventBuffer(TICK_EVENT_CAPACITY);
    for (usize tick = 0; tick < TICK_COUNT; ++tick)
    {
        // Vary rate and direction so wraps happen both ways.
        if (tick % 50 == 0)
        {
            const float64 rate = (tick / 50) % 2 == 0 ? 1.7 : -2.3;
            REQUIRE(visible.TrySetRate(rate) == AnimationStatus::Success);
            REQUIRE(invisible.TrySetRate(rate) == AnimationStatus::Success);
        }
        ClipAdvanceResult visibleResult{};
        ClipAdvanceResult invisibleResult{};
        REQUIRE(TryAdvanceClipPlayer(fixture.LoopClip,
                                     TICK_SECONDS,
                                     MakeEventView(visibleEvents),
                                     visible,
                                     visibleResult) == AnimationStatus::Success);
        // Only the visible character evaluates its pose; sampling reads the player and changes nothing.
        REQUIRE(TrySampleClip(fixture.LoopClip, visible.GetTimeSeconds(), pose) == AnimationStatus::Success);
        REQUIRE(TryAdvanceClipPlayer(fixture.LoopClip,
                                     TICK_SECONDS,
                                     MakeEventView(invisibleEvents),
                                     invisible,
                                     invisibleResult) == AnimationStatus::Success);

        REQUIRE(AreSamePlayers(visible, invisible) == true);
        REQUIRE(visibleResult.EventCount == invisibleResult.EventCount);
        REQUIRE(visibleResult.CyclesCrossed == invisibleResult.CyclesCrossed);
        REQUIRE(visibleResult.RootDelta.Translation == invisibleResult.RootDelta.Translation);
        REQUIRE(visibleResult.RootDelta.Rotation == invisibleResult.RootDelta.Rotation);
        for (usize index = 0; index < visibleResult.EventCount; ++index)
        {
            REQUIRE(visibleEvents[index].EventId == invisibleEvents[index].EventId);
            REQUIRE(visibleEvents[index].Cycle == invisibleEvents[index].Cycle);
            REQUIRE(visibleEvents[index].Direction == invisibleEvents[index].Direction);
        }
    }
    REQUIRE(pose.GetRevision() == 1 + TICK_COUNT);
}

TEST_CASE("Traverse seeks latch completion only on the endpoint they target", "[animation][player][seek]")
{
    const PlayerFixture fixture = MakePlayerFixture();
    ClipAdvanceResult result{};
    foundation::Array<ClipEvent> buffer = MakeEventBuffer(TICK_EVENT_CAPACITY);

    // 1e-17 - 0.5 rounds to -0.5, but the target is not the start: no completion, and the
    // start event stays ahead of the cursor for the next reverse step.
    ClipPlayer player = ActivatePlayer(fixture.ClampClip, 0.5, -1);
    REQUIRE(
        TrySeekClipPlayer(fixture.ClampClip, 1e-17, ClipSeekPolicy::Traverse, MakeEventView(buffer), player, result) ==
        AnimationStatus::Success);
    REQUIRE(result.HasCompleted == false);
    REQUIRE(player.IsCompleted() == false);
    REQUIRE(player.GetTimeSeconds() == 1e-17);
    RequireIds(Advance(fixture.ClampClip, 0.1, player, result), {EVENT_START}, ClipTraversalDirection::Reverse);
    REQUIRE(result.HasCompleted == true);

    // A seek onto the endpoint in its direction latches at once and fires the endpoint event.
    player = ActivatePlayer(fixture.ClampClip, 0.3, 1);
    REQUIRE(TrySeekClipPlayer(fixture.ClampClip,
                              CLIP_DURATION_SECONDS,
                              ClipSeekPolicy::Traverse,
                              MakeEventView(buffer),
                              player,
                              result) == AnimationStatus::Success);
    REQUIRE(result.HasCompleted == true);
    REQUIRE(player.IsCompleted() == true);
    REQUIRE(result.EventCount == 2);
    REQUIRE(buffer[1].EventId == EVENT_END);
}

TEST_CASE("Scrubbing a loop to its duration enters the next cycle like a traverse", "[animation][player][seek]")
{
    const PlayerFixture fixture = MakePlayerFixture();
    ClipAdvanceResult result{};
    foundation::Array<ClipEvent> buffer = MakeEventBuffer(TICK_EVENT_CAPACITY);
    ClipPlayer scrubbed = ActivatePlayer(fixture.LoopClip, 0.6, 1);
    ClipPlayer traversed = scrubbed;
    REQUIRE(TrySeekClipPlayer(fixture.LoopClip,
                              CLIP_DURATION_SECONDS,
                              ClipSeekPolicy::Scrub,
                              MakeEventView(buffer),
                              scrubbed,
                              result) == AnimationStatus::Success);
    REQUIRE(result.EventCount == 0);
    REQUIRE(TrySeekClipPlayer(fixture.LoopClip,
                              CLIP_DURATION_SECONDS,
                              ClipSeekPolicy::Traverse,
                              MakeEventView(buffer),
                              traversed,
                              result) == AnimationStatus::Success);
    REQUIRE(scrubbed.GetCycle() == 1);
    REQUIRE(scrubbed.GetCycle() == traversed.GetCycle());
    REQUIRE(scrubbed.GetTimeSeconds() == traversed.GetTimeSeconds());

    // The next step delivers cycle-1 crossings, not a second copy of cycle 0's.
    const foundation::Array<DeliveredEvent> delivered = Advance(fixture.LoopClip, 0.25, scrubbed, result);
    RequireIds(delivered, {EVENT_QUARTER}, ClipTraversalDirection::Forward);
    REQUIRE(delivered[0].Cycle == 1);
}
