#pragma once

// Shared clip fixtures for the Animation A1 tests: a reference clip on the four-joint chain
// skeleton with two tracks, four events and a full rigid root trajectory.

#include "skeleton_fixtures.hpp"

#include <ludus/animation/clip.hpp>
#include <ludus/animation/clip_player.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>

namespace ludus::animation::test
{
using foundation::float64;
using foundation::int64;

inline constexpr uint64 CLIP_AUTHORING_ID = 0xC11F;
inline constexpr usize CLIP_TRACK_COUNT = 2;
inline constexpr uint32 CLIP_SAMPLE_COUNT = 3;
inline constexpr usize CLIP_SAMPLE_VALUE_COUNT = CLIP_TRACK_COUNT * CLIP_SAMPLE_COUNT;
inline constexpr float64 CLIP_SAMPLE_RATE_HERTZ = 2;
/// The last sample (index 2 at 2 Hz) lies on the duration.
inline constexpr float64 CLIP_DURATION_SECONDS = 1;
inline constexpr usize CLIP_EVENT_COUNT = 4;
/// Yaw of the root trajectory per sample, in radians.
inline constexpr float32 ROOT_YAW_PER_SAMPLE = 0.25F;
/// Forward (+Z) trajectory translation per sample.
inline constexpr float32 ROOT_STEP_PER_SAMPLE = 0.5F;

inline constexpr uint64 EVENT_START = 11;   ///< Time 0, reversible.
inline constexpr uint64 EVENT_QUARTER = 12; ///< Time 0.25, forward only.
inline constexpr uint64 EVENT_HALF = 13;    ///< Time 0.5, reversible.
inline constexpr uint64 EVENT_END = 14;     ///< Time 1 (the duration), reversible.

/// Backing storage for a clip package; the package views borrow it.
struct ClipSource final
{
    foundation::StaticArray<uint64, CLIP_TRACK_COUNT> TrackJointIds{};
    foundation::StaticArray<math::TransformTRS, CLIP_SAMPLE_VALUE_COUNT> Samples{};
    foundation::StaticArray<ClipEventRecord, CLIP_EVENT_COUNT> Events{};
    foundation::StaticArray<RootTransform, CLIP_SAMPLE_COUNT> RootSamples{};
};

/// The two animated chain joints, in track order.
enum class ChainTrack : foundation::uint8
{
    Root,  ///< Joint 100.
    Elbow, ///< Joint 300.
};

inline math::TransformTRS MakeChainSample(ChainTrack track, usize sample)
{
    const float32 step = static_cast<float32>(sample);
    if (track == ChainTrack::Root)
    {
        // Root joint: residual pelvis bob and turn only; the trajectory was extracted at cook time.
        return MakeTransform({0, 1 + 0.1F * step, 0}, MakeRotation({0, 1, 0}, 0.2F * step), {1, 1, 1});
    }
    return MakeTransform({1, 0.5F, 0}, MakeRotation({1, 0, 0}, 0.5F * step), {1, 1 + 0.5F * step, 1});
}

inline RootTransform MakeRootSample(usize sample)
{
    const float32 step = static_cast<float32>(sample);
    if (sample == 0)
    {
        return RootTransform{};
    }
    return
    {
        .Translation = {0, 0, ROOT_STEP_PER_SAMPLE * step},
        .Rotation = MakeRotation({0, 1, 0}, ROOT_YAW_PER_SAMPLE * step),
    };
}

inline ClipSource MakeClipSource()
{
    ClipSource source{};
    source.TrackJointIds = {{100, 300}};
    for (usize sample = 0; sample < CLIP_SAMPLE_COUNT; ++sample)
    {
        for (usize track = 0; track < CLIP_TRACK_COUNT; ++track)
        {
            source.Samples[sample * CLIP_TRACK_COUNT + track] = MakeChainSample(static_cast<ChainTrack>(track), sample);
        }
        source.RootSamples[sample] = MakeRootSample(sample);
    }
    source.Events = {{
        { .EventId = EVENT_START, .TimeSeconds = 0, .IsReversible = true },
        { .EventId = EVENT_QUARTER, .TimeSeconds = 0.25, .IsReversible = false },
        { .EventId = EVENT_HALF, .TimeSeconds = 0.5, .IsReversible = true },
        { .EventId = EVENT_END, .TimeSeconds = CLIP_DURATION_SECONDS, .IsReversible = true },
    }};
    return source;
}

inline ClipPackage MakeClipPackage(const ClipSource& source, ClipWrapMode wrapMode)
{
    return
    {
        .Identity = { .AuthoringId = CLIP_AUTHORING_ID, .Revision = 1 },
        .DurationSeconds = CLIP_DURATION_SECONDS,
        .SampleRateHertz = CLIP_SAMPLE_RATE_HERTZ,
        .SampleCount = CLIP_SAMPLE_COUNT,
        .WrapMode = wrapMode,
        .TrackJointIds = {source.TrackJointIds.GetData(), source.TrackJointIds.GetSize()},
        .Samples = {source.Samples.GetData(), source.Samples.GetSize()},
        .Events = {source.Events.GetData(), source.Events.GetSize()},
        .RootMode = RootMotionMode::Full,
        .RootSamples = {source.RootSamples.GetData(), source.RootSamples.GetSize()},
        .RootEnd = source.RootSamples[CLIP_SAMPLE_COUNT - 1],
        .RootReferenceOffset = { .Translation = {0, 0.9F, 0}, .Rotation = math::QuaternionIdentity() },
    };
}

inline Clip PrepareClip(const ClipPackage& package, const Skeleton& skeleton)
{
    Clip clip;
    REQUIRE(TryPrepareClip(package, skeleton, {}, clip) == AnimationStatus::Success);
    return clip;
}

inline ClipPlayer ActivatePlayer(const Clip& clip, float64 startTimeSeconds, float64 rate)
{
    ClipPlayer player;
    REQUIRE(TryActivateClipPlayer(clip,
                                  {
                                      .InstanceId = 7,
                                      .ActivationId = 1,
                                      .StartTimeSeconds = startTimeSeconds,
                                      .Rate = rate,
                                  },
                                  player) == AnimationStatus::Success);
    return player;
}

/// Caller-owned event buffer of a chosen capacity.
inline foundation::Array<ClipEvent> MakeEventBuffer(usize capacity)
{
    foundation::Array<ClipEvent> events;
    REQUIRE(events.TryResize(capacity) == true);
    return events;
}

inline foundation::BorrowedView<ClipEvent> MakeEventView(foundation::Array<ClipEvent>& inoutEvents)
{
    return {inoutEvents.GetData(), inoutEvents.GetSize()};
}

inline bool IsNear(float32 left, float32 right, float32 tolerance)
{
    return math::Abs(left - right) <= tolerance;
}

inline bool IsNear(math::Vector3 left, math::Vector3 right, float32 tolerance)
{
    return IsNear(left.X, right.X, tolerance) && IsNear(left.Y, right.Y, tolerance) &&
           IsNear(left.Z, right.Z, tolerance);
}

inline bool IsNear(const RootTransform& left, const RootTransform& right, float32 tolerance)
{
    return IsNear(left.Translation, right.Translation, tolerance) &&
           math::SameRotation(left.Rotation, right.Rotation, tolerance) == true;
}
} // namespace ludus::animation::test
