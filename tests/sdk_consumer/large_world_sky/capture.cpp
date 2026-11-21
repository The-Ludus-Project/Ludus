// Consume the assembled World, camera and linear default deferred package emitted by the fixture.
#include "renderer_composite.h"

#include <ludus/content/render_graph_format/runtime_package.h>
#include <ludus/content/scene_format/limits.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/measurement.h>
#include <ludus/runtime/resources/scene.h>

#include <ludus/graphics/rhi/gpu_scope.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <CommonCrypto/CommonDigest.h>

namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace res = ludus::resources;
using Status = rhi::RasterStatus;

constexpr time::Duration GPU_TIMEOUT = time::Seconds(30);
constexpr usize RGBA8_BYTES_PER_PIXEL = 4;
constexpr uint32 CAPTURE_WIDTH = 128;
constexpr uint32 BENCHMARK_WIDTH = 512;
constexpr uint32 WARMUP_FRAMES = 8;
constexpr uint32 MEASURED_FRAMES = 41;
constexpr uint32 FRAME_COUNT = 1 + WARMUP_FRAMES + MEASURED_FRAMES;
constexpr uint32 VIEWPORT_MIN_BINDINGS = 11;

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

bool ReadBytes(const std::string& path, usize limit, Array<uint8>& outBytes)
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

void WriteTimingPlanIdentity(const Array<uint8>& bytes)
{
    constexpr char HEX_DIGITS[] = "0123456789abcdef";
    constexpr uint32 HEX_RADIX = 16;
    StaticArray<uint8, CC_SHA256_DIGEST_LENGTH> digest;
    CC_SHA256(bytes.GetData(), static_cast<CC_LONG>(bytes.GetSize()), digest.GetData());
    std::cout << "plan_sha256,";
    for (uint8 byte : digest)
    {
        std::cout << HEX_DIGITS[byte / HEX_RADIX] << HEX_DIGITS[byte % HEX_RADIX];
    }
    std::cout << '\n';
}

bool Capture(rhi::DeviceHandle device,
             rhi::SurfaceHandle surface,
             uint32 extent,
             const rr::RenderGraphPackage& package,
             const std::string& directory,
             rr::DeferredLightingView lightingView,
             bool hasAtmosphere,
             bool shouldMeasure)
{
    Array<uint8> bytes;
    res::SceneDefinition scene;
    if (ReadBytes(directory + "/scene.luscn", ludus::content::scene_format::MAX_ASSEMBLED_BUNDLE_BYTES, bytes) ==
            false ||
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
    std::ifstream camera(directory + "/camera.txt");
    for (math::Vector4& column : view.WorldToClip.Columns)
    {
        camera >> column.X >> column.Y >> column.Z >> column.W;
    }
    camera >> view.CameraPosition.X >> view.CameraPosition.Y >> view.CameraPosition.Z;
    if (camera.fail() == true)
    {
        return false;
    }
    view.LightingView = lightingView;
    view.Atmosphere.IsEnabled = hasAtmosphere;
    view.Atmosphere.ShouldLightScene = true;
    view.Atmosphere.SolarIlluminance = {16, 16, 16};
    view.Atmosphere.DirectionToSunWorld = {0, 0.25F, 0.96824584F};
    rr::SceneShaderInputs inputs;
    inputs.Environment = {2, 2, 2};
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    rr::RenderGraph graph;
    const rr::RenderGraphPrograms programs = package.GetPrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                 ludus::shaders::renderer_composite::Fragment());
    const auto poll = [&graph]() { return graph.Poll(); };
    const uint64 preparationStart = time::NowTicks();
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
    const uint64 preparationNanoseconds = time::NowTicks() - preparationStart;
    const uint32 frames = shouldMeasure == true ? FRAME_COUNT : 2;
    for (uint32 frame = 0; frame < frames; ++frame)
    {
        if (shouldMeasure == true &&
            (rhi::ArmGpuTiming(device) != Status::Ready || rhi::ArmGpuPassTimings(device) != Status::Ready))
        {
            return false;
        }
        rhi::SubmissionToken completion;
        const uint64 cpuStart = time::NowTicks();
        if (graph.Render(surface, extent, extent, completion) != Status::Ready)
        {
            return false;
        }
        const uint64 cpuNanoseconds = time::NowTicks() - cpuStart;
        if (WaitForReady(Status::Pending, [&] {
                static_cast<void>(rhi::PollLifetime(device));
                return rhi::GetStatus(device, completion);
            }) == false)
        {
            return false;
        }
        if (shouldMeasure == false)
        {
            continue;
        }
        rhi::GpuTiming timing;
        rhi::GpuPassTimings passes;
        if (rhi::GetGpuTiming(device, timing) != Status::Ready || timing.State != rhi::GpuTimingState::Available ||
            rhi::GetGpuPassTimings(device, passes) != Status::Ready || passes.State != rhi::GpuTimingState::Available)
        {
            return false;
        }
        std::cout << "frame," << frame << ',' << timing.DurationNanoseconds << ',' << cpuNanoseconds << ','
                  << passes.DroppedPasses << '\n';
        for (usize operation = 0; operation < passes.Count; ++operation)
        {
            const rhi::GpuPassTiming& pass = passes.Passes[operation];
            constexpr float64 MAX_DURATION_NANOSECONDS = 60'000'000'000;
            const float64 duration = pass.Timing.DurationNanoseconds;
            const bool isAvailable = pass.Timing.State == rhi::GpuTimingState::Available && duration >= 0 &&
                                     duration < MAX_DURATION_NANOSECONDS;
            if (isAvailable == true)
            {
                std::cout << "pass," << frame << ',' << pass.Scope.Name.GetData() << ',' << pass.ScopePassIndex << ','
                          << duration << '\n';
            }
            std::cout << "scope," << frame << ',' << pass.Scope.Identity << ',' << pass.ScopePassIndex << ','
                      << pass.Scope.LogicalPassCount << ',' << pass.Scope.Name.GetData() << ',';
            if (isAvailable == true)
            {
                std::cout << static_cast<uint64>(duration + 0.5);
            }
            std::cout << '\n';
        }
    }
    if (shouldMeasure == true)
    {
        rhi::AllocationTelemetry steady;
        if (rhi::GetAllocationTelemetry(device, steady) != Status::Ready || steady.ApiBytesAvailable == false)
        {
            return false;
        }
        items[items.GetSize() - 1].MeshToWorld.Translation.X += 1;
        rhi::AllocationTelemetry peak = steady;
        Status next = graph.Update({meshes.GetData(), meshes.GetSize()},
                                   {items.GetData(), items.GetSize()},
                                   {&view, 1},
                                   2,
                                   rr::RenderGraphCoveragePolicy::RejectIncomplete,
                                   report,
                                   &inputs);
        const bool isReplaced = WaitForReady(next, [&] {
            rhi::AllocationTelemetry current;
            if (rhi::GetAllocationTelemetry(device, current) == Status::Ready && current.ApiBytes > peak.ApiBytes)
            {
                peak = current;
            }
            return graph.Poll();
        });
        rhi::StorageUsage overlap;
        if (rhi::GetStorageUsage(device, overlap) != Status::Ready)
        {
            return false;
        }
        rhi::AllocationTelemetry published;
        if (isReplaced == false || rhi::GetAllocationTelemetry(device, published) != Status::Ready)
        {
            return false;
        }
        std::cout << "prepare_ns," << preparationNanoseconds << "\nsteady_bytes," << steady.ApiBytes
                  << "\nlogical_peak_bytes," << overlap.PeakResourceBytes << "\nlogical_steady_bytes,"
                  << steady.LogicalBytes << "\nreplacement_sample_bytes," << std::max(peak.ApiBytes, published.ApiBytes)
                  << '\n';
        return true;
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
    std::ofstream output("sdk-" + std::to_string(static_cast<int>(lightingView)) + "-" +
                             (hasAtmosphere == true ? "1" : "0") + ".rgba",
                         std::ios::binary);
    output.write(reinterpret_cast<const char*>(pixels.GetData()), static_cast<std::streamsize>(pixels.GetSize()));
    output.close();
    return output.good();
}
} // namespace
int main(int count, char** arguments)
{
    if (count != 3)
    {
        return 2;
    }
    const std::string_view mode(arguments[1]);
    const bool shouldMeasure = mode == "timing";
    if (shouldMeasure == false && mode != "capture")
    {
        return 2;
    }
    const std::string executable(arguments[0]);
    const std::string resources = executable.substr(0, executable.find_last_of('/') + 1) + "../Resources/";
    const std::string directory = resources + arguments[2];
    const uint32 extent = shouldMeasure == true ? BENCHMARK_WIDTH : CAPTURE_WIDTH;
    Array<uint8> bytes;
    rr::RenderGraphPackage package;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.EnableGpuTiming = shouldMeasure;
    description.RequiredProfile.Raster.MinBindings = VIEWPORT_MIN_BINDINGS;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    if (ReadBytes(resources + "skylight.lurgrt", ludus::content::render_graph_format::MAX_PACKAGE_BYTES, bytes) ==
            false ||
        package.Decode({bytes.GetData(), bytes.GetSize()}) != rr::RenderGraphPackageStatus::Ready ||
        rhi::CreateDevice({}, { .Width = extent, .Height = extent }, description, device, surface) !=
            rhi::DeviceStatus::Ready)
    {
        std::cerr << "Deferred package or native raster device unavailable\n";
        return 1;
    }
    rhi::DeviceInfo info;
    bool isReady = rhi::GetDeviceInfo(device, info) == rhi::DeviceStatus::Ready &&
                   info.Startup.SelectedBackend == rhi::Backend::Metal;
    if (isReady == true && shouldMeasure == true)
    {
        WriteTimingPlanIdentity(bytes);
        isReady = Capture(device, surface, extent, package, directory, rr::DeferredLightingView::Final, true, true);
    }
    else if (isReady == true)
    {
        for (bool hasAtmosphere : {false, true})
        {
            for (rr::DeferredLightingView view :
                 {rr::DeferredLightingView::SkyVisibility, rr::DeferredLightingView::SkyDiffuse})
            {
                isReady =
                    isReady == true && Capture(device, surface, extent, package, directory, view, hasAtmosphere, false);
            }
        }
    }
    rhi::MeasurementAdapter adapter;
    static_cast<void>(rhi::GetMeasurementAdapter(device, adapter));
    std::cout << adapter.Name << ", Metal" << '\n';
    const bool isDestroyed = rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready;
    rhi::Shutdown();
    return isReady == true && isDestroyed == true ? 0 : 1;
}
