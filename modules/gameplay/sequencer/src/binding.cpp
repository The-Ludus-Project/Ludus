#include <ludus/gameplay/sequencer/binding.hpp>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <new>

namespace ludus::gameplay::sequencer
{
using foundation::uint64;
using foundation::usize;
namespace identity = foundation::identity;
namespace math = foundation::math;

namespace detail
{
struct SequencePlanStorage final
{
    PreparedSequence Sequence;
    foundation::Array<SequenceResolvedBinding> Bindings;
    foundation::Array<SequenceCameraInterval> Selection;
    uint64 PlaybackStartTick = 0;
    uint64 PlaybackEndTick = 0;
    foundation::uint32 TicksPerSecond = 0;
    uint64 SceneRevision = 0;
};
} // namespace detail

namespace
{
const PreparedSequence UNPREPARED_SEQUENCE{};

/// Sampler groups built from a definition; object bases come from the catalog when one is supplied.
struct SamplerGroups final
{
    foundation::Array<SequenceObjectTrack> Objects;
    foundation::Array<SequenceCameraTrack> Cameras;
};

SequenceStatus Fail(SequenceStatus status, SequenceDiagnostic& outDiagnostic) noexcept
{
    outDiagnostic.Status = status;
    return status;
}

bool IsBindingKindValid(SequenceBindingKind kind) noexcept
{
    return kind == SequenceBindingKind::WorldObject || kind == SequenceBindingKind::SequenceCamera ||
           kind == SequenceBindingKind::LegacyWorldObject;
}

bool IsObjectChannel(SequenceChannel channel) noexcept
{
    return channel == SequenceChannel::Translation || channel == SequenceChannel::Rotation ||
           channel == SequenceChannel::Scale || channel == SequenceChannel::Visibility;
}

bool IsCameraChannel(SequenceChannel channel) noexcept
{
    return channel == SequenceChannel::CameraPosition || channel == SequenceChannel::CameraRotation ||
           channel == SequenceChannel::CameraLens;
}

bool IsObjectKind(SequenceBindingKind kind) noexcept
{
    return kind == SequenceBindingKind::WorldObject || kind == SequenceBindingKind::LegacyWorldObject;
}

bool AreTrackViewsValid(const SequenceTrack& track) noexcept
{
    return track.VectorKeys.IsValid() == true && track.RotationKeys.IsValid() == true &&
           track.PositionKeys.IsValid() == true && track.VisibilityKeys.IsValid() == true &&
           track.LensKeys.IsValid() == true;
}

/// Number of keys in the view that matches the track's channel.
usize GetChannelKeyCount(const SequenceTrack& track) noexcept
{
    switch (track.Channel)
    {
        case SequenceChannel::Translation:
        case SequenceChannel::Scale:
            return track.VectorKeys.GetSize();
        case SequenceChannel::Rotation:
        case SequenceChannel::CameraRotation:
            return track.RotationKeys.GetSize();
        case SequenceChannel::CameraPosition:
            return track.PositionKeys.GetSize();
        case SequenceChannel::Visibility:
            return track.VisibilityKeys.GetSize();
        case SequenceChannel::CameraLens:
            return track.LensKeys.GetSize();
    }
    return 0;
}

usize GetTotalKeyCount(const SequenceTrack& track) noexcept
{
    return track.VectorKeys.GetSize() + track.RotationKeys.GetSize() + track.PositionKeys.GetSize() +
           track.VisibilityKeys.GetSize() + track.LensKeys.GetSize();
}

usize FindBinding(const SequenceDefinition& definition, uint64 bindingId) noexcept
{
    for (usize index = 0; index < definition.Bindings.GetSize(); ++index)
    {
        if (definition.Bindings[index].BindingId == bindingId)
        {
            return index;
        }
    }
    return SEQUENCE_NO_INDEX;
}

bool IsRotationValid(math::Quaternion rotation) noexcept
{
    return math::IsFinite(rotation) == true &&
           math::Abs(math::Dot(rotation, rotation) - 1.0F) <= SEQUENCE_ROTATION_UNIT_TOLERANCE;
}

bool IsBaseTransformValid(const math::TransformTRS& transform) noexcept
{
    return math::IsFinite(transform.Translation) == true && IsRotationValid(transform.Rotation) == true &&
           math::IsFinite(transform.Scale) == true && transform.Scale.X > 0 && transform.Scale.Y > 0 &&
           transform.Scale.Z > 0;
}

bool IsVersion4(const identity::Uuid& value) noexcept
{
    return identity::ValidateUuid(value, identity::UuidPolicy::RequireVersion4) == identity::UuidStatus::Success;
}

SequenceStatus ValidateBindingFields(const SequenceBinding& binding) noexcept
{
    if (binding.BindingId == 0 || IsBindingKindValid(binding.Kind) == false)
    {
        return SequenceStatus::InvalidArgument;
    }
    const bool hasNoObject = binding.Object.IsNil() == true && binding.SubobjectKey == 0;
    const bool hasNoLegacyKey = binding.LegacyKey.empty() == true;
    const bool hasNoCamera = binding.Camera == camera::CameraSample{};
    switch (binding.Kind)
    {
        case SequenceBindingKind::WorldObject:
            return IsVersion4(binding.Object.Value) == true && hasNoLegacyKey == true && hasNoCamera == true
                       ? SequenceStatus::Success
                       : SequenceStatus::InvalidArgument;
        case SequenceBindingKind::LegacyWorldObject:
            return hasNoLegacyKey == false && binding.LegacyKey.size() <= MAX_SEQUENCE_LEGACY_KEY_BYTES &&
                           hasNoObject == true && hasNoCamera == true
                       ? SequenceStatus::Success
                       : SequenceStatus::InvalidArgument;
        case SequenceBindingKind::SequenceCamera:
            return camera::ValidateCameraSample(binding.Camera) == camera::CameraStatus::Success &&
                           hasNoObject == true && hasNoLegacyKey == true
                       ? SequenceStatus::Success
                       : SequenceStatus::InvalidArgument;
    }
    return SequenceStatus::InvalidArgument;
}

SequenceStatus ValidateBindings(const SequenceDefinition& definition, SequenceDiagnostic& outDiagnostic) noexcept
{
    bool hasWorldObject = false;
    bool hasLegacyObject = false;
    for (usize index = 0; index < definition.Bindings.GetSize(); ++index)
    {
        const SequenceBinding& binding = definition.Bindings[index];
        outDiagnostic.BindingId = binding.BindingId;
        const SequenceStatus fields = ValidateBindingFields(binding);
        if (fields != SequenceStatus::Success)
        {
            return Fail(fields, outDiagnostic);
        }
        for (usize previous = 0; previous < index; ++previous)
        {
            if (definition.Bindings[previous].BindingId == binding.BindingId)
            {
                return Fail(SequenceStatus::DuplicateBinding, outDiagnostic);
            }
        }
        hasWorldObject = hasWorldObject == true || binding.Kind == SequenceBindingKind::WorldObject;
        hasLegacyObject = hasLegacyObject == true || binding.Kind == SequenceBindingKind::LegacyWorldObject;
    }
    outDiagnostic.BindingId = 0;
    if (definition.World.Document.IsNil() == false && IsVersion4(definition.World.Document.Value) == false)
    {
        return Fail(SequenceStatus::InvalidArgument, outDiagnostic);
    }
    if ((hasWorldObject == true && definition.World.Document.IsNil() == true) ||
        (hasLegacyObject == true && definition.World.HasContentDigest == false))
    {
        // A World-object binding is meaningless without the World it names, and a
        // version-1 entity key is only stable for one pinned saved resource.
        for (const SequenceBinding& binding : definition.Bindings)
        {
            if ((binding.Kind == SequenceBindingKind::WorldObject && definition.World.Document.IsNil() == true) ||
                (binding.Kind == SequenceBindingKind::LegacyWorldObject && definition.World.HasContentDigest == false))
            {
                outDiagnostic.BindingId = binding.BindingId;
                break;
            }
        }
        return Fail(SequenceStatus::InvalidArgument, outDiagnostic);
    }
    return SequenceStatus::Success;
}

SequenceStatus ValidateTrackShape(const SequenceDefinition& definition,
                                  const SequenceTrack& track,
                                  SequenceDiagnostic& outDiagnostic) noexcept
{
    outDiagnostic.TrackId = track.TrackId;
    outDiagnostic.BindingId = track.BindingId;
    if (track.TrackId == 0 || AreTrackViewsValid(track) == false ||
        (IsObjectChannel(track.Channel) == false && IsCameraChannel(track.Channel) == false))
    {
        return Fail(SequenceStatus::InvalidArgument, outDiagnostic);
    }
    const usize bindingIndex = FindBinding(definition, track.BindingId);
    if (bindingIndex == SEQUENCE_NO_INDEX)
    {
        return Fail(SequenceStatus::MissingBinding, outDiagnostic);
    }
    const bool isObjectBinding = IsObjectKind(definition.Bindings[bindingIndex].Kind);
    if (isObjectBinding != IsObjectChannel(track.Channel))
    {
        return Fail(SequenceStatus::UnsupportedChannel, outDiagnostic);
    }
    const usize keys = GetChannelKeyCount(track);
    if (keys != GetTotalKeyCount(track))
    {
        // Keys in a view that does not match the channel would be silently ignored.
        return Fail(SequenceStatus::InvalidArgument, outDiagnostic);
    }
    if (keys == 0)
    {
        return Fail(SequenceStatus::InvalidKeys, outDiagnostic);
    }
    return SequenceStatus::Success;
}

SequenceStatus ValidateTracks(const SequenceDefinition& definition,
                              const SequenceDefinitionLimits& limits,
                              SequenceDiagnostic& outDiagnostic) noexcept
{
    usize totalKeys = 0;
    for (usize index = 0; index < definition.Tracks.GetSize(); ++index)
    {
        const SequenceTrack& track = definition.Tracks[index];
        const SequenceStatus shape = ValidateTrackShape(definition, track, outDiagnostic);
        if (shape != SequenceStatus::Success)
        {
            return shape;
        }
        if (foundation::TryAdd(totalKeys, GetChannelKeyCount(track), totalKeys) == false ||
            totalKeys > limits.Sampler.MaxKeys)
        {
            return Fail(SequenceStatus::BudgetExceeded, outDiagnostic);
        }
        for (usize previous = 0; previous < index; ++previous)
        {
            const SequenceTrack& other = definition.Tracks[previous];
            if (other.TrackId == track.TrackId)
            {
                outDiagnostic.OtherTrackId = other.TrackId;
                return Fail(SequenceStatus::DuplicateBinding, outDiagnostic);
            }
        }
    }
    // Conflicts are checked after every identity is known to be unique, so the
    // reported pair does not depend on which malformed row was met first.
    for (usize index = 0; index < definition.Tracks.GetSize(); ++index)
    {
        const SequenceTrack& track = definition.Tracks[index];
        for (usize previous = 0; previous < index; ++previous)
        {
            const SequenceTrack& other = definition.Tracks[previous];
            if (track.IsEnabled == true && other.IsEnabled == true && other.BindingId == track.BindingId &&
                other.Channel == track.Channel)
            {
                outDiagnostic.BindingId = track.BindingId;
                outDiagnostic.TrackId = other.TrackId < track.TrackId ? other.TrackId : track.TrackId;
                outDiagnostic.OtherTrackId = other.TrackId < track.TrackId ? track.TrackId : other.TrackId;
                return Fail(SequenceStatus::ConflictingWriter, outDiagnostic);
            }
        }
    }
    outDiagnostic.TrackId = 0;
    outDiagnostic.BindingId = 0;
    return SequenceStatus::Success;
}

bool HasCameraBinding(const SequenceDefinition& definition) noexcept
{
    for (const SequenceBinding& binding : definition.Bindings)
    {
        if (binding.Kind == SequenceBindingKind::SequenceCamera)
        {
            return true;
        }
    }
    return false;
}

SequenceStatus ValidateSelection(const SequenceDefinition& definition, SequenceDiagnostic& outDiagnostic) noexcept
{
    const foundation::BorrowedView<const SequenceCameraInterval> intervals = definition.CameraSelection;
    if (HasCameraBinding(definition) == false)
    {
        if (intervals.IsEmpty() == false)
        {
            outDiagnostic.Index = 0;
            return Fail(SequenceStatus::InvalidSelection, outDiagnostic);
        }
        return SequenceStatus::Success;
    }
    if (intervals.IsEmpty() == true)
    {
        return Fail(SequenceStatus::InvalidSelection, outDiagnostic);
    }
    for (usize index = 0; index < intervals.GetSize(); ++index)
    {
        const SequenceCameraInterval& interval = intervals[index];
        const uint64 expectedStart = index == 0 ? definition.PlaybackStartTick : intervals[index - 1].EndTick;
        const usize bindingIndex = FindBinding(definition, interval.CameraBindingId);
        outDiagnostic.Index = index;
        outDiagnostic.BindingId = interval.CameraBindingId;
        if (interval.StartTick != expectedStart || interval.EndTick <= interval.StartTick ||
            interval.EndTick > definition.PlaybackEndTick || bindingIndex == SEQUENCE_NO_INDEX ||
            definition.Bindings[bindingIndex].Kind != SequenceBindingKind::SequenceCamera)
        {
            return Fail(SequenceStatus::InvalidSelection, outDiagnostic);
        }
    }
    if (intervals[intervals.GetSize() - 1].EndTick != definition.PlaybackEndTick)
    {
        return Fail(SequenceStatus::InvalidSelection, outDiagnostic);
    }
    outDiagnostic.Index = SEQUENCE_NO_INDEX;
    outDiagnostic.BindingId = 0;
    return SequenceStatus::Success;
}

void AssignChannel(const SequenceTrack& track, SequenceObjectTrack& inoutGroup) noexcept
{
    switch (track.Channel)
    {
        case SequenceChannel::Translation:
            inoutGroup.TranslationKeys = track.VectorKeys;
            break;
        case SequenceChannel::Rotation:
            inoutGroup.RotationKeys = track.RotationKeys;
            break;
        case SequenceChannel::Scale:
            inoutGroup.ScaleKeys = track.VectorKeys;
            break;
        case SequenceChannel::Visibility:
            inoutGroup.VisibilityKeys = track.VisibilityKeys;
            break;
        case SequenceChannel::CameraPosition:
        case SequenceChannel::CameraRotation:
        case SequenceChannel::CameraLens:
            break;
    }
}

void AssignChannel(const SequenceTrack& track, SequenceCameraTrack& inoutGroup) noexcept
{
    switch (track.Channel)
    {
        case SequenceChannel::CameraPosition:
            inoutGroup.PositionKeys = track.PositionKeys;
            break;
        case SequenceChannel::CameraRotation:
            inoutGroup.RotationKeys = track.RotationKeys;
            break;
        case SequenceChannel::CameraLens:
            inoutGroup.LensKeys = track.LensKeys;
            break;
        case SequenceChannel::Translation:
        case SequenceChannel::Rotation:
        case SequenceChannel::Scale:
        case SequenceChannel::Visibility:
            break;
    }
}

/// Validate one track's keys alone, so a failure names the track that holds them.
SequenceStatus ValidateTrackKeys(const SequenceDefinition& definition,
                                 const SequenceTrack& track,
                                 const SequenceLimits& limits,
                                 SequenceDiagnostic& outDiagnostic) noexcept
{
    const SequenceBinding& binding = definition.Bindings[FindBinding(definition, track.BindingId)];
    PreparedSequence candidate;
    SequenceStatus status = SequenceStatus::Success;
    if (IsObjectChannel(track.Channel) == true)
    {
        SequenceObjectTrack group{ .BindingId = binding.BindingId };
        AssignChannel(track, group);
        status = TryPrepareSequence({ .Objects = {&group, 1} }, limits, candidate);
    }
    else
    {
        SequenceCameraTrack group{ .BindingId = binding.BindingId, .BaseSample = binding.Camera };
        AssignChannel(track, group);
        status = TryPrepareSequence({ .Cameras = {&group, 1} }, limits, candidate);
    }
    if (status != SequenceStatus::Success)
    {
        outDiagnostic.BindingId = track.BindingId;
        outDiagnostic.TrackId = track.TrackId;
        return Fail(status, outDiagnostic);
    }
    return SequenceStatus::Success;
}

bool IsSelectedCamera(const SequenceDefinition& definition, uint64 bindingId) noexcept
{
    for (const SequenceCameraInterval& interval : definition.CameraSelection)
    {
        if (interval.CameraBindingId == bindingId)
        {
            return true;
        }
    }
    return false;
}

bool HasEnabledTrack(const SequenceDefinition& definition, uint64 bindingId) noexcept
{
    for (const SequenceTrack& track : definition.Tracks)
    {
        if (track.IsEnabled == true && track.BindingId == bindingId)
        {
            return true;
        }
    }
    return false;
}

bool IsActive(const SequenceDefinition& definition, const SequenceBinding& binding) noexcept
{
    return HasEnabledTrack(definition, binding.BindingId) == true ||
           (binding.Kind == SequenceBindingKind::SequenceCamera && IsSelectedCamera(definition, binding.BindingId));
}

/// Build one sampler group per active binding, in definition order.
/// @param targetIndices Catalog index of each binding (SEQUENCE_NO_INDEX for cameras), or empty for identity bases.
SequenceStatus BuildSamplerGroups(const SequenceDefinition& definition,
                                  const SequenceSceneCatalog& catalog,
                                  foundation::BorrowedView<const usize> targetIndices,
                                  SamplerGroups& outGroups) noexcept
{
    usize objects = 0;
    usize cameras = 0;
    for (const SequenceBinding& binding : definition.Bindings)
    {
        if (IsActive(definition, binding) == true)
        {
            ++(binding.Kind == SequenceBindingKind::SequenceCamera ? cameras : objects);
        }
    }
    if (outGroups.Objects.TryEnsureCapacity(objects) == false || outGroups.Cameras.TryEnsureCapacity(cameras) == false)
    {
        return SequenceStatus::OutOfMemory;
    }
    for (usize index = 0; index < definition.Bindings.GetSize(); ++index)
    {
        const SequenceBinding& binding = definition.Bindings[index];
        if (IsActive(definition, binding) == false)
        {
            continue;
        }
        if (binding.Kind == SequenceBindingKind::SequenceCamera)
        {
            SequenceCameraTrack group{ .BindingId = binding.BindingId, .BaseSample = binding.Camera };
            for (const SequenceTrack& track : definition.Tracks)
            {
                if (track.IsEnabled == true && track.BindingId == binding.BindingId)
                {
                    AssignChannel(track, group);
                }
            }
            [[maybe_unused]] const bool isAdded = outGroups.Cameras.TryAdd(group);
            LUDUS_ASSERT(isAdded == true);
            continue;
        }
        SequenceObjectTrack group{ .BindingId = binding.BindingId };
        if (targetIndices.IsEmpty() == false)
        {
            const SequenceSceneTarget& target = catalog.Targets[targetIndices[index]];
            group.BaseTransformLocal = target.BaseTransformLocal;
            group.IsBaseVisible = target.IsBaseVisible;
        }
        for (const SequenceTrack& track : definition.Tracks)
        {
            if (track.IsEnabled == true && track.BindingId == binding.BindingId)
            {
                AssignChannel(track, group);
            }
        }
        [[maybe_unused]] const bool isAdded = outGroups.Objects.TryAdd(group);
        LUDUS_ASSERT(isAdded == true);
    }
    return SequenceStatus::Success;
}

SequenceStatus
PrepareGroups(const SamplerGroups& groups, const SequenceLimits& limits, PreparedSequence& outSequence) noexcept
{
    const SequenceSource source
    {
        .Objects = groups.Objects.AsView(),
        .Cameras = groups.Cameras.AsView(),
    };
    return TryPrepareSequence(source, limits, outSequence);
}

bool DoesTargetMatch(const SequenceBinding& binding, const SequenceSceneTarget& target) noexcept
{
    if (binding.Kind == SequenceBindingKind::WorldObject)
    {
        return target.Object.IsNil() == false && target.Object == binding.Object &&
               target.SubobjectKey == binding.SubobjectKey;
    }
    return target.LegacyKey.empty() == false && target.LegacyKey == binding.LegacyKey;
}

bool IsChannelSupported(const SequenceSceneTarget& target, SequenceChannel channel) noexcept
{
    return channel == SequenceChannel::Visibility ? target.CanWriteVisibility : target.CanWriteLocalTransform;
}

SequenceStatus ValidateCatalog(const SequenceDefinition& definition,
                               const SequenceSceneCatalog& catalog,
                               SequenceDiagnostic& outDiagnostic) noexcept
{
    if (catalog.Targets.IsValid() == false)
    {
        return Fail(SequenceStatus::InvalidArgument, outDiagnostic);
    }
    for (usize index = 0; index < catalog.Targets.GetSize(); ++index)
    {
        const usize parent = catalog.Targets[index].ParentIndex;
        if (parent != SEQUENCE_NO_INDEX && parent >= catalog.Targets.GetSize())
        {
            outDiagnostic.Index = index;
            return Fail(SequenceStatus::InvalidArgument, outDiagnostic);
        }
    }
    const SequenceWorldReference& world = definition.World;
    if ((world.Document.IsNil() == false && world.Document != catalog.WorldDocument) ||
        (world.HasContentDigest == true &&
         (catalog.HasContentDigest == false || world.ContentDigest != catalog.ContentDigest)))
    {
        return Fail(SequenceStatus::WorldMismatch, outDiagnostic);
    }
    return SequenceStatus::Success;
}

SequenceStatus ResolveObjectBinding(const SequenceDefinition& definition,
                                    const SequenceSceneCatalog& catalog,
                                    const SequenceBinding& binding,
                                    usize& outTargetIndex,
                                    SequenceDiagnostic& outDiagnostic) noexcept
{
    outDiagnostic.BindingId = binding.BindingId;
    usize found = SEQUENCE_NO_INDEX;
    for (usize index = 0; index < catalog.Targets.GetSize(); ++index)
    {
        if (DoesTargetMatch(binding, catalog.Targets[index]) == false)
        {
            continue;
        }
        if (found != SEQUENCE_NO_INDEX)
        {
            // Never choose between equally named targets; the adapter must supply a stable subobject key.
            outDiagnostic.Index = index;
            return Fail(SequenceStatus::AmbiguousTarget, outDiagnostic);
        }
        found = index;
    }
    if (found == SEQUENCE_NO_INDEX)
    {
        return Fail(SequenceStatus::MissingBinding, outDiagnostic);
    }
    outDiagnostic.Index = found;
    const SequenceSceneTarget& target = catalog.Targets[found];
    if (IsBaseTransformValid(target.BaseTransformLocal) == false)
    {
        return Fail(SequenceStatus::InvalidArgument, outDiagnostic);
    }
    for (const SequenceTrack& track : definition.Tracks)
    {
        if (track.IsEnabled == true && track.BindingId == binding.BindingId &&
            IsChannelSupported(target, track.Channel) == false)
        {
            outDiagnostic.TrackId = track.TrackId;
            return Fail(SequenceStatus::UnsupportedChannel, outDiagnostic);
        }
    }
    // A chain longer than the catalog must revisit a target.
    usize parent = target.ParentIndex;
    for (usize steps = 0; parent != SEQUENCE_NO_INDEX; ++steps)
    {
        if (steps >= catalog.Targets.GetSize())
        {
            return Fail(SequenceStatus::ParentCycle, outDiagnostic);
        }
        parent = catalog.Targets[parent].ParentIndex;
    }
    outTargetIndex = found;
    outDiagnostic.BindingId = 0;
    outDiagnostic.Index = SEQUENCE_NO_INDEX;
    return SequenceStatus::Success;
}

SequenceStatus ResolveBindings(const SequenceDefinition& definition,
                               const SequenceSceneCatalog& catalog,
                               foundation::Array<usize>& outTargetIndices,
                               SequenceDiagnostic& outDiagnostic) noexcept
{
    if (outTargetIndices.TryEnsureCapacity(definition.Bindings.GetSize()) == false ||
        outTargetIndices.TryResize(definition.Bindings.GetSize(), SEQUENCE_NO_INDEX) == false)
    {
        return Fail(SequenceStatus::OutOfMemory, outDiagnostic);
    }
    for (usize index = 0; index < definition.Bindings.GetSize(); ++index)
    {
        const SequenceBinding& binding = definition.Bindings[index];
        if (IsObjectKind(binding.Kind) == false || IsActive(definition, binding) == false)
        {
            continue;
        }
        const SequenceStatus status =
            ResolveObjectBinding(definition, catalog, binding, outTargetIndices[index], outDiagnostic);
        if (status != SequenceStatus::Success)
        {
            return status;
        }
        for (usize previous = 0; previous < index; ++previous)
        {
            if (outTargetIndices[previous] == outTargetIndices[index])
            {
                // Two bindings (for example a UUID and a legacy key) would write one object twice.
                outDiagnostic.BindingId = binding.BindingId;
                outDiagnostic.Index = outTargetIndices[index];
                return Fail(SequenceStatus::DuplicateBinding, outDiagnostic);
            }
        }
    }
    return SequenceStatus::Success;
}

bool IsTimeValid(SequenceTime time) noexcept
{
    return time.Denominator > 0 && time.Numerator < time.Denominator;
}
} // namespace

PreparedSequencePlan::PreparedSequencePlan() noexcept = default;
PreparedSequencePlan::~PreparedSequencePlan() noexcept = default;
PreparedSequencePlan::PreparedSequencePlan(PreparedSequencePlan&& other) noexcept = default;
PreparedSequencePlan& PreparedSequencePlan::operator=(PreparedSequencePlan&& other) noexcept = default;

bool PreparedSequencePlan::IsPrepared() const noexcept
{
    return mStorage.Get() != nullptr;
}

const PreparedSequence& PreparedSequencePlan::GetSequence() const noexcept
{
    return mStorage.Get() == nullptr ? UNPREPARED_SEQUENCE : mStorage->Sequence;
}

foundation::BorrowedView<const SequenceResolvedBinding> PreparedSequencePlan::GetResolvedBindings() const noexcept
{
    if (mStorage.Get() == nullptr)
    {
        return {};
    }
    return mStorage->Bindings.AsView();
}

foundation::BorrowedView<const SequenceCameraInterval> PreparedSequencePlan::GetCameraSelection() const noexcept
{
    if (mStorage.Get() == nullptr)
    {
        return {};
    }
    return mStorage->Selection.AsView();
}

uint64 PreparedSequencePlan::GetPlaybackStartTick() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : mStorage->PlaybackStartTick;
}

uint64 PreparedSequencePlan::GetPlaybackEndTick() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : mStorage->PlaybackEndTick;
}

foundation::uint32 PreparedSequencePlan::GetTicksPerSecond() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : mStorage->TicksPerSecond;
}

uint64 PreparedSequencePlan::GetSceneRevision() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : mStorage->SceneRevision;
}

SequenceStatus TryValidateSequenceDefinition(const SequenceDefinition& definition,
                                             const SequenceDefinitionLimits& limits,
                                             SequenceDiagnostic& outDiagnostic) noexcept
{
    outDiagnostic = {};
    if (definition.Bindings.IsValid() == false || definition.Tracks.IsValid() == false ||
        definition.CameraSelection.IsValid() == false || definition.TicksPerSecond == 0 ||
        definition.PlaybackEndTick <= definition.PlaybackStartTick || limits.MaxTracks == 0 ||
        limits.MaxCameraIntervals == 0 || limits.Sampler.MaxTracks == 0 || limits.Sampler.MaxKeys == 0 ||
        limits.Sampler.MaxPreparedBytes == 0)
    {
        return Fail(SequenceStatus::InvalidArgument, outDiagnostic);
    }
    if (definition.Bindings.GetSize() > limits.Sampler.MaxTracks || definition.Tracks.GetSize() > limits.MaxTracks ||
        definition.CameraSelection.GetSize() > limits.MaxCameraIntervals)
    {
        return Fail(SequenceStatus::BudgetExceeded, outDiagnostic);
    }
    const SequenceStatus bindings = ValidateBindings(definition, outDiagnostic);
    if (bindings != SequenceStatus::Success)
    {
        return bindings;
    }
    const SequenceStatus tracks = ValidateTracks(definition, limits, outDiagnostic);
    if (tracks != SequenceStatus::Success)
    {
        return tracks;
    }
    const SequenceStatus selection = ValidateSelection(definition, outDiagnostic);
    if (selection != SequenceStatus::Success)
    {
        return selection;
    }
    for (const SequenceTrack& track : definition.Tracks)
    {
        const SequenceStatus keys = ValidateTrackKeys(definition, track, limits.Sampler, outDiagnostic);
        if (keys != SequenceStatus::Success)
        {
            return keys;
        }
    }
    SamplerGroups groups;
    const SequenceStatus built = BuildSamplerGroups(definition, {}, {}, groups);
    if (built != SequenceStatus::Success)
    {
        return Fail(built, outDiagnostic);
    }
    if (groups.Objects.IsEmpty() == true && groups.Cameras.IsEmpty() == true)
    {
        // A new or fully disabled sequence is valid content; it has nothing to prepare.
        return SequenceStatus::Success;
    }
    PreparedSequence candidate;
    const SequenceStatus prepared = PrepareGroups(groups, limits.Sampler, candidate);
    return prepared == SequenceStatus::Success ? prepared : Fail(prepared, outDiagnostic);
}

SequenceStatus TryPrepareSequencePlan(const SequenceDefinition& definition,
                                      const SequenceSceneCatalog& catalog,
                                      const SequenceDefinitionLimits& limits,
                                      PreparedSequencePlan& outPlan,
                                      SequenceDiagnostic& outDiagnostic) noexcept
{
    const SequenceStatus valid = TryValidateSequenceDefinition(definition, limits, outDiagnostic);
    if (valid != SequenceStatus::Success)
    {
        return valid;
    }
    const SequenceStatus world = ValidateCatalog(definition, catalog, outDiagnostic);
    if (world != SequenceStatus::Success)
    {
        return world;
    }
    foundation::Array<usize> targetIndices;
    const SequenceStatus resolved = ResolveBindings(definition, catalog, targetIndices, outDiagnostic);
    if (resolved != SequenceStatus::Success)
    {
        return resolved;
    }
    SamplerGroups groups;
    const SequenceStatus built = BuildSamplerGroups(definition, catalog, targetIndices.AsView(), groups);
    if (built != SequenceStatus::Success)
    {
        return Fail(built, outDiagnostic);
    }
    if (groups.Objects.IsEmpty() == true && groups.Cameras.IsEmpty() == true)
    {
        // Nothing is written, so there is no evaluation plan to publish.
        return Fail(SequenceStatus::InvalidArgument, outDiagnostic);
    }
    detail::SequencePlanStorage* storageOrNull = new (std::nothrow) detail::SequencePlanStorage;
    if (storageOrNull == nullptr)
    {
        return Fail(SequenceStatus::OutOfMemory, outDiagnostic);
    }
    foundation::UniquePtr<detail::SequencePlanStorage> candidate(storageOrNull);
    const SequenceStatus prepared = PrepareGroups(groups, limits.Sampler, candidate->Sequence);
    if (prepared != SequenceStatus::Success)
    {
        return Fail(prepared, outDiagnostic);
    }
    const usize activeCount = groups.Objects.GetSize() + groups.Cameras.GetSize();
    if (candidate->Bindings.TryEnsureCapacity(activeCount) == false ||
        candidate->Selection.TryEnsureCapacity(definition.CameraSelection.GetSize()) == false ||
        candidate->Selection.TryAddRange(definition.CameraSelection) == false)
    {
        return Fail(SequenceStatus::OutOfMemory, outDiagnostic);
    }
    for (usize index = 0; index < definition.Bindings.GetSize(); ++index)
    {
        const SequenceBinding& binding = definition.Bindings[index];
        if (IsActive(definition, binding) == false)
        {
            continue;
        }
        const SequenceResolvedBinding entry
        {
            .BindingId = binding.BindingId,
            .Kind = binding.Kind,
            .TargetIndex = targetIndices[index],
        };
        [[maybe_unused]] const bool isAdded = candidate->Bindings.TryAdd(entry);
        LUDUS_ASSERT(isAdded == true);
    }
    candidate->PlaybackStartTick = definition.PlaybackStartTick;
    candidate->PlaybackEndTick = definition.PlaybackEndTick;
    candidate->TicksPerSecond = definition.TicksPerSecond;
    candidate->SceneRevision = catalog.SceneRevision;
    outPlan.mStorage = foundation::Move(candidate);
    return SequenceStatus::Success;
}

SequenceStatus TryEvaluateSequenceCameraSelection(const PreparedSequencePlan& plan,
                                                  SequenceTime time,
                                                  SequenceCameraSelection& outSelection) noexcept
{
    if (plan.IsPrepared() == false)
    {
        return SequenceStatus::Unprepared;
    }
    if (IsTimeValid(time) == false)
    {
        return SequenceStatus::InvalidArgument;
    }
    // The fraction is in [0, 1), so comparing whole ticks against integer
    // boundaries is exact: Tick + f >= B exactly when Tick >= B.
    if (time.Tick < plan.GetPlaybackStartTick() || time.Tick >= plan.GetPlaybackEndTick())
    {
        return SequenceStatus::OutOfRange;
    }
    const foundation::BorrowedView<const SequenceCameraInterval> intervals = plan.GetCameraSelection();
    if (intervals.IsEmpty() == true)
    {
        return SequenceStatus::MissingBinding;
    }
    usize first = 0;
    usize end = intervals.GetSize();
    while (first < end)
    {
        const usize middle = first + (end - first) / 2;
        if (intervals[middle].StartTick <= time.Tick)
        {
            first = middle + 1;
        }
        else
        {
            end = middle;
        }
    }
    const usize index = first - 1;
    outSelection =
    {
        .CameraBindingId = intervals[index].CameraBindingId,
        .IntervalIndex = index,
    };
    return SequenceStatus::Success;
}
} // namespace ludus::gameplay::sequencer
