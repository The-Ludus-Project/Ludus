#include "camera.hpp"

#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/precision.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/gameplay/camera/evaluation.hpp>

namespace cornell
{
namespace math = ludus::foundation::math;
using namespace ludus::foundation;
using namespace ludus::gameplay::camera;

CameraRigDefinition DefaultCamera() noexcept
{
    return
    {
        .Pose = { .Position = {0, 1, 3.6} },
        .Lens =
        {
            .Projection = CameraProjection::PerspectiveInfinite,
            .VerticalFov = 0.76101275F, // 2 * atan(0.4), matching the original shot.
            .NearPlane = 0.01F,
        },
    };
}

CameraStatus TryBuildCameraRays(const CameraSample& sample, uint32 width, uint32 height, CameraRays& output) noexcept
{
    if (ValidateCameraSample(sample) != CameraStatus::Success || width == 0 || height == 0 ||
        sample.Lens.Projection == CameraProjection::OrthographicFinite)
    {
        return CameraStatus::InvalidInput;
    }
    math::Vector3 eye;
    auto status = math::TryMakeRelative(sample.Pose.Position, {}, 10000.0, eye);
    if (status != math::MathStatus::Success)
    {
        return status == math::MathStatus::OutOfRange ? CameraStatus::OutOfRange : CameraStatus::InvalidInput;
    }
    const auto aspect = static_cast<float32>(width) / static_cast<float32>(height);
    math::Matrix4 projection;
    if (sample.Lens.Projection == CameraProjection::PerspectiveInfinite)
    {
        status =
            math::TryPerspectiveReverseZInfinite(sample.Lens.VerticalFov, aspect, sample.Lens.NearPlane, projection);
    }
    else
    {
        status = math::TryPerspectiveReverseZ(sample.Lens.VerticalFov,
                                              aspect,
                                              sample.Lens.NearPlane,
                                              sample.Lens.FarPlane,
                                              projection);
    }
    if (status != math::MathStatus::Success)
    {
        return status == math::MathStatus::OutOfRange ? CameraStatus::OutOfRange : CameraStatus::InvalidInput;
    }
    // The sample owns ray extraction from the C0 pose/lens publication. Invert
    // canonical projection X/Y scales rather than keeping a second shader FOV.
    // Contract and renderer boundary: docs/architecture/camera-systems.md.
    const auto basis = math::ToMatrix3(sample.Pose.Orientation);
    const auto right = basis.Columns[0] / projection.Columns[0].X;
    const auto up = basis.Columns[1] / projection.Columns[1].Y;
    const auto forward = -basis.Columns[2];
    if (math::IsFinite(right) == false || math::IsFinite(up) == false || math::IsFinite(forward) == false)
    {
        return CameraStatus::OutOfRange;
    }
    output =
    {
        .Eye = {eye.X, eye.Y, eye.Z, 0},
        .Right = {right.X, right.Y, right.Z, 0},
        .Up = {up.X, up.Y, up.Z, 0},
        .Forward = {forward.X, forward.Y, forward.Z, 0},
    };
    return CameraStatus::Success;
}
} // namespace cornell
