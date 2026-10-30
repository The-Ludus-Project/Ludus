#include "internal/viewport_device_description.h"
#include "internal/preview_lighting.h"

#include <ludus/foundation/base/types.h>
#include <ludus/graphics/renderer/render_graph.hpp>

namespace ludus::editor
{
using namespace foundation;
namespace rhi = graphics::rhi;
namespace rr = graphics::renderer;
namespace
{
constexpr uint32 VIEWPORT_RESOURCES_PER_KIND = 4096;
constexpr uint32 VIEWPORT_BINDING_DESCRIPTORS = 32768;
constexpr uint32 VIEWPORT_DRAWS_PER_FRAME = 32768;
constexpr uint32 VIEWPORT_DRAWS_PER_PLAN = 1024;
constexpr usize VIEWPORT_HOST_BYTES = usize{512} * 1024 * 1024;
constexpr uint32 VIEWPORT_MIN_BINDINGS = 11;
constexpr usize VIEWPORT_MIN_UNIFORM_RANGE = 16384;
constexpr uint32 VIEWPORT_MIN_VERTEX_ATTRIBUTES = 16;
} // namespace
// Shared by the Qt-shared and offscreen devices: authored lighting needs the
// same budgets whichever way the viewport presents.
rhi::DeviceDescription MakeViewportDeviceDescription() noexcept
{
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Preferred.RayQueries = true;
    // Two retained GPU generations, 80 caster views, and direct-lighting material versions.
    // These are explicit admission budgets, not allocations or inferred native limits.
    description.Storage.ResourcesPerKind = VIEWPORT_RESOURCES_PER_KIND;
    description.Storage.BindingDescriptors = VIEWPORT_BINDING_DESCRIPTORS;
    description.Storage.DrawsPerFrame = VIEWPORT_DRAWS_PER_FRAME;
    description.Storage.DrawsPerPlan = VIEWPORT_DRAWS_PER_PLAN;
    description.Storage.ResourcesPerGraph = rr::VISIBILITY_GRAPH_RESOURCE_CAPACITY;
    // Native ray UV metadata and temporary opacity tables overlap across scene replacement.
    description.Storage.HostBytes = VIEWPORT_HOST_BYTES;
    // Ray previews retain the previous scene during replacement. Raster snapshots,
    // material textures and graph targets have a separate device-wide allowance.
    constexpr uint64 RASTER_RESOURCE_BYTES = uint64{1} * 1024 * 1024 * 1024;
    description.Storage.ResourceBytes = uint64{rr::RENDER_GRAPH_RAY_GPU_BYTES} + RASTER_RESOURCE_BYTES;
    description.RequiredProfile.Raster.MinBindings = VIEWPORT_MIN_BINDINGS;
    description.RequiredProfile.Raster.MinUniformRange = VIEWPORT_MIN_UNIFORM_RANGE;
    description.RequiredProfile.Raster.MinVertexAttributes = VIEWPORT_MIN_VERTEX_ATTRIBUTES;
    description.RequiredProfile.Raster.MinTextureDimension2D = preview_lighting::SHADOW_ATLAS_PIXELS;
    // Optional frame timestamps for the viewport statistics overlay; backends without
    // timestamp queries report Unavailable and the overlay shows "GPU: n/a".
    description.EnableGpuTiming = true;
    return description;
}
} // namespace ludus::editor
