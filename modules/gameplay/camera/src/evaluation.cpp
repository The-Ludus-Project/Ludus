#include <ludus/gameplay/camera/evaluation.hpp>

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::gameplay::camera
{
namespace math = foundation::math;

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
    if (ValidateCameraSample(candidate) != CameraStatus::Success)
    {
        return CameraStatus::InvalidInput;
    }
    foundation::uint64 binding = 0;
    switch (definition.Recipe)
    {
        case CameraRecipe::Fixed:
            break;
        case CameraRecipe::OrthographicFollow:
            if (definition.Lens.Projection != CameraProjection::OrthographicFinite ||
                math::IsFinite(definition.FollowOffset) == false || definition.Pose.Orientation.X != 0 ||
                definition.Pose.Orientation.Y != 0 || definition.Pose.Orientation.Z != 0)
            {
                return CameraStatus::InvalidInput;
            }
            if (target == nullptr)
            {
                return CameraStatus::MissingTarget;
            }
            if (math::IsFinite(target->Position) == false || target->BindingId == 0)
            {
                return CameraStatus::InvalidInput;
            }
            candidate.Pose.Position.X =
                target->Position.X + static_cast<foundation::float64>(definition.FollowOffset.X);
            candidate.Pose.Position.Y =
                target->Position.Y + static_cast<foundation::float64>(definition.FollowOffset.Y);
            if (math::IsFinite(candidate.Pose.Position) == false)
            {
                return CameraStatus::OutOfRange;
            }
            binding = target->BindingId;
            break;
        default:
            return CameraStatus::InvalidInput;
    }
    const CameraTrace checkpoint
    {
        .Recipe = definition.Recipe,
        .BindingId = binding,
        .Desired = candidate.Pose,
        .Published = candidate,
    };
    output = candidate;
    if (trace != nullptr)
    {
        *trace = checkpoint;
    }
    return CameraStatus::Success;
}
} // namespace ludus::gameplay::camera
