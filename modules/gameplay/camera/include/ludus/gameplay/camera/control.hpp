#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/gameplay/camera/camera.hpp>

namespace ludus::gameplay::camera
{
/// Default pitch bound in radians (about 80 degrees), leaving the vertical heading well conditioned.
inline constexpr foundation::float32 DEFAULT_CAMERA_PITCH_LIMIT = 1.4F;
/// Default pointer sensitivity in radians per pointer unit (pixel or device count).
inline constexpr foundation::float32 DEFAULT_POINTER_RADIANS_PER_UNIT = 0.0025F;
/// Default stick look speed in radians per second at full deflection.
inline constexpr foundation::float32 DEFAULT_STICK_RADIANS_PER_SECOND = 3.0F;

/// Inclusive pitch bounds in radians; both lie strictly inside (-pi/2, pi/2) with Minimum <= Maximum.
struct CameraPitchLimits final
{
    /// Lowest pitch (looking down is negative).
    foundation::float32 Minimum = -DEFAULT_CAMERA_PITCH_LIMIT;
    /// Highest pitch (looking up is positive).
    foundation::float32 Maximum = DEFAULT_CAMERA_PITCH_LIMIT;
    /// Exact memberwise comparison.
    [[nodiscard]] friend constexpr bool operator==(const CameraPitchLimits&, const CameraPitchLimits&) = default;
};

/// Game-owned authoritative aim. Fixed ticks advance it from input; shooting and movement read it, and cameras
/// derive their presentation orientation from it without writing back.
///
/// Orientation is a rotation by Yaw about world +Y followed by Pitch about the yawed local +X. Zero looks along
/// world -Z; positive yaw turns left (counter-clockwise seen from above) and positive pitch looks up.
struct CameraControlView final
{
    /// Finite heading in radians, kept in [-pi, pi) by TryAdvanceControlView.
    foundation::float32 Yaw = 0.0F;
    /// Finite elevation in radians, strictly inside (-pi/2, pi/2).
    foundation::float32 Pitch = 0.0F;
};

/// One tick of look input. Pointer motion is displacement; stick deflection is a rate.
struct CameraLookInput final
{
    /// Pointer displacement accumulated since the last consumed tick, in screen units (+X right, +Y down).
    /// Apply it to exactly one tick; it is never multiplied by the tick duration.
    foundation::math::Vector2 PointerDelta{};
    /// Normalized stick deflection in [-1, 1] (+X right, +Y up), integrated over the tick duration.
    foundation::math::Vector2 StickDeflection{};
};

/// Game-selected look response.
struct CameraLookSettings final
{
    /// Finite nonnegative radians per pointer unit.
    foundation::float32 PointerRadiansPerUnit = DEFAULT_POINTER_RADIANS_PER_UNIT;
    /// Finite nonnegative radians per second at full stick deflection.
    foundation::float32 StickRadiansPerSecond = DEFAULT_STICK_RADIANS_PER_SECOND;
    /// Pitch bounds applied after integrating input.
    CameraPitchLimits Pitch{};
};

/// Check pitch bounds: finite, ordered and strictly inside (-pi/2, pi/2). No allocation.
[[nodiscard]] CameraStatus ValidatePitchLimits(const CameraPitchLimits& limits) noexcept;

/// Check a control view: finite yaw and a finite pitch strictly inside (-pi/2, pi/2). No allocation.
[[nodiscard]] CameraStatus ValidateControlView(const CameraControlView& view) noexcept;

/// Advance authoritative aim by one tick of input.
/// @param current Accepted view before this tick.
/// @param input Pointer displacement applied once and stick deflection integrated over tickSeconds.
/// @param settings Sensitivities and pitch bounds.
/// @param tickSeconds Finite nonnegative tick duration; zero ignores the stick but still applies pointer motion.
/// @param outView Replaced only on Success. Yaw is wrapped to [-pi, pi); exactly half a turn maps to -pi.
/// Pitch is clamped to the settings' bounds.
/// @return InvalidInput for nonfinite or out-of-range values; failure preserves outView. No allocation.
/// @note The caller consumes each pointer delta once; catch-up ticks must not replay it.
[[nodiscard]] CameraStatus TryAdvanceControlView(const CameraControlView& current,
                                                 const CameraLookInput& input,
                                                 const CameraLookSettings& settings,
                                                 foundation::float64 tickSeconds,
                                                 CameraControlView& outView) noexcept;

/// Unit local-to-world orientation of a control view (camera forward is local -Z).
/// @param view Control view to convert.
/// @param outOrientation Replaced only on Success.
/// @return InvalidInput for an invalid view; failure preserves output. No allocation.
[[nodiscard]] CameraStatus TryComputeControlOrientation(const CameraControlView& view,
                                                        foundation::math::Quaternion& outOrientation) noexcept;

/// Unit movement heading: the control forward projected onto the world XZ plane, derived from yaw alone so a
/// near-vertical pitch never normalizes a zero vector.
/// @param view Control view whose yaw defines the heading.
/// @param outForward Replaced only on Success; Y is zero.
/// @return InvalidInput for an invalid view; failure preserves output. No allocation.
[[nodiscard]] CameraStatus TryComputeMovementForward(const CameraControlView& view,
                                                     foundation::math::Vector3& outForward) noexcept;
} // namespace ludus::gameplay::camera
