#include "internal/render_graph_device_profile.h"

#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/ray.h>
#include <ludus/graphics/rhi/rhi.h>

namespace ludus::editor
{
namespace rhi = graphics::rhi;
namespace renderer = graphics::renderer;
namespace
{
QString DescribeBackend(rhi::Backend backend)
{
    switch (backend)
    {
        case rhi::Backend::Vulkan:
            return QStringLiteral("Vulkan");
        case rhi::Backend::Metal:
            return QStringLiteral("Metal");
        case rhi::Backend::Direct3D12:
            return QStringLiteral("Direct3D 12");
        case rhi::Backend::WebGPU:
            return QStringLiteral("WebGPU");
        case rhi::Backend::WebGL2:
            return QStringLiteral("WebGL 2");
    }
    return {};
}
} // namespace

RenderGraphDeviceProfile ProbeRenderGraphDeviceProfile(rhi::DeviceHandle device) noexcept
{
    RenderGraphDeviceProfile profile;
    rhi::DeviceInfo information;
    if (rhi::GetDeviceInfo(device, information) != rhi::DeviceStatus::Ready)
    {
        return profile;
    }
    renderer::RenderGraphRenderer selected = renderer::RenderGraphRenderer::Raster;
    if (renderer::SelectRenderGraphRenderer(device, renderer::RenderGraphPreference::Automatic, selected) !=
        rhi::RasterStatus::Ready)
    {
        return profile;
    }
    rhi::ComputeCapabilities compute;
    rhi::RayCapabilities ray;
    profile.Kind = RenderGraphDeviceKind::Native;
    profile.DeviceName = DescribeBackend(information.Startup.SelectedBackend);
    profile.HasCompute = rhi::GetComputeCapabilities(device, compute) == rhi::RasterStatus::Ready;
    profile.HasNativeRayQueries = selected == renderer::RenderGraphRenderer::HardwareRayTracing;
    profile.HasRayPipelines = profile.HasNativeRayQueries == true &&
                              rhi::GetRayCapabilities(device, ray) == rhi::RasterStatus::Ready &&
                              ray.RayPipelines == true;
    profile.HasDenoiser = renderer::IsDenoisingAvailable();
    return profile;
}
} // namespace ludus::editor
