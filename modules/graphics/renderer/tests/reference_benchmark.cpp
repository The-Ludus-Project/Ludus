// Reference capture timing on a cooked LUSCN scene (for example `ludus-cook cook`
// output for staircase2). Each run begins a fresh capture, so setup includes the
// CPU BVH build, scene packing and upload; the first-sample and completion times
// are reported separately, with the device time of the transport dispatches, which
// other processes' CPU load does not inflate. The radiance checksum identifies the converged image,
// so traversal changes that must not alter results can be compared bit for bit.
// When LUDUS_REFERENCE_BENCHMARK_OUTPUT names a directory, the last run's linear
// radiance is published there as reference-{gpu,cpu}.rgb32f (float32 RGB rows).
//
// usage: ludus_reference_benchmark SCENE_DIRECTORY [WIDTH HEIGHT SAMPLES DEPTH {gpu|cpu} RUNS]
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_lit.h"
#include "renderer_lit_shadow.h"
#include "renderer_output.h"
#include "renderer_reference.h"
#include "renderer_reference_reduce.h"
#include "renderer_shadow.h"

#include <ludus/content/content.h>
#include <ludus/foundation/base/parse_number.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/runtime/resources/scene.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace resources = ludus::resources;
namespace shaders = ludus::shaders;
using Status = rhi::RasterStatus;

namespace
{
// Defaults and budgets match the editor capture this benchmark was written for.
constexpr uint32 DEFAULT_WIDTH = 512;
constexpr uint32 DEFAULT_HEIGHT = 225;
constexpr uint32 DEFAULT_SAMPLES = 64;
constexpr uint32 DEFAULT_DEPTH = 8;
constexpr uint32 DEFAULT_RUNS = 3;
constexpr uint32 MAX_EXTENT = 4096;
constexpr uint32 MAX_SAMPLES = 4096;
constexpr uint32 MAX_DEPTH = 64;
constexpr uint32 MAX_RUNS = 64;
constexpr uint32 ROULETTE_DEPTH = 3;
constexpr uint64 SEED = 871;
constexpr usize SCENE_BYTES = usize{384} * 1024 * 1024;
constexpr usize CPU_BUDGET = usize{1024} * 1024 * 1024;
constexpr usize GPU_BUDGET = usize{512} * 1024 * 1024;
constexpr uint32 CPU_TILE_PIXELS = 2048; // The largest tile ReferenceRenderingOptions admits.
constexpr float32 NEAR_DISTANCE = .05F;
constexpr float32 FAR_DISTANCE = 100;
constexpr uint64 TIMEOUT = 3600 * time::NANOSECONDS_PER_SECOND;
constexpr float64 NANOSECONDS_PER_MILLISECOND = 1e6;
constexpr float64 RAYS_PER_MEGARAY_MILLISECOND = 1e3;
constexpr usize RGBA8_BYTES = 4;
constexpr usize TRIANGLE_CORNERS = 3;
// A lit material retains several texture views, in addition to its parameter buffer.
constexpr uint32 REFERENCE_RESOURCES_PER_KIND = 256;
constexpr uint64 FNV_OFFSET = 0xcbf29ce484222325;
constexpr uint64 FNV_PRIME = 0x100000001b3;
// Positional arguments after the scene directory.
constexpr int WIDTH_ARGUMENT = 2;
constexpr int HEIGHT_ARGUMENT = 3;
constexpr int SAMPLES_ARGUMENT = 4;
constexpr int DEPTH_ARGUMENT = 5;
constexpr int TRAVERSAL_ARGUMENT = 6;
constexpr int RUNS_ARGUMENT = 7;
constexpr int ARGUMENT_COUNT = 8;

template <typename Handle>
[[nodiscard]] Status Wait(rr::Renderer& inoutRenderer, const Handle& handle, Status status) noexcept
{
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        status = inoutRenderer.GetStatus(handle);
    }
    return status;
}

[[nodiscard]] bool ParseArgument(int argc, char** argv, int index, uint32 maximum, uint32& inoutValue) noexcept
{
    if (index >= argc)
    {
        return true;
    }
    uint64 value = 0;
    if (ParseUint64(argv[index], std::strlen(argv[index]), value) != NumberParseStatus::Success || value == 0 ||
        value > maximum)
    {
        return false;
    }
    inoutValue = static_cast<uint32>(value);
    return true;
}

[[nodiscard]] bool LoadScene(const resources::SceneDefinition& scene,
                             rr::Renderer& inoutRenderer,
                             rhi::DeviceHandle device,
                             rr::MaterialProgram program,
                             rr::Snapshot& outSnapshot,
                             usize& outTriangles) noexcept
{
    Array<rhi::TextureHandle> textures;
    Array<rr::MaterialVersion> materials;
    Array<rr::Mesh> meshes;
    Array<rr::SceneItem> items;
    if (textures.TryResize(scene.Textures.GetSize()) == false ||
        materials.TryResize(scene.Materials.GetSize()) == false || meshes.TryResize(scene.Meshes.GetSize()) == false ||
        items.TryResize(scene.Objects.GetSize()) == false)
    {
        return false;
    }
    for (usize i = 0; i < scene.Textures.GetSize(); ++i)
    {
        const resources::SceneTexture& source = scene.Textures[i];
        rhi::TextureDescription description{source.Width,
                                            source.Height,
                                            source.IsLinear == true ? rhi::RasterFormat::Rgba8Unorm
                                                                    : rhi::RasterFormat::Rgba8Srgb};
        description.RetainCpuContent = true;
        if (rhi::CreateTexture(device,
                               description,
                               {{source.Pixels.GetData(), source.Pixels.GetSize()}, usize(source.Width) * RGBA8_BYTES},
                               textures[i]) != Status::Ready)
        {
            std::fprintf(stderr, "texture installation failed at %zu\n", i);
            return false;
        }
    }
    for (usize i = 0; i < scene.Materials.GetSize(); ++i)
    {
        const resources::SceneMaterial& source = scene.Materials[i];
        rr::LitMaterialDescription description;
        description.Metallic = source.Metallic;
        description.Roughness = source.Roughness;
        description.Scattering = source.Scattering;
        if (source.HasSourceMaterial == true)
        {
            description.Surface.Tint = {source.Reflectance.X, source.Reflectance.Y, source.Reflectance.Z, 1};
        }
        description.Emission = source.Emission;
        description.IsEmissionTwoSided = source.IsEmissionTwoSided;
        if (source.Texture != ~uint32{0})
        {
            description.Surface.Texture = textures[source.Texture];
            description.Surface.Sampler.Filter = rhi::RasterFilter::Linear;
            description.Surface.Sampler.U = description.Surface.Sampler.V =
                source.Repeat == true ? rhi::RasterAddress::Repeat : rhi::RasterAddress::Clamp;
            if (source.IsAlphaMasked == true)
            {
                description.Surface.Alpha = rr::MaterialAlpha::Masked;
                description.Surface.AlphaCutoff = source.AlphaCutoff;
            }
        }
        else
        {
            if (source.HasSourceMaterial == false)
            {
                description.Surface.Tint = {0, 0, 0, 1};
            }
        }
        if (source.SurfaceTexture != ~uint32{0})
        {
            description.SurfaceMap = textures[source.SurfaceTexture];
            description.NormalScale = source.NormalScale;
        }
        if (source.EmissionTexture != ~uint32{0})
        {
            description.EmissionMap = textures[source.EmissionTexture];
        }
        rr::LightSelection selection;
        const Status materialStatus =
            Wait(inoutRenderer,
                 materials[i],
                 inoutRenderer.CreateLitMaterial(program, description, materials[i], selection));
        if (materialStatus != Status::Ready)
        {
            std::fprintf(stderr,
                         "material installation failed at %zu (%.*s): status %u\n",
                         i,
                         static_cast<int>(source.Name.View().size()),
                         source.Name.View().data(),
                         static_cast<uint32>(materialStatus));
            return false;
        }
    }
    for (usize i = 0; i < scene.Meshes.GetSize(); ++i)
    {
        const resources::SceneMesh& source = scene.Meshes[i];
        rr::MeshDescription description{source.Positions.GetData(),
                                        source.Positions.GetSize(),
                                        source.Indices.GetData(),
                                        source.Indices.GetSize()};
        description.Normals = source.Normals.GetData();
        description.Texcoords = source.Texcoords.GetData();
        if (source.Tangents.GetSize() == source.Positions.GetSize())
        {
            description.Tangents = {source.Tangents.GetData(), source.Tangents.GetSize()};
        }
        if (Wait(inoutRenderer, meshes[i], inoutRenderer.CreateMesh(description, meshes[i])) != Status::Ready)
        {
            std::fprintf(stderr, "mesh installation failed at %zu\n", i);
            return false;
        }
    }
    outTriangles = 0;
    for (usize i = 0; i < scene.Objects.GetSize(); ++i)
    {
        const resources::SceneObject& source = scene.Objects[i];
        items[i].Geometry = meshes[source.Mesh];
        items[i].Surface = materials[source.Material];
        items[i].Transform = source.Transform;
        items[i].SourceId = source.SourceId != 0 ? source.SourceId : i + 1;
        outTriangles += scene.Meshes[source.Mesh].Indices.GetSize() / TRIANGLE_CORNERS;
    }
    const bool isCreated = inoutRenderer.CreateSnapshot(items.GetData(), items.GetSize(), outSnapshot) == Status::Ready;
    for (rr::Mesh& mesh : meshes)
    {
        static_cast<void>(inoutRenderer.Release(mesh));
    }
    for (rr::MaterialVersion& material : materials)
    {
        static_cast<void>(inoutRenderer.Release(material));
    }
    for (rhi::TextureHandle texture : textures)
    {
        static_cast<void>(rhi::Destroy(device, texture));
    }
    return isCreated;
}

struct RunResult final
{
    float64 FirstSampleMilliseconds = 0;
    float64 TotalMilliseconds = 0;
    rr::ReferenceCaptureInfo Info;
    uint64 Checksum = FNV_OFFSET;
    float64 MeanLuminance = 0;
    Array<math::Vector3> Radiance;
    float64 TransportMilliseconds = 0;
    uint32 TimedDispatches = 0;
};
// The ordered graph scopes each pass by name; see rhi::GpuPassTimings.
constexpr const char* TRANSPORT_PASS_NAME = "Reference compute transport";
// Adds a completed timing batch's transport passes, then arms the next batch.
void CollectTransportTimings(rhi::DeviceHandle device, RunResult& inoutResult) noexcept
{
    rhi::GpuPassTimings timings;
    if (rhi::GetGpuPassTimings(device, timings) == Status::Ready && timings.State == rhi::GpuTimingState::Available)
    {
        for (usize i = 0; i < timings.Count; ++i)
        {
            const rhi::GpuPassTiming& pass = timings.Passes[i];
            if (pass.Timing.State == rhi::GpuTimingState::Available &&
                std::strcmp(pass.Scope.Name.GetData(), TRANSPORT_PASS_NAME) == 0)
            {
                inoutResult.TransportMilliseconds += pass.Timing.DurationNanoseconds / NANOSECONDS_PER_MILLISECOND;
                ++inoutResult.TimedDispatches;
            }
        }
    }
    if (timings.State != rhi::GpuTimingState::Armed && timings.State != rhi::GpuTimingState::Pending)
    {
        static_cast<void>(rhi::ArmGpuPassTimings(device));
    }
}

[[nodiscard]] bool RunCapture(rr::Renderer& inoutRenderer,
                              rhi::DeviceHandle device,
                              rr::Snapshot snapshot,
                              const rr::ReferenceCaptureDescription& description,
                              rhi::SurfaceHandle surface,
                              RunResult& outResult) noexcept
{
    const uint64 start = time::NowTicks();
    rr::ReferenceCapture capture;
    Status status = inoutRenderer.BeginReferenceCapture(snapshot, description, capture);
    if (status != Status::Ready && status != Status::Pending)
    {
        return false;
    }
    // A Ready setup still has zero samples; only Advance reports completion.
    status = Status::Pending;
    bool hasFirstSample = false;
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        CollectTransportTimings(device, outResult);
        status = inoutRenderer.AdvanceReferenceCapture(capture, surface);
        if (hasFirstSample == false &&
            inoutRenderer.GetReferenceCaptureInfo(capture, outResult.Info) == Status::Ready &&
            outResult.Info.MinimumSamples != 0)
        {
            outResult.FirstSampleMilliseconds = float64(time::NowTicks() - start) / NANOSECONDS_PER_MILLISECOND;
            hasFirstSample = true;
        }
    }
    outResult.TotalMilliseconds = float64(time::NowTicks() - start) / NANOSECONDS_PER_MILLISECOND;
    Array<rr::ReferencePixel> pixels;
    if (status != Status::Ready || inoutRenderer.GetReferenceCaptureInfo(capture, outResult.Info) != Status::Ready ||
        pixels.TryResize(usize(description.Width) * description.Height) == false ||
        inoutRenderer.CopyReferencePixels(capture, pixels.GetData(), pixels.GetSize()) != Status::Ready)
    {
        static_cast<void>(inoutRenderer.Release(capture));
        return false;
    }
    float64 luminance = 0;
    if (outResult.Radiance.TryResize(pixels.GetSize()) == false)
    {
        static_cast<void>(inoutRenderer.Release(capture));
        return false;
    }
    for (usize i = 0; i < pixels.GetSize(); ++i)
    {
        const rr::ReferencePixel& pixel = pixels[i];
        const math::Vector3 radiance = pixel.Emission + pixel.Direct + pixel.Indirect;
        outResult.Radiance[i] = radiance;
        luminance += .2126 * float64(radiance.X) + .7152 * float64(radiance.Y) + .0722 * float64(radiance.Z);
        const math::Vector3 parts[]{pixel.Emission, pixel.Direct, pixel.Indirect};
        for (const math::Vector3& part : parts)
        {
            for (const float32 component : {part.X, part.Y, part.Z})
            {
                uint32 bits = 0;
                std::memcpy(&bits, &component, sizeof(bits));
                outResult.Checksum = (outResult.Checksum ^ bits) * FNV_PRIME;
            }
        }
    }
    outResult.MeanLuminance = luminance / float64(pixels.GetSize());
    return inoutRenderer.Release(capture) == Status::Ready;
}
} // namespace

int main(int argc, char** argv)
{
    rr::ReferenceCaptureDescription description;
    description.Width = DEFAULT_WIDTH;
    description.Height = DEFAULT_HEIGHT;
    description.TargetSamples = DEFAULT_SAMPLES;
    description.MaxDepth = DEFAULT_DEPTH;
    description.RouletteDepth = ROULETTE_DEPTH;
    description.Seed = SEED;
    uint32 runs = DEFAULT_RUNS;
    const bool isCpu = argc > TRAVERSAL_ARGUMENT && std::strcmp(argv[TRAVERSAL_ARGUMENT], "cpu") == 0;
    if (argc < 2 || argc > ARGUMENT_COUNT ||
        ParseArgument(argc, argv, WIDTH_ARGUMENT, MAX_EXTENT, description.Width) == false ||
        ParseArgument(argc, argv, HEIGHT_ARGUMENT, MAX_EXTENT, description.Height) == false ||
        ParseArgument(argc, argv, SAMPLES_ARGUMENT, MAX_SAMPLES, description.TargetSamples) == false ||
        ParseArgument(argc, argv, DEPTH_ARGUMENT, MAX_DEPTH, description.MaxDepth) == false ||
        ParseArgument(argc, argv, RUNS_ARGUMENT, MAX_RUNS, runs) == false ||
        (argc > TRAVERSAL_ARGUMENT && isCpu == false && std::strcmp(argv[TRAVERSAL_ARGUMENT], "gpu") != 0))
    {
        std::fprintf(stderr,
                     "usage: %s SCENE_DIRECTORY [WIDTH HEIGHT SAMPLES DEPTH {gpu|cpu} RUNS]\n",
                     argc > 0 ? argv[0] : "ludus_reference_benchmark");
        return 1;
    }
    ludus::content::Bytes bundle;
    resources::SceneDefinition scene;
    if (ludus::content::ReadFile(argv[1], "scene.luscn", SCENE_BYTES, bundle) != ludus::content::Status::Ok ||
        resources::SceneDefinition::Decode(bundle.Data().data(), bundle.Data().size(), scene) != resources::Status::Ok)
    {
        std::fprintf(stderr, "could not read %s/scene.luscn\n", argv[1]);
        return 2;
    }
    math::Matrix4 view;
    math::Matrix4 projection;
    if (math::TryLookAt(scene.Eye, scene.Target, scene.Up, view) != math::MathStatus::Success ||
        math::TryPerspectiveReverseZ(scene.VerticalFov,
                                     float32(description.Width) / float32(description.Height),
                                     NEAR_DISTANCE,
                                     FAR_DISTANCE,
                                     projection) != math::MathStatus::Success)
    {
        return 3;
    }
    description.WorldToClip = projection * view;
    description.CameraPosition = scene.Eye;
    description.ReverseZ = true;
    description.Environment = scene.Environment.Width == 0 ? scene.Environment.Rgb : math::Vector3{};

    rhi::DeviceDescription request;
    request.Storage.ResourcesPerKind = REFERENCE_RESOURCES_PER_KIND;
    request.Required.PortableRaster = true;
    request.Required.Compute = isCpu == false;
    request.EnableGpuTiming = isCpu == false;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    if (rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, request, device, surface) != rhi::DeviceStatus::Ready)
    {
        std::fprintf(stderr, "no device with the requested capabilities\n");
        return 4;
    }
    struct Shutdown final
    {
        ~Shutdown() noexcept
        {
            rhi::Shutdown();
        }
    } shutdown;
    int exitCode = 0;
    {
        rr::Renderer renderer;
        const rr::ReferenceRenderingOptions options{isCpu == true ? rr::ReferenceTraversal::CpuOracle
                                                                  : rr::ReferenceTraversal::GpuCompute,
                                                    CPU_BUDGET,
                                                    0,
                                                    CPU_TILE_PIXELS,
                                                    isCpu == true ? usize{0} : GPU_BUDGET};
        rr::MaterialProgram program;
        rr::Snapshot snapshot;
        rr::Limits limits;
        limits.Meshes = static_cast<uint32>(scene.Meshes.GetSize());
        limits.Materials = static_cast<uint32>(scene.Materials.GetSize());
        limits.Objects = static_cast<uint32>(scene.Objects.GetSize());
        usize triangles = 0;
        const char* failureOrNull = nullptr;
        if (renderer.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment(), limits) !=
            Status::Ready)
        {
            failureOrNull = "renderer initialization";
        }
        else if ((isCpu == true ? renderer.EnableReferenceRendering({}, options)
                                : renderer.EnableReferenceRendering(shaders::renderer_reference::Compute(),
                                                                    shaders::renderer_reference_reduce::Compute(),
                                                                    options)) != Status::Ready)
        {
            failureOrNull = "reference rendering";
        }
        else if (renderer.InitializeViews(shaders::renderer_composite::Vertex(),
                                          shaders::renderer_composite::Fragment()) != Status::Ready ||
                 renderer.InitializeLighting(shaders::renderer_output::Vertex(),
                                             shaders::renderer_output::Fragment(),
                                             shaders::renderer_shadow::Fragment()) != Status::Ready ||
                 Wait(renderer,
                      program,
                      renderer.PrewarmLitProgram(shaders::renderer_lit::Vertex(),
                                                 shaders::renderer_lit::Fragment(),
                                                 shaders::renderer_lit_shadow::Fragment(),
                                                 program)) != Status::Ready)
        {
            failureOrNull = "lighting programs";
        }
        else if (LoadScene(scene, renderer, device, program, snapshot, triangles) == false)
        {
            failureOrNull = "scene installation";
        }
        if (failureOrNull != nullptr)
        {
            std::fprintf(stderr, "setup failed: %s\n", failureOrNull);
            return 5;
        }
        std::printf("scene %s: %zu objects, %zu triangles; %ux%u, %u spp, depth %u, %s\n",
                    argv[1],
                    scene.Objects.GetSize(),
                    triangles,
                    description.Width,
                    description.Height,
                    description.TargetSamples,
                    description.MaxDepth,
                    isCpu == true ? "CPU oracle" : "GPU compute");
        Array<float64> totals;
        RunResult result; // The last run's radiance is published below.
        for (uint32 run = 0; run < runs; ++run)
        {
            result = RunResult{};
            if (RunCapture(renderer, device, snapshot, description, surface, result) == false ||
                result.Info.InvalidContributions != 0 || totals.TryAdd(result.TotalMilliseconds) == false)
            {
                std::fprintf(stderr, "capture %u failed\n", run);
                exitCode = 6;
                break;
            }
            const float64 queries = float64(result.Info.ClosestQueries + result.Info.VisibilityQueries);
            std::printf("run %u: %u-%u spp, first sample %.1f ms, total %.1f ms, GPU transport %.1f ms in %u timed "
                        "dispatches, %.2f Mrays/s (%llu closest, %llu visibility), mean luminance %.6f, checksum "
                        "%016llx\n",
                        run,
                        result.Info.MinimumSamples,
                        result.Info.MaximumSamples,
                        result.FirstSampleMilliseconds,
                        result.TotalMilliseconds,
                        result.TransportMilliseconds,
                        result.TimedDispatches,
                        queries / (result.TotalMilliseconds * RAYS_PER_MEGARAY_MILLISECOND),
                        static_cast<unsigned long long>(result.Info.ClosestQueries),
                        static_cast<unsigned long long>(result.Info.VisibilityQueries),
                        result.MeanLuminance,
                        static_cast<unsigned long long>(result.Checksum));
        }
        const char* outputOrNull = std::getenv("LUDUS_REFERENCE_BENCHMARK_OUTPUT");
        if (exitCode == 0 && outputOrNull != nullptr)
        {
            filesystem::WriteDirectory directory;
            if (directory.Open(outputOrNull).Succeeded() == false ||
                directory
                        .Publish(isCpu == true ? "reference-cpu.rgb32f" : "reference-gpu.rgb32f",
                                 {reinterpret_cast<const uint8*>(result.Radiance.GetData()),
                                  result.Radiance.GetSize() * sizeof(math::Vector3)})
                        .Outcome.Succeeded() == false)
            {
                std::fprintf(stderr, "could not publish radiance to %s\n", outputOrNull);
                exitCode = 7;
            }
        }
        if (totals.IsEmpty() == false)
        {
            Sort(totals.GetData(), totals.GetSize(), [](float64 left, float64 right) noexcept { return left < right; });
            std::printf("median total %.1f ms over %zu runs\n", totals[totals.GetSize() / 2], totals.GetSize());
        }
        static_cast<void>(renderer.Release(snapshot));
    }
    return exitCode;
}
