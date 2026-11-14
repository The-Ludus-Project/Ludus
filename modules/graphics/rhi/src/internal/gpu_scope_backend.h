#pragma once

#include "gpu_pass_timing_buffer.h"
#include <ludus/graphics/rhi/gpu_scope.h>

namespace ludus::graphics::rhi::internal
{
RasterStatus ValidateMeasurementDevice(DeviceHandle device) noexcept;
}
namespace ludus::graphics::rhi::backend
{
RasterStatus ArmPassTimings() noexcept;
GpuPassTimings PollPassTimings() noexcept;
/// Push a native region and the scope stack entry. CapacityExceeded when MAX_GPU_SCOPE_DEPTH
/// scopes are open; InvalidState outside a recording frame.
RasterStatus BeginScope(const GpuScopeDescription& scope) noexcept;
/// Pop the innermost scope and its native region; InvalidState when none is open.
RasterStatus EndScope() noexcept;
/// Current scope stack position, used to keep lexical scopes from closing newer ones.
internal::GpuScopePosition GetScopePosition() noexcept;
/// Untimed native label for one logical pass inside the current physical raster pass, for
/// example a member of a merged render pass. Labels nest inside the physical pass only and
/// are closed before the pass ends; they never enter the scope stack or timing results.
void BeginPassLabel(const char* name) noexcept;
/// Close the innermost open pass label; ignored when none is open.
void EndPassLabel() noexcept;
void ResetPassTimings() noexcept;
/// Vulkan only: whether the device admitted calibrated timestamps, so armed pass batches
/// may report CPU intervals. False without the extension, including borrowed devices that
/// did not declare it in NativeDeviceDescription::CalibratedTimestamps.
bool HasPassClockCalibration() noexcept;
} // namespace ludus::graphics::rhi::backend
