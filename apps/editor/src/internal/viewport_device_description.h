#pragma once

#include <ludus/graphics/rhi/device.h>

namespace ludus::editor
{
/// Describe the shared/native and offscreen viewport's device admission policy.
/// Byte quotas are ceilings, allocated only on demand; native device limits still apply.
[[nodiscard]] graphics::rhi::DeviceDescription MakeViewportDeviceDescription() noexcept;
} // namespace ludus::editor
