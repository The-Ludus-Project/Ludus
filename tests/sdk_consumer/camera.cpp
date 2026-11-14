#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/precision.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/gameplay/camera/director.hpp>
#include <ludus/gameplay/camera/evaluation.hpp>
#include <ludus/graphics/renderer/views.hpp>

bool ExerciseInstalledCamera() noexcept
{
    using namespace ludus::gameplay::camera;
    const CameraRigDefinition definition{ .Recipe = CameraRecipe::OrthographicFollow, .FollowOffset = {2, 3} };
    const CameraTargetSample target{ .Position = {1.0e12 + 0.25, 4, 0} };
    CameraSample sample;
    CameraTrace trace;
    return TryEvaluateCamera(definition, { .OutputId = 7 }, &target, sample, &trace) == CameraStatus::Success &&
           sample.Pose.Position.X == 1.0e12 + 2.25 && sample.Pose.Position.Y == 7 && sample.OutputId == 7 &&
           trace.Published.OutputId == 7 && ValidateCameraSample(sample) == CameraStatus::Success;
}

namespace
{
namespace camera = ludus::gameplay::camera;
namespace math = ludus::foundation::math;
namespace renderer = ludus::graphics::renderer;
using ludus::foundation::float32;
using ludus::foundation::float64;
using ludus::foundation::uint32;
using ludus::foundation::uint64;

constexpr uint32 DRAWABLE_WIDTH = 1280;
constexpr uint32 DRAWABLE_HEIGHT = 720;
constexpr uint64 PLAYER_BINDING = 7;
/// The recognizable World target: the player's chest, the orbit pivot both perspective rigs share.
constexpr math::Vector3d TARGET_FEET{4, 0, -3};
constexpr math::Vector3 TARGET_CHEST_OFFSET{0, 1.5F, 0};
constexpr math::Vector3d TARGET_CHEST{4, 1.5, -3};

/// Projection through the installed renderer interface: rigid inverse pose, renderer-owned lens factory.
bool TryProjectTarget(const camera::CameraSample& sample, math::Vector2& outPixel) noexcept
{
    math::TransformTRS pose{ .Rotation = sample.Pose.Orientation };
    math::Affine3 viewToWorld;
    math::Affine3 worldToView;
    if (math::TryMakeRelative(sample.Pose.Position, {}, 1.0e4, pose.Translation) != math::MathStatus::Success ||
        math::TryToAffine3(pose, 1e-4F, viewToWorld) != math::MathStatus::Success ||
        math::TryInverse(viewToWorld, worldToView) != math::MathStatus::Success)
    {
        return false;
    }
    const float32 aspect = static_cast<float32>(DRAWABLE_WIDTH) / static_cast<float32>(DRAWABLE_HEIGHT);
    renderer::ProjectionDescription projection
    {
        .WorldToView = math::ToMatrix4(worldToView),
        .VerticalFovRadians = sample.Lens.VerticalFov,
        .OrthographicHeight = math::Max(sample.Lens.VerticalSpan, sample.Lens.MinimumVisibleWidth / aspect),
        .Near = sample.Lens.NearPlane,
        .Far = sample.Lens.FarPlane,
    };
    projection.Kind = sample.Lens.Projection == camera::CameraProjection::OrthographicFinite
                          ? renderer::ProjectionKind::Orthographic
                      : sample.Lens.Projection == camera::CameraProjection::PerspectiveFinite
                          ? renderer::ProjectionKind::Perspective
                          : renderer::ProjectionKind::PerspectiveInfinite;
    renderer::ViewMapping mapping;
    renderer::ViewDescription view;
    if (renderer::ResolveViewMapping(
            {
                .Width = static_cast<float32>(DRAWABLE_WIDTH),
                .Height = static_cast<float32>(DRAWABLE_HEIGHT),
            },
            { .Width = DRAWABLE_WIDTH, .Height = DRAWABLE_HEIGHT },
            DRAWABLE_WIDTH,
            DRAWABLE_HEIGHT,
            mapping) != ludus::graphics::rhi::RasterStatus::Ready ||
        renderer::BuildViewDescription(projection, mapping, view) != ludus::graphics::rhi::RasterStatus::Ready)
    {
        return false;
    }
    const math::Vector4 clip = view.WorldToClip * math::Vector4{static_cast<float32>(TARGET_CHEST.X),
                                                                static_cast<float32>(TARGET_CHEST.Y),
                                                                static_cast<float32>(TARGET_CHEST.Z),
                                                                1};
    math::Vector3 ndc;
    if (clip.W <= 0 || math::TryPerspectiveDivide(clip, 1e-6F, ndc) != math::MathStatus::Success)
    {
        return false;
    }
    outPixel = {(ndc.X + 1) * 0.5F * static_cast<float32>(DRAWABLE_WIDTH),
                (1 - ndc.Y) * 0.5F * static_cast<float32>(DRAWABLE_HEIGHT)};
    return true;
}

camera::CameraRigRequest MakeOrbit(float32 arm, float32 fov, float32 shoulder) noexcept
{
    return
    {
        .Definition =
        {
            .Recipe = camera::CameraRecipe::Orbit,
            .Lens =
            {
                .Projection = camera::CameraProjection::PerspectiveInfinite,
                .VerticalFov = fov,
                .NearPlane = 0.1F,
            },
            .Orbit = { .PivotOffset = TARGET_CHEST_OFFSET, .ShoulderOffset = {shoulder, 0, 0}, .ArmLength = arm },
        },
        .BindingId = PLAYER_BINDING,
        .Transition = { .DurationSeconds = 1, .Path = camera::CameraBlendPath::AroundPivot },
    };
}

/// Orthographic overview straight above the target.
camera::CameraRigRequest MakeOverview() noexcept
{
    camera::CameraRigRequest request
    {
        .Definition =
        {
            .Pose = { .Position = {TARGET_FEET.X, 30, TARGET_FEET.Z} },
            .Lens = { .VerticalSpan = 40, .NearPlane = 0, .FarPlane = 100 },
        },
        .Transition = { .DurationSeconds = 1 },
    };
    return math::TryFromAxisAngle({1, 0, 0}, -math::kHalfPiF, request.Definition.Pose.Orientation) ==
                   math::MathStatus::Success
               ? request
               : camera::CameraRigRequest{};
}

/// One recorded presentation: the target's screen position and the cut flags the renderer would read.
struct FrameRecord final
{
    math::Vector2 TargetPixel{};
    camera::CameraCutReason CutReason = camera::CameraCutReason::None;
    uint64 DiscontinuityRevision = 0;
    bool IsBlending = false;
};

struct Presentation final
{
    camera::CameraDirector Director{};
    camera::CameraTargetSample Target{ .Position = TARGET_FEET, .BindingId = PLAYER_BINDING };
    uint64 Sequence = 0;
    camera::CameraSample Sample{};
    camera::CameraDirectorTrace Trace{};
    FrameRecord Record{};

    bool Step(float64 dt) noexcept
    {
        ++Sequence;
        const camera::CameraFrameInput input
        {
            .PreviousTick = Sequence,
            .CurrentTick = Sequence,
            .PresentationSequence = Sequence,
            .DeltaSeconds = dt,
            .Control = { .Yaw = 0.6F, .Pitch = -0.25F },
            .Targets = {&Target, 1},
        };
        if (Director.TryUpdate(input, Sample, &Trace) != camera::CameraStatus::Success)
        {
            return false;
        }
        Record =
        {
            .CutReason = Trace.CutReason,
            .DiscontinuityRevision = Sample.DiscontinuityRevision,
            .IsBlending = Trace.IsBlending,
        };
        return TryProjectTarget(Sample, Record.TargetPixel);
    }
};

/// Reverse a shared-pivot mode change mid-blend at one render rate and check target continuity.
bool ExerciseModeReversal(uint32 rate) noexcept
{
    const float64 dt = 1.0 / rate;
    Presentation presentation;
    const camera::CameraRigRequest explore = MakeOrbit(6, 1.0F, 0);
    const camera::CameraRigRequest inspect = MakeOrbit(2.5F, 0.7F, 0.6F);
    if (presentation.Director.SetBaseRig(explore) != camera::CameraStatus::Success || presentation.Step(0) == false ||
        presentation.Record.CutReason != camera::CameraCutReason::FirstPublication ||
        math::Distance(presentation.Record.TargetPixel, {DRAWABLE_WIDTH * 0.5F, DRAWABLE_HEIGHT * 0.5F}) > 0.01F ||
        presentation.Director.SetBaseRig(inspect) != camera::CameraStatus::Success)
    {
        return false;
    }
    const uint64 revision = presentation.Record.DiscontinuityRevision;
    float32 largestBlendStep = 0;
    math::Vector2 previous = presentation.Record.TargetPixel;
    for (uint32 frame = 0; frame < rate / 2; ++frame)
    {
        if (presentation.Step(dt) == false || presentation.Record.IsBlending == false ||
            presentation.Record.DiscontinuityRevision != revision)
        {
            return false;
        }
        largestBlendStep = math::Max(largestBlendStep, math::Distance(presentation.Record.TargetPixel, previous));
        previous = presentation.Record.TargetPixel;
    }
    const camera::CameraSample shown = presentation.Sample;
    if (presentation.Director.SetBaseRig(explore) != camera::CameraStatus::Success || presentation.Step(dt) == false)
    {
        return false;
    }
    // The reversed blend starts from the last published pre-effects pose and lens (declared tolerance: one
    // centimetre and one milliradian of field of view after one frame), without a cut or a screen-space jump.
    const float32 reversalStep = math::Distance(presentation.Record.TargetPixel, previous);
    if (presentation.Trace.IsOutgoingFrozen == false || presentation.Trace.Outgoing.Pose != shown.Pose ||
        presentation.Trace.Outgoing.Lens != shown.Lens ||
        math::Length(presentation.Sample.Pose.Position - shown.Pose.Position) > 0.01 ||
        math::Abs(presentation.Sample.Lens.VerticalFov - shown.Lens.VerticalFov) > 0.001F ||
        presentation.Record.CutReason != camera::CameraCutReason::None ||
        presentation.Record.DiscontinuityRevision != revision || reversalStep > largestBlendStep + 0.5F)
    {
        return false;
    }
    for (uint32 frame = 0; frame <= rate; ++frame)
    {
        if (presentation.Step(dt) == false)
        {
            return false;
        }
    }
    return presentation.Record.IsBlending == false && presentation.Record.DiscontinuityRevision == revision &&
           math::Distance(presentation.Record.TargetPixel, {DRAWABLE_WIDTH * 0.5F, DRAWABLE_HEIGHT * 0.5F}) < 0.01F;
}

/// Overview to inspect crosses projection kinds: the contract cuts and resets view history both ways.
bool ExerciseProjectionCut(uint32 rate) noexcept
{
    const float64 dt = 1.0 / rate;
    Presentation presentation;
    if (presentation.Director.SetBaseRig(MakeOverview()) != camera::CameraStatus::Success ||
        presentation.Step(dt) == false ||
        math::Distance(presentation.Record.TargetPixel, {DRAWABLE_WIDTH * 0.5F, DRAWABLE_HEIGHT * 0.5F}) > 0.5F)
    {
        return false;
    }
    const uint64 overview = presentation.Record.DiscontinuityRevision;
    if (presentation.Director.SetBaseRig(MakeOrbit(2.5F, 0.7F, 0.6F)) != camera::CameraStatus::Success ||
        presentation.Step(dt) == false || presentation.Record.CutReason != camera::CameraCutReason::IncompatibleLens ||
        presentation.Record.IsBlending == true || presentation.Record.DiscontinuityRevision != overview + 1 ||
        presentation.Sample.Lens.Projection != camera::CameraProjection::PerspectiveInfinite)
    {
        return false;
    }
    for (uint32 frame = 0; frame < rate / 4; ++frame)
    {
        if (presentation.Step(dt) == false || presentation.Record.CutReason != camera::CameraCutReason::None)
        {
            return false;
        }
    }
    return presentation.Director.SetBaseRig(MakeOverview()) == camera::CameraStatus::Success &&
           presentation.Step(dt) == true &&
           presentation.Record.CutReason == camera::CameraCutReason::IncompatibleLens &&
           presentation.Record.DiscontinuityRevision == overview + 2;
}
} // namespace

bool ExerciseInstalledCameraDirector() noexcept
{
    constexpr uint32 rates[] = {30, 60, 120, 240};
    for (const uint32 rate : rates)
    {
        if (ExerciseModeReversal(rate) == false || ExerciseProjectionCut(rate) == false)
        {
            return false;
        }
    }
    return true;
}
