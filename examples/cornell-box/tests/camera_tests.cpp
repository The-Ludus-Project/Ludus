#include "camera.hpp"

#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/gameplay/camera/evaluation.hpp>

namespace
{
using namespace ludus::foundation;
using namespace ludus::gameplay::camera;
namespace math = ludus::foundation::math;
bool Near(float32 first, float32 second) noexcept
{
    return math::Abs(first - second) < 1e-5F;
}
bool Same(const cornell::CameraRays& first, const cornell::CameraRays& second) noexcept
{
    return first.Eye == second.Eye && first.Right == second.Right && first.Up == second.Up &&
           first.Forward == second.Forward;
}
bool CheckCamera() noexcept
{
    CameraSample sample;
    CameraTrace trace;
    if (TryEvaluateCamera(cornell::DefaultCamera(), { .PresentationSequence = 17 }, nullptr, sample, &trace) !=
            CameraStatus::Success ||
        trace.Published.PresentationSequence != 17)
    {
        return false;
    }
    constexpr uint32 viewports[][2] = {{720, 720}, {1280, 720}, {480, 740}};
    for (const auto& viewport : viewports)
    {
        cornell::CameraRays rays;
        const auto aspect = static_cast<float32>(viewport[0]) / static_cast<float32>(viewport[1]);
        if (cornell::TryBuildCameraRays(sample, viewport[0], viewport[1], rays) != CameraStatus::Success ||
            Near(rays.Eye.Y, 1) == false || Near(rays.Eye.Z, 3.6F) == false ||
            Near(rays.Right.X, 0.4F * aspect) == false || Near(rays.Up.Y, 0.4F) == false ||
            Near(rays.Forward.Z, -1) == false)
        {
            return false;
        }
        math::Matrix4 projection;
        if (math::TryPerspectiveReverseZInfinite(sample.Lens.VerticalFov, aspect, sample.Lens.NearPlane, projection) !=
            math::MathStatus::Success)
        {
            return false;
        }
        constexpr math::Vector2 points[] = {{0, 0}, {-0.7F, 0.8F}, {0.9F, -0.4F}};
        for (const auto point : points)
        {
            const math::Vector4 direction{rays.Right.X * point.X + rays.Up.X * point.Y + rays.Forward.X,
                                          rays.Right.Y * point.X + rays.Up.Y * point.Y + rays.Forward.Y,
                                          rays.Right.Z * point.X + rays.Up.Z * point.Y + rays.Forward.Z,
                                          1};
            math::Vector3 ndc;
            if (math::TryPerspectiveDivide(projection * direction, 0, ndc) != math::MathStatus::Success ||
                Near(ndc.X, point.X) == false || Near(ndc.Y, point.Y) == false)
            {
                return false;
            }
        }
    }
    if (math::TryFromAxisAngle({0, 1, 0}, math::kHalfPiF, sample.Pose.Orientation) != math::MathStatus::Success)
    {
        return false;
    }
    cornell::CameraRays rotated;
    if (cornell::TryBuildCameraRays(sample, 720, 720, rotated) != CameraStatus::Success ||
        Near(rotated.Forward.X, -1) == false)
    {
        return false;
    }
    auto preserved = rotated;
    if (cornell::TryBuildCameraRays(sample, 0, 720, preserved) != CameraStatus::InvalidInput ||
        Same(preserved, rotated) == false)
    {
        return false;
    }
    sample.Lens.Projection = CameraProjection::OrthographicFinite;
    if (cornell::TryBuildCameraRays(sample, 720, 720, preserved) != CameraStatus::InvalidInput ||
        Same(preserved, rotated) == false)
    {
        return false;
    }
    sample.Lens.Projection = CameraProjection::PerspectiveFinite;
    if (cornell::TryBuildCameraRays(sample, 720, 720, preserved) != CameraStatus::Success)
    {
        return false;
    }
    const auto finite = preserved;
    sample.Lens.VerticalFov = -1;
    if (cornell::TryBuildCameraRays(sample, 720, 720, preserved) != CameraStatus::InvalidInput ||
        Same(preserved, finite) == false)
    {
        return false;
    }
    sample.Lens.VerticalFov = cornell::DefaultCamera().Lens.VerticalFov;
    sample.Pose.Position.X = 10001;
    return cornell::TryBuildCameraRays(sample, 720, 720, preserved) == CameraStatus::OutOfRange &&
           Same(preserved, finite);
}
} // namespace
ludus::foundation::int32 main()
{
    return CheckCamera() ? 0 : 1;
}
