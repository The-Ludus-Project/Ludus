// Consume the assembled World, camera and linear default deferred package emitted by the fixture.
#include "renderer_composite.h"

#include <ludus/content/render_graph_format/runtime_package.h>
#include <ludus/content/scene_format/limits.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/measurement.h>
#include <ludus/runtime/resources/scene.h>

#include <charconv>
#include <fstream>
#include <iostream>
#include <string_view>

namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace res = ludus::resources;
using Status = rhi::RasterStatus;

constexpr time::Duration GPU_TIMEOUT = time::Seconds(30);
constexpr usize RGBA8_BYTES_PER_PIXEL = 4;
constexpr uint32 ONE_SIDED_PANEL_FACE_COUNT = 5;
constexpr uint32 MAX_CAPTURE_EXTENT = 512;
constexpr uint32 VIEWPORT_MIN_BINDINGS = 11;
constexpr int ARGUMENT_COUNT = 3;

template <typename Poll>
bool WaitForReady(Status status, Poll poll)
{
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < GPU_TIMEOUT.Nanoseconds)
    {
        threading::YieldThread();
        status = poll();
    }
    if (status != Status::Ready)
    {
        std::cerr << "GPU operation returned " << static_cast<int>(status) << '\n';
    }
    return status == Status::Ready;
}

bool ReadBytes(const char* path, usize limit, Array<uint8>& outBytes)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const std::streamoff length = stream.tellg();
    if (length <= 0 || static_cast<uint64>(length) > limit || outBytes.TryResize(static_cast<usize>(length)) == false)
    {
        return false;
    }
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(outBytes.GetData()), length);
    return stream.good();
}

bool Draw(rhi::DeviceHandle device, rhi::SurfaceHandle surface, uint32 extent, const rr::RenderGraphPackage& package)
{
    Array<uint8> bytes;
    res::SceneDefinition scene;
    if (ReadBytes("scene.luscn", ludus::content::scene_format::MAX_ASSEMBLED_BUNDLE_BYTES, bytes) == false ||
        res::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) != res::Status::Ok)
    {
        std::cerr << "Invalid cooked scene\n";
        return false;
    }
    Array<rr::RenderGraphMesh> meshes;
    for (const res::SceneMesh& source : scene.Meshes)
    {
        rr::RenderGraphMesh mesh;
        mesh.Geometry = {source.Positions.GetData(),
                         source.Positions.GetSize(),
                         source.Indices.GetData(),
                         source.Indices.GetSize()};
        mesh.Geometry.Normals = source.Normals.GetData();
        mesh.Geometry.Texcoords = source.Texcoords.GetData();
        mesh.Geometry.Tangents = {source.Tangents.GetData(), source.Tangents.GetSize()};
        meshes.Add(mesh);
    }
    Array<rr::SceneShaderMaterial> materials;
    for (const res::SceneMaterial& source : scene.Materials)
    {
        rr::SceneShaderMaterial material;
        material.BaseColor = source.Reflectance;
        material.Scattering = source.Scattering;
        material.Metallic = source.Metallic;
        material.Roughness = source.Roughness;
        material.NormalScale = source.NormalScale;
        material.Emission = source.Emission;
        material.IsEmissionTwoSided = source.IsEmissionTwoSided;
        if (source.Texture < scene.Textures.GetSize())
        {
            const res::SceneTexture& texture = scene.Textures[source.Texture];
            material.ColorTexture = {texture.Pixels.GetData(),
                                     texture.Pixels.GetSize(),
                                     usize{texture.Width} * RGBA8_BYTES_PER_PIXEL,
                                     texture.Width,
                                     texture.Height,
                                     rhi::RasterFormat::Rgba8Srgb};
            material.Sampler.Filter = rhi::RasterFilter::Linear;
        }
        materials.Add(material);
    }
    Array<rr::RenderGraphItem> items;
    for (const res::SceneObject& object : scene.Objects)
    {
        rr::RenderGraphItem item;
        item.MeshIndex = object.Mesh;
        item.MeshToWorld = object.Transform;
        item.MaterialOverrides = {&object.Material, 1};
        item.SourceId = items.GetSize() + 1;
        items.Add(item);
    }
    rr::ViewDescription view;
    view.Depth = rr::DepthConvention::ReverseZ;
    std::ifstream camera("camera.txt");
    for (math::Vector4& column : view.WorldToClip.Columns)
    {
        camera >> column.X >> column.Y >> column.Z >> column.W;
    }
    camera >> view.CameraPosition.X >> view.CameraPosition.Y >> view.CameraPosition.Z;
    if (camera.fail() == true)
    {
        return false;
    }
    rr::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    rr::RenderGraph graph;
    const rr::RenderGraphPrograms programs = package.GetPrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                 ludus::shaders::renderer_composite::Fragment());
    const auto poll = [&graph]() { return graph.Poll(); };
    if (programs.HasClusteredDeferred == false ||
        WaitForReady(graph.Initialize(device, package.GetPlan(), programs, nullptr, extent, extent), poll) == false)
    {
        return false;
    }
    rr::RenderGraphReport report;
    if (WaitForReady(graph.Update({meshes.GetData(), meshes.GetSize()},
                                  {items.GetData(), items.GetSize()},
                                  {&view, 1},
                                  1,
                                  rr::RenderGraphCoveragePolicy::RejectIncomplete,
                                  report,
                                  &inputs),
                     poll) == false)
    {
        return false;
    }
    const uint64 start = time::NowTicks();
    do
    {
        rhi::SubmissionToken completion;
        if (graph.Render(surface, extent, extent, completion) != Status::Ready ||
            WaitForReady(Status::Pending, [&] {
                static_cast<void>(rhi::PollLifetime(device));
                return rhi::GetStatus(device, completion);
            }) == false)
        {
            return false;
        }
        report = graph.GetReport();
    } while (report.PendingMeshLightShadowFaces > 0 && time::NowTicks() - start < GPU_TIMEOUT.Nanoseconds);
    std::cout << report.Resources.MeshLightShadowCasters << " casters, " << report.Resources.MeshLightShadowFaces
              << " faces, " << report.PendingMeshLightShadowFaces << " pending\n";
    if (report.PendingMeshLightShadowFaces != 0 || report.Resources.MeshLightShadowCasters != 1 ||
        report.Resources.MeshLightShadowFaces != ONE_SIDED_PANEL_FACE_COUNT)
    {
        return false;
    }
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    if (graph.GetExport(0, texture, aspect) != Status::Ready)
    {
        return false;
    }
    rhi::ReadbackTicket ticket;
    const usize pitch = usize{extent} * RGBA8_BYTES_PER_PIXEL;
    Array<uint8> pixels(pitch * extent);
    if (WaitForReady(rhi::RequestTextureReadback(device, texture, { .Width = extent, .Height = extent }, pitch, ticket),
                     [&] { return rhi::GetStatus(device, ticket); }) == false ||
        rhi::CopyReadback(device, ticket, pixels.GetData(), pixels.GetSize()) != Status::Ready ||
        rhi::Release(device, ticket) != Status::Ready)
    {
        return false;
    }
    std::ofstream output("sdk.rgba", std::ios::binary);
    output.write(reinterpret_cast<const char*>(pixels.GetData()), static_cast<std::streamsize>(pixels.GetSize()));
    output.close();
    return output.good();
}
} // namespace
int main(int count, char** arguments)
{
    if (count != ARGUMENT_COUNT)
    {
        return 2;
    }
    const std::string_view dimension(arguments[2]);
    uint32 extent = 0;
    const std::from_chars_result parsed = std::from_chars(dimension.begin(), dimension.end(), extent);
    if (parsed.ec != std::errc{} || parsed.ptr != dimension.end() || extent == 0 || extent > MAX_CAPTURE_EXTENT)
    {
        return 2;
    }
    Array<uint8> bytes;
    rr::RenderGraphPackage package;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.RequiredProfile.Raster.MinBindings = VIEWPORT_MIN_BINDINGS;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    if (ReadBytes(arguments[1], ludus::content::render_graph_format::MAX_PACKAGE_BYTES, bytes) == false ||
        package.Decode({bytes.GetData(), bytes.GetSize()}) != rr::RenderGraphPackageStatus::Ready ||
        rhi::CreateDevice({}, { .Width = extent, .Height = extent }, description, device, surface) !=
            rhi::DeviceStatus::Ready)
    {
        std::cerr << "Deferred package or native raster device unavailable\n";
        return 1;
    }
    rhi::DeviceInfo info;
    const bool isReady = rhi::GetDeviceInfo(device, info) == rhi::DeviceStatus::Ready &&
                         info.Startup.SelectedBackend == rhi::Backend::Metal && Draw(device, surface, extent, package);
    rhi::MeasurementAdapter adapter;
    static_cast<void>(rhi::GetMeasurementAdapter(device, adapter));
    std::cout << adapter.Name << ", Metal" << '\n';
    const bool isDestroyed = rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready;
    rhi::Shutdown();
    return isReady == true && isDestroyed == true ? 0 : 1;
}
