// Uncompressed reference clips: package validation, canonical events, the reference pose
// sampler and the root trajectory sampler.
//
// Clip duration and endpoint behavior are asset semantics; looping clips canonicalize an
// endpoint marker to a loop-start marker at zero; the trajectory is a separate scale-free rigid
// curve with R(0) = identity and an explicit endpoint R(duration), kept even when the pose omits
// a duplicated loop sample. See docs/architecture/skeletal-animation.md, "Playback
// synchronization and events" and "Root motion and locomotion quality". The endpoint handling
// follows Nicholas Frechette's ACL documentation, "Handling looping playback"
// (https://github.com/nfrechette/acl/blob/develop/docs/handling_looping_playback.md).

#include <ludus/animation/clip.hpp>

#include "internal/canonical_bytes.hpp"
#include "internal/root_math.hpp"
#include "internal/storage.hpp"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <new>

namespace ludus::animation
{
using foundation::float32;
using foundation::float64;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;
namespace math = foundation::math;

namespace
{
/// Translation (3), rotation (4) and scale (3) components of one canonical sample.
constexpr usize TRANSFORM_COMPONENT_COUNT = 10;
/// Translation (3) and rotation (4) components of one canonical root transform.
constexpr usize ROOT_COMPONENT_COUNT = 7;
/// Canonical header: version, skeleton compatibility, duration, rate, sample/track/event counts and modes.
constexpr usize FINGERPRINT_HEADER_BYTES = sizeof(uint32) + sizeof(uint64) + sizeof(uint64) + sizeof(float64) +
                                           sizeof(float64) + sizeof(uint32) + sizeof(uint32) + sizeof(uint32) +
                                           sizeof(uint8) + sizeof(uint8);
/// Canonical event: identity, time and reversibility.
constexpr usize FINGERPRINT_EVENT_BYTES = sizeof(uint64) + sizeof(float64) + sizeof(uint8);
/// Canonical sample and root transform widths.
constexpr usize FINGERPRINT_SAMPLE_BYTES = TRANSFORM_COMPONENT_COUNT * sizeof(float32);
constexpr usize FINGERPRINT_ROOT_BYTES = ROOT_COMPONENT_COUNT * sizeof(float32);
/// Root end and reference offset are hashed after the trajectory samples.
constexpr usize FINGERPRINT_ROOT_TRAILER_COUNT = 2;

/// Bracketing samples of one timeline position.
struct SampleSpan final
{
    uint32 First = 0;
    uint32 Second = 0;
    float32 Fraction = 0;
    bool IsInEndpointGap = false;
};

template <typename ElementType>
bool IsViewValid(foundation::BorrowedView<const ElementType> view) noexcept
{
    return view.GetSize() == 0 || view.GetDataOrNull() != nullptr;
}

bool IsWrapModeValid(ClipWrapMode mode) noexcept
{
    return mode == ClipWrapMode::Clamp || mode == ClipWrapMode::Loop;
}

bool IsRootModeValid(RootMotionMode mode) noexcept
{
    return mode == RootMotionMode::None || mode == RootMotionMode::Planar || mode == RootMotionMode::Full;
}

AnimationStatus ValidateTimeline(const ClipPackage& package) noexcept
{
    const float64 duration = package.DurationSeconds;
    const float64 rate = package.SampleRateHertz;
    if (math::IsFinite(duration) == false || duration < 0 || duration > MAX_CLIP_DURATION_SECONDS ||
        math::IsFinite(rate) == false || rate <= 0 || rate > MAX_CLIP_SAMPLE_RATE_HERTZ || package.SampleCount == 0 ||
        IsWrapModeValid(package.WrapMode) == false || IsRootModeValid(package.RootMode) == false)
    {
        return AnimationStatus::InvalidArgument;
    }
    // A zero-length timeline cannot loop, and an eventful one would fire every marker at once.
    if (duration == 0 && (package.WrapMode == ClipWrapMode::Loop || package.Events.GetSize() != 0))
    {
        return AnimationStatus::InvalidArgument;
    }
    const float64 lastSampleTime = static_cast<float64>(package.SampleCount - 1) / rate;
    if (lastSampleTime > duration + CLIP_SAMPLE_TIME_TOLERANCE_SECONDS)
    {
        return AnimationStatus::InvalidArgument;
    }
    return AnimationStatus::Success;
}

AnimationStatus ValidateShape(const ClipPackage& package, const ClipLimits& limits, usize& outSampleValueCount) noexcept
{
    if (limits.MaxTracks == 0 || limits.MaxSamples == 0 || IsViewValid(package.TrackJointIds) == false ||
        IsViewValid(package.Samples) == false || IsViewValid(package.Events) == false ||
        IsViewValid(package.RootSamples) == false)
    {
        return AnimationStatus::InvalidArgument;
    }
    if (package.TrackJointIds.GetSize() > limits.MaxTracks || package.SampleCount > limits.MaxSamples ||
        package.Events.GetSize() > limits.MaxEvents)
    {
        return AnimationStatus::BudgetExceeded;
    }
    usize sampleValueCount = 0;
    if (foundation::TryMultiply(static_cast<usize>(package.SampleCount),
                                package.TrackJointIds.GetSize(),
                                sampleValueCount) == false ||
        sampleValueCount != package.Samples.GetSize())
    {
        return AnimationStatus::InvalidArgument;
    }
    const usize expectedRootSamples = package.RootMode == RootMotionMode::None ? 0 : package.SampleCount;
    if (package.RootSamples.GetSize() != expectedRootSamples)
    {
        return AnimationStatus::InvalidArgument;
    }
    outSampleValueCount = sampleValueCount;
    return AnimationStatus::Success;
}

/// Validate and normalize one rigid root transform.
bool TryNormalizeRoot(const RootTransform& value, RootTransform& outValue) noexcept
{
    if (math::IsFinite(value.Translation) == false || math::IsFinite(value.Rotation) == false ||
        math::Abs(math::Dot(value.Rotation, value.Rotation) - 1.0F) > JOINT_ROTATION_UNIT_TOLERANCE)
    {
        return false;
    }
    RootTransform normalized = value;
    if (math::TryNormalize(value.Rotation, normalized.Rotation) != math::MathStatus::Success)
    {
        return false;
    }
    outValue = normalized;
    return true;
}

bool IsPlanar(const RootTransform& value) noexcept
{
    return value.Translation.Y == 0 && value.Rotation.X == 0 && value.Rotation.Z == 0;
}

bool IsIdentity(const RootTransform& value) noexcept
{
    const math::Vector3 zero{0, 0, 0};
    // q and -q are the same rotation; a normalized identity is exactly (0, 0, 0, +-1).
    return value.Translation == zero && value.Rotation.X == 0 && value.Rotation.Y == 0 && value.Rotation.Z == 0 &&
           math::Abs(value.Rotation.W) == 1;
}

bool AreRootsEqual(const RootTransform& left, const RootTransform& right) noexcept
{
    return left.Translation == right.Translation && left.Rotation == right.Rotation;
}

AnimationStatus PrepareTracks(const ClipPackage& package,
                              const Skeleton& skeleton,
                              usize sampleValueCount,
                              detail::ClipStorage& inoutStorage) noexcept
{
    const usize jointCount = skeleton.GetJointCount();
    const usize trackCount = package.TrackJointIds.GetSize();
    if (detail::TryAllocateArray(jointCount, inoutStorage.JointTracks) == false ||
        detail::TryAllocateArray(jointCount, inoutStorage.RestPose) == false ||
        detail::TryAllocateArray(trackCount, inoutStorage.TrackJointIds) == false ||
        detail::TryAllocateArray(sampleValueCount, inoutStorage.Samples) == false)
    {
        return AnimationStatus::OutOfMemory;
    }
    const foundation::BorrowedView<const SkeletonJointRecord> joints = skeleton.GetJoints();
    for (usize index = 0; index < jointCount; ++index)
    {
        inoutStorage.JointTracks[index] = detail::NO_CLIP_TRACK;
        inoutStorage.RestPose[index] = joints[index].RestLocal;
    }
    for (usize track = 0; track < trackCount; ++track)
    {
        uint32 jointIndex = 0;
        if (skeleton.TryFindJointIndex(package.TrackJointIds[track], jointIndex) != AnimationStatus::Success ||
            inoutStorage.JointTracks[jointIndex] != detail::NO_CLIP_TRACK)
        {
            return AnimationStatus::InvalidJointId;
        }
        inoutStorage.JointTracks[jointIndex] = static_cast<uint32>(track);
        inoutStorage.TrackJointIds[track] = package.TrackJointIds[track];
    }
    for (usize index = 0; index < sampleValueCount; ++index)
    {
        const AnimationStatus status = detail::ValidateLocalTransform(package.Samples[index]);
        if (status != AnimationStatus::Success)
        {
            return status;
        }
        inoutStorage.Samples[index] = detail::NormalizeLocalTransform(package.Samples[index]);
    }
    return AnimationStatus::Success;
}

AnimationStatus PrepareEvents(const ClipPackage& package, detail::ClipStorage& inoutStorage) noexcept
{
    const usize eventCount = package.Events.GetSize();
    foundation::Array<uint64> sortedIds;
    if (detail::TryAllocateArray(eventCount, inoutStorage.Events) == false ||
        detail::TryAllocateArray(eventCount, sortedIds) == false)
    {
        return AnimationStatus::OutOfMemory;
    }
    float64 previousTime = 0;
    for (usize index = 0; index < eventCount; ++index)
    {
        const ClipEventRecord& event = package.Events[index];
        if (event.EventId == 0 || math::IsFinite(event.TimeSeconds) == false || event.TimeSeconds < previousTime ||
            event.TimeSeconds > package.DurationSeconds)
        {
            return AnimationStatus::InvalidArgument;
        }
        previousTime = event.TimeSeconds;
        ClipEventRecord canonical = event;
        // Positive zero is the one spelling of the loop start and of a time-zero marker.
        canonical.TimeSeconds = event.TimeSeconds == 0 ? 0.0 : event.TimeSeconds;
        if (package.WrapMode == ClipWrapMode::Loop && event.TimeSeconds == package.DurationSeconds)
        {
            // A loop endpoint and loop start are the same crossing; keep only the loop-start alias.
            canonical.TimeSeconds = 0;
        }
        inoutStorage.Events[index] = canonical;
        sortedIds[index] = event.EventId;
    }
    foundation::Sort(sortedIds.GetData(), sortedIds.GetSize(), [](uint64 left, uint64 right) noexcept {
        return left < right;
    });
    for (usize index = 1; index < eventCount; ++index)
    {
        if (sortedIds[index] == sortedIds[index - 1])
        {
            return AnimationStatus::InvalidArgument;
        }
    }
    foundation::Sort(inoutStorage.Events.GetData(),
                     inoutStorage.Events.GetSize(),
                     [](const ClipEventRecord& left, const ClipEventRecord& right) noexcept {
                         return left.TimeSeconds < right.TimeSeconds ||
                                (left.TimeSeconds == right.TimeSeconds && left.EventId < right.EventId);
                     });
    return AnimationStatus::Success;
}

AnimationStatus PrepareRoot(const ClipPackage& package, detail::ClipStorage& inoutStorage) noexcept
{
    if (package.RootMode == RootMotionMode::None)
    {
        return AnimationStatus::Success;
    }
    const usize count = package.RootSamples.GetSize();
    if (detail::TryAllocateArray(count, inoutStorage.RootSamples) == false)
    {
        return AnimationStatus::OutOfMemory;
    }
    const bool isPlanar = package.RootMode == RootMotionMode::Planar;
    for (usize index = 0; index < count; ++index)
    {
        if (TryNormalizeRoot(package.RootSamples[index], inoutStorage.RootSamples[index]) == false ||
            (isPlanar == true && IsPlanar(inoutStorage.RootSamples[index]) == false))
        {
            return AnimationStatus::InvalidTransform;
        }
    }
    if (TryNormalizeRoot(package.RootEnd, inoutStorage.RootEnd) == false ||
        TryNormalizeRoot(package.RootReferenceOffset, inoutStorage.RootReferenceOffset) == false ||
        (isPlanar == true && IsPlanar(inoutStorage.RootEnd) == false) ||
        IsIdentity(inoutStorage.RootSamples[0]) == false)
    {
        return AnimationStatus::InvalidTransform;
    }
    // Without a gap the last sample is R(duration); the endpoint must not disagree with it.
    if (inoutStorage.HasEndpointGap == false &&
        AreRootsEqual(inoutStorage.RootEnd, inoutStorage.RootSamples[count - 1]) == false)
    {
        return AnimationStatus::InvalidTransform;
    }
    return AnimationStatus::Success;
}

void WriteRoot(detail::CanonicalByteWriter& inoutWriter, const RootTransform& value) noexcept
{
    inoutWriter.WriteFloat32(value.Translation.X);
    inoutWriter.WriteFloat32(value.Translation.Y);
    inoutWriter.WriteFloat32(value.Translation.Z);
    inoutWriter.WriteFloat32(value.Rotation.X);
    inoutWriter.WriteFloat32(value.Rotation.Y);
    inoutWriter.WriteFloat32(value.Rotation.Z);
    inoutWriter.WriteFloat32(value.Rotation.W);
}

bool TryComputeFingerprint(detail::ClipStorage& inoutStorage) noexcept
{
    const usize trackCount = inoutStorage.TrackJointIds.GetSize();
    const usize sampleCount = inoutStorage.Samples.GetSize();
    const usize eventCount = inoutStorage.Events.GetSize();
    const usize rootCount = inoutStorage.RootSamples.GetSize() + FINGERPRINT_ROOT_TRAILER_COUNT;
    usize trackBytes = 0;
    usize sampleBytes = 0;
    usize eventBytes = 0;
    usize rootBytes = 0;
    usize totalBytes = FINGERPRINT_HEADER_BYTES;
    if (foundation::TryMultiply(trackCount, sizeof(uint64), trackBytes) == false ||
        foundation::TryMultiply(sampleCount, FINGERPRINT_SAMPLE_BYTES, sampleBytes) == false ||
        foundation::TryMultiply(eventCount, FINGERPRINT_EVENT_BYTES, eventBytes) == false ||
        foundation::TryMultiply(rootCount, FINGERPRINT_ROOT_BYTES, rootBytes) == false ||
        foundation::TryAdd(totalBytes, trackBytes, totalBytes) == false ||
        foundation::TryAdd(totalBytes, sampleBytes, totalBytes) == false ||
        foundation::TryAdd(totalBytes, eventBytes, totalBytes) == false ||
        foundation::TryAdd(totalBytes, rootBytes, totalBytes) == false)
    {
        return false;
    }
    foundation::Array<uint8> bytes;
    if (detail::TryAllocateArray(totalBytes, bytes) == false)
    {
        return false;
    }
    detail::CanonicalByteWriter writer({bytes.GetData(), bytes.GetSize()});
    writer.WriteUint32(CLIP_FINGERPRINT_VERSION);
    writer.WriteUint64(inoutStorage.SkeletonCompatibility.Low);
    writer.WriteUint64(inoutStorage.SkeletonCompatibility.High);
    writer.WriteFloat64(inoutStorage.DurationSeconds);
    writer.WriteFloat64(inoutStorage.SampleRateHertz);
    writer.WriteUint32(inoutStorage.SampleCount);
    writer.WriteUint32(static_cast<uint32>(trackCount));
    writer.WriteUint32(static_cast<uint32>(eventCount));
    writer.WriteUint8(static_cast<uint8>(inoutStorage.WrapMode));
    writer.WriteUint8(static_cast<uint8>(inoutStorage.RootMode));
    for (const uint64 jointId : inoutStorage.TrackJointIds)
    {
        writer.WriteUint64(jointId);
    }
    for (const math::TransformTRS& sample : inoutStorage.Samples)
    {
        writer.WriteFloat32(sample.Translation.X);
        writer.WriteFloat32(sample.Translation.Y);
        writer.WriteFloat32(sample.Translation.Z);
        writer.WriteFloat32(sample.Rotation.X);
        writer.WriteFloat32(sample.Rotation.Y);
        writer.WriteFloat32(sample.Rotation.Z);
        writer.WriteFloat32(sample.Rotation.W);
        writer.WriteFloat32(sample.Scale.X);
        writer.WriteFloat32(sample.Scale.Y);
        writer.WriteFloat32(sample.Scale.Z);
    }
    for (const ClipEventRecord& event : inoutStorage.Events)
    {
        writer.WriteUint64(event.EventId);
        writer.WriteFloat64(event.TimeSeconds);
        writer.WriteUint8(event.IsReversible == true ? 1 : 0);
    }
    for (const RootTransform& root : inoutStorage.RootSamples)
    {
        WriteRoot(writer, root);
    }
    WriteRoot(writer, inoutStorage.RootEnd);
    WriteRoot(writer, inoutStorage.RootReferenceOffset);
    LUDUS_ASSERT(writer.GetOffset() == totalBytes);
    inoutStorage.Fingerprint = foundation::StableFingerprint128({bytes.GetData(), bytes.GetSize()});
    return true;
}

float64 ClampUnit(float64 value) noexcept
{
    if (value < 0)
    {
        return 0;
    }
    return value > 1 ? 1 : value;
}

SampleSpan LocateSample(const detail::ClipStorage& storage, float64 timeSeconds) noexcept
{
    const uint32 last = storage.SampleCount - 1;
    if (timeSeconds >= storage.LastSampleTimeSeconds)
    {
        if (storage.HasEndpointGap == false)
        {
            return { .First = last, .Second = last, .Fraction = 0, .IsInEndpointGap = false };
        }
        const float64 gap = storage.DurationSeconds - storage.LastSampleTimeSeconds;
        const float64 fraction = ClampUnit((timeSeconds - storage.LastSampleTimeSeconds) / gap);
        // A looping clip returns towards its first sample; a clamped clip holds the last.
        const uint32 second = storage.WrapMode == ClipWrapMode::Loop ? 0 : last;
        return { .First = last, .Second = second, .Fraction = static_cast<float32>(fraction), .IsInEndpointGap = true };
    }
    // timeSeconds < LastSampleTimeSeconds implies at least two samples.
    const float64 position = timeSeconds * storage.SampleRateHertz;
    float64 index = math::Floor(position);
    const float64 lastPairIndex = static_cast<float64>(last - 1);
    index = index > lastPairIndex ? lastPairIndex : index;
    index = index < 0 ? 0 : index;
    const uint32 first = static_cast<uint32>(index);
    return
    {
        .First = first,
        .Second = first + 1,
        .Fraction = static_cast<float32>(ClampUnit(position - index)),
        .IsInEndpointGap = false,
    };
}

math::TransformTRS
InterpolateTransform(const math::TransformTRS& first, const math::TransformTRS& second, float32 fraction) noexcept
{
    return
    {
        .Translation = math::Lerp(first.Translation, second.Translation, fraction),
        .Rotation = math::SlerpShortest(first.Rotation, second.Rotation, fraction),
        .Scale = math::Lerp(first.Scale, second.Scale, fraction),
    };
}

math::TransformTRS SampleJoint(const detail::ClipStorage& storage, usize jointIndex, const SampleSpan& span) noexcept
{
    const uint32 track = storage.JointTracks[jointIndex];
    if (track == detail::NO_CLIP_TRACK)
    {
        return storage.RestPose[jointIndex];
    }
    const usize trackCount = storage.TrackJointIds.GetSize();
    const math::TransformTRS& first = storage.Samples[static_cast<usize>(span.First) * trackCount + track];
    const math::TransformTRS& second = storage.Samples[static_cast<usize>(span.Second) * trackCount + track];
    return InterpolateTransform(first, second, span.Fraction);
}

bool IsSampleTimeValid(const detail::ClipStorage& storage, float64 timeSeconds) noexcept
{
    return math::IsFinite(timeSeconds) == true && timeSeconds >= 0 && timeSeconds <= storage.DurationSeconds;
}

} // namespace

AnimationStatus
TryComposeRootTransforms(const RootTransform& left, const RootTransform& right, RootTransform& outComposed) noexcept
{
    const detail::RigidTransformD leftRigid = detail::ConvertToRigid(left);
    const detail::RigidTransformD rightRigid = detail::ConvertToRigid(right);
    if (detail::IsFinite(leftRigid) == false || detail::IsFinite(rightRigid) == false)
    {
        return AnimationStatus::InvalidTransform;
    }
    return detail::TryConvertToRootTransform(detail::Compose(leftRigid, rightRigid), outComposed) == true
               ? AnimationStatus::Success
               : AnimationStatus::OutOfRange;
}

RootTransform InvertRootTransform(const RootTransform& value) noexcept
{
    RootTransform inverse{};
    [[maybe_unused]] const bool isRepresentable =
        detail::TryConvertToRootTransform(detail::Invert(detail::ConvertToRigid(value)), inverse);
    LUDUS_ASSERT(isRepresentable == true);
    return inverse;
}

Clip::Clip() noexcept = default;
Clip::~Clip() noexcept = default;
Clip::Clip(Clip&& other) noexcept = default;
Clip& Clip::operator=(Clip&& other) noexcept = default;

bool Clip::IsPrepared() const noexcept
{
    return mStorage.Get() != nullptr;
}

ClipIdentity Clip::GetIdentity() const noexcept
{
    return mStorage.Get() == nullptr ? ClipIdentity{} : mStorage->Identity;
}

float64 Clip::GetDurationSeconds() const noexcept
{
    return mStorage.Get() == nullptr ? 0.0 : mStorage->DurationSeconds;
}

float64 Clip::GetSampleRateHertz() const noexcept
{
    return mStorage.Get() == nullptr ? 0.0 : mStorage->SampleRateHertz;
}

uint32 Clip::GetSampleCount() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : mStorage->SampleCount;
}

uint32 Clip::GetTrackCount() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : static_cast<uint32>(mStorage->TrackJointIds.GetSize());
}

ClipWrapMode Clip::GetWrapMode() const noexcept
{
    return mStorage.Get() == nullptr ? ClipWrapMode::Clamp : mStorage->WrapMode;
}

RootMotionMode Clip::GetRootMode() const noexcept
{
    return mStorage.Get() == nullptr ? RootMotionMode::None : mStorage->RootMode;
}

RootTransform Clip::GetRootEnd() const noexcept
{
    return mStorage.Get() == nullptr ? RootTransform{} : mStorage->RootEnd;
}

RootTransform Clip::GetRootReferenceOffset() const noexcept
{
    return mStorage.Get() == nullptr ? RootTransform{} : mStorage->RootReferenceOffset;
}

foundation::Fingerprint128 Clip::GetSkeletonCompatibility() const noexcept
{
    return mStorage.Get() == nullptr ? foundation::Fingerprint128{} : mStorage->SkeletonCompatibility;
}

foundation::Fingerprint128 Clip::GetFingerprint() const noexcept
{
    return mStorage.Get() == nullptr ? foundation::Fingerprint128{} : mStorage->Fingerprint;
}

foundation::BorrowedView<const ClipEventRecord> Clip::GetEvents() const noexcept
{
    if (mStorage.Get() == nullptr)
    {
        return {};
    }
    return {mStorage->Events.GetData(), mStorage->Events.GetSize()};
}

AnimationStatus
TryPrepareClip(const ClipPackage& package, const Skeleton& skeleton, const ClipLimits& limits, Clip& outClip) noexcept
{
    if (skeleton.IsPrepared() == false || package.Identity.AuthoringId == 0)
    {
        return AnimationStatus::InvalidArgument;
    }
    usize sampleValueCount = 0;
    AnimationStatus status = ValidateShape(package, limits, sampleValueCount);
    if (status != AnimationStatus::Success)
    {
        return status;
    }
    status = ValidateTimeline(package);
    if (status != AnimationStatus::Success)
    {
        return status;
    }
    detail::ClipStorage* storageOrNull = new (std::nothrow) detail::ClipStorage;
    if (storageOrNull == nullptr)
    {
        return AnimationStatus::OutOfMemory;
    }
    foundation::UniquePtr<detail::ClipStorage> candidate(storageOrNull);
    candidate->Identity = package.Identity;
    candidate->DurationSeconds = package.DurationSeconds == 0 ? 0.0 : package.DurationSeconds;
    candidate->SampleRateHertz = package.SampleRateHertz;
    candidate->SampleCount = package.SampleCount;
    candidate->LastSampleTimeSeconds = static_cast<float64>(package.SampleCount - 1) / package.SampleRateHertz;
    candidate->HasEndpointGap =
        candidate->DurationSeconds - candidate->LastSampleTimeSeconds > CLIP_SAMPLE_TIME_TOLERANCE_SECONDS;
    candidate->WrapMode = package.WrapMode;
    candidate->RootMode = package.RootMode;
    candidate->SkeletonCompatibility = skeleton.GetCompatibility();
    status = PrepareTracks(package, skeleton, sampleValueCount, *candidate);
    if (status != AnimationStatus::Success)
    {
        return status;
    }
    status = PrepareEvents(package, *candidate);
    if (status != AnimationStatus::Success)
    {
        return status;
    }
    status = PrepareRoot(package, *candidate);
    if (status != AnimationStatus::Success)
    {
        return status;
    }
    if (TryComputeFingerprint(*candidate) == false)
    {
        return AnimationStatus::OutOfMemory;
    }
    outClip.mStorage = foundation::Move(candidate);
    return AnimationStatus::Success;
}

AnimationStatus TrySampleClip(const Clip& clip, float64 sampleTimeSeconds, LocalPose& inoutPose) noexcept
{
    if (clip.IsPrepared() == false || inoutPose.IsPrepared() == false)
    {
        return AnimationStatus::InvalidArgument;
    }
    const detail::ClipStorage& storage = *clip.mStorage;
    if (IsSampleTimeValid(storage, sampleTimeSeconds) == false)
    {
        return AnimationStatus::InvalidArgument;
    }
    detail::LocalPoseStorage& pose = *inoutPose.mStorage;
    if (pose.Compatibility != storage.SkeletonCompatibility || pose.Joints.GetSize() != storage.JointTracks.GetSize())
    {
        return AnimationStatus::IncompatibleSkeleton;
    }
    const SampleSpan span = LocateSample(storage, sampleTimeSeconds);
    const usize jointCount = pose.Joints.GetSize();
    // Validate every interpolated value before the first write so a failure leaves the pose intact.
    for (usize index = 0; index < jointCount; ++index)
    {
        const AnimationStatus status = detail::ValidateLocalTransform(SampleJoint(storage, index, span));
        if (status != AnimationStatus::Success)
        {
            return status;
        }
    }
    for (usize index = 0; index < jointCount; ++index)
    {
        pose.Joints[index] = detail::NormalizeLocalTransform(SampleJoint(storage, index, span));
    }
    ++pose.Revision;
    return AnimationStatus::Success;
}

AnimationStatus TrySampleClipRoot(const Clip& clip, float64 sampleTimeSeconds, RootTransform& outRoot) noexcept
{
    if (clip.IsPrepared() == false)
    {
        return AnimationStatus::InvalidArgument;
    }
    const detail::ClipStorage& storage = *clip.mStorage;
    if (IsSampleTimeValid(storage, sampleTimeSeconds) == false)
    {
        return AnimationStatus::InvalidArgument;
    }
    if (storage.RootMode == RootMotionMode::None)
    {
        outRoot = RootTransform{};
        return AnimationStatus::Success;
    }
    if (sampleTimeSeconds == storage.DurationSeconds)
    {
        outRoot = storage.RootEnd;
        return AnimationStatus::Success;
    }
    const SampleSpan span = LocateSample(storage, sampleTimeSeconds);
    const RootTransform& first = storage.RootSamples[span.First];
    const RootTransform& second = span.IsInEndpointGap == true ? storage.RootEnd : storage.RootSamples[span.Second];
    outRoot =
    {
        .Translation = math::Lerp(first.Translation, second.Translation, span.Fraction),
        .Rotation = math::SlerpShortest(first.Rotation, second.Rotation, span.Fraction),
    };
    return AnimationStatus::Success;
}
} // namespace ludus::animation
