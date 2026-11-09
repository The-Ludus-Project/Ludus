#pragma once

#include "internal/render_graph_availability.h"

#include <ludus/graphics/rhi/device.h>

namespace ludus::editor
{
/// Capabilities the render-graph preview would use on this live viewport device: the renderer's own
/// native-ray selection, ray-pipeline and compute queries, and whether the optional denoiser is linked.
/// A null, lost or not-yet-ready device returns an Unknown profile, which makes no device claim.
/// Owner thread, between frames; no allocation or GPU work.
[[nodiscard]] RenderGraphDeviceProfile ProbeRenderGraphDeviceProfile(graphics::rhi::DeviceHandle device) noexcept;
} // namespace ludus::editor
