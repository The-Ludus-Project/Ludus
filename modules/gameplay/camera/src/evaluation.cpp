#include <ludus/gameplay/camera/evaluation.hpp>

#include <ludus/foundation/math/dynamics.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/gameplay/camera/control.hpp>

namespace ludus::gameplay::camera
{
namespace math = foundation::math;
using foundation::float32;
using foundation::float64;
using foundation::uint64;

namespace
{
/// Undamped placement of one recipe before publication.
struct Placement final
{
    CameraPose Pose{};
    math::Vector3d Pivot{};
    uint64 BindingId = 0;
};

[[nodiscard]] math::Vector3d Widen(math::Vector3 value) noexcept
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

[[nodiscard]] bool IsNonnegative(float32 value) noexcept
{
    return math::IsFinite(value) == true && value >= 0;
}

/// Orbit arm root offset plus arm, in camera-local coordinates.
[[nodiscard]] math::Vector3 GetOrbitLocalEye(const CameraOrbitParameters& orbit) noexcept
{
    return orbit.ShoulderOffset + math::Vector3{0, 0, orbit.ArmLength};
}

[[nodiscard]] CameraStatus TryComposeOrbitEye(const math::Vector3d& pivot,
                                              const math::Quaternion& look,
                                              const CameraOrbitParameters& orbit,
                                              math::Vector3d& outEye) noexcept
{
    const math::Vector3d eye = pivot + Widen(math::Rotate(look, GetOrbitLocalEye(orbit)));
    if (math::IsFinite(eye) == false)
    {
        return CameraStatus::OutOfRange;
    }
    outEye = eye;
    return CameraStatus::Success;
}

[[nodiscard]] CameraStatus TryResolveTarget(const CameraTargetSample* targetOrNull) noexcept
{
    if (targetOrNull == nullptr)
    {
        return CameraStatus::MissingTarget;
    }
    if (math::IsFinite(targetOrNull->Position) == false || targetOrNull->BindingId == 0)
    {
        return CameraStatus::InvalidInput;
    }
    return CameraStatus::Success;
}

/// Compute the desired placement of a validated definition.
[[nodiscard]] CameraStatus TryComputePlacement(const CameraRigDefinition& definition,
                                               const CameraUpdateContext& context,
                                               const CameraTargetSample* targetOrNull,
                                               Placement& outPlacement) noexcept
{
    Placement placement{ .Pose = definition.Pose, .Pivot = definition.Pose.Position };
    if (definition.Recipe == CameraRecipe::Fixed)
    {
        outPlacement = placement;
        return CameraStatus::Success;
    }
    const CameraStatus targetStatus = TryResolveTarget(targetOrNull);
    if (targetStatus != CameraStatus::Success)
    {
        return targetStatus;
    }
    placement.BindingId = targetOrNull->BindingId;
    if (definition.Recipe == CameraRecipe::OrthographicFollow)
    {
        placement.Pivot = targetOrNull->Position;
        placement.Pose.Position.X = targetOrNull->Position.X + static_cast<float64>(definition.FollowOffset.X);
        placement.Pose.Position.Y = targetOrNull->Position.Y + static_cast<float64>(definition.FollowOffset.Y);
        if (math::IsFinite(placement.Pose.Position) == false)
        {
            return CameraStatus::OutOfRange;
        }
        outPlacement = placement;
        return CameraStatus::Success;
    }
    // Orbit: the rig clamps the authoritative pitch to its own bounds; yaw is used as supplied.
    if (ValidateControlView(context.Control) != CameraStatus::Success)
    {
        return CameraStatus::InvalidInput;
    }
    const CameraControlView clamped
    {
        .Yaw = context.Control.Yaw,
        .Pitch = math::Clamp(context.Control.Pitch, definition.Orbit.Pitch.Minimum, definition.Orbit.Pitch.Maximum),
    };
    math::Quaternion look;
    if (TryComputeControlOrientation(clamped, look) != CameraStatus::Success)
    {
        return CameraStatus::OutOfRange;
    }
    placement.Pivot = targetOrNull->Position + Widen(definition.Orbit.PivotOffset);
    placement.Pose.Orientation = look;
    if (math::IsFinite(placement.Pivot) == false ||
        TryComposeOrbitEye(placement.Pivot, look, definition.Orbit, placement.Pose.Position) != CameraStatus::Success)
    {
        return CameraStatus::OutOfRange;
    }
    outPlacement = placement;
    return CameraStatus::Success;
}
} // namespace

CameraStatus ValidateCameraSample(const CameraSample& sample) noexcept
{
    const auto& pose = sample.Pose;
    const auto& lens = sample.Lens;
    if (math::IsFinite(pose.Position) == false || math::IsFinite(pose.Orientation) == false ||
        math::Abs(math::Dot(pose.Orientation, pose.Orientation) - 1.0F) > 1e-4F || sample.OutputId == 0 ||
        sample.DefinitionRevision == 0 || sample.PreviousTick > sample.CurrentTick ||
        math::IsFinite(sample.RenderAlpha) == false || sample.RenderAlpha < 0 || sample.RenderAlpha > 1 ||
        math::IsFinite(lens.NearPlane) == false)
    {
        return CameraStatus::InvalidInput;
    }
    switch (lens.Projection)
    {
        case CameraProjection::PerspectiveFinite:
        case CameraProjection::PerspectiveInfinite:
            if (math::IsFinite(lens.VerticalFov) == false || lens.VerticalFov <= 0 || lens.VerticalFov >= math::kPiF ||
                lens.NearPlane <= 0)
            {
                return CameraStatus::InvalidInput;
            }
            break;
        case CameraProjection::OrthographicFinite:
            if (math::IsFinite(lens.VerticalSpan) == false || lens.VerticalSpan <= 0 ||
                math::IsFinite(lens.MinimumVisibleWidth) == false || lens.MinimumVisibleWidth < 0 || lens.NearPlane < 0)
            {
                return CameraStatus::InvalidInput;
            }
            break;
        default:
            return CameraStatus::InvalidInput;
    }
    if (lens.Projection != CameraProjection::PerspectiveInfinite &&
        (math::IsFinite(lens.FarPlane) == false || lens.FarPlane <= lens.NearPlane))
    {
        return CameraStatus::InvalidInput;
    }
    return CameraStatus::Success;
}

CameraStatus ValidateCameraRigDefinition(const CameraRigDefinition& definition) noexcept
{
    const CameraSample authored
    {
        .Pose = definition.Pose,
        .Lens = definition.Lens,
        .DefinitionRevision = definition.Revision,
    };
    if (ValidateCameraSample(authored) != CameraStatus::Success ||
        IsNonnegative(definition.Damping.PivotSmoothTimeSeconds) == false ||
        IsNonnegative(definition.Damping.LookHalfLifeSeconds) == false)
    {
        return CameraStatus::InvalidInput;
    }
    switch (definition.Recipe)
    {
        case CameraRecipe::Fixed:
            return CameraStatus::Success;
        case CameraRecipe::OrthographicFollow:
            if (definition.Lens.Projection != CameraProjection::OrthographicFinite ||
                math::IsFinite(definition.FollowOffset) == false || definition.Pose.Orientation.X != 0 ||
                definition.Pose.Orientation.Y != 0 || definition.Pose.Orientation.Z != 0)
            {
                return CameraStatus::InvalidInput;
            }
            return CameraStatus::Success;
        case CameraRecipe::Orbit:
            if (math::IsFinite(definition.Orbit.PivotOffset) == false ||
                math::IsFinite(definition.Orbit.ShoulderOffset) == false ||
                IsNonnegative(definition.Orbit.ArmLength) == false ||
                ValidatePitchLimits(definition.Orbit.Pitch) != CameraStatus::Success)
            {
                return CameraStatus::InvalidInput;
            }
            return CameraStatus::Success;
        default:
            return CameraStatus::InvalidInput;
    }
}

CameraStatus TryEvaluateCamera(const CameraRigDefinition& definition,
                               const CameraUpdateContext& context,
                               const CameraTargetSample* target,
                               CameraSample& output,
                               CameraTrace* trace) noexcept
{
    // Thanks to David Paull, "The Vector Camera", Game Programming Gems 1,
    // section 4.2, pp. 366-370: inspect pose/lens independently of matrices.
    // Adapted to checked RH unit quaternions, foundation::float64 world coordinates and
    // value publication; no historical projection or code is copied.
    // Reading scope and source links: docs/architecture/camera-systems-gems-review.md.
    CameraSample candidate
    {
        .Pose = definition.Pose,
        .Lens = definition.Lens,
        .OutputId = context.OutputId,
        .DefinitionRevision = definition.Revision,
        .PreviousTick = context.PreviousTick,
        .CurrentTick = context.CurrentTick,
        .RenderAlpha = context.RenderAlpha,
        .PresentationSequence = context.PresentationSequence,
        .DiscontinuityRevision = context.DiscontinuityRevision,
    };
    if (ValidateCameraSample(candidate) != CameraStatus::Success ||
        ValidateCameraRigDefinition(definition) != CameraStatus::Success)
    {
        return CameraStatus::InvalidInput;
    }
    Placement placement;
    const CameraStatus status = TryComputePlacement(definition, context, target, placement);
    if (status != CameraStatus::Success)
    {
        return status;
    }
    candidate.Pose = placement.Pose;
    if (ValidateCameraSample(candidate) != CameraStatus::Success)
    {
        return CameraStatus::OutOfRange;
    }
    const CameraTrace checkpoint
    {
        .Recipe = definition.Recipe,
        .BindingId = placement.BindingId,
        .Desired = candidate.Pose,
        .Published = candidate,
        .Pivot = placement.Pivot,
    };
    output = candidate;
    if (trace != nullptr)
    {
        *trace = checkpoint;
    }
    return CameraStatus::Success;
}

CameraStatus TryAdvanceCameraRig(const CameraRigDefinition& definition,
                                 const CameraUpdateContext& context,
                                 const CameraTargetSample* targetOrNull,
                                 float64 deltaSeconds,
                                 CameraRigState& inoutState,
                                 CameraSample& outSample,
                                 CameraTrace* traceOrNull) noexcept
{
    if (math::IsFinite(deltaSeconds) == false || deltaSeconds < 0)
    {
        return CameraStatus::InvalidInput;
    }
    CameraSample desired;
    CameraTrace checkpoint;
    const CameraStatus status = TryEvaluateCamera(definition, context, targetOrNull, desired, &checkpoint);
    if (status != CameraStatus::Success)
    {
        return status;
    }
    CameraRigState state = inoutState;
    CameraSample sample = desired;
    if (definition.Recipe == CameraRecipe::Fixed)
    {
        checkpoint.IsReprimed = state.IsPrimed == false;
        state = { .IsPrimed = true, .Pivot = checkpoint.Pivot, .Look = desired.Pose.Orientation };
    }
    else
    {
        checkpoint.IsReprimed = state.IsPrimed == false || state.BindingId != targetOrNull->BindingId ||
                                state.TargetDiscontinuityRevision != targetOrNull->DiscontinuityRevision;
        if (checkpoint.IsReprimed == true)
        {
            state =
            {
                .IsPrimed = true,
                .BindingId = targetOrNull->BindingId,
                .TargetDiscontinuityRevision = targetOrNull->DiscontinuityRevision,
                .Pivot = checkpoint.Pivot,
                .Look = desired.Pose.Orientation,
            };
        }
        else
        {
            // The spring target is the desired pivot from this presentation's target sample; the held-target
            // step is exact, and a moving target is followed sample by sample.
            math::CriticalSpringStep step;
            math::Vector3d displacement;
            if (math::TryComputeCriticalSpringStep(static_cast<float64>(definition.Damping.PivotSmoothTimeSeconds),
                                                   deltaSeconds,
                                                   step) != math::MathStatus::Success ||
                math::TryAdvanceCriticalSpring(step,
                                               state.Pivot - checkpoint.Pivot,
                                               state.PivotVelocity,
                                               displacement,
                                               state.PivotVelocity) != math::MathStatus::Success)
            {
                return CameraStatus::OutOfRange;
            }
            state.Pivot = checkpoint.Pivot + displacement;
            if (definition.Recipe == CameraRecipe::Orbit)
            {
                float64 weight = 0;
                if (math::TryComputeExponentialDampingWeight(
                        static_cast<float64>(definition.Damping.LookHalfLifeSeconds),
                        deltaSeconds,
                        weight) != math::MathStatus::Success)
                {
                    return CameraStatus::OutOfRange;
                }
                state.Look = math::SlerpShortest(state.Look, desired.Pose.Orientation, static_cast<float32>(weight));
            }
        }
        if (definition.Recipe == CameraRecipe::Orbit)
        {
            sample.Pose.Orientation = state.Look;
            if (TryComposeOrbitEye(state.Pivot, state.Look, definition.Orbit, sample.Pose.Position) !=
                CameraStatus::Success)
            {
                return CameraStatus::OutOfRange;
            }
        }
        else
        {
            sample.Pose.Position.X = state.Pivot.X + static_cast<float64>(definition.FollowOffset.X);
            sample.Pose.Position.Y = state.Pivot.Y + static_cast<float64>(definition.FollowOffset.Y);
        }
        if (math::IsFinite(state.Pivot) == false || ValidateCameraSample(sample) != CameraStatus::Success)
        {
            return CameraStatus::OutOfRange;
        }
    }
    checkpoint.Published = sample;
    checkpoint.Pivot = state.Pivot;
    inoutState = state;
    outSample = sample;
    if (traceOrNull != nullptr)
    {
        *traceOrNull = checkpoint;
    }
    return CameraStatus::Success;
}
} // namespace ludus::gameplay::camera
