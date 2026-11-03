#include <ludus/gameplay/sequencer/sequence.hpp>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <new>

namespace ludus::gameplay::sequencer
{
using foundation::float32;
using foundation::float64;
using foundation::uint64;
using foundation::usize;
namespace math = foundation::math;

namespace detail
{
struct SequenceStorage final
{
    foundation::Array<SequenceObjectTrack> Objects;
    foundation::Array<SequenceCameraTrack> Cameras;
    foundation::Array<SequenceVectorKey> Vectors;
    foundation::Array<SequenceRotationKey> Rotations;
    foundation::Array<SequencePositionKey> Positions;
    foundation::Array<SequenceVisibilityKey> Visibility;
    foundation::Array<SequenceLensKey> Lenses;
    usize PreparedBytes = 0;
};
} // namespace detail

namespace
{
struct StorageCounts final
{
    usize Vectors = 0;
    usize Rotations = 0;
    usize Positions = 0;
    usize Visibility = 0;
    usize Lenses = 0;
    usize Keys = 0;
    usize Bytes = sizeof(detail::SequenceStorage);
};

bool IsInterpolationValid(SequenceInterpolation interpolation) noexcept
{
    return interpolation == SequenceInterpolation::Step || interpolation == SequenceInterpolation::Linear;
}

bool IsRotationValid(math::Quaternion rotation) noexcept
{
    return math::IsFinite(rotation) == true &&
           math::Abs(math::Dot(rotation, rotation) - 1.0F) <= SEQUENCE_ROTATION_UNIT_TOLERANCE;
}

bool IsScaleValid(math::Vector3 scale) noexcept
{
    return math::IsFinite(scale) == true && scale.X > 0 && scale.Y > 0 && scale.Z > 0;
}

bool IsKeyValid(const SequenceVectorKey& key) noexcept
{
    return math::IsFinite(key.Value) == true && IsInterpolationValid(key.Interpolation) == true;
}

bool IsKeyValid(const SequencePositionKey& key) noexcept
{
    return math::IsFinite(key.PositionWorld) == true && IsInterpolationValid(key.Interpolation) == true;
}

bool IsKeyValid(const SequenceRotationKey& key) noexcept
{
    return IsRotationValid(key.Value) == true && IsInterpolationValid(key.Interpolation) == true;
}

bool IsKeyValid(const SequenceVisibilityKey&) noexcept
{
    return true;
}

bool IsKeyValid(const SequenceLensKey& key) noexcept
{
    camera::CameraSample sample;
    sample.Lens = key.Value;
    return camera::ValidateCameraSample(sample) == camera::CameraStatus::Success &&
           IsInterpolationValid(key.Interpolation) == true;
}

template <typename KeyType>
SequenceStatus ValidateKeys(foundation::BorrowedView<const KeyType> keys) noexcept
{
    if (keys.IsValid() == false)
    {
        return SequenceStatus::InvalidArgument;
    }
    for (usize index = 0; index < keys.GetSize(); ++index)
    {
        if (IsKeyValid(keys[index]) == false)
        {
            return SequenceStatus::InvalidKeys;
        }
        if (index > 0 && (keys[index].Tick <= keys[index - 1].Tick ||
                          keys[index].Tick - keys[index - 1].Tick > MAX_SEQUENCE_SPAN_TICKS))
        {
            return SequenceStatus::InvalidKeys;
        }
    }
    return SequenceStatus::Success;
}

template <typename KeyType>
SequenceStatus CountKeys(foundation::BorrowedView<const KeyType> keys,
                         const SequenceLimits& limits,
                         usize& inoutCount,
                         StorageCounts& inoutCounts) noexcept
{
    if (keys.IsValid() == false)
    {
        return SequenceStatus::InvalidArgument;
    }
    usize bytes = 0;
    if (foundation::TryAdd(inoutCounts.Keys, keys.GetSize(), inoutCounts.Keys) == false ||
        inoutCounts.Keys > limits.MaxKeys || foundation::TryAdd(inoutCount, keys.GetSize(), inoutCount) == false ||
        foundation::TryMultiply(keys.GetSize(), sizeof(KeyType), bytes) == false ||
        foundation::TryAdd(inoutCounts.Bytes, bytes, inoutCounts.Bytes) == false ||
        inoutCounts.Bytes > limits.MaxPreparedBytes)
    {
        return SequenceStatus::BudgetExceeded;
    }
    return ValidateKeys(keys);
}

SequenceStatus
ValidateObject(const SequenceObjectTrack& track, const SequenceLimits& limits, StorageCounts& inoutCounts) noexcept
{
    if (track.BindingId == 0 || math::IsFinite(track.BaseTransformLocal.Translation) == false ||
        IsRotationValid(track.BaseTransformLocal.Rotation) == false ||
        IsScaleValid(track.BaseTransformLocal.Scale) == false)
    {
        return SequenceStatus::InvalidArgument;
    }
    const SequenceStatus translation = CountKeys(track.TranslationKeys, limits, inoutCounts.Vectors, inoutCounts);
    if (translation != SequenceStatus::Success)
    {
        return translation;
    }
    const SequenceStatus rotation = CountKeys(track.RotationKeys, limits, inoutCounts.Rotations, inoutCounts);
    if (rotation != SequenceStatus::Success)
    {
        return rotation;
    }
    const SequenceStatus scale = CountKeys(track.ScaleKeys, limits, inoutCounts.Vectors, inoutCounts);
    if (scale != SequenceStatus::Success)
    {
        return scale;
    }
    for (usize index = 0; index < track.ScaleKeys.GetSize(); ++index)
    {
        if (IsScaleValid(track.ScaleKeys[index].Value) == false)
        {
            return SequenceStatus::InvalidKeys;
        }
    }
    return CountKeys(track.VisibilityKeys, limits, inoutCounts.Visibility, inoutCounts);
}

SequenceStatus
ValidateCamera(const SequenceCameraTrack& track, const SequenceLimits& limits, StorageCounts& inoutCounts) noexcept
{
    if (track.BindingId == 0 || camera::ValidateCameraSample(track.BaseSample) != camera::CameraStatus::Success ||
        IsRotationValid(track.BaseSample.Pose.Orientation) == false)
    {
        return SequenceStatus::InvalidArgument;
    }
    const SequenceStatus position = CountKeys(track.PositionKeys, limits, inoutCounts.Positions, inoutCounts);
    if (position != SequenceStatus::Success)
    {
        return position;
    }
    const SequenceStatus rotation = CountKeys(track.RotationKeys, limits, inoutCounts.Rotations, inoutCounts);
    if (rotation != SequenceStatus::Success)
    {
        return rotation;
    }
    const SequenceStatus lens = CountKeys(track.LensKeys, limits, inoutCounts.Lenses, inoutCounts);
    if (lens != SequenceStatus::Success)
    {
        return lens;
    }
    for (usize index = 1; index < track.LensKeys.GetSize(); ++index)
    {
        const SequenceLensKey& previous = track.LensKeys[index - 1];
        if (previous.Value.Projection != track.LensKeys[index].Value.Projection &&
            previous.Interpolation != SequenceInterpolation::Step)
        {
            return SequenceStatus::InvalidKeys;
        }
    }
    return SequenceStatus::Success;
}

bool HasDuplicateBinding(const SequenceSource& source) noexcept
{
    for (usize index = 0; index < source.Objects.GetSize(); ++index)
    {
        for (usize previous = 0; previous < index; ++previous)
        {
            if (source.Objects[index].BindingId == source.Objects[previous].BindingId)
            {
                return true;
            }
        }
        for (usize cameraIndex = 0; cameraIndex < source.Cameras.GetSize(); ++cameraIndex)
        {
            if (source.Objects[index].BindingId == source.Cameras[cameraIndex].BindingId)
            {
                return true;
            }
        }
    }
    for (usize index = 0; index < source.Cameras.GetSize(); ++index)
    {
        for (usize previous = 0; previous < index; ++previous)
        {
            if (source.Cameras[index].BindingId == source.Cameras[previous].BindingId)
            {
                return true;
            }
        }
    }
    return false;
}

template <typename ElementType>
bool TryAllocateArray(usize count, foundation::Array<ElementType>& outElements) noexcept
{
    // Reserving first uses exact capacity, avoiding geometric growth beyond the
    // admitted payload. The candidate's arrays are initially empty.
    return outElements.TryEnsureCapacity(count) == true && outElements.TryResize(count) == true;
}

template <typename KeyType>
foundation::BorrowedView<const KeyType> CopyKeys(foundation::BorrowedView<const KeyType> keys,
                                                 foundation::Array<KeyType>& inoutStorage,
                                                 usize& inoutOffset) noexcept
{
    if (keys.IsEmpty() == true)
    {
        return {};
    }
    const usize first = inoutOffset;
    for (usize index = 0; index < keys.GetSize(); ++index)
    {
        inoutStorage[inoutOffset] = keys[index];
        ++inoutOffset;
    }
    return {inoutStorage.GetData() + first, keys.GetSize()};
}

math::Quaternion NormalizeRotation(math::Quaternion rotation) noexcept
{
    [[maybe_unused]] const math::MathStatus status = math::TryNormalize(rotation, rotation);
    LUDUS_ASSERT(status == math::MathStatus::Success);
    return rotation;
}

math::Quaternion ChooseRotationEndpoint(math::Quaternion left, math::Quaternion right) noexcept
{
    const float32 dot = math::Dot(left, right);
    const math::Quaternion relative = math::Conjugate(left) * right;
    const float32 firstNonzero = relative.X != 0 ? relative.X : (relative.Y != 0 ? relative.Y : relative.Z);
    if (dot < 0 || (dot == 0 && firstNonzero < 0))
    {
        return {-right.X, -right.Y, -right.Z, -right.W};
    }
    return right;
}

void CopyTracks(const SequenceSource& source, detail::SequenceStorage& outStorage) noexcept
{
    StorageCounts offsets{};
    for (usize index = 0; index < source.Objects.GetSize(); ++index)
    {
        const SequenceObjectTrack& track = source.Objects[index];
        SequenceObjectTrack& copied = outStorage.Objects[index];
        copied = track;
        copied.BaseTransformLocal.Rotation = NormalizeRotation(track.BaseTransformLocal.Rotation);
        copied.TranslationKeys = CopyKeys(track.TranslationKeys, outStorage.Vectors, offsets.Vectors);
        copied.RotationKeys = CopyKeys(track.RotationKeys, outStorage.Rotations, offsets.Rotations);
        copied.ScaleKeys = CopyKeys(track.ScaleKeys, outStorage.Vectors, offsets.Vectors);
        copied.VisibilityKeys = CopyKeys(track.VisibilityKeys, outStorage.Visibility, offsets.Visibility);
    }
    for (usize index = 0; index < source.Cameras.GetSize(); ++index)
    {
        const SequenceCameraTrack& track = source.Cameras[index];
        SequenceCameraTrack& copied = outStorage.Cameras[index];
        copied = track;
        copied.BaseSample.Pose.Orientation = NormalizeRotation(track.BaseSample.Pose.Orientation);
        copied.PositionKeys = CopyKeys(track.PositionKeys, outStorage.Positions, offsets.Positions);
        copied.RotationKeys = CopyKeys(track.RotationKeys, outStorage.Rotations, offsets.Rotations);
        copied.LensKeys = CopyKeys(track.LensKeys, outStorage.Lenses, offsets.Lenses);
    }
    for (SequenceRotationKey& key : outStorage.Rotations)
    {
        key.Value = NormalizeRotation(key.Value);
    }
}

bool IsTimeValid(SequenceTime time) noexcept
{
    return time.Denominator > 0 && time.Numerator < time.Denominator;
}

template <typename KeyType>
usize FindKey(foundation::BorrowedView<const KeyType> keys, SequenceTime time) noexcept
{
    usize first = 0;
    usize end = keys.GetSize();
    while (first < end)
    {
        const usize middle = first + (end - first) / 2;
        if (keys[middle].Tick <= time.Tick)
        {
            first = middle + 1;
        }
        else
        {
            end = middle;
        }
    }
    return first == 0 ? 0 : first - 1;
}

math::Vector3 GetKeyValue(const SequenceVectorKey& key) noexcept
{
    return key.Value;
}
math::Vector3d GetKeyValue(const SequencePositionKey& key) noexcept
{
    return key.PositionWorld;
}
math::Quaternion GetKeyValue(const SequenceRotationKey& key) noexcept
{
    return key.Value;
}
bool GetKeyValue(const SequenceVisibilityKey& key) noexcept
{
    return key.IsVisible;
}
camera::CameraLens GetKeyValue(const SequenceLensKey& key) noexcept
{
    return key.Value;
}

template <typename KeyType, typename ValueType, typename BlendFunction>
ValueType EvaluateKeys(foundation::BorrowedView<const KeyType> keys,
                       SequenceTime time,
                       ValueType base,
                       BlendFunction blend) noexcept
{
    if (keys.IsEmpty() == true)
    {
        return base;
    }
    const usize index = FindKey(keys, time);
    const KeyType& left = keys[index];
    if constexpr (requires { left.Interpolation; })
    {
        if (index + 1 < keys.GetSize() && time.Tick >= left.Tick && left.Interpolation == SequenceInterpolation::Linear)
        {
            const KeyType& right = keys[index + 1];
            const float64 duration = static_cast<float64>(right.Tick - left.Tick);
            const float64 fraction = static_cast<float64>(time.Numerator) / time.Denominator;
            const float64 alpha = (static_cast<float64>(time.Tick - left.Tick) + fraction) / duration;
            return blend(GetKeyValue(left), GetKeyValue(right), alpha);
        }
    }
    return GetKeyValue(left);
}

math::Vector3 BlendVector(math::Vector3 left, math::Vector3 right, float64 alpha) noexcept
{
    return math::Lerp(left, right, static_cast<float32>(alpha));
}

math::Quaternion BlendRotation(math::Quaternion left, math::Quaternion right, float64 alpha) noexcept
{
    // Thanks to Epic Games, "Cinematics and Sequencer", Tracks and Cameras,
    // https://dev.epicgames.com/documentation/unreal-engine/cinematics-and-movie-making-in-unreal-engine:
    // separate typed object/camera channels inform this sampler. Quaternion
    // interpolation itself reuses FoundationMath; the exact half-turn tie is
    // Ludus policy. See docs/architecture/sequencer-cinematics.md.
    return math::SlerpShortest(left, ChooseRotationEndpoint(left, right), static_cast<float32>(alpha));
}

math::Vector3d BlendPosition(math::Vector3d left, math::Vector3d right, float64 alpha) noexcept
{
    return {math::Lerp(left.X, right.X, alpha), math::Lerp(left.Y, right.Y, alpha), math::Lerp(left.Z, right.Z, alpha)};
}

camera::CameraLens BlendLens(camera::CameraLens left, camera::CameraLens right, float64 alpha) noexcept
{
    const float32 weight = static_cast<float32>(alpha);
    camera::CameraLens result = left;
    result.NearPlane = math::Lerp(left.NearPlane, right.NearPlane, weight);
    if (left.Projection != camera::CameraProjection::PerspectiveInfinite)
    {
        result.FarPlane = math::Lerp(left.FarPlane, right.FarPlane, weight);
    }
    if (left.Projection == camera::CameraProjection::OrthographicFinite)
    {
        result.VerticalSpan = math::Lerp(left.VerticalSpan, right.VerticalSpan, weight);
        result.MinimumVisibleWidth = math::Lerp(left.MinimumVisibleWidth, right.MinimumVisibleWidth, weight);
    }
    else
    {
        result.VerticalFov = math::Lerp(left.VerticalFov, right.VerticalFov, weight);
    }
    return result;
}
} // namespace

PreparedSequence::PreparedSequence() noexcept = default;
PreparedSequence::~PreparedSequence() noexcept = default;
PreparedSequence::PreparedSequence(PreparedSequence&& other) noexcept = default;
PreparedSequence& PreparedSequence::operator=(PreparedSequence&& other) noexcept = default;

bool PreparedSequence::IsPrepared() const noexcept
{
    return mStorage.Get() != nullptr;
}
usize PreparedSequence::GetObjectCount() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : mStorage->Objects.GetSize();
}
usize PreparedSequence::GetCameraCount() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : mStorage->Cameras.GetSize();
}
usize PreparedSequence::GetPreparedBytes() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : mStorage->PreparedBytes;
}

SequenceStatus
TryPrepareSequence(const SequenceSource& source, const SequenceLimits& limits, PreparedSequence& outSequence) noexcept
{
    if (source.Objects.IsValid() == false || source.Cameras.IsValid() == false || limits.MaxTracks == 0 ||
        limits.MaxKeys == 0 || limits.MaxPreparedBytes == 0)
    {
        return SequenceStatus::InvalidArgument;
    }
    usize tracks = 0;
    StorageCounts counts;
    usize objectBytes = 0;
    usize cameraBytes = 0;
    if (foundation::TryAdd(source.Objects.GetSize(), source.Cameras.GetSize(), tracks) == false ||
        tracks > limits.MaxTracks ||
        foundation::TryMultiply(source.Objects.GetSize(), sizeof(SequenceObjectTrack), objectBytes) == false ||
        foundation::TryMultiply(source.Cameras.GetSize(), sizeof(SequenceCameraTrack), cameraBytes) == false ||
        foundation::TryAdd(counts.Bytes, objectBytes, counts.Bytes) == false ||
        foundation::TryAdd(counts.Bytes, cameraBytes, counts.Bytes) == false || counts.Bytes > limits.MaxPreparedBytes)
    {
        return SequenceStatus::BudgetExceeded;
    }
    if (tracks == 0)
    {
        return SequenceStatus::InvalidArgument;
    }
    for (usize index = 0; index < source.Objects.GetSize(); ++index)
    {
        const SequenceStatus status = ValidateObject(source.Objects[index], limits, counts);
        if (status != SequenceStatus::Success)
        {
            return status;
        }
    }
    for (usize index = 0; index < source.Cameras.GetSize(); ++index)
    {
        const SequenceStatus status = ValidateCamera(source.Cameras[index], limits, counts);
        if (status != SequenceStatus::Success)
        {
            return status;
        }
    }
    if (HasDuplicateBinding(source) == true)
    {
        return SequenceStatus::DuplicateBinding;
    }
    detail::SequenceStorage* storageOrNull = new (std::nothrow) detail::SequenceStorage;
    if (storageOrNull == nullptr)
    {
        return SequenceStatus::OutOfMemory;
    }
    foundation::UniquePtr<detail::SequenceStorage> candidate(storageOrNull);
    if (TryAllocateArray(source.Objects.GetSize(), candidate->Objects) == false ||
        TryAllocateArray(source.Cameras.GetSize(), candidate->Cameras) == false ||
        TryAllocateArray(counts.Vectors, candidate->Vectors) == false ||
        TryAllocateArray(counts.Rotations, candidate->Rotations) == false ||
        TryAllocateArray(counts.Positions, candidate->Positions) == false ||
        TryAllocateArray(counts.Visibility, candidate->Visibility) == false ||
        TryAllocateArray(counts.Lenses, candidate->Lenses) == false)
    {
        return SequenceStatus::OutOfMemory;
    }
    CopyTracks(source, *candidate);
    candidate->PreparedBytes = counts.Bytes;
    outSequence.mStorage = foundation::Move(candidate);
    return SequenceStatus::Success;
}

SequenceStatus TryEvaluateSequenceObject(const PreparedSequence& sequence,
                                         uint64 bindingId,
                                         SequenceTime time,
                                         SequenceObjectSample& outSample) noexcept
{
    if (IsTimeValid(time) == false || bindingId == 0)
    {
        return SequenceStatus::InvalidArgument;
    }
    if (sequence.mStorage.Get() == nullptr)
    {
        return SequenceStatus::Unprepared;
    }
    for (const SequenceObjectTrack& track : sequence.mStorage->Objects)
    {
        if (track.BindingId != bindingId)
        {
            continue;
        }
        SequenceObjectSample candidate
        {
            .BindingId = bindingId,
            .TransformLocal = track.BaseTransformLocal,
            .IsVisible = track.IsBaseVisible,
        };
        candidate.TransformLocal.Translation =
            EvaluateKeys(track.TranslationKeys, time, track.BaseTransformLocal.Translation, BlendVector);
        candidate.TransformLocal.Rotation =
            EvaluateKeys(track.RotationKeys, time, track.BaseTransformLocal.Rotation, BlendRotation);
        candidate.TransformLocal.Scale =
            EvaluateKeys(track.ScaleKeys, time, track.BaseTransformLocal.Scale, BlendVector);
        const auto holdVisibility = [](bool isLeftVisible, bool, float64) noexcept { return isLeftVisible; };
        candidate.IsVisible = EvaluateKeys(track.VisibilityKeys, time, track.IsBaseVisible, holdVisibility);
        outSample = candidate;
        return SequenceStatus::Success;
    }
    return SequenceStatus::MissingBinding;
}

SequenceStatus TryEvaluateSequenceCamera(const PreparedSequence& sequence,
                                         uint64 bindingId,
                                         SequenceTime time,
                                         camera::CameraSample& outSample) noexcept
{
    if (IsTimeValid(time) == false || bindingId == 0)
    {
        return SequenceStatus::InvalidArgument;
    }
    if (sequence.mStorage.Get() == nullptr)
    {
        return SequenceStatus::Unprepared;
    }
    for (const SequenceCameraTrack& track : sequence.mStorage->Cameras)
    {
        if (track.BindingId != bindingId)
        {
            continue;
        }
        camera::CameraSample candidate = track.BaseSample;
        candidate.Pose.Position = EvaluateKeys(track.PositionKeys, time, track.BaseSample.Pose.Position, BlendPosition);
        candidate.Pose.Orientation =
            EvaluateKeys(track.RotationKeys, time, track.BaseSample.Pose.Orientation, BlendRotation);
        candidate.Lens = EvaluateKeys(track.LensKeys, time, track.BaseSample.Lens, BlendLens);
        if (camera::ValidateCameraSample(candidate) != camera::CameraStatus::Success)
        {
            return SequenceStatus::OutOfRange;
        }
        outSample = candidate;
        return SequenceStatus::Success;
    }
    return SequenceStatus::MissingBinding;
}
} // namespace ludus::gameplay::sequencer
