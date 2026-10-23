#include "internal/editor_camera_controller.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::editor
{
using namespace foundation;
namespace math = foundation::math;
namespace camera = gameplay::camera;
namespace
{
// Absolute numerical safety bounds, independent of scene size, in world units.
constexpr float32 MINIMUM_DISTANCE = 0.000001F;
constexpr float32 MAXIMUM_DISTANCE = 10000000;
constexpr float64 MAXIMUM_POSITION_WORLD = 10000000;
constexpr float32 LOOK_RADIANS_PER_PIXEL = math::kPiF / 720;
constexpr float32 MAXIMUM_PITCH_RADIANS = math::kHalfPiF - 0.001F;
/// Orbit pitch of the Top and Bottom views. Not a full 90 degrees, so yaw stays defined and
/// a later orbit continues smoothly; the resulting tilt is below a pixel at editor scales.
constexpr float32 VERTICAL_VIEW_PITCH_DEGREES = 89.9F;
/// Largest angle between a camera's forward direction and an axis that still names that view.
constexpr float32 AXIS_VIEW_TOLERANCE_DEGREES = 0.5F;
/// Perspective near plane as a fraction of the focus distance after leaving an orthographic view.
constexpr float32 PERSPECTIVE_NEAR_FRACTION = 0.001F;
/// Bounds of that near plane, in world units: positive, and no farther than the default 0.01.
constexpr float32 MINIMUM_PERSPECTIVE_NEAR_PLANE = 0.0000001F;
constexpr float32 MAXIMUM_PERSPECTIVE_NEAR_PLANE = 0.01F;
/// Fraction of the scene radius kept between an orthographic eye and the scene bounds.
constexpr float32 ORTHOGRAPHIC_CLEARANCE = 0.01F;
struct AxisViewDescription final
{
    EditorAxisView View;
    const char* Label;
    /// Orbit angles that produce the view, as TrySetOrbit takes them.
    math::Vector2 OrbitDegrees;
    /// World direction the camera looks along.
    math::Vector3 ForwardWorld;
};
constexpr StaticArray<AxisViewDescription, EDITOR_AXIS_VIEW_COUNT> AXIS_VIEWS{{
    {EditorAxisView::Top, "Top", {0, VERTICAL_VIEW_PITCH_DEGREES}, {0, -1, 0}},
    {EditorAxisView::Bottom, "Bottom", {0, -VERTICAL_VIEW_PITCH_DEGREES}, {0, 1, 0}},
    {EditorAxisView::Front, "Front", {0, 0}, {0, 0, -1}},
    {EditorAxisView::Back, "Back", {180, 0}, {0, 0, 1}},
    {EditorAxisView::Right, "Right", {90, 0}, {-1, 0, 0}},
    {EditorAxisView::Left, "Left", {-90, 0}, {1, 0, 0}},
}};
const AxisViewDescription& Describe(EditorAxisView view) noexcept
{
    const usize index = static_cast<usize>(view);
    return AXIS_VIEWS[index < EDITOR_AXIS_VIEW_COUNT ? index : 0];
}
math::Vector3d ConvertToWorld(math::Vector3 value) noexcept
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}
/// Height the lens shows at distance from the eye, in world units.
float32 GetVisibleHeight(const camera::CameraLens& lens, float32 distance) noexcept
{
    if (lens.Projection == camera::CameraProjection::OrthographicFinite)
    {
        return lens.VerticalSpan;
    }
    const float32 halfFov = lens.VerticalFov * 0.5F;
    return 2 * distance * math::Sin(halfFov) / math::Cos(halfFov);
}
} // namespace
bool TryGetEditorAxisView(const camera::CameraSample& sample, EditorAxisView& outView) noexcept
{
    if (sample.Lens.Projection != camera::CameraProjection::OrthographicFinite)
    {
        return false;
    }
    const math::Vector3 forwardWorld = math::Rotate(sample.Pose.Orientation, {0, 0, -1});
    const float32 minimumCosine = math::Cos(math::DegreesToRadians(AXIS_VIEW_TOLERANCE_DEGREES));
    for (const AxisViewDescription& description : AXIS_VIEWS)
    {
        if (math::Dot(forwardWorld, description.ForwardWorld) >= minimumCosine)
        {
            outView = description.View;
            return true;
        }
    }
    return false;
}
const char* GetEditorAxisViewLabel(EditorAxisView view) noexcept
{
    return Describe(view).Label;
}
EditorCameraController::EditorCameraController() noexcept
{
    mSample.Lens.Projection = camera::CameraProjection::PerspectiveInfinite;
    mSample.Lens.NearPlane = 0.01F;
    static_cast<void>(TrySetOrbit({}, {25, 20}, mDistance));
}
bool EditorCameraController::tryPublish(const camera::CameraSample& sample, float32 distance) noexcept
{
    if (math::IsFinite(distance) == false || distance < MINIMUM_DISTANCE || distance > MAXIMUM_DISTANCE ||
        math::Abs(sample.Pose.Position.X) > MAXIMUM_POSITION_WORLD ||
        math::Abs(sample.Pose.Position.Y) > MAXIMUM_POSITION_WORLD ||
        math::Abs(sample.Pose.Position.Z) > MAXIMUM_POSITION_WORLD)
    {
        return false;
    }
    const camera::CameraRigDefinition definition{ .Pose = sample.Pose, .Lens = sample.Lens };
    camera::CameraSample accepted;
    if (camera::TryEvaluateCamera(definition, {}, nullptr, accepted) != camera::CameraStatus::Success)
    {
        return false;
    }
    mSample = accepted;
    mDistance = distance;
    return true;
}
bool EditorCameraController::TryAdopt(const camera::CameraSample& sample, float32 distance) noexcept
{
    return tryPublish(sample, distance);
}
math::Vector3d EditorCameraController::GetPivotWorld() const noexcept
{
    return mSample.Pose.Position +
           ConvertToWorld(math::Rotate(mSample.Pose.Orientation, {0, 0, -1})) * static_cast<float64>(mDistance);
}
float32 EditorCameraController::GetYawDegrees() const noexcept
{
    const math::Vector3 forwardWorld = math::Rotate(mSample.Pose.Orientation, {0, 0, -1});
    return math::RadiansToDegrees(math::Atan2(-forwardWorld.X, -forwardWorld.Z));
}
float32 EditorCameraController::GetPitchDegrees() const noexcept
{
    const math::Vector3 forwardWorld = math::Rotate(mSample.Pose.Orientation, {0, 0, -1});
    return math::RadiansToDegrees(
        math::Atan2(-forwardWorld.Y, math::Sqrt(forwardWorld.X * forwardWorld.X + forwardWorld.Z * forwardWorld.Z)));
}
bool EditorCameraController::TrySetOrbit(math::Vector3d pivotWorld,
                                         math::Vector2 orbitDegrees,
                                         float32 distance) noexcept
{
    if (math::IsFinite(orbitDegrees) == false || math::IsFinite(pivotWorld) == false)
    {
        return false;
    }
    math::Quaternion yaw;
    math::Quaternion pitch;
    if (math::TryFromAxisAngle({0, 1, 0}, math::DegreesToRadians(orbitDegrees.X), yaw) != math::MathStatus::Success ||
        math::TryFromAxisAngle({1, 0, 0}, -math::DegreesToRadians(math::Clamp(orbitDegrees.Y, -89.9F, 89.9F)), pitch) !=
            math::MathStatus::Success)
    {
        return false;
    }
    camera::CameraSample candidate = mSample;
    if (math::TryNormalize(yaw * pitch, candidate.Pose.Orientation) != math::MathStatus::Success)
    {
        return false;
    }
    candidate.Pose.Position = pivotWorld - ConvertToWorld(math::Rotate(candidate.Pose.Orientation, {0, 0, -1})) *
                                               static_cast<float64>(distance);
    return tryPublish(candidate, distance);
}
bool EditorCameraController::TryRotate(float32 deltaX, float32 deltaY, bool isOrbit) noexcept
{
    if (math::IsFinite(deltaX) == false || math::IsFinite(deltaY) == false)
    {
        return false;
    }
    const math::Vector3d pivotWorld = GetPivotWorld();
    const float32 currentPitch = -math::DegreesToRadians(GetPitchDegrees());
    const float32 nextPitch =
        math::Clamp(currentPitch - deltaY * LOOK_RADIANS_PER_PIXEL, -MAXIMUM_PITCH_RADIANS, MAXIMUM_PITCH_RADIANS);
    math::Quaternion yaw;
    math::Quaternion pitch;
    if (math::TryFromAxisAngle({0, 1, 0}, -deltaX * LOOK_RADIANS_PER_PIXEL, yaw) != math::MathStatus::Success ||
        math::TryFromAxisAngle({1, 0, 0}, nextPitch - currentPitch, pitch) != math::MathStatus::Success)
    {
        return false;
    }
    camera::CameraSample candidate = mSample;
    if (math::TryNormalize(yaw * candidate.Pose.Orientation * pitch, candidate.Pose.Orientation) !=
        math::MathStatus::Success)
    {
        return false;
    }
    if (isOrbit == true)
    {
        candidate.Pose.Position = pivotWorld - ConvertToWorld(math::Rotate(candidate.Pose.Orientation, {0, 0, -1})) *
                                                   static_cast<float64>(mDistance);
    }
    return tryPublish(candidate, mDistance);
}
bool EditorCameraController::TryPan(float32 deltaX, float32 deltaY, float32 heightLogical, float32 aspect) noexcept
{
    if (math::IsFinite(deltaX) == false || math::IsFinite(deltaY) == false || math::IsFinite(heightLogical) == false ||
        heightLogical <= 0 || math::IsFinite(aspect) == false || aspect <= 0)
    {
        return false;
    }
    const camera::CameraLens& lens = mSample.Lens;
    const float32 halfFov = lens.VerticalFov * 0.5F;
    const float32 span = lens.Projection == camera::CameraProjection::OrthographicFinite
                             ? math::Max(lens.VerticalSpan, lens.MinimumVisibleWidth / aspect)
                             : 2 * mDistance * math::Sin(halfFov) / math::Cos(halfFov);
    return TryFly({-deltaX, deltaY, 0}, span / heightLogical);
}
bool EditorCameraController::TryFly(math::Vector3 movementLocal, float32 distance) noexcept
{
    if (math::IsFinite(movementLocal) == false || math::IsFinite(distance) == false)
    {
        return false;
    }
    camera::CameraSample candidate = mSample;
    candidate.Pose.Position =
        candidate.Pose.Position +
        ConvertToWorld(math::Rotate(candidate.Pose.Orientation, movementLocal)) * static_cast<float64>(distance);
    return tryPublish(candidate, mDistance);
}
bool EditorCameraController::TryDolly(float32 steps) noexcept
{
    if (math::IsFinite(steps) == false)
    {
        return false;
    }
    const float32 factor = math::Clamp(1 - steps * 0.15F, 0.1F, 10.0F);
    return TrySetDistance(math::Clamp(mDistance * factor, MINIMUM_DISTANCE, MAXIMUM_DISTANCE));
}
bool EditorCameraController::TrySetDistance(float32 distance) noexcept
{
    if (math::IsFinite(distance) == false || distance < MINIMUM_DISTANCE || distance > MAXIMUM_DISTANCE)
    {
        return false;
    }
    camera::CameraSample candidate = mSample;
    if (candidate.Lens.Projection == camera::CameraProjection::OrthographicFinite)
    {
        const float32 factor = distance / mDistance;
        candidate.Lens.VerticalSpan *= factor;
        candidate.Lens.MinimumVisibleWidth *= factor;
    }
    else
    {
        candidate.Pose.Position =
            GetPivotWorld() -
            ConvertToWorld(math::Rotate(candidate.Pose.Orientation, {0, 0, -1})) * static_cast<float64>(distance);
        candidate.Lens.NearPlane = math::Min(candidate.Lens.NearPlane, math::Max(0.0000001F, distance * 0.01F));
    }
    return tryPublish(candidate, distance);
}
bool EditorCameraController::IsOrthographic() const noexcept
{
    return mSample.Lens.Projection == camera::CameraProjection::OrthographicFinite;
}
bool EditorCameraController::tryPublishOrthographic(camera::CameraSample candidate,
                                                    const math::Aabb3* boundsWorldOrNull) noexcept
{
    const math::Vector3d pivotWorld = GetPivotWorld();
    candidate.Lens.VerticalSpan = GetVisibleHeight(mSample.Lens, mDistance);
    if (mSample.Lens.Projection != camera::CameraProjection::OrthographicFinite)
    {
        candidate.Lens.MinimumVisibleWidth = 0;
    }
    candidate.Lens.Projection = camera::CameraProjection::OrthographicFinite;
    candidate.Lens.NearPlane = 0;
    float32 distance = mDistance;
    float32 farPlane = math::Max(mSample.Lens.FarPlane, 2 * distance);
    if (boundsWorldOrNull != nullptr && math::IsFinite(*boundsWorldOrNull) == true &&
        math::IsEmpty(*boundsWorldOrNull) == false)
    {
        // Back off beyond the bounding sphere, measured from the pivot, so the eye is outside it.
        const math::Vector3 centerWorld = (boundsWorldOrNull->Min + boundsWorldOrNull->Max) * 0.5F;
        const float32 radius = math::Length(boundsWorldOrNull->Max - centerWorld);
        const float32 offset = static_cast<float32>(math::Distance(pivotWorld, ConvertToWorld(centerWorld)));
        distance = math::Max(distance, offset + radius * (1 + ORTHOGRAPHIC_CLEARANCE));
        farPlane = distance + offset + radius * (1 + ORTHOGRAPHIC_CLEARANCE);
    }
    candidate.Lens.FarPlane = math::Max(farPlane, MINIMUM_DISTANCE);
    candidate.Pose.Position = pivotWorld - ConvertToWorld(math::Rotate(candidate.Pose.Orientation, {0, 0, -1})) *
                                               static_cast<float64>(distance);
    return tryPublish(candidate, distance);
}
bool EditorCameraController::TrySetAxisView(EditorAxisView view, const math::Aabb3* boundsWorldOrNull) noexcept
{
    const math::Vector2 orbitDegrees = Describe(view).OrbitDegrees;
    math::Quaternion yaw;
    math::Quaternion pitch;
    if (math::TryFromAxisAngle({0, 1, 0}, math::DegreesToRadians(orbitDegrees.X), yaw) != math::MathStatus::Success ||
        math::TryFromAxisAngle({1, 0, 0}, -math::DegreesToRadians(orbitDegrees.Y), pitch) != math::MathStatus::Success)
    {
        return false;
    }
    camera::CameraSample candidate = mSample;
    if (math::TryNormalize(yaw * pitch, candidate.Pose.Orientation) != math::MathStatus::Success)
    {
        return false;
    }
    return tryPublishOrthographic(candidate, boundsWorldOrNull);
}
bool EditorCameraController::TrySetOrthographic(bool isOrthographic, const math::Aabb3* boundsWorldOrNull) noexcept
{
    if (isOrthographic == IsOrthographic())
    {
        return false;
    }
    if (isOrthographic == true)
    {
        return tryPublishOrthographic(mSample, boundsWorldOrNull);
    }
    camera::CameraSample candidate = mSample;
    const float32 halfFov = candidate.Lens.VerticalFov * 0.5F;
    // Place the eye where the perspective lens shows the orthographic span at the pivot.
    const float32 distance =
        math::Clamp(GetVisibleHeight(mSample.Lens, mDistance) * 0.5F * math::Cos(halfFov) / math::Sin(halfFov),
                    MINIMUM_DISTANCE,
                    MAXIMUM_DISTANCE);
    candidate.Lens.Projection = camera::CameraProjection::PerspectiveInfinite;
    candidate.Lens.NearPlane = math::Clamp(distance * PERSPECTIVE_NEAR_FRACTION,
                                           MINIMUM_PERSPECTIVE_NEAR_PLANE,
                                           MAXIMUM_PERSPECTIVE_NEAR_PLANE);
    candidate.Pose.Position = GetPivotWorld() - ConvertToWorld(math::Rotate(candidate.Pose.Orientation, {0, 0, -1})) *
                                                    static_cast<float64>(distance);
    return tryPublish(candidate, distance);
}
bool EditorCameraController::TrySetVerticalFov(float32 radians) noexcept
{
    if (math::IsFinite(radians) == false || radians < math::DegreesToRadians(MINIMUM_EDITOR_VERTICAL_FOV_DEGREES) ||
        radians > math::DegreesToRadians(MAXIMUM_EDITOR_VERTICAL_FOV_DEGREES))
    {
        return false;
    }
    camera::CameraSample candidate = mSample;
    candidate.Lens.VerticalFov = radians;
    return tryPublish(candidate, mDistance);
}
bool EditorCameraController::TryFrame(const math::Aabb3& boundsWorld, float32 aspect) noexcept
{
    if (math::IsFinite(boundsWorld) == false || math::IsEmpty(boundsWorld) == true || math::IsFinite(aspect) == false ||
        aspect <= 0)
    {
        return false;
    }
    const math::Vector3 centerWorld = (boundsWorld.Min + boundsWorld.Max) * 0.5F;
    const float32 radius = math::Max(MINIMUM_DISTANCE, math::Length(boundsWorld.Max - centerWorld));
    camera::CameraSample candidate = mSample;
    const float32 verticalHalfFov = candidate.Lens.Projection == camera::CameraProjection::OrthographicFinite
                                        ? 0.5F
                                        : candidate.Lens.VerticalFov * 0.5F;
    const float32 horizontalHalfFov = math::Atan2(math::Sin(verticalHalfFov) * aspect, math::Cos(verticalHalfFov));
    // Ten percent margin around the enclosing sphere; fit the limiting viewport axis.
    const float32 distance = radius * 1.1F / math::Sin(math::Min(verticalHalfFov, horizontalHalfFov));
    if (candidate.Lens.Projection == camera::CameraProjection::OrthographicFinite)
    {
        candidate.Lens.VerticalSpan = radius * 2.2F / math::Min(1.0F, aspect);
        candidate.Lens.MinimumVisibleWidth = 0;
        candidate.Lens.FarPlane = math::Max(candidate.Lens.FarPlane, distance + radius * 2);
    }
    else
    {
        candidate.Lens.Projection = camera::CameraProjection::PerspectiveInfinite;
    }
    candidate.Lens.NearPlane = math::Clamp(radius * 0.001F, 0.0000001F, 0.01F);
    candidate.Pose.Position =
        ConvertToWorld(centerWorld) -
        ConvertToWorld(math::Rotate(candidate.Pose.Orientation, {0, 0, -1})) * static_cast<float64>(distance);
    return tryPublish(candidate, distance);
}
} // namespace ludus::editor
