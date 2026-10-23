#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/gameplay/camera/evaluation.hpp>

namespace ludus::editor
{
/// Orthographic editor view along a world axis, named for the side of the scene it shows.
/// The world is Y-up: Top looks down -Y, Front looks along -Z, Right looks along -X.
enum class EditorAxisView : foundation::uint8
{
    Top,
    Bottom,
    Front,
    Back,
    Right,
    Left,
};
/// Number of EditorAxisView values, for enumeration and validation.
inline constexpr foundation::usize EDITOR_AXIS_VIEW_COUNT = static_cast<foundation::usize>(EditorAxisView::Left) + 1;
/// Lowest admitted perspective vertical field of view for editor navigation, in degrees.
inline constexpr foundation::float32 MINIMUM_EDITOR_VERTICAL_FOV_DEGREES = 10;
/// Highest admitted perspective vertical field of view for editor navigation, in degrees.
inline constexpr foundation::float32 MAXIMUM_EDITOR_VERTICAL_FOV_DEGREES = 120;
/// Find the axis view whose direction sample looks along, within a fraction of a degree.
/// Only orthographic samples match; failure preserves outView.
[[nodiscard]] bool TryGetEditorAxisView(const gameplay::camera::CameraSample& sample, EditorAxisView& outView) noexcept;
/// Menu label for an axis view, such as "Top".
[[nodiscard]] const char* GetEditorAxisViewLabel(EditorAxisView view) noexcept;
/// Private editor navigation state. Inputs are logical pixels, radians or world units;
/// failures preserve the accepted C0 publication. No World, Qt or GPU ownership.
class EditorCameraController final
{
public:
    EditorCameraController() noexcept;
    [[nodiscard]] LUDUS_INLINE const gameplay::camera::CameraSample& GetSample() const noexcept
    {
        return mSample;
    }
    [[nodiscard]] LUDUS_INLINE foundation::float32 GetDistance() const noexcept
    {
        return mDistance;
    }
    [[nodiscard]] foundation::math::Vector3d GetPivotWorld() const noexcept;
    [[nodiscard]] foundation::float32 GetYawDegrees() const noexcept;
    [[nodiscard]] foundation::float32 GetPitchDegrees() const noexcept;
    [[nodiscard]] bool TryAdopt(const gameplay::camera::CameraSample& sample, foundation::float32 distance) noexcept;
    /// Set a world pivot, yaw/pitch pair in degrees, and absolute focus distance in world units.
    [[nodiscard]] bool TrySetOrbit(foundation::math::Vector3d pivotWorld,
                                   foundation::math::Vector2 orbitDegrees,
                                   foundation::float32 distance) noexcept;
    [[nodiscard]] bool TryRotate(foundation::float32 deltaX, foundation::float32 deltaY, bool isOrbit) noexcept;
    [[nodiscard]] bool TryPan(foundation::float32 deltaX,
                              foundation::float32 deltaY,
                              foundation::float32 heightLogical,
                              foundation::float32 aspect = 1) noexcept;
    [[nodiscard]] bool TrySetDistance(foundation::float32 distance) noexcept;
    [[nodiscard]] bool TryDolly(foundation::float32 steps) noexcept;
    [[nodiscard]] bool TryFly(foundation::math::Vector3 movementLocal, foundation::float32 distance) noexcept;
    [[nodiscard]] bool TryFrame(const foundation::math::Aabb3& boundsWorld, foundation::float32 aspect) noexcept;
    /// Look along a world axis through an orthographic lens, keeping the pivot and the visible
    /// height at the pivot. With boundsWorldOrNull, the eye backs off outside the bounds and the
    /// far plane covers them, so nothing in the scene is clipped. Failure preserves the camera.
    [[nodiscard]] bool TrySetAxisView(EditorAxisView view, const foundation::math::Aabb3* boundsWorldOrNull) noexcept;
    /// Switch between perspective and orthographic projection, keeping the orientation, pivot
    /// and visible height at the pivot. Perspective uses the lens's vertical field of view.
    /// boundsWorldOrNull as for TrySetAxisView. Failure, or no change, preserves the camera.
    [[nodiscard]] bool TrySetOrthographic(bool isOrthographic,
                                          const foundation::math::Aabb3* boundsWorldOrNull) noexcept;
    /// Set the perspective vertical field of view in radians, within the editor's admitted
    /// range; the pose and pivot are unchanged. An orthographic lens keeps it for the next
    /// switch to perspective. Failure preserves the camera.
    [[nodiscard]] bool TrySetVerticalFov(foundation::float32 radians) noexcept;
    [[nodiscard]] bool IsOrthographic() const noexcept;

private:
    [[nodiscard]] bool tryPublish(const gameplay::camera::CameraSample& sample, foundation::float32 distance) noexcept;
    /// Publish candidate as an orthographic view of the current pivot (see TrySetAxisView).
    [[nodiscard]] bool tryPublishOrthographic(gameplay::camera::CameraSample candidate,
                                              const foundation::math::Aabb3* boundsWorldOrNull) noexcept;
    gameplay::camera::CameraSample mSample;
    foundation::float32 mDistance = 3;
};
} // namespace ludus::editor
