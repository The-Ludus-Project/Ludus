#pragma once

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/measurement.h>

namespace ludus::qa::normal_minification
{
using namespace foundation;
namespace rr = graphics::renderer;
inline constexpr uint32 EXTENT = 64;
inline constexpr uint32 DEVICE_RESOURCES = 256;
inline constexpr float32 NEAR_METRES = 0.01F;
inline constexpr float32 FAR_METRES = 100;
struct Frame final
{
    math::Vector3 EyeWorld;
    math::Vector3 TargetWorld;
    math::Vector3 TowardsLightWorld;
};
static_assert(sizeof(Frame) == 9 * sizeof(float32));

LUDUS_INLINE bool MakeView(const Frame& frame, float32 verticalFovRadians, rr::ViewDescription& outView) noexcept
{
    outView.CameraPosition = frame.EyeWorld;
    math::Matrix4 view;
    math::Matrix4 projection;
    if (math::TryLookAt(frame.EyeWorld, frame.TargetWorld, {0, 1, 0}, view) != math::MathStatus::Success ||
        math::TryPerspectiveReverseZ(verticalFovRadians, 1, NEAR_METRES, FAR_METRES, projection) !=
            math::MathStatus::Success)
    {
        return false;
    }
    outView.WorldToClip = projection * view;
    for (math::Vector4& column : outView.WorldToClip.Columns)
    {
        column.Z = column.W - column.Z;
    }
    outView.Depth = rr::DepthConvention::Conventional;
    return true;
}
/// Device-clock frame duration, or -1 when the backend did not return a timestamp.
LUDUS_INLINE float64 GetFrameNanoseconds(graphics::rhi::DeviceHandle device, bool hasTimer) noexcept
{
    graphics::rhi::GpuTiming timing;
    if (hasTimer == true && graphics::rhi::GetGpuTiming(device, timing) == graphics::rhi::RasterStatus::Ready &&
        timing.State == graphics::rhi::GpuTimingState::Available)
    {
        return timing.DurationNanoseconds;
    }
    return -1;
}
} // namespace ludus::qa::normal_minification
