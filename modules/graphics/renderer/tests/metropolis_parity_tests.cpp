#include "../src/internal/gpu_preview_data.h"
#include "../src/internal/metropolis.h"
#include "metropolis_parity.h"
#include "metropolis_parity_layout.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
namespace
{
using namespace ludus::foundation;
namespace internal = ludus::graphics::renderer::internal;
namespace rhi = ludus::graphics::rhi;
using rhi::RasterStatus;
constexpr uint64 TIMEOUT_NANOSECONDS = 30 * time::NANOSECONDS_PER_SECOND;
constexpr usize READBACK_CHUNK_BYTES = 64 * 1024;
constexpr usize GROUP_COUNT = LUDUS_MH_PARITY_INVOCATIONS / LUDUS_MH_PARITY_GROUP_SIZE;
constexpr usize RESULT_BYTES = LUDUS_MH_PARITY_INVOCATIONS * LUDUS_MH_PARITY_RESULT_WORDS * sizeof(math::Vector4);
constexpr usize CHAIN_BYTES = LUDUS_MH_PARITY_INVOCATIONS * LUDUS_MH_PARITY_CHAIN_WORDS * sizeof(math::Vector4);
constexpr usize FILM_BYTES = LUDUS_MH_PARITY_INVOCATIONS * LUDUS_MH_PARITY_FILM_WORDS * sizeof(math::Vector4);
constexpr usize SURFACE_COUNT = 2;
constexpr uint32 TRIANGLE_COUNT = 3;
constexpr math::RandomKey RANDOM_KEY{0x0123456789abcdefULL};
constexpr math::Vector3 ENVIRONMENT{0.2F, 0.3F, 0.5F};
float32 GetFloat(uint32 bits)
{
    float32 value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
void BuildScene(ludus::content::scene_format::MaterialModel model,
                StaticArray<internal::ReferenceSurfaceData, SURFACE_COUNT>& outSurfaces,
                internal::ReferenceSceneData& outScene,
                Array<math::Vector4>& outPacked)
{
    outSurfaces[0].Bsdf.Scattering.Model = model;
    outSurfaces[0].Bsdf.Scattering.Eta = {{0.2F, 0.4F, 1.5F}};
    outSurfaces[0].Bsdf.Scattering.K = {{3, 2, 1}};
    outSurfaces[0].Bsdf.Scattering.AlphaU = 0.3F;
    outSurfaces[0].Bsdf.Scattering.AlphaV = 0.3F;
    outSurfaces[0].Bsdf.Metallic = 0.5F;
    outSurfaces[0].Bsdf.BaseColor = {0.7F, 0.5F, 0.3F};
    outSurfaces[0].AreAuthoredInputsEnabled = true;
    outSurfaces[1].Bsdf.Scattering.Model = ludus::content::scene_format::MaterialModel::Diffuse;
    outSurfaces[1].Bsdf.BaseColor = {0.25F, 0.25F, 0.25F};
    outSurfaces[1].Emission = {4, 2, 1};
    outSurfaces[1].AreAuthoredInputsEnabled = true;
    REQUIRE(outScene.Instances.TryResize(SURFACE_COUNT) == true);
    outScene.InstanceCount = SURFACE_COUNT;
    outScene.Instances[0].Surface = &outSurfaces[0];
    outScene.Instances[0].FirstTriangle = 0;
    outScene.Instances[0].TriangleCount = 2;
    outScene.Instances[1].Surface = &outSurfaces[1];
    outScene.Instances[1].FirstTriangle = 2;
    outScene.Instances[1].TriangleCount = 1;
    REQUIRE(outScene.Triangles.TryResize(TRIANGLE_COUNT) == true);
    const StaticArray<math::Vector3, TRIANGLE_COUNT * 3> positions{{
        {-10, -10, 0.5F},
        {-10, 10, 0.5F},
        {10, -10, 0.5F},
        {10, -10, 0.5F},
        {-10, 10, 0.5F},
        {10, 10, 0.5F},
        {-1, -1, -0.5F},
        {1, -1, -0.5F},
        {0, 1, -0.5F},
    }};
    const usize surfaceOffset = TRIANGLE_COUNT * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
    REQUIRE(outPacked.TryResize(surfaceOffset + SURFACE_COUNT * LUDUS_GPU_PREVIEW_SURFACE_WORDS) == true);
    for (uint32 index = 0; index < TRIANGLE_COUNT; ++index)
    {
        internal::ReferenceTriangle& triangle = outScene.Triangles[index];
        triangle.Instance = index < 2 ? 0 : 1;
        triangle.Primitive = index < 2 ? index : 0;
        triangle.GeometricNormal = index < 2 ? math::Vector3{0, 0, -1} : math::Vector3{0, 0, 1};
        triangle.Error = {0.00001F, 0.00001F, 0.00001F};
        const usize base = index * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
        for (usize corner = 0; corner < 3; ++corner)
        {
            triangle.Positions[corner] = positions[index * 3 + corner];
            triangle.Normals[corner] = triangle.GeometricNormal;
            triangle.Colors[corner] = {1, 1, 1, 1};
            const math::Vector3 positionWorld = triangle.Positions[corner];
            const math::Vector3 normalWorld = triangle.Normals[corner];
            outPacked[base + corner] = {positionWorld.X, positionWorld.Y, positionWorld.Z, 0};
            outPacked[base + 3 + corner] = {normalWorld.X, normalWorld.Y, normalWorld.Z, 0};
            outPacked[base + 13 + corner] = {1, 1, 1, 1};
        }
        outPacked[base].W = GetFloat(triangle.Instance);
        triangle.Area = 0.5F * math::Length(math::Cross(triangle.Positions[1] - triangle.Positions[0],
                                                        triangle.Positions[2] - triangle.Positions[0]));
        outPacked[base + 1].W = triangle.Area;
        outPacked[base + 2].W = GetFloat(triangle.Primitive);
        outPacked[base + 8] = {triangle.GeometricNormal.X, triangle.GeometricNormal.Y, triangle.GeometricNormal.Z, 0};
        outPacked[base + 9] = {triangle.Error.X, triangle.Error.Y, triangle.Error.Z, 0};
    }
    for (usize index = 0; index < SURFACE_COUNT; ++index)
    {
        const internal::ReferenceSurfaceData& surface = outSurfaces[index];
        const usize base = surfaceOffset + index * LUDUS_GPU_PREVIEW_SURFACE_WORDS;
        outPacked[base] = {surface.Bsdf.BaseColor.X,
                           surface.Bsdf.BaseColor.Y,
                           surface.Bsdf.BaseColor.Z,
                           surface.Bsdf.Metallic};
        outPacked[base + 1].X = surface.Bsdf.Roughness;
        outPacked[base + 2] = surface.Tint;
        outPacked[base + 3] = surface.UvTransform;
        outPacked[base + 4] = {surface.Emission.X, surface.Emission.Y, surface.Emission.Z, 0};
        outPacked[base + 5].W = GetFloat(1);
        const ludus::content::scene_format::MaterialScattering& scattering = surface.Bsdf.Scattering;
        outPacked[base + LUDUS_GPU_PREVIEW_SCATTERING_ETA_WORD] = {scattering.Eta[0],
                                                                   scattering.Eta[1],
                                                                   scattering.Eta[2],
                                                                   GetFloat(static_cast<uint32>(scattering.Model))};
        outPacked[base + LUDUS_GPU_PREVIEW_SCATTERING_K_WORD] = {scattering.K[0],
                                                                 scattering.K[1],
                                                                 scattering.K[2],
                                                                 scattering.AlphaU};
        outPacked[base + LUDUS_GPU_PREVIEW_SCATTERING_ALPHA_WORD] = {scattering.AlphaV,
                                                                     scattering.CoatingEta,
                                                                     scattering.CoatingThickness,
                                                                     0};
    }
    REQUIRE(internal::BuildReferenceBvh(outScene) == RasterStatus::Ready);
}
struct PathContext final
{
    const internal::ReferenceSceneData& Scene;
    uint32 Pixel = 0;
};
rhi::RayInput MakeCameraRay(const PathContext& context, const internal::MetropolisPrimary& primary) noexcept
{
    const float32 x = (static_cast<float32>(context.Pixel) + primary[0]) / LUDUS_MH_PARITY_INVOCATIONS * 2 - 1;
    const float32 y = 1 - primary[1] * 2;
    const float32 maximumDistanceWorld = context.Pixel % 2 == 1 ? 0.25F : 1;
    return internal::ReferenceRay({x, y, 0}, {0, 0, 1}, maximumDistanceWorld);
}
RasterStatus
EvaluatePath(const void* context, const internal::MetropolisPrimary& primary, math::Vector3& outRadiance) noexcept
{
    const PathContext& path = *static_cast<const PathContext*>(context);
    return internal::EvaluateMetropolisPath(path.Scene,
                                            MakeCameraRay(path, primary),
                                            primary,
                                            internal::METROPOLIS_MAXIMUM_DEPTH,
                                            ENVIRONMENT,
                                            outRadiance);
}
template <typename Handle>
void WaitReady(rhi::DeviceHandle device, Handle handle)
{
    const uint64 start = time::NowTicks();
    RasterStatus status = rhi::GetStatus(device, handle);
    while (status == RasterStatus::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
    {
        REQUIRE(rhi::PollLifetime(device) == RasterStatus::Ready);
        status = rhi::GetStatus(device, handle);
    }
    REQUIRE(status == RasterStatus::Ready);
}

bool IsAccepted(RasterStatus status) noexcept
{
    return status == RasterStatus::Ready || status == RasterStatus::Pending;
}

/// Run the estimator kernel once and read back every record. Returns false without a native GPU adapter.
bool RunEstimatorKernel(ludus::content::scene_format::MaterialModel model, Array<uint8>& outBytes)
{
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    static_assert((LUDUS_MH_PARITY_CHAIN_WORDS + LUDUS_MH_PARITY_FILM_WORDS) * sizeof(math::Vector4) ==
                  LUDUS_MH_PIXEL_BYTES);
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.RequiredProfile.Raster.MinBufferSize = RESULT_BYTES;
    description.RequiredProfile.Compute.MinStorageRange = RESULT_BYTES;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        return false;
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    REQUIRE(outBytes.TryResize(RESULT_BYTES, uint8{0}) == true);
    internal::ReferenceSceneData cpuScene;
    StaticArray<internal::ReferenceSurfaceData, SURFACE_COUNT> surfaces;
    Array<math::Vector4> packed;
    BuildScene(model, surfaces, cpuScene, packed);
    internal::GpuPreviewUniform uniform{};
    uniform.Dimensions[0] = LUDUS_MH_PARITY_INVOCATIONS;
    uniform.Dimensions[1] = 1;
    uniform.Counts[0] = TRIANGLE_COUNT;
    uniform.Offsets[3] = TRIANGLE_COUNT * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
    uniform.SurfaceLayout[0] = LUDUS_GPU_PREVIEW_SURFACE_WORDS;
    uniform.Sampling[0] = static_cast<uint32>(RANDOM_KEY.Value);
    uniform.Sampling[1] = static_cast<uint32>(RANDOM_KEY.Value >> 32);
    uniform.Environment = {ENVIRONMENT.X, ENVIRONMENT.Y, ENVIRONMENT.Z, 0};
    uniform.Options.Z = GetFloat(SURFACE_COUNT);
    uniform.Options.W = GetFloat(ludus::content::render_graph_format::SCENE_DATA_ALL);
    const usize sceneBytes = packed.GetSize() * sizeof(math::Vector4);
    Array<uint8> scratch;
    REQUIRE(scratch.TryResize(CHAIN_BYTES, uint8{0}) == true);
    rhi::BufferHandle uniformBuffer;
    rhi::BufferHandle sceneBuffer;
    rhi::BufferHandle chainBuffer;
    rhi::BufferHandle texelBuffer;
    rhi::BufferHandle filmBuffer;
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Uniform, .Size = sizeof(uniform) },
                                         {reinterpret_cast<const uint8*>(&uniform), sizeof(uniform)},
                                         uniformBuffer)) == true);
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = sceneBytes },
                                         {reinterpret_cast<const uint8*>(packed.GetData()), sceneBytes},
                                         sceneBuffer)) == true);
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = CHAIN_BYTES },
                                         scratch.AsSpan(),
                                         chainBuffer)) == true);
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = sizeof(math::Vector4) },
                                         {scratch.GetData(), sizeof(math::Vector4)},
                                         texelBuffer)) == true);
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = FILM_BYTES },
                                         {scratch.GetData(), FILM_BYTES},
                                         filmBuffer)) == true);
    WaitReady(device, filmBuffer);
    WaitReady(device, uniformBuffer);
    WaitReady(device, sceneBuffer);
    WaitReady(device, chainBuffer);
    WaitReady(device, texelBuffer);
    rhi::BufferHandle results;
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = RESULT_BYTES },
                                         {outBytes.GetData(), outBytes.GetSize()},
                                         results)) == true);
    const rhi::RasterShaderDescription shader = ludus::shaders::metropolis_parity::Compute();
    rhi::RasterShaderHandle shaderHandle;
    rhi::BindingLayoutHandle layout;
    REQUIRE(IsAccepted(rhi::CreateRasterShader(device, shader, shaderHandle)) == true);
    REQUIRE(IsAccepted(rhi::CreateBindingLayout(device, shader.Bindings, layout)) == true);
    WaitReady(device, results);
    WaitReady(device, shaderHandle);
    WaitReady(device, layout);
    constexpr usize BINDING_COUNT = 6;
    const StaticArray<rhi::RasterBindingResource, BINDING_COUNT> resources{{
        { .Binding = 0, .Buffer = uniformBuffer, .Size = sizeof(uniform) },
        { .Binding = 1, .Buffer = sceneBuffer, .Size = sceneBytes },
        { .Binding = 4, .Buffer = texelBuffer, .Size = sizeof(math::Vector4) },
        { .Binding = 6, .Buffer = chainBuffer, .Size = CHAIN_BYTES },
        { .Binding = 7, .Buffer = filmBuffer, .Size = FILM_BYTES },
        { .Binding = 8, .Buffer = results, .Size = RESULT_BYTES },
    }};
    rhi::BindingSetHandle set;
    rhi::ComputePipelineHandle pipeline;
    REQUIRE(IsAccepted(rhi::CreateBindingSet(device, layout, resources.AsSpan(), set)) == true);
    REQUIRE(IsAccepted(rhi::CreateComputePipeline(device, { .Shader = shaderHandle, .Layout = layout }, pipeline)) ==
            true);
    WaitReady(device, set);
    WaitReady(device, pipeline);
    rhi::OrderedGraph graph;
    REQUIRE(rhi::CreateOrderedGraph(device, graph) == RasterStatus::Ready);
    rhi::GraphVersion original;
    rhi::GraphVersion version;
    REQUIRE(rhi::ImportGraphBuffer(device, graph, results, original) == RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, original, version) == RasterStatus::Ready);
    rhi::GraphVersion uniformVersion;
    rhi::GraphVersion sceneVersion;
    rhi::GraphVersion texelVersion;
    rhi::GraphVersion initialFilm;
    rhi::GraphVersion filmVersion;
    REQUIRE(rhi::ImportGraphBuffer(device, graph, filmBuffer, initialFilm) == RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, initialFilm, filmVersion) == RasterStatus::Ready);
    rhi::GraphVersion initialChain;
    rhi::GraphVersion chainVersion;
    REQUIRE(rhi::ImportGraphBuffer(device, graph, uniformBuffer, uniformVersion) == RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, sceneBuffer, sceneVersion) == RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, texelBuffer, texelVersion) == RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, chainBuffer, initialChain) == RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, initialChain, chainVersion) == RasterStatus::Ready);
    const StaticArray<rhi::GraphUse, BINDING_COUNT> uses{{
        {uniformVersion, rhi::GraphAccessMode::Uniform, 0, sizeof(uniform), rhi::RasterVisibility::Compute},
        {sceneVersion, rhi::GraphAccessMode::StorageRead, 0, sceneBytes, rhi::RasterVisibility::Compute},
        {texelVersion, rhi::GraphAccessMode::StorageRead, 0, sizeof(math::Vector4), rhi::RasterVisibility::Compute},
        {chainVersion, rhi::GraphAccessMode::StorageReadWrite, 0, CHAIN_BYTES, rhi::RasterVisibility::Compute},
        {filmVersion, rhi::GraphAccessMode::StorageReadWrite, 0, FILM_BYTES, rhi::RasterVisibility::Compute},
        {version, rhi::GraphAccessMode::StorageReadWrite, 0, RESULT_BYTES, rhi::RasterVisibility::Compute},
    }};
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Groups = {static_cast<uint32>(GROUP_COUNT), 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "slice sampling luminaire estimators";
    pass.Uses = uses.GetData();
    pass.UseCount = static_cast<uint32>(uses.GetSize());
    pass.SideEffect = true;
    pass.Dispatch = &dispatch;
    REQUIRE(rhi::AddGraphPass(device, graph, pass) == RasterStatus::Ready);
    rhi::GraphReport graphReport;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, graphReport) == RasterStatus::Ready);
    REQUIRE(rhi::SetFrameTarget(device, surface, {16, 16, 0, 0, 0, 1}) == rhi::DeviceStatus::Ready);
    rhi::SubmissionToken completion;
    RasterStatus executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    const uint64 beginning = time::NowTicks();
    while (executed == RasterStatus::NotReady && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
    {
        executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    }
    REQUIRE(executed == RasterStatus::Ready);
    WaitReady(device, completion);
    for (usize offset = 0; offset < RESULT_BYTES; offset += READBACK_CHUNK_BYTES)
    {
        const usize size = RESULT_BYTES - offset < READBACK_CHUNK_BYTES ? RESULT_BYTES - offset : READBACK_CHUNK_BYTES;
        rhi::ReadbackTicket ticket;
        RasterStatus status = rhi::RequestBufferReadback(device, results, offset, size, ticket);
        const uint64 start = time::NowTicks();
        while (status == RasterStatus::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
        {
            REQUIRE(rhi::PollLifetime(device) == RasterStatus::Ready);
            status = rhi::GetStatus(device, ticket);
        }
        REQUIRE(status == RasterStatus::Ready);
        REQUIRE(rhi::CopyReadback(device, ticket, outBytes.GetData() + offset, size) == RasterStatus::Ready);
        REQUIRE(rhi::Release(device, ticket) == RasterStatus::Ready);
    }
    return true;
}

} // namespace

TEST_CASE("Template MH transport and all weighted estimators match an independent CPU chain",
          "[renderer][gpu][metropolis]")
{
    using ludus::content::scene_format::MaterialModel;
    MaterialModel model = MaterialModel::Diffuse;
    SECTION("typed diffuse") {}
    SECTION("typed conductor")
    {
        model = MaterialModel::Conductor;
    }
    SECTION("legacy metallic roughness")
    {
        model = MaterialModel::MetallicRoughness;
    }
    rhi::Shutdown();
    Array<uint8> bytes;
    if (RunEstimatorKernel(model, bytes) == false)
    {
        rhi::Shutdown();
        SKIP("No native compute adapter");
    }
    internal::ReferenceSceneData scene;
    StaticArray<internal::ReferenceSurfaceData, SURFACE_COUNT> surfaces;
    Array<math::Vector4> packed;
    BuildScene(model, surfaces, scene, packed);
    internal::MetropolisOptions options;
    options.BurnIn = 32;
    constexpr uint32 ESTIMATOR_COUNT = 3;
    constexpr float32 RAW_LINEAR_TOLERANCE = 0.001F;
    for (uint32 pixel = 0; pixel < LUDUS_MH_PARITY_INVOCATIONS; ++pixel)
    {
        const PathContext context{scene, pixel};
        for (uint32 estimator = 0; estimator < ESTIMATOR_COUNT; ++estimator)
        {
            internal::MetropolisChain chain;
            math::Vector3 expected;
            for (uint32 iteration = 0; iteration < LUDUS_MH_PARITY_ITERATIONS; ++iteration)
                REQUIRE(internal::AdvanceMetropolisChain(options,
                                                         static_cast<internal::MetropolisEstimator>(estimator),
                                                         RANDOM_KEY,
                                                         pixel,
                                                         EvaluatePath,
                                                         &context,
                                                         chain,
                                                         expected) == RasterStatus::Ready);
            if (estimator == 0)
            {
                internal::MetropolisPathAovs aovs;
                math::Vector3 replayed;
                REQUIRE(internal::EvaluateMetropolisPath(scene,
                                                         MakeCameraRay(context, chain.Primary),
                                                         chain.Primary,
                                                         internal::METROPOLIS_MAXIMUM_DEPTH,
                                                         ENVIRONMENT,
                                                         replayed,
                                                         nullptr,
                                                         &aovs) == RasterStatus::Ready);
                constexpr usize AOV_COUNT = 3;
                const StaticArray<math::Vector3, AOV_COUNT> expectedAovs{{aovs.Emission, aovs.Direct, aovs.Indirect}};
                for (usize aov = 0; aov < AOV_COUNT; ++aov)
                {
                    math::Vector4 actualAov;
                    const usize aovRecord = pixel * LUDUS_MH_PARITY_RESULT_WORDS + LUDUS_MH_PARITY_EMISSION_WORD + aov;
                    std::memcpy(&actualAov, bytes.GetData() + aovRecord * sizeof(actualAov), sizeof(actualAov));
                    CAPTURE(pixel, aov);
                    CHECK(actualAov.W == 0);
                    CHECK(math::Abs(actualAov.X - expectedAovs[aov].X) < RAW_LINEAR_TOLERANCE);
                    CHECK(math::Abs(actualAov.Y - expectedAovs[aov].Y) < RAW_LINEAR_TOLERANCE);
                    CHECK(math::Abs(actualAov.Z - expectedAovs[aov].Z) < RAW_LINEAR_TOLERANCE);
                }
                CHECK(math::Abs(replayed.X - chain.Radiance.X) < RAW_LINEAR_TOLERANCE);
            }
            math::Vector4 actual;
            const usize record = pixel * LUDUS_MH_PARITY_RESULT_WORDS + estimator;
            std::memcpy(&actual, bytes.GetData() + record * sizeof(actual), sizeof(actual));
            CAPTURE(pixel, estimator, actual.X, actual.Y, actual.Z, expected.X, expected.Y, expected.Z);
            CHECK(actual.W == 0);
            CHECK(math::Abs(actual.X - expected.X) < RAW_LINEAR_TOLERANCE);
            CHECK(math::Abs(actual.Y - expected.Y) < RAW_LINEAR_TOLERANCE);
            CHECK(math::Abs(actual.Z - expected.Z) < RAW_LINEAR_TOLERANCE);
        }
        math::Vector4 lifecycle;
        const usize record = pixel * LUDUS_MH_PARITY_RESULT_WORDS + ESTIMATOR_COUNT;
        std::memcpy(&lifecycle, bytes.GetData() + record * sizeof(lifecycle), sizeof(lifecycle));
        CHECK(lifecycle.X == 0.25F);
        CHECK(lifecycle.Y == 1.75F);
        CHECK(lifecycle.Z == 0);
        CHECK(lifecycle.W == 0);
    }
    rhi::Shutdown();
}
