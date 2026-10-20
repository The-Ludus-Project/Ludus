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
} // namespace ludus::graphics::rhi::backend
