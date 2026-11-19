#include "fixture.h"
#include "patch_seam.h"
#include "renderer_composite.h"

#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/lifetime.h>

#include <cstdio>
#include <fstream>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <string>
#include <vector>

namespace
{
using namespace ludus::qa::patch;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace graph = ludus::render_graph_packages::patch_seam;

template <typename Poll>
bool Wait(Poll poll)
{
    const uint64 start = time::NowTicks();
    rhi::RasterStatus status;
    do
    {
        status = poll();
        if (status == rhi::RasterStatus::Ready)
        {
            return true;
        }
        if (status != rhi::RasterStatus::Pending && status != rhi::RasterStatus::NotReady)
        {
            break;
        }
        threading::SleepFor(time::Microseconds(100));
    } while (time::NowTicks() - start < time::Seconds(30).Nanoseconds);
    std::fprintf(stderr, "GPU operation stopped with status %u\n", static_cast<unsigned>(status));
    return false;
}

bool Capture(rhi::DeviceHandle device,
             rhi::SurfaceHandle surface,
             uint32 extent,
             const Camera& camera,
             const geom::SurfaceMeshView& mesh,
             const std::filesystem::path& path)
{
    renderer::RenderGraph raster;
    const renderer::RenderGraphPlan plan = graph::MakePlan();
    const renderer::RenderGraphPrograms programs = graph::MakePrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                       ludus::shaders::renderer_composite::Fragment());
    rhi::RasterStatus status = raster.Initialize(device, plan, programs, nullptr, extent, extent);
    const auto pollGraph = [&]() {
        if (status == rhi::RasterStatus::Pending)
        {
            status = raster.Poll();
        }
        return status;
    };
    if (Wait(pollGraph) == false)
    {
        return false;
    }
    std::vector<math::Vector3> positions;
    std::vector<math::Vector3> normals;
    for (const geom::SurfaceMeshVertex& vertex : mesh.Vertices)
    {
        positions.push_back(vertex.Position);
        normals.push_back(vertex.Normal);
    }
    const uint32 materialIndex = 0;
    renderer::RenderGraphMesh model;
    model.Geometry.Positions = positions.data();
    model.Geometry.Normals = normals.data();
    model.Geometry.VertexCount = positions.size();
    model.Geometry.Indices = mesh.Indices.begin();
    model.Geometry.IndexCount = mesh.Indices.GetSize();
    model.MaterialBindings = {&materialIndex, 1};
    renderer::RenderGraphItem item;
    item.SourceId = 1;
    item.MeshToWorld.Columns[0].X = camera.Scale.X;
    item.MeshToWorld.Columns[1].Y = camera.Scale.Y;
    item.MeshToWorld.Columns[2].Z = camera.Scale.Z;
    renderer::SceneShaderMaterial material;
    material.Emission = {1, 1, 1};
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&material, 1};
    const renderer::ViewDescription view = MakeView(camera);
    renderer::RenderGraphReport report;
    status = raster.Update({&model, 1},
                           {&item, 1},
                           {&view, 1},
                           1,
                           renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                           report,
                           &inputs);
    if (Wait(pollGraph) == false)
    {
        return false;
    }
    rhi::SubmissionToken completion;
    if (raster.Render(surface, extent, extent, completion) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    uint32 resource = 0;
    while (resource < plan.Resources.GetSize() && plan.Resources[resource].IsExported == 0)
    {
        ++resource;
    }
    if (Wait([&]() {
            static_cast<void>(rhi::PollLifetime(device));
            return raster.GetExport(resource, texture, aspect);
        }) == false)
    {
        return false;
    }
    rhi::ReadbackTicket readback;
    const usize pitch = extent * 4;
    status = rhi::RequestTextureReadback(device, texture, { .Width = extent, .Height = extent }, pitch, readback);
    if (status != rhi::RasterStatus::Ready && status != rhi::RasterStatus::Pending)
    {
        return false;
    }
    std::vector<uint8> pixels(pitch * extent);
    if (Wait([&]() {
            static_cast<void>(rhi::PollLifetime(device));
            return rhi::CopyReadback(device, readback, pixels.data(), pixels.size());
        }) == false ||
        rhi::Release(device, readback) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    output.close();
    return output.good();
}

bool Run(const std::filesystem::path& root, bool shouldDraw)
{
    for (const Case& fixture : CASES)
    {
        for (usize level = 0; level < std::size(ERRORS); ++level)
        {
            const std::filesystem::path directory = root / (std::string(fixture.Name) + "-" + std::to_string(level));
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            geom::SurfaceMesh mesh;
            if (error.value() != 0 || BuildFixture(fixture, ERRORS[level], mesh, directory) == false)
            {
                return false;
            }
            if (shouldDraw == false)
            {
                continue;
            }
            for (const uint32 extent : EXTENTS)
            {
                rhi::DeviceHandle device;
                rhi::SurfaceHandle surface;
                rhi::DeviceDescription description;
                description.Required.PortableRaster = true;
                if (rhi::CreateDevice({}, { .Width = extent, .Height = extent }, description, device, surface) !=
                    rhi::DeviceStatus::Ready)
                {
                    std::fprintf(stderr, "A native portable raster adapter is required.\n");
                    return false;
                }
                for (usize i = 0; i < GetCameraCount(fixture.Surface); ++i)
                {
                    const std::filesystem::path path =
                        directory / (std::to_string(extent) + "-" + std::to_string(i) + ".rgba");
                    if (Capture(device, surface, extent, CAMERAS[i], mesh.GetView(), path) == false)
                    {
                        std::fprintf(stderr, "capture failed: %s\n", path.c_str());
                        return false;
                    }
                }
                if (rhi::DestroyDevice(device) != rhi::DeviceStatus::Ready)
                {
                    return false;
                }
            }
        }
    }
    return true;
}
} // namespace
int main(int count, char** arguments)
{
    if (count != 3 || (std::string_view(arguments[1]) != "geometry" && std::string_view(arguments[1]) != "raster"))
    {
        return 2;
    }
    const bool isOk = Run(arguments[2], std::string_view(arguments[1]) == "raster");
    ludus::graphics::rhi::Shutdown();
    return isOk == true ? 0 : 1;
}
