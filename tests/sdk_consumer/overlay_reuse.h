#pragma once

#include <ludus/graphics/renderer/overlays.hpp>
#include <ludus/graphics/rhi/device.h>

namespace ludus::qa
{
/// Advance the installed overlay fixture's offscreen pixel checks on its existing device.
/// Pending yields to the caller so browser resource and readback callbacks can finish.
/// The fixture owns its resources until the caller resets the renderer and shuts down RHI.
[[nodiscard]] graphics::rhi::RasterStatus
AdvanceOverlayReuse(graphics::rhi::DeviceHandle device,
                    graphics::rhi::SurfaceHandle surface,
                    graphics::renderer::OverlayRenderer& inoutOverlays) noexcept;
} // namespace ludus::qa
