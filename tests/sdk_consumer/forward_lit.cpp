// Installed-SDK forward-lit consumer: prepare real scene inputs, render, then
// replace material values while retaining immutable mesh/texture resources.
#include "forward_lit.h"
#include "renderer_composite.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

namespace
{
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace graph = ludus::render_graph_packages::forward_lit;
constexpr uint32 WIDTH = 640;
constexpr uint32 HEIGHT = 360;
constexpr usize TRIANGLE_CORNERS = 3;
constexpr usize MAX_PREPARATION_ATTEMPTS = 1000000;
bool WaitForGraph(renderer::RenderGraph& inoutGraph, rhi::RasterStatus status) noexcept
{
    for (usize attempt = 0; attempt < MAX_PREPARATION_ATTEMPTS && status == rhi::RasterStatus::Pending; ++attempt)
    {
        status = inoutGraph.Poll();
    }
    return status == rhi::RasterStatus::Ready;
}
int Run() noexcept
{
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    if (rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, device, surface) !=
        rhi::DeviceStatus::Ready)
    {
        return 1;
    }
    renderer::RenderGraph rendererGraph;
    const renderer::RenderGraphPrograms programs = graph::MakePrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                       ludus::shaders::renderer_composite::Fragment());
    if (WaitForGraph(rendererGraph,
                     rendererGraph.Initialize(device, graph::MakePlan(), programs, nullptr, WIDTH, HEIGHT)) == false)
    {
        return 1;
    }
    const StaticArray<math::Vector3, TRIANGLE_CORNERS> positions{{{-.5F, -.5F, .5F}, {.5F, -.5F, .5F}, {0, .5F, .5F}}};
    const StaticArray<math::Vector3, TRIANGLE_CORNERS> normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    const StaticArray<uint32, TRIANGLE_CORNERS> indices{{0, 1, 2}};
    renderer::RenderGraphMesh mesh;
    mesh.Geometry.Positions = positions.GetData();
    mesh.Geometry.VertexCount = positions.GetSize();
    mesh.Geometry.Normals = normals.GetData();
    mesh.Geometry.Indices = indices.GetData();
    mesh.Geometry.IndexCount = indices.GetSize();
    const uint32 materialIndex = 0;
    mesh.MaterialBindings = {&materialIndex, 1};
    renderer::RenderGraphItem item;
    item.SourceId = 1;
    renderer::SceneShaderMaterial material;
    material.Emission = {1, 0, 0};
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&material, 1};
    renderer::ViewDescription camera;
    renderer::RenderGraphSceneRevisions revisions{1, 1, 1, 1, 1};
    renderer::RenderGraphReport report;
    const auto update = [&](uint64 revision) noexcept {
        return WaitForGraph(rendererGraph,
                            rendererGraph.Update({&mesh, 1},
                                                 {&item, 1},
                                                 {&camera, 1},
                                                 revision,
                                                 renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                 report,
                                                 &inputs,
                                                 0,
                                                 revisions));
    };
    if (update(1) == false)
    {
        return 1;
    }
    rhi::SubmissionToken completion;
    if (rendererGraph.Render(surface, WIDTH, HEIGHT, completion) != rhi::RasterStatus::Ready)
    {
        return 1;
    }
    for (usize attempt = 0;
         attempt < MAX_PREPARATION_ATTEMPTS && rhi::GetStatus(device, completion) == rhi::RasterStatus::Pending;
         ++attempt)
    {
        static_cast<void>(rhi::PollLifetime(device));
    }
    if (rhi::GetStatus(device, completion) != rhi::RasterStatus::Ready)
    {
        return 1;
    }
    material.Emission = {0, 1, 0};
    revisions.Materials = 2;
    if (update(2) == false)
    {
        return 1;
    }
    const renderer::RenderGraphSceneResourceReport resources = rendererGraph.GetReport().Resources;
    return resources.CreatedMeshes == 1 && resources.ReusedMeshes == 1 ? 0 : 1;
}
} // namespace
int main()
{
    const int result = Run();
    ludus::graphics::rhi::Shutdown();
    return result;
}
