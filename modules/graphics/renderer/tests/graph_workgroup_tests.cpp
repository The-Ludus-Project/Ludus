#include "../src/internal/render_graph_ray.h"
#include "../src/internal/render_graph_ray_scene.h"
#include "renderer_composite.h"
#include "workgroups_compute_ray_tracing_linear.h"
#include "workgroups_compute_ray_tracing_tiled.h"
#include "workgroups_veach_guibas_mis_linear.h"
#include "workgroups_veach_guibas_mis_tiled.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/measurement.h>
#include <new>
#include <string>

namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace internal = rr::internal;
namespace rhi = ludus::graphics::rhi;
namespace packages = ludus::render_graph_packages;
namespace shaders = ludus::shaders;
namespace format = ludus::content::render_graph_format;
using Status = rhi::RasterStatus;
constexpr usize VARIANTS = 2;
constexpr usize MAX_PASSES = 4;
constexpr usize MAX_OPERATIONS = MAX_PASSES * 2 - 1;
constexpr uint64 TIMEOUT = 30 * time::NANOSECONDS_PER_SECOND;
constexpr uint32 TEST_EXTENT = 64;
constexpr uint32 TEST_SAMPLES = 128;
constexpr uint32 BENCHMARK_EXTENT = 512;
constexpr uint32 BENCHMARK_SAMPLES = 128;
constexpr uint32 WARMUP = 16;
constexpr uint32 GRID = 32;
constexpr usize QUAD_VERTICES = 6;
constexpr usize CHANNELS = 4;
constexpr usize WORDS_PER_PIXEL = 2;
constexpr usize READBACK_BYTES = usize{64} * 1024;
constexpr usize BUDGET = usize{256} * 1024 * 1024;
constexpr uint32 TEXTURE_EXTENT = 4;
constexpr float32 DEPTH_WORLD = .75F;
constexpr float32 RAW_TOLERANCE = 1e-5F;
constexpr float32 MEAN_RELATIVE_TOLERANCE = .02F;
constexpr float32 MEAN_ABSOLUTE_TOLERANCE = .002F;
constexpr float64 NORMALIZED_RMSE_TOLERANCE = .12;
constexpr usize SCENE_COUNT = 3;
constexpr StaticArray<const char*, SCENE_COUNT> SCENES{{"opaque", "cutout", "textured"}};
constexpr StaticArray<const char*, VARIANTS> VARIANT_NAMES{{"baseline", "candidate"}};
struct Session final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    explicit Session(uint32 extent)
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Preferred.RayQueries = true;
        description.EnableGpuTiming = true;
        description.Storage.ResourcesPerGraph = rr::VISIBILITY_GRAPH_RESOURCE_CAPACITY;
        REQUIRE(rhi::CreateDevice({}, { .Width = extent, .Height = extent }, description, Device, Surface) ==
                rhi::DeviceStatus::Ready);
    }
    ~Session() noexcept
    {
        rhi::Shutdown();
    }
};
core::UniquePtr<rr::RenderGraphPackage> LoadPackage(const char* path)
{
    std::FILE* fileOrNull = std::fopen(path, "rb");
    REQUIRE(fileOrNull != nullptr);
    REQUIRE(std::fseek(fileOrNull, 0, SEEK_END) == 0);
    const long bytes = std::ftell(fileOrNull);
    REQUIRE(bytes > 0);
    REQUIRE(std::fseek(fileOrNull, 0, SEEK_SET) == 0);
    Array<uint8> contents;
    REQUIRE(contents.TryResize(static_cast<usize>(bytes)) == true);
    REQUIRE(std::fread(contents.GetData(), 1, contents.GetSize(), fileOrNull) == contents.GetSize());
    REQUIRE(std::fclose(fileOrNull) == 0);
    rr::RenderGraphPackage* allocationOrNull = new (std::nothrow) rr::RenderGraphPackage;
    core::UniquePtr<rr::RenderGraphPackage> package{allocationOrNull};
    REQUIRE(package.Get() != nullptr);
    REQUIRE(package->Decode({contents.GetData(), contents.GetSize()}) == rr::RenderGraphPackageStatus::Ready);
    return package;
}
void WaitGraph(rr::RenderGraph& inoutGraph, Status status)
{
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
        status = inoutGraph.Poll();
    REQUIRE(status == Status::Ready);
}
struct Scene final
{
    Array<math::Vector3> PositionsMesh;
    Array<math::Vector3> NormalsMesh;
    Array<math::Vector2> TexcoordsUv;
    Array<uint32> Indices;
    StaticArray<uint8, TEXTURE_EXTENT * TEXTURE_EXTENT * CHANNELS> Texels{};
    rr::SceneShaderMaterial Material;
    uint32 MaterialBinding = 0;
    rr::RenderGraphMesh Mesh;
    rr::RenderGraphItem Item;
    rr::ViewDescription Camera;
    rr::SceneShaderInputs Inputs;
    explicit Scene(usize scene)
    {
        const usize count = usize(GRID) * GRID * QUAD_VERTICES;
        REQUIRE(PositionsMesh.TryEnsureCapacity(count));
        REQUIRE(NormalsMesh.TryEnsureCapacity(count));
        REQUIRE(TexcoordsUv.TryEnsureCapacity(count));
        REQUIRE(Indices.TryEnsureCapacity(count));
        for (uint32 y = 0; y < GRID; ++y)
            for (uint32 x = 0; x < GRID; ++x)
            {
                if (scene == 1 && x % 2 != 0)
                    continue;
                const float32 left = float32(x) * 2 / float32(GRID) - 1;
                const float32 right = float32(x + 1) * 2 / float32(GRID) - 1;
                const float32 bottom = float32(y) * 2 / float32(GRID) - 1;
                const float32 top = float32(y + 1) * 2 / float32(GRID) - 1;
                const StaticArray<math::Vector3, QUAD_VERTICES> corners{{{left, bottom, DEPTH_WORLD},
                                                                         {right, top, DEPTH_WORLD},
                                                                         {right, bottom, DEPTH_WORLD},
                                                                         {left, bottom, DEPTH_WORLD},
                                                                         {left, top, DEPTH_WORLD},
                                                                         {right, top, DEPTH_WORLD}}};
                for (const math::Vector3& positionMesh : corners)
                {
                    REQUIRE(Indices.TryAdd(static_cast<uint32>(PositionsMesh.GetSize())));
                    REQUIRE(PositionsMesh.TryAdd(positionMesh));
                    REQUIRE(NormalsMesh.TryAdd({0, 0, -1}));
                    REQUIRE(TexcoordsUv.TryAdd({positionMesh.X * .5F + .5F, positionMesh.Y * .5F + .5F}));
                }
            }
        Mesh.Geometry = {PositionsMesh.GetData(), PositionsMesh.GetSize(), Indices.GetData(), Indices.GetSize()};
        Mesh.Geometry.Normals = NormalsMesh.GetData();
        Mesh.Geometry.Texcoords = TexcoordsUv.GetData();
        Mesh.MaterialBindings = {&MaterialBinding, 1};
        Item.SourceId = 1;
        Item.Material.Color = {1, 1, 1, 1};
        Material.BaseColor = {.6F, .5F, .4F};
        Material.Roughness = 1;
        if (scene == 2)
        {
            constexpr uint8 LOW = 64;
            constexpr uint8 HIGH = 224;
            constexpr uint8 MIDDLE = 128;
            constexpr uint8 ALPHA = 255;
            for (usize index = 0; index < Texels.GetSize(); index += CHANNELS)
            {
                Texels[index] = (index / CHANNELS) % 2 == 0 ? LOW : HIGH;
                Texels[index + 1] = MIDDLE;
                Texels[index + 2] = HIGH;
                Texels[index + 3] = ALPHA;
            }
            Material.ColorTexture = {Texels.GetData(),
                                     Texels.GetSize(),
                                     TEXTURE_EXTENT * CHANNELS,
                                     TEXTURE_EXTENT,
                                     TEXTURE_EXTENT,
                                     rhi::RasterFormat::Rgba8Unorm};
            Material.Sampler.Filter = rhi::RasterFilter::Linear;
            Material.Sampler.U = Material.Sampler.V = rhi::RasterAddress::Repeat;
            constexpr float32 UV_REPETITIONS = 3;
            Material.UvTransform = {UV_REPETITIONS, UV_REPETITIONS, 0, 0};
        }
        Inputs.Materials = {&Material, 1};
        Inputs.Environment = {.25F, .5F, 1};
        Camera.OrthographicCamera = true;
    }
};
struct Timings final
{
    Array<float64> Frame;
    StaticArray<Array<float64>, MAX_OPERATIONS> Pass;
    StaticArray<rhi::GpuPassTiming, MAX_OPERATIONS> Metadata{};
    bool HasMetadata = false;
};
void Render(rr::RenderGraph& inoutGraph, const Session& session, uint32 extent)
{
    rhi::SubmissionToken token;
    REQUIRE(inoutGraph.Render(session.Surface, extent, extent, token) == Status::Ready);
    Status status = Status::Pending;
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
        status = rhi::GetStatus(session.Device, token);
    REQUIRE(status == Status::Ready);
}
void Collect(const Session& session, usize sample, usize operations, Timings& inoutTimings)
{
    rhi::GpuTiming frame;
    REQUIRE(rhi::GetGpuTiming(session.Device, frame) == Status::Ready);
    REQUIRE(frame.State == rhi::GpuTimingState::Available);
    REQUIRE(math::IsFinite(frame.DurationNanoseconds));
    REQUIRE(frame.DurationNanoseconds > 0);
    inoutTimings.Frame[sample] = frame.DurationNanoseconds;
    rhi::GpuPassTimings passes;
    const uint64 start = time::NowTicks();
    do
    {
        REQUIRE(rhi::GetGpuPassTimings(session.Device, passes) == Status::Ready);
    } while (passes.State == rhi::GpuTimingState::Pending && time::NowTicks() - start < TIMEOUT);
    REQUIRE(passes.State == rhi::GpuTimingState::Available);
    REQUIRE(passes.Count == operations);
    REQUIRE(passes.DroppedPasses == 0);
    for (usize index = 0; index < operations; ++index)
    {
        const rhi::GpuPassTiming& pass = passes.Passes[index];
        REQUIRE(pass.Kind == ((index == 0 || index % 2 == 1) ? rhi::GpuPassKind::Compute : rhi::GpuPassKind::Raster));
        REQUIRE(pass.Timing.State == rhi::GpuTimingState::Available);
        REQUIRE(math::IsFinite(pass.Timing.DurationNanoseconds));
        REQUIRE(pass.Timing.DurationNanoseconds >= 0);
        REQUIRE(math::IsFinite(pass.Timing.TickNanoseconds) == true);
        REQUIRE(pass.Timing.TickNanoseconds >= 0);
        REQUIRE(pass.Timing.ValidBits <= 64);
        if (inoutTimings.HasMetadata == true)
        {
            CHECK(pass.Scope.Identity == inoutTimings.Metadata[index].Scope.Identity);
            CHECK(pass.ScopePassIndex == inoutTimings.Metadata[index].ScopePassIndex);
            CHECK(pass.Scope.LogicalPassCount == inoutTimings.Metadata[index].Scope.LogicalPassCount);
            CHECK(pass.Timing.ValidBits == inoutTimings.Metadata[index].Timing.ValidBits);
            CHECK(pass.Timing.TickNanoseconds == inoutTimings.Metadata[index].Timing.TickNanoseconds);
            CHECK(std::strcmp(pass.Scope.Name.GetData(), inoutTimings.Metadata[index].Scope.Name.GetData()) == 0);
        }
        inoutTimings.Metadata[index] = pass;
        inoutTimings.Pass[index][sample] = pass.Timing.DurationNanoseconds;
    }
    inoutTimings.HasMetadata = true;
}
void ReadBuffer(const Session& session, rhi::BufferHandle buffer, Array<math::Vector4>& outValues)
{
    const usize bytes = outValues.GetSize() * sizeof(math::Vector4);
    for (usize offset = 0; offset < bytes; offset += READBACK_BYTES)
    {
        const usize chunk = math::Min(READBACK_BYTES, bytes - offset);
        rhi::ReadbackTicket ticket;
        Status status = rhi::RequestBufferReadback(session.Device, buffer, offset, chunk, ticket);
        const uint64 start = time::NowTicks();
        while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
            status = rhi::GetStatus(session.Device, ticket);
        REQUIRE(status == Status::Ready);
        REQUIRE(
            rhi::CopyReadback(session.Device, ticket, reinterpret_cast<uint8*>(outValues.GetData()) + offset, chunk) ==
            Status::Ready);
        REQUIRE(rhi::Release(session.Device, ticket) == Status::Ready);
    }
}
void Write(const std::string& directory, const std::string& name, const void* data, usize bytes)
{
    const std::string path = directory + "/" + name;
    std::FILE* fileOrNull = std::fopen(path.c_str(), "wb");
    REQUIRE(fileOrNull != nullptr);
    REQUIRE(std::fwrite(data, 1, bytes, fileOrNull) == bytes);
    REQUIRE(std::fclose(fileOrNull) == 0);
}
void WriteTimes(const std::string& directory, const std::string& name, const Array<float64>& values)
{
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(values.GetSize() * sizeof(float64)));
    for (usize i = 0; i < values.GetSize(); ++i)
    {
        uint64 bits = 0;
        std::memcpy(&bits, &values[i], sizeof(bits));
        REQUIRE(TryWriteLittleEndian(bits, {bytes.GetData() + i * sizeof(bits), sizeof(bits)}));
    }
    Write(directory, name, bytes.GetData(), bytes.GetSize());
}
void WriteRaw(const std::string& directory, const std::string& name, const Array<math::Vector4>& values)
{
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(values.GetSize() * CHANNELS * sizeof(uint32)) == true);
    for (usize index = 0; index < values.GetSize(); ++index)
    {
        const math::Vector4& value = values[index];
        const StaticArray<float32, CHANNELS> components{{value.X, value.Y, value.Z, value.W}};
        for (usize channel = 0; channel < CHANNELS; ++channel)
        {
            uint32 bits = 0;
            std::memcpy(&bits, &components[channel], sizeof(bits));
            REQUIRE(TryWriteLittleEndian(
                        bits,
                        {bytes.GetData() + (index * CHANNELS + channel) * sizeof(bits), sizeof(bits)}) == true);
        }
    }
    Write(directory, name, bytes.GetData(), bytes.GetSize());
}
void WriteMetadata(const std::string& directory, const std::string& stem, usize operations, const Timings& timings)
{
    constexpr usize HEADER_WORDS = 2;
    constexpr usize RECORD_WORDS = 6;
    constexpr uint64 VERSION = 1;
    const usize stride = RECORD_WORDS * sizeof(uint64) + rhi::GPU_SCOPE_NAME_BYTES;
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(HEADER_WORDS * sizeof(uint64) + operations * stride) == true);
    REQUIRE(TryWriteLittleEndian(VERSION, {bytes.GetData(), sizeof(uint64)}) == true);
    REQUIRE(TryWriteLittleEndian(static_cast<uint64>(operations), {bytes.GetData() + sizeof(uint64), sizeof(uint64)}) ==
            true);
    for (usize index = 0; index < operations; ++index)
    {
        const rhi::GpuPassTiming& pass = timings.Metadata[index];
        uint64 tickBits = 0;
        std::memcpy(&tickBits, &pass.Timing.TickNanoseconds, sizeof(tickBits));
        const StaticArray<uint64, RECORD_WORDS> words{{static_cast<uint64>(pass.Kind),
                                                       pass.Scope.Identity,
                                                       pass.Scope.LogicalPassCount,
                                                       pass.ScopePassIndex,
                                                       pass.Timing.ValidBits,
                                                       tickBits}};
        const usize offset = HEADER_WORDS * sizeof(uint64) + index * stride;
        for (usize word = 0; word < RECORD_WORDS; ++word)
            REQUIRE(TryWriteLittleEndian(words[word],
                                         {bytes.GetData() + offset + word * sizeof(uint64), sizeof(uint64)}) == true);
        std::memcpy(bytes.GetData() + offset + RECORD_WORDS * sizeof(uint64),
                    pass.Scope.Name.GetData(),
                    rhi::GPU_SCOPE_NAME_BYTES);
    }
    Write(directory, stem + ".metadata.bin", bytes.GetData(), bytes.GetSize());
}
void CheckOracle(const Scene& scene, uint32 extent, uint32 samples, const Array<math::Vector4>& linear)
{
    rr::RenderGraphSceneDescription input;
    input.Meshes = {&scene.Mesh, 1};
    input.Items = {&scene.Item, 1};
    input.Inputs = scene.Inputs;
    input.HasMaterialInterface = true;
    input.SceneData = format::SCENE_DATA_ALL;
    rr::ReferenceCaptureDescription description;
    description.Width = description.Height = extent;
    description.TargetSamples = samples;
    description.MaxDepth = 1;
    description.RouletteDepth = 2;
    description.OrthographicCamera = true;
    description.Environment = scene.Inputs.Environment;
    description.Seed = 1;
    core::UniquePtr<internal::ReferenceJob> job;
    REQUIRE(internal::BuildMaterialGraphRayScene(input, description, BUDGET, job) == Status::Ready);
    rr::ReferenceCaptureInfo information;
    math::Vector3 cpuMean{};
    math::Vector3 gpuMean{};
    float64 errorSquared = 0;
    float64 referenceSquared = 0;
    const usize pixels = usize(extent) * extent;
    for (usize pixel = 0; pixel < pixels; ++pixel)
    {
        math::Vector3 expected{};
        for (uint32 sample = 0; sample < samples; ++sample)
        {
            internal::ReferencePath path;
            path.Pixel = static_cast<uint32>(pixel);
            path.Sample = sample;
            REQUIRE(internal::GenerateReferencePrimary(*job, path, path.Ray) == Status::Ready);
            while (path.IsActive == true)
            {
                rhi::RayResult hit;
                REQUIRE(internal::TraceReferenceOracle(job->Scene, path.Ray, hit) == Status::Ready);
                REQUIRE(internal::PrepareReferenceVertex(*job, information, path, hit) == Status::Ready);
                for (uint32 shadow = 0; shadow < path.ShadowCount; ++shadow)
                {
                    rhi::RayResult visibility;
                    REQUIRE(internal::TraceReferenceOracle(job->Scene, path.Shadows[shadow], visibility) ==
                            Status::Ready);
                    if (visibility.Hit == 0)
                        path.Direct = path.Direct + path.PendingDirects[shadow];
                }
                if (path.HasNext == false)
                    break;
                path.Ray = path.Next;
            }
            expected = expected + path.Emission + path.Direct + path.Indirect;
        }
        expected = expected / float32(samples);
        const math::Vector4& value = linear[pixel * WORDS_PER_PIXEL];
        const math::Vector3 actual{value.X / float32(samples), value.Y / float32(samples), value.Z / float32(samples)};
        cpuMean = cpuMean + expected;
        gpuMean = gpuMean + actual;
        const math::Vector3 difference = actual - expected;
        errorSquared += static_cast<float64>(math::Dot(difference, difference));
        referenceSquared += static_cast<float64>(math::Dot(expected, expected));
    }
    cpuMean = cpuMean / float32(pixels);
    gpuMean = gpuMean / float32(pixels);
    CAPTURE(cpuMean.X, cpuMean.Y, cpuMean.Z, gpuMean.X, gpuMean.Y, gpuMean.Z, errorSquared, referenceSquared);
    CHECK(information.InvalidContributions == 0);
    CHECK(math::Abs(gpuMean.X - cpuMean.X) <= MEAN_ABSOLUTE_TOLERANCE + MEAN_RELATIVE_TOLERANCE * cpuMean.X);
    CHECK(math::Abs(gpuMean.Y - cpuMean.Y) <= MEAN_ABSOLUTE_TOLERANCE + MEAN_RELATIVE_TOLERANCE * cpuMean.Y);
    CHECK(math::Abs(gpuMean.Z - cpuMean.Z) <= MEAN_ABSOLUTE_TOLERANCE + MEAN_RELATIVE_TOLERANCE * cpuMean.Z);
    CHECK(errorSquared <= NORMALIZED_RMSE_TOLERANCE * NORMALIZED_RMSE_TOLERANCE * referenceSquared);
}
void Run(bool isMis)
{
    const char* directoryOrNull = std::getenv("LUDUS_GRAPH_WORKGROUP_DIRECTORY");
    const bool isBenchmark = directoryOrNull != nullptr;
    const bool isControl = std::getenv("LUDUS_GRAPH_WORKGROUP_CONTROL") != nullptr;
    const bool shouldReverse = std::getenv("LUDUS_GRAPH_WORKGROUP_REVERSE") != nullptr;
    constexpr uint32 ODD_EXTENT = 67;
    const bool hasOddExtent = std::getenv("LUDUS_GRAPH_WORKGROUP_ODD_EXTENT") != nullptr;
    const uint32 extent = isBenchmark == true ? BENCHMARK_EXTENT : (hasOddExtent == true ? ODD_EXTENT : TEST_EXTENT);
    const uint32 samples = isBenchmark == true ? BENCHMARK_SAMPLES : TEST_SAMPLES;
    const uint32 warmup = isBenchmark == true ? WARMUP : 0;
    for (usize sceneIndex = 0; sceneIndex < SCENE_COUNT; ++sceneIndex)
    {
        CAPTURE(sceneIndex, isMis, isControl);
        Session session(extent);
        rhi::RayCapabilities capabilities;
        if (rhi::GetRayCapabilities(session.Device, capabilities) == Status::Unsupported)
            SKIP("Authored workgroup comparison requires native ray queries; no CPU fallback is selected");
        Scene scene(sceneIndex);
        StaticArray<core::UniquePtr<rr::RenderGraphPackage>, VARIANTS> runtime;
        StaticArray<rr::RenderGraph, VARIANTS> graphs;
        StaticArray<rr::RenderGraphPlan, VARIANTS> plans;
        StaticArray<Timings, VARIANTS> timings;
        Array<uint8> order;
        REQUIRE(order.TryResize(samples - warmup));
        for (usize variant = 0; variant < VARIANTS; ++variant)
        {
            const bool isTiled = variant == 1 && isControl == false;
            rr::RenderGraphPrograms programs;
            if (isMis == true)
            {
                plans[variant] = isTiled == true ? packages::workgroups_veach_guibas_mis_tiled::MakePlan()
                                                 : packages::workgroups_veach_guibas_mis_linear::MakePlan();
                programs = isTiled == true ? packages::workgroups_veach_guibas_mis_tiled::MakePrograms(
                                                 shaders::renderer_composite::Vertex(),
                                                 shaders::renderer_composite::Fragment())
                                           : packages::workgroups_veach_guibas_mis_linear::MakePrograms(
                                                 shaders::renderer_composite::Vertex(),
                                                 shaders::renderer_composite::Fragment());
            }
            else
            {
                plans[variant] = isTiled == true ? packages::workgroups_compute_ray_tracing_tiled::MakePlan()
                                                 : packages::workgroups_compute_ray_tracing_linear::MakePlan();
                programs = isTiled == true ? packages::workgroups_compute_ray_tracing_tiled::MakePrograms(
                                                 shaders::renderer_composite::Vertex(),
                                                 shaders::renderer_composite::Fragment())
                                           : packages::workgroups_compute_ray_tracing_linear::MakePrograms(
                                                 shaders::renderer_composite::Vertex(),
                                                 shaders::renderer_composite::Fragment());
            }
            const char* path = isMis == true ? (isTiled == true ? LUDUS_WORKGROUP_veach_guibas_mis_tiled
                                                                : LUDUS_WORKGROUP_veach_guibas_mis_linear)
                                             : (isTiled == true ? LUDUS_WORKGROUP_compute_ray_tracing_tiled
                                                                : LUDUS_WORKGROUP_compute_ray_tracing_linear);
            runtime[variant] = LoadPackage(path);
            const rr::RenderGraphPrograms decoded =
                runtime[variant]->GetPrograms(shaders::renderer_composite::Vertex(),
                                              shaders::renderer_composite::Fragment());
            CHECK(decoded.RayCompute.Artifact.Msl == programs.RayCompute.Artifact.Msl);
            for (usize axis = 0; axis < format::RAY_WORKGROUP_DIMENSIONS; ++axis)
                CHECK(decoded.RayCompute.WorkgroupSize[axis] == programs.RayCompute.WorkgroupSize[axis]);
            CHECK(runtime[variant]->GetPlan().Passes.GetSize() == plans[variant].Passes.GetSize());
            programs = decoded;
            plans[variant] = runtime[variant]->GetPlan();
            CHECK(programs.RayCompute.WorkgroupSize[0] == (isTiled == true ? 8 : 64));
            CHECK(programs.RayCompute.WorkgroupSize[1] == (isTiled == true ? 8 : 1));
            WaitGraph(graphs[variant],
                      graphs[variant].Initialize(session.Device, plans[variant], programs, nullptr, extent, extent));
            rr::RenderGraphReport report;
            WaitGraph(graphs[variant],
                      graphs[variant].Update({&scene.Mesh, 1},
                                             {&scene.Item, 1},
                                             {&scene.Camera, 1},
                                             1,
                                             rr::RenderGraphCoveragePolicy::RejectIncomplete,
                                             report,
                                             &scene.Inputs,
                                             1));
            REQUIRE(timings[variant].Frame.TryResize(samples - warmup));
            for (usize pass = 0; pass < plans[variant].Passes.GetSize() * 2 - 1; ++pass)
                REQUIRE(timings[variant].Pass[pass].TryResize(samples - warmup));
        }
        const usize passes = plans[0].Passes.GetSize();
        REQUIRE(passes == (isMis == true ? MAX_PASSES : 2));
        for (uint32 sample = 0; sample < samples; ++sample)
        {
            const bool isBaselineFirst = (sample % 2 == 0) != shouldReverse;
            if (sample >= warmup)
                order[sample - warmup] = isBaselineFirst == true ? 1 : 0;
            for (usize turn = 0; turn < VARIANTS; ++turn)
            {
                const usize variant = isBaselineFirst == true ? turn : 1 - turn;
                const Status frameTiming = rhi::ArmGpuTiming(session.Device);
                const Status passTiming = rhi::ArmGpuPassTimings(session.Device);
                if (frameTiming == Status::Unsupported || passTiming == Status::Unsupported)
                    SKIP("Frame and per-pass GPU timestamps are required for this comparison");
                REQUIRE(frameTiming == Status::Ready);
                REQUIRE(passTiming == Status::Ready);
                Render(graphs[variant], session, extent);
                Collect(session, sample >= warmup ? sample - warmup : 0, passes * 2 - 1, timings[variant]);
            }
        }
        const std::string stem = std::string(isMis == true ? "mis-" : "compute-") + SCENES[sceneIndex];
        for (usize pass = 0; pass < passes; ++pass)
        {
            StaticArray<Array<math::Vector4>, VARIANTS> raw;
            for (usize variant = 0; variant < VARIANTS; ++variant)
            {
                rhi::BufferHandle accumulation;
                REQUIRE(internal::RenderGraphRayPass::GetGraphAccumulationForTesting(graphs[variant],
                                                                                     static_cast<uint32>(pass),
                                                                                     accumulation) == Status::Ready);
                REQUIRE(raw[variant].TryResize(usize(extent) * extent * WORDS_PER_PIXEL));
                ReadBuffer(session, accumulation, raw[variant]);
                for (usize pixel = 0; pixel < usize(extent) * extent; ++pixel)
                {
                    uint32 count = 0;
                    std::memcpy(&count, &raw[variant][pixel * WORDS_PER_PIXEL].W, sizeof(count));
                    REQUIRE(count == samples);
                }
                if (isBenchmark == true)
                {
                    WriteRaw(std::string(directoryOrNull) + "/" + VARIANT_NAMES[variant],
                             stem + ".raw" + std::to_string(pass) + ".bin",
                             raw[variant]);
                }
            }
            for (usize pixel = 0; pixel < usize(extent) * extent; ++pixel)
            {
                const math::Vector4& a = raw[0][pixel * WORDS_PER_PIXEL];
                const math::Vector4& b = raw[1][pixel * WORDS_PER_PIXEL];
                CHECK(math::Abs(a.X - b.X) / float32(samples) <= RAW_TOLERANCE);
                CHECK(math::Abs(a.Y - b.Y) / float32(samples) <= RAW_TOLERANCE);
                CHECK(math::Abs(a.Z - b.Z) / float32(samples) <= RAW_TOLERANCE);
            }
            if (isBenchmark == false && pass != 0)
                CheckOracle(scene, extent, samples, raw[1]);
        }
        for (uint32 resource = 0; resource < plans[0].Resources.GetSize(); ++resource)
        {
            if (plans[0].Resources[resource].IsExported == 0)
                continue;
            StaticArray<Array<uint8>, VARIANTS> images;
            for (usize variant = 0; variant < VARIANTS; ++variant)
            {
                rhi::TextureHandle texture;
                rhi::TextureAspect aspect;
                REQUIRE(graphs[variant].GetExport(resource, texture, aspect) == Status::Ready);
                REQUIRE(images[variant].TryResize(usize(extent) * extent * CHANNELS));
                constexpr uint32 ROWS = 16;
                for (uint32 y = 0; y < extent; y += ROWS)
                {
                    const uint32 rows = math::Min(ROWS, extent - y);
                    rhi::ReadbackTicket ticket;
                    Status status = rhi::RequestTextureReadback(session.Device,
                                                                texture,
                                                                {0, 0, y, extent, rows},
                                                                usize(extent) * CHANNELS,
                                                                ticket);
                    const uint64 start = time::NowTicks();
                    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
                        status = rhi::GetStatus(session.Device, ticket);
                    REQUIRE(status == Status::Ready);
                    REQUIRE(rhi::CopyReadback(session.Device,
                                              ticket,
                                              images[variant].GetData() + usize(y) * extent * CHANNELS,
                                              usize(rows) * extent * CHANNELS) == Status::Ready);
                    REQUIRE(rhi::Release(session.Device, ticket) == Status::Ready);
                }
                if (isBenchmark == true)
                    Write(std::string(directoryOrNull) + "/" + VARIANT_NAMES[variant],
                          stem + ".image" + std::to_string(resource) + ".rgba",
                          images[variant].GetData(),
                          images[variant].GetSize());
            }
            CHECK(std::memcmp(images[0].GetData(), images[1].GetData(), images[0].GetSize()) == 0);
        }
        if (isBenchmark == true)
        {
            const std::string directory = directoryOrNull;
            Write(directory, stem + ".order.bin", order.GetData(), order.GetSize());
            for (usize variant = 0; variant < VARIANTS; ++variant)
            {
                const std::string output = directory + "/" + VARIANT_NAMES[variant];
                WriteTimes(output, stem + ".frame.bin", timings[variant].Frame);
                WriteMetadata(output, stem, passes * 2 - 1, timings[variant]);
                for (usize pass = 0; pass < passes * 2 - 1; ++pass)
                    WriteTimes(output, stem + ".pass" + std::to_string(pass) + ".bin", timings[variant].Pass[pass]);
            }
        }
    }
}
} // namespace
TEST_CASE("Compute graph workgroups preserve raw output and CPU reference parity",
          "[gpu][graph-workgroups][compute-workgroups]")
{
    Run(false);
}
TEST_CASE("MIS graph workgroups preserve all outputs and CPU reference parity",
          "[gpu][graph-workgroups][mis-workgroups]")
{
    Run(true);
}
