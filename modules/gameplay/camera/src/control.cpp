#include <ludus/gameplay/camera/control.hpp>

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::gameplay::camera
{
namespace math = foundation::math;
using foundation::float32;
using foundation::float64;

namespace
{
[[nodiscard]] bool IsUnitDeflection(float32 value) noexcept
{
    return math::IsFinite(value) == true && value >= -1.0F && value <= 1.0F;
}

/// Wrap to [-pi, pi). The floor rule is deterministic at exactly half a turn: +pi maps to -pi.
[[nodiscard]] float32 WrapYaw(float64 yaw) noexcept
{
    const float64 wrapped = yaw - (math::kTau * math::Floor((yaw + math::kPi) / math::kTau));
    const float32 narrowed = static_cast<float32>(wrapped);
    // Narrowing can round a value just below pi up to float32(pi), which lies above pi.
    return narrowed >= math::kPiF ? -math::kPiF : narrowed;
}
} // namespace

CameraStatus ValidatePitchLimits(const CameraPitchLimits& limits) noexcept
{
    if (math::IsFinite(limits.Minimum) == false || math::IsFinite(limits.Maximum) == false ||
        limits.Minimum > limits.Maximum || limits.Minimum <= -math::kHalfPiF || limits.Maximum >= math::kHalfPiF)
    {
        return CameraStatus::InvalidInput;
    }
    return CameraStatus::Success;
}

CameraStatus ValidateControlView(const CameraControlView& view) noexcept
{
    if (math::IsFinite(view.Yaw) == false || math::IsFinite(view.Pitch) == false || view.Pitch <= -math::kHalfPiF ||
        view.Pitch >= math::kHalfPiF)
    {
        return CameraStatus::InvalidInput;
    }
    return CameraStatus::Success;
}

CameraStatus TryAdvanceControlView(const CameraControlView& current,
                                   const CameraLookInput& input,
                                   const CameraLookSettings& settings,
                                   float64 tickSeconds,
                                   CameraControlView& outView) noexcept
{
    if (ValidateControlView(current) != CameraStatus::Success ||
        ValidatePitchLimits(settings.Pitch) != CameraStatus::Success ||
        math::IsFinite(settings.PointerRadiansPerUnit) == false || settings.PointerRadiansPerUnit < 0 ||
        math::IsFinite(settings.StickRadiansPerSecond) == false || settings.StickRadiansPerSecond < 0 ||
        math::IsFinite(input.PointerDelta) == false || IsUnitDeflection(input.StickDeflection.X) == false ||
        IsUnitDeflection(input.StickDeflection.Y) == false || math::IsFinite(tickSeconds) == false || tickSeconds < 0)
    {
        return CameraStatus::InvalidInput;
    }
    // Pointer motion is a displacement applied once; stick deflection is an angular rate over the tick.
    const float64 pointerScale = static_cast<float64>(settings.PointerRadiansPerUnit);
    const float64 stickAngle = static_cast<float64>(settings.StickRadiansPerSecond) * tickSeconds;
    const float64 yaw = static_cast<float64>(current.Yaw) -
                        (static_cast<float64>(input.PointerDelta.X) * pointerScale) -
                        (static_cast<float64>(input.StickDeflection.X) * stickAngle);
    const float64 pitch = static_cast<float64>(current.Pitch) -
                          (static_cast<float64>(input.PointerDelta.Y) * pointerScale) +
                          (static_cast<float64>(input.StickDeflection.Y) * stickAngle);
    if (math::IsFinite(yaw) == false || math::IsFinite(pitch) == false)
    {
        return CameraStatus::OutOfRange;
    }
    outView =
    {
        .Yaw = WrapYaw(yaw),
        .Pitch = static_cast<float32>(math::Clamp(pitch,
                                                  static_cast<float64>(settings.Pitch.Minimum),
                                                  static_cast<float64>(settings.Pitch.Maximum))),
    };
    return CameraStatus::Success;
}

CameraStatus TryComputeControlOrientation(const CameraControlView& view, math::Quaternion& outOrientation) noexcept
{
    if (ValidateControlView(view) != CameraStatus::Success)
    {
        return CameraStatus::InvalidInput;
    }
    math::Quaternion yaw;
    math::Quaternion pitch;
    math::Quaternion orientation;
    if (math::TryFromAxisAngle({0, 1, 0}, view.Yaw, yaw) != math::MathStatus::Success ||
        math::TryFromAxisAngle({1, 0, 0}, view.Pitch, pitch) != math::MathStatus::Success ||
        math::TryNormalize(yaw * pitch, orientation) != math::MathStatus::Success)
    {
        return CameraStatus::OutOfRange;
    }
    outOrientation = orientation;
    return CameraStatus::Success;
}

CameraStatus TryComputeMovementForward(const CameraControlView& view, math::Vector3& outForward) noexcept
{
    if (ValidateControlView(view) != CameraStatus::Success)
    {
        return CameraStatus::InvalidInput;
    }
    const float64 yaw = static_cast<float64>(view.Yaw);
    outForward = {static_cast<float32>(-math::Sin(yaw)), 0, static_cast<float32>(-math::Cos(yaw))};
    return CameraStatus::Success;
}
} // namespace ludus::gameplay::camera
