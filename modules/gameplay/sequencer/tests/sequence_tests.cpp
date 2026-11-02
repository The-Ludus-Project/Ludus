#include <ludus/gameplay/sequencer/sequence.hpp>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/memory/allocation_statistics.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace ludus::gameplay::sequencer;
using ludus::foundation::float32;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::usize;
namespace foundation = ludus::foundation;
namespace math = foundation::math;
namespace camera = ludus::gameplay::camera;

namespace
{
constexpr usize PAIR_COUNT = 2;
constexpr usize THREE_KEY_COUNT = 3;
constexpr uint64 OBJECT_BINDING = 1;
constexpr uint64 CAMERA_BINDING = 2;

template <typename KeyType, usize Count>
foundation::BorrowedView<const KeyType> BorrowKeys(const foundation::StaticArray<KeyType, Count>& keys) noexcept
{
    return {keys.GetData(), Count};
}

void RequireSameObject(const SequenceObjectSample& left, const SequenceObjectSample& right)
{
    REQUIRE(left.BindingId == right.BindingId);
    REQUIRE(left.TransformLocal.Translation == right.TransformLocal.Translation);
    REQUIRE(left.TransformLocal.Rotation == right.TransformLocal.Rotation);
    REQUIRE(left.TransformLocal.Scale == right.TransformLocal.Scale);
    REQUIRE(left.IsVisible == right.IsVisible);
}
} // namespace

TEST_CASE("Sequence frame times preserve fractional rates and the 96-frame traversal", "[sequencer][time]")
{
    SequenceTime time;
    for (uint64 ordinal = 0; ordinal < 96; ++ordinal)
    {
        REQUIRE(TryGetSequenceFrameTime({ .Rate = {24, 1}, .TicksPerSecond = 24, .StartTick = 0 }, ordinal, time) ==
                SequenceStatus::Success);
        REQUIRE(time.Tick == ordinal);
        REQUIRE(time.Numerator == 0);
        REQUIRE(time.Denominator == 1);
    }
    REQUIRE(TryGetSequenceFrameTime({ .Rate = {30000, 1001}, .TicksPerSecond = 24, .StartTick = 0 }, 1, time) ==
            SequenceStatus::Success);
    REQUIRE(time.Tick == 0);
    REQUIRE(time.Numerator == 1001);
    REQUIRE(time.Denominator == 1250);
    REQUIRE(TryGetSequenceFrameTime({ .Rate = {30000, 1001}, .TicksPerSecond = 24, .StartTick = 7 }, 30000, time) ==
            SequenceStatus::Success);
    REQUIRE(time.Tick == 24031);
    REQUIRE(time.Numerator == 0);
    REQUIRE(time.Denominator == 1);
    REQUIRE(TryGetSequenceFrameTime({ .Rate = {6, 4}, .TicksPerSecond = 1, .StartTick = 7 }, 2, time) ==
            SequenceStatus::Success);
    REQUIRE(time.Tick == 8);
    REQUIRE(time.Numerator == 1);
    REQUIRE(time.Denominator == 3);
}

TEST_CASE("Sequence timing agrees with an independent bounded integer product", "[sequencer][time]")
{
    // This bounded reference can multiply directly; the production path must
    // also handle the large ordinals exercised by the next test.
    for (uint32 numerator = 1; numerator < 29; ++numerator)
    {
        for (uint32 denominator = 1; denominator < 11; ++denominator)
        {
            for (uint64 ordinal = 0; ordinal < 101; ordinal += 7)
            {
                const uint64 product = ordinal * 17 * denominator;
                SequenceTime time;
                REQUIRE(
                    TryGetSequenceFrameTime({ .Rate = {numerator, denominator}, .TicksPerSecond = 17, .StartTick = 3 },
                                            ordinal,
                                            time) == SequenceStatus::Success);
                REQUIRE(time.Tick == 3 + product / numerator);
                REQUIRE(uint64{time.Numerator} * numerator == (product % numerator) * time.Denominator);
            }
        }
    }
}

TEST_CASE("Sequence timing avoids intermediate overflow and preserves failed outputs", "[sequencer][time]")
{
    constexpr uint64 MAXIMUM = std::numeric_limits<uint64>::max();
    constexpr uint32 MAXIMUM_RATE = std::numeric_limits<uint32>::max();
    SequenceTime time{19, 1, 3};
    REQUIRE(TryGetSequenceFrameTime({ .Rate = {MAXIMUM_RATE, 1}, .TicksPerSecond = MAXIMUM_RATE, .StartTick = 0 },
                                    MAXIMUM,
                                    time) == SequenceStatus::Success);
    REQUIRE(time.Tick == MAXIMUM);
    REQUIRE(time.Numerator == 0);
    REQUIRE(time.Denominator == 1);
    REQUIRE(TryGetSequenceFrameTime({ .Rate = {2, 1}, .TicksPerSecond = 1, .StartTick = 0 }, MAXIMUM, time) ==
            SequenceStatus::Success);
    REQUIRE(time.Tick == MAXIMUM / 2);
    REQUIRE(time.Numerator == 1);
    REQUIRE(time.Denominator == 2);
    REQUIRE(TryGetSequenceFrameTime({ .Rate = {3, 2} }, MAXIMUM, time) == SequenceStatus::Success);
    REQUIRE(time.Tick == (MAXIMUM / 3) * 2);
    REQUIRE(time.Numerator == 0);
    REQUIRE(time.Denominator == 1);
    REQUIRE(TryGetSequenceFrameTime({ .Rate = {MAXIMUM_RATE, MAXIMUM_RATE - 1} }, MAXIMUM, time) ==
            SequenceStatus::Success);
    REQUIRE(time.Tick == MAXIMUM - MAXIMUM / MAXIMUM_RATE);
    REQUIRE(time.Numerator == 0);
    REQUIRE(time.Denominator == 1);
    REQUIRE(TryGetSequenceFrameTime({ .Rate = {3, 2}, .TicksPerSecond = 1, .StartTick = MAXIMUM - 1 }, 2, time) ==
            SequenceStatus::Success);
    REQUIRE(time.Tick == MAXIMUM);
    REQUIRE(time.Numerator == 1);
    REQUIRE(time.Denominator == 3);
    const auto requireFailure = [&](SequenceStatus expected, SequenceFrameSchedule schedule, uint64 ordinal) {
        REQUIRE(TryGetSequenceFrameTime(schedule, ordinal, time) == expected);
        REQUIRE(time.Tick == MAXIMUM);
        REQUIRE(time.Numerator == 1);
        REQUIRE(time.Denominator == 3);
    };
    requireFailure(SequenceStatus::OutOfRange, { .Rate = {1, 1}, .TicksPerSecond = 2, .StartTick = 0 }, MAXIMUM);
    requireFailure(SequenceStatus::OutOfRange, { .Rate = {3, 2}, .TicksPerSecond = 1, .StartTick = MAXIMUM - 1 }, 3);
    requireFailure(SequenceStatus::InvalidArgument, { .Rate = {0, 1}, .TicksPerSecond = 1, .StartTick = 0 }, 0);
    requireFailure(SequenceStatus::InvalidArgument, { .Rate = {1, 0}, .TicksPerSecond = 1, .StartTick = 0 }, 0);
    requireFailure(SequenceStatus::InvalidArgument, { .Rate = {1, 1}, .TicksPerSecond = 0, .StartTick = 0 }, 0);
}

TEST_CASE("Prepared sequences copy channels and sample complete local object values", "[sequencer]")
{
    PreparedSequence sequence;
    {
        const foundation::StaticArray<SequenceVectorKey, PAIR_COUNT> translation{{{0, {0, 0, 0}}, {10, {10, 20, 30}}}};
        const foundation::StaticArray<SequenceRotationKey, PAIR_COUNT> rotation{{{0, {}}, {10, {0, -1, 0, 0}}}};
        const foundation::StaticArray<SequenceVectorKey, PAIR_COUNT> scale{{{0, {1, 1, 1}}, {10, {3, 3, 3}}}};
        const foundation::StaticArray<SequenceVisibilityKey, PAIR_COUNT> visibility{{{0, true}, {5, false}}};
        const SequenceObjectTrack object
        {
            .BindingId = OBJECT_BINDING,
            .TranslationKeys = BorrowKeys(translation),
            .RotationKeys = BorrowKeys(rotation),
            .ScaleKeys = BorrowKeys(scale),
            .VisibilityKeys = BorrowKeys(visibility),
        };
        REQUIRE(TryPrepareSequence({ .Objects = {&object, 1} }, {}, sequence) == SequenceStatus::Success);
    }
    REQUIRE(sequence.IsPrepared() == true);
    REQUIRE(sequence.GetObjectCount() == 1);
    REQUIRE(sequence.GetCameraCount() == 0);
    SequenceObjectSample sample;
    REQUIRE(TryEvaluateSequenceObject(sequence, OBJECT_BINDING, {5}, sample) == SequenceStatus::Success);
    REQUIRE(sample.TransformLocal.Translation == math::Vector3{5, 10, 15});
    REQUIRE(sample.TransformLocal.Scale == math::Vector3{2, 2, 2});
    REQUIRE(sample.IsVisible == false);
    REQUIRE(sample.TransformLocal.Rotation.Y > 0); // Exact half-turn tie chooses positive relative Y.
    REQUIRE(math::Abs(math::Rotate(sample.TransformLocal.Rotation, {1, 0, 0}).Z + 1) < 1e-5F);
    REQUIRE(TryEvaluateSequenceObject(sequence, OBJECT_BINDING, {100}, sample) == SequenceStatus::Success);
    REQUIRE(sample.TransformLocal.Translation == math::Vector3{10, 20, 30});
    REQUIRE(sample.TransformLocal.Scale == math::Vector3{3, 3, 3});
}

TEST_CASE("Sequence spans select exact keys and preserve absent and single-key channels", "[sequencer]")
{
    foundation::StaticArray<SequenceVectorKey, THREE_KEY_COUNT> translation{};
    translation[0] = {2, {1, 0, 0}, SequenceInterpolation::Step};
    translation[1] = {5, {4, 0, 0}, SequenceInterpolation::Linear};
    translation[2] = {10, {9, 0, 0}};
    const SequenceVectorKey scale{7, {2, 3, 4}};
    const SequenceObjectTrack object
    {
        .BindingId = OBJECT_BINDING,
        .BaseTransformLocal = { .Translation = {99, 99, 99}, .Scale = {9, 9, 9} },
        .IsBaseVisible = false,
        .TranslationKeys = BorrowKeys(translation),
        .ScaleKeys = {&scale, 1},
    };
    PreparedSequence sequence;
    REQUIRE(TryPrepareSequence({ .Objects = {&object, 1} }, {}, sequence) == SequenceStatus::Success);
    SequenceObjectSample sample;
    REQUIRE(TryEvaluateSequenceObject(sequence, OBJECT_BINDING, {}, sample) == SequenceStatus::Success);
    REQUIRE(sample.TransformLocal.Translation.X == 1);
    REQUIRE(sample.TransformLocal.Scale == scale.Value);
    REQUIRE(sample.IsVisible == false);
    REQUIRE(TryEvaluateSequenceObject(sequence, OBJECT_BINDING, {4, 999, 1000}, sample) == SequenceStatus::Success);
    REQUIRE(sample.TransformLocal.Translation.X == 1);
    REQUIRE(TryEvaluateSequenceObject(sequence, OBJECT_BINDING, {5}, sample) == SequenceStatus::Success);
    REQUIRE(sample.TransformLocal.Translation.X == 4);
    REQUIRE(TryEvaluateSequenceObject(sequence, OBJECT_BINDING, {5, 1, 2}, sample) == SequenceStatus::Success);
    REQUIRE(sample.TransformLocal.Translation.X == 4.5F);
    REQUIRE(TryEvaluateSequenceObject(sequence, OBJECT_BINDING, {10}, sample) == SequenceStatus::Success);
    REQUIRE(sample.TransformLocal.Translation.X == 9);
}

TEST_CASE("Seeking and sampling subsets does not change sequence values or allocate", "[sequencer]")
{
    const foundation::StaticArray<SequenceVectorKey, PAIR_COUNT> translation{{{0, {}}, {96, {96, 0, 0}}}};
    const SequenceObjectTrack object{ .BindingId = OBJECT_BINDING, .TranslationKeys = BorrowKeys(translation) };
    PreparedSequence sequence;
    REQUIRE(TryPrepareSequence({ .Objects = {&object, 1} }, {}, sequence) == SequenceStatus::Success);
    SequenceObjectSample direct;
    REQUIRE(TryEvaluateSequenceObject(sequence, OBJECT_BINDING, {47}, direct) == SequenceStatus::Success);
    SequenceObjectSample sample;
    const foundation::AllocationStatistics before =
        foundation::GetAllocationStatistics(foundation::GetContainerAllocationDomain());
    bool hasFailure = false;
    for (uint64 ordinal = 0; ordinal < 96; ++ordinal)
    {
        SequenceTime time;
        hasFailure = hasFailure == true ||
                     TryGetSequenceFrameTime(
                         {
                             .Rate = {24, 1},
                             .TicksPerSecond = 24,
                             .StartTick = 0,
                         },
                         ordinal,
                         time) != SequenceStatus::Success ||
                     TryEvaluateSequenceObject(sequence, OBJECT_BINDING, time, sample) != SequenceStatus::Success ||
                     sample.TransformLocal.Translation.X != static_cast<float32>(ordinal);
    }
    const foundation::AllocationStatistics after =
        foundation::GetAllocationStatistics(foundation::GetContainerAllocationDomain());
    REQUIRE(hasFailure == false);
    REQUIRE(before.AllocationCount == after.AllocationCount);
    REQUIRE(before.LiveRequestedBytes == after.LiveRequestedBytes);
    REQUIRE(TryEvaluateSequenceObject(sequence, OBJECT_BINDING, {3}, sample) == SequenceStatus::Success);
    REQUIRE(TryEvaluateSequenceObject(sequence, OBJECT_BINDING, {47}, sample) == SequenceStatus::Success);
    RequireSameObject(sample, direct);
}

TEST_CASE("Camera sequence samples retain float64 positions and explicit metadata", "[sequencer][camera]")
{
    constexpr uint64 EPOCH = std::numeric_limits<uint64>::max() - 16;
    foundation::StaticArray<SequencePositionKey, PAIR_COUNT> positions{};
    positions[0] = {EPOCH, {1.0e12, 0, 0}};
    positions[1] = {EPOCH + 8, {1.0e12 + 8, 16, -8}};
    const SequenceRotationKey rotation{EPOCH, {0, 0, 1, 0}};
    SequenceCameraTrack track
    {
        .BindingId = CAMERA_BINDING,
        .PositionKeys = BorrowKeys(positions),
        .RotationKeys = {&rotation, 1},
    };
    track.BaseSample.OutputId = 91;
    track.BaseSample.DefinitionRevision = 17;
    track.BaseSample.PresentationSequence = 23;
    track.BaseSample.DiscontinuityRevision = 11;
    track.BaseSample.Lens.Projection = camera::CameraProjection::PerspectiveInfinite;
    track.BaseSample.Lens.NearPlane = 0.1F;
    track.BaseSample.Lens.FarPlane = std::numeric_limits<float32>::quiet_NaN(); // Inactive field.
    PreparedSequence sequence;
    REQUIRE(TryPrepareSequence({ .Cameras = {&track, 1} }, {}, sequence) == SequenceStatus::Success);
    camera::CameraSample sample;
    REQUIRE(TryEvaluateSequenceCamera(sequence, CAMERA_BINDING, {EPOCH + 4, 1, 4}, sample) == SequenceStatus::Success);
    REQUIRE(sample.Pose.Position == math::Vector3d{1.0e12 + 4.25, 8.5, -4.25});
    REQUIRE(sample.Pose.Orientation == rotation.Value);
    REQUIRE(sample.OutputId == 91);
    REQUIRE(sample.DefinitionRevision == 17);
    REQUIRE(sample.PresentationSequence == 23);
    REQUIRE(sample.DiscontinuityRevision == 11);
    REQUIRE(camera::ValidateCameraSample(sample) == camera::CameraStatus::Success);
}

TEST_CASE("Rotation tracks normalize copies and choose the same half-turn for signed keys", "[sequencer]")
{
    foundation::StaticArray<SequenceRotationKey, PAIR_COUNT> keys{{{0, {}}, {10, {0, -1, 0, 0}}}};
    SequenceObjectTrack track{ .BindingId = OBJECT_BINDING, .RotationKeys = BorrowKeys(keys) };
    track.BaseTransformLocal.Rotation.W = 1.00001F;
    PreparedSequence sequence;
    REQUIRE(TryPrepareSequence({ .Objects = {&track, 1} }, {}, sequence) == SequenceStatus::Success);
    REQUIRE(track.BaseTransformLocal.Rotation.W == 1.00001F);
    SequenceObjectSample first;
    REQUIRE(TryEvaluateSequenceObject(sequence, OBJECT_BINDING, {5}, first) == SequenceStatus::Success);
    keys[0].Value.W = -1;
    keys[1].Value.Y = 1;
    PreparedSequence signedSequence;
    REQUIRE(TryPrepareSequence({ .Objects = {&track, 1} }, {}, signedSequence) == SequenceStatus::Success);
    SequenceObjectSample second;
    REQUIRE(TryEvaluateSequenceObject(signedSequence, OBJECT_BINDING, {5}, second) == SequenceStatus::Success);
    REQUIRE(math::SameRotation(first.TransformLocal.Rotation, second.TransformLocal.Rotation, 1e-3F) == true);
    keys[1].Value = keys[0].Value; // Equivalent endpoint orientation, including negative identity.
    REQUIRE(TryPrepareSequence({ .Objects = {&track, 1} }, {}, signedSequence) == SequenceStatus::Success);
    REQUIRE(TryEvaluateSequenceObject(signedSequence, OBJECT_BINDING, {5}, second) == SequenceStatus::Success);
    REQUIRE(math::SameRotation(second.TransformLocal.Rotation, {}, 1e-3F) == true);
}

TEST_CASE("Lens tracks interpolate active fields and require explicit projection steps", "[sequencer][camera]")
{
    foundation::StaticArray<SequenceLensKey, PAIR_COUNT> keys;
    keys[0].Value.Projection = camera::CameraProjection::PerspectiveInfinite;
    keys[0].Value.NearPlane = 1;
    keys[0].Value.VerticalFov = 1;
    keys[0].Value.FarPlane = std::numeric_limits<float32>::quiet_NaN();
    keys[1] = keys[0];
    keys[1].Tick = 10;
    keys[1].Value.NearPlane = 2;
    keys[1].Value.VerticalFov = 2;
    const SequenceCameraTrack track{ .BindingId = CAMERA_BINDING, .LensKeys = BorrowKeys(keys) };
    PreparedSequence sequence;
    REQUIRE(TryPrepareSequence({ .Cameras = {&track, 1} }, {}, sequence) == SequenceStatus::Success);
    camera::CameraSample sample;
    REQUIRE(TryEvaluateSequenceCamera(sequence, CAMERA_BINDING, {5}, sample) == SequenceStatus::Success);
    REQUIRE(sample.Lens.NearPlane == 1.5F);
    REQUIRE(sample.Lens.VerticalFov == 1.5F);
    keys[1].Value = {};
    REQUIRE(TryPrepareSequence({ .Cameras = {&track, 1} }, {}, sequence) == SequenceStatus::InvalidKeys);
    keys[0].Interpolation = SequenceInterpolation::Step;
    REQUIRE(TryPrepareSequence({ .Cameras = {&track, 1} }, {}, sequence) == SequenceStatus::Success);
    REQUIRE(TryEvaluateSequenceCamera(sequence, CAMERA_BINDING, {9, 1, 2}, sample) == SequenceStatus::Success);
    REQUIRE(sample.Lens.Projection == camera::CameraProjection::PerspectiveInfinite);
    REQUIRE(TryEvaluateSequenceCamera(sequence, CAMERA_BINDING, {10}, sample) == SequenceStatus::Success);
    REQUIRE(sample.Lens.Projection == camera::CameraProjection::OrthographicFinite);
}

TEST_CASE("A numerically collapsed interpolated lens cannot replace the accepted camera", "[sequencer][camera]")
{
    foundation::StaticArray<SequenceLensKey, PAIR_COUNT> keys;
    keys[0].Value.Projection = camera::CameraProjection::PerspectiveFinite;
    keys[0].Value.NearPlane = 1;
    keys[0].Value.FarPlane = std::nextafter(1.0F, 2.0F);
    keys[1] = keys[0];
    keys[1].Tick = 2;
    keys[1].Value.NearPlane = std::nextafter(1.0F, 0.0F);
    keys[1].Value.FarPlane = 1;
    const SequenceCameraTrack track{ .BindingId = CAMERA_BINDING, .LensKeys = BorrowKeys(keys) };
    PreparedSequence sequence;
    REQUIRE(TryPrepareSequence({ .Cameras = {&track, 1} }, {}, sequence) == SequenceStatus::Success);
    camera::CameraSample sample;
    sample.Pose.Position = {19, 20, 21};
    REQUIRE(TryEvaluateSequenceCamera(sequence, CAMERA_BINDING, {1}, sample) == SequenceStatus::OutOfRange);
    REQUIRE(sample.Pose.Position == math::Vector3d{19, 20, 21});
    REQUIRE(sample.Lens.Projection == camera::CameraProjection::OrthographicFinite);
}

TEST_CASE("Invalid source channels preserve the accepted prepared revision", "[sequencer]")
{
    foundation::StaticArray<SequenceVectorKey, PAIR_COUNT> keys{{{0, {1, 1, 1}}, {10, {2, 2, 2}}}};
    SequenceObjectTrack track{ .BindingId = OBJECT_BINDING, .ScaleKeys = BorrowKeys(keys) };
    PreparedSequence sequence;
    REQUIRE(TryPrepareSequence({ .Objects = {&track, 1} }, {}, sequence) == SequenceStatus::Success);
    const usize bytes = sequence.GetPreparedBytes();
    const auto requireRejected = [&](SequenceStatus expected) {
        REQUIRE(TryPrepareSequence({ .Objects = {&track, 1} }, {}, sequence) == expected);
        REQUIRE(sequence.GetPreparedBytes() == bytes);
        SequenceObjectSample sample;
        REQUIRE(TryEvaluateSequenceObject(sequence, OBJECT_BINDING, {10}, sample) == SequenceStatus::Success);
        REQUIRE(sample.TransformLocal.Scale == math::Vector3{2, 2, 2});
    };
    keys[1].Value.X = 0;
    requireRejected(SequenceStatus::InvalidKeys);
    keys[1].Value.X = -1;
    requireRejected(SequenceStatus::InvalidKeys);
    keys[1].Value.X = std::numeric_limits<float32>::quiet_NaN();
    requireRejected(SequenceStatus::InvalidKeys);
    keys[1].Value.X = 2;
    keys[1].Tick = 0;
    requireRejected(SequenceStatus::InvalidKeys);
    keys[1].Tick = MAX_SEQUENCE_SPAN_TICKS + 1;
    requireRejected(SequenceStatus::InvalidKeys);
    keys[1].Tick = 10;
    keys[0].Interpolation = static_cast<SequenceInterpolation>(255);
    requireRejected(SequenceStatus::InvalidKeys);
    keys[0].Interpolation = SequenceInterpolation::Linear;
    track.BaseTransformLocal.Rotation.W = 2;
    requireRejected(SequenceStatus::InvalidArgument);
    track.BaseTransformLocal.Rotation.W = 1;
    track.BindingId = 0;
    requireRejected(SequenceStatus::InvalidArgument);
    track.BindingId = OBJECT_BINDING;
    track.ScaleKeys = {nullptr, 1};
    requireRejected(SequenceStatus::InvalidArgument);
}

TEST_CASE("Preparation rejects duplicate writers and exact storage budget violations", "[sequencer]")
{
    const SequenceVectorKey key{0, {1, 2, 3}};
    foundation::StaticArray<SequenceObjectTrack, PAIR_COUNT> objects;
    objects[0].BindingId = OBJECT_BINDING;
    objects[0].TranslationKeys = {&key, 1};
    objects[1] = objects[0];
    PreparedSequence sequence;
    REQUIRE(TryPrepareSequence({ .Objects = BorrowKeys(objects) }, {}, sequence) == SequenceStatus::DuplicateBinding);
    REQUIRE(sequence.IsPrepared() == false);
    objects[1].BindingId = 3;
    REQUIRE(TryPrepareSequence({ .Objects = BorrowKeys(objects) }, { .MaxTracks = 1 }, sequence) ==
            SequenceStatus::BudgetExceeded);
    REQUIRE(TryPrepareSequence({ .Objects = BorrowKeys(objects) }, { .MaxKeys = 1 }, sequence) ==
            SequenceStatus::BudgetExceeded);
    REQUIRE(TryPrepareSequence({ .Objects = BorrowKeys(objects) }, {}, sequence) == SequenceStatus::Success);
    const usize exactBytes = sequence.GetPreparedBytes();
    REQUIRE(TryPrepareSequence({ .Objects = BorrowKeys(objects) }, { .MaxPreparedBytes = exactBytes - 1 }, sequence) ==
            SequenceStatus::BudgetExceeded);
    REQUIRE(sequence.GetPreparedBytes() == exactBytes);
    REQUIRE(TryPrepareSequence({ .Objects = BorrowKeys(objects) }, { .MaxPreparedBytes = exactBytes }, sequence) ==
            SequenceStatus::Success);
    const SequenceCameraTrack cameraTrack{ .BindingId = OBJECT_BINDING };
    REQUIRE(TryPrepareSequence({ .Objects = {objects.GetData(), 1}, .Cameras = {&cameraTrack, 1} }, {}, sequence) ==
            SequenceStatus::DuplicateBinding);
    REQUIRE(TryPrepareSequence({ .Objects = {nullptr, 1} }, {}, sequence) == SequenceStatus::InvalidArgument);
    REQUIRE(TryPrepareSequence({}, {}, sequence) == SequenceStatus::InvalidArgument);
}

TEST_CASE("Unprepared missing and invalid time requests preserve object and camera outputs", "[sequencer]")
{
    PreparedSequence sequence;
    SequenceObjectSample objectSample;
    objectSample.BindingId = 91;
    camera::CameraSample cameraSample;
    cameraSample.OutputId = 91;
    REQUIRE(TryEvaluateSequenceObject(sequence, OBJECT_BINDING, {}, objectSample) == SequenceStatus::Unprepared);
    REQUIRE(TryEvaluateSequenceCamera(sequence, CAMERA_BINDING, {}, cameraSample) == SequenceStatus::Unprepared);
    const SequenceObjectTrack object{ .BindingId = OBJECT_BINDING };
    REQUIRE(TryPrepareSequence({ .Objects = {&object, 1} }, {}, sequence) == SequenceStatus::Success);
    REQUIRE(TryEvaluateSequenceObject(sequence, CAMERA_BINDING, {}, objectSample) == SequenceStatus::MissingBinding);
    REQUIRE(TryEvaluateSequenceCamera(sequence, OBJECT_BINDING, {}, cameraSample) == SequenceStatus::MissingBinding);
    REQUIRE(TryEvaluateSequenceObject(sequence, OBJECT_BINDING, {0, 1, 1}, objectSample) ==
            SequenceStatus::InvalidArgument);
    REQUIRE(TryEvaluateSequenceCamera(sequence, CAMERA_BINDING, {0, 0, 0}, cameraSample) ==
            SequenceStatus::InvalidArgument);
    REQUIRE(objectSample.BindingId == 91);
    REQUIRE(cameraSample.OutputId == 91);
}

TEST_CASE("Moving a prepared sequence transfers every owned channel and releases replaced storage", "[sequencer]")
{
    const SequenceVectorKey key{0, {1, 2, 3}};
    const SequenceObjectTrack object{ .BindingId = OBJECT_BINDING, .TranslationKeys = {&key, 1} };
    PreparedSequence source;
    REQUIRE(TryPrepareSequence({ .Objects = {&object, 1} }, {}, source) == SequenceStatus::Success);
    PreparedSequence moved(foundation::Move(source));
    REQUIRE(source.IsPrepared() == false);
    REQUIRE(source.GetPreparedBytes() == 0);
    PreparedSequence destination;
    REQUIRE(TryPrepareSequence({ .Objects = {&object, 1} }, {}, destination) == SequenceStatus::Success);
    destination = foundation::Move(moved);
    REQUIRE(moved.IsPrepared() == false);
    SequenceObjectSample sample;
    REQUIRE(TryEvaluateSequenceObject(destination, OBJECT_BINDING, {}, sample) == SequenceStatus::Success);
    REQUIRE(sample.TransformLocal.Translation == key.Value);
}
