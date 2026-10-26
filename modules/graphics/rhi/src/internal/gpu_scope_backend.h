#pragma once

#include <ludus/graphics/rhi/gpu_scope.h>

namespace ludus::graphics::rhi::internal
{
RasterStatus ValidateMeasurementDevice(DeviceHandle device) noexcept;
}
namespace ludus::graphics::rhi::backend
{
RasterStatus ArmPassTimings() noexcept;
GpuPassTimings PollPassTimings() noexcept;
RasterStatus BeginScope(const GpuScopeDescription& scope) noexcept;
RasterStatus EndScope() noexcept;
void ResetPassTimings() noexcept;
/// Vulkan only: whether the device admitted calibrated timestamps, so armed pass batches
/// may report CPU intervals. False without the extension, including borrowed devices that
/// did not declare it in NativeDeviceDescription::CalibratedTimestamps.
bool HasPassClockCalibration() noexcept;
} // namespace ludus::graphics::rhi::backend
