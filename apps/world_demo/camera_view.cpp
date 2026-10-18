#include "internal/camera_view.h"

#include <ludus/foundation/math/precision.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>

namespace ludus::world_demo
{
namespace math = foundation::math;
using gameplay::camera::CameraProjection;
using gameplay::camera::CameraSample;
using gameplay::camera::CameraStatus;

namespace
{
CameraStatus Convert(math::MathStatus status) noexcept
{
    return status == math::MathStatus::OutOfRange ? CameraStatus::OutOfRange : CameraStatus::InvalidInput;
}
} // namespace

CameraStatus TryBuildCameraView(const CameraSample& sample,
                                CameraViewport viewport,
                                math::Vector3d origin,
                                foundation::float64 maxRelative,
                                CameraRenderView& output) noexcept
{
    if (gameplay::camera::ValidateCameraSample(sample) != CameraStatus::Success || viewport.Width == 0 ||
        viewport.Height == 0)
    {
        return CameraStatus::InvalidInput;
    }
    math::Vector3 eye;
    auto status = math::TryMakeRelative(sample.Pose.Position, origin, maxRelative, eye);
    if (status != math::MathStatus::Success)
    {
        return Convert(status);
    }
    CameraRenderView candidate{ .Origin = origin, .Viewport = viewport };
    // Thanks to David Paull, "The Vector Camera", Game Programming Gems 1,
    // section 4.2, pp. 366-370, for keeping pose/basis inspectable. Rigid inverse
    // uses Ludus RH quaternion Math, not the article's LH projection pipeline.
    // See docs/architecture/camera-systems-gems-review.md for the reading scope.
    const auto rotation = math::ToMatrix3(math::Conjugate(sample.Pose.Orientation));
    const math::Affine3 inverse
    {
        .Columns = {rotation.Columns[0], rotation.Columns[1], rotation.Columns[2]},
        .Translation = -(rotation * eye),
    };
    candidate.View = math::ToMatrix4(inverse);
    const auto aspect =
        static_cast<foundation::float32>(viewport.Width) / static_cast<foundation::float32>(viewport.Height);
    const auto& lens = sample.Lens;
    switch (lens.Projection)
    {
        case CameraProjection::PerspectiveFinite:
            status = math::TryPerspectiveReverseZ(lens.VerticalFov,
                                                  aspect,
                                                  lens.NearPlane,
                                                  lens.FarPlane,
                                                  candidate.Projection);
            break;
        case CameraProjection::PerspectiveInfinite:
            status =
                math::TryPerspectiveReverseZInfinite(lens.VerticalFov, aspect, lens.NearPlane, candidate.Projection);
            break;
        case CameraProjection::OrthographicFinite: {
            const foundation::float64 span = math::Max(static_cast<foundation::float64>(lens.VerticalSpan),
                                                       static_cast<foundation::float64>(lens.MinimumVisibleWidth) /
                                                           static_cast<foundation::float64>(aspect));
            const foundation::float64 halfHeight = span * 0.5;
            const foundation::float64 halfWidth = halfHeight * static_cast<foundation::float64>(aspect);
            // Avoid an unchecked float narrowing and intervals that collapse at zero.
            constexpr foundation::float64 kMaxFloat = 0x1.fffffep127;
            if (span > kMaxFloat || halfWidth > kMaxFloat)
            {
                return CameraStatus::OutOfRange;
            }
            candidate.VerticalSpan = static_cast<foundation::float32>(span);
            status = math::TryOrthographicReverseZ(-static_cast<foundation::float32>(halfWidth),
                                                   static_cast<foundation::float32>(halfWidth),
                                                   -static_cast<foundation::float32>(halfHeight),
                                                   static_cast<foundation::float32>(halfHeight),
                                                   lens.NearPlane,
                                                   lens.FarPlane,
                                                   candidate.Projection);
            break;
        }
        default:
            return CameraStatus::InvalidInput;
    }
    if (status != math::MathStatus::Success)
    {
        return Convert(status);
    }
    candidate.ViewProjection = candidate.Projection * candidate.View;
    if (math::IsFinite(candidate.View) == false || math::IsFinite(candidate.ViewProjection) == false)
    {
        return CameraStatus::OutOfRange;
    }
    output = candidate;
    return CameraStatus::Success;
}

CameraStatus TryBuildPlanarCameraView(const CameraSample& sample,
                                      CameraViewport viewport,
                                      math::Vector3d origin,
                                      foundation::float64 maxRelative,
                                      PlanarCameraView& output) noexcept
{
    if (sample.Lens.Projection != CameraProjection::OrthographicFinite || viewport.X != 0 || viewport.Y != 0 ||
        sample.Pose.Orientation.X != 0 || sample.Pose.Orientation.Y != 0 || sample.Pose.Orientation.Z != 0)
    {
        return CameraStatus::InvalidInput;
    }
    CameraRenderView view;
    const auto status = TryBuildCameraView(sample, viewport, origin, maxRelative, view);
    if (status != CameraStatus::Success)
    {
        return status;
    }
    // Identity rigid inverse stores the negative relative eye in its translation.
    output = { .Center = {-view.View.Columns[3].X, -view.View.Columns[3].Y}, .VerticalSpan = view.VerticalSpan };
    return CameraStatus::Success;
}
} // namespace ludus::world_demo
