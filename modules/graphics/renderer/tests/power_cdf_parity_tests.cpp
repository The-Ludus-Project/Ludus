#include "../src/internal/gpu_preview_data.h"
#include "../src/internal/reference_data.h"
#include "power_cdf_parity.h"
#include "power_cdf_parity_layout.h"

#include <ludus/content/render_graph_format/scene_data.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <span>

// Native execution of scene_shader.slang's power-CDF accessors against the CPU reference
// accessors over the one packed CDF that RT1 and graph shaders share.
namespace
{
using namespace ludus::foundation;
namespace internal = ludus::graphics::renderer::internal;
namespace format = ludus::content::render_graph_format;
namespace rhi = ludus::graphics::rhi;
using S = rhi::RasterStatus;

constexpr uint64 TIMEOUT_NANOSECONDS = 10 * time::NANOSECONDS_PER_SECOND;
constexpr uint32 EXTENT_PIXELS = 64;
constexpr uint32 INVOCATION_COUNT = EXTENT_PIXELS * EXTENT_PIXELS;
constexpr usize RESULT_BYTES = usize{INVOCATION_COUNT} * sizeof(uint32) * 4;
// Per-request readback limit; well below the default 256 KiB transfer profile.
constexpr usize READBACK_CHUNK_BYTES = usize{16} * 1024;
// Fewer triangles than invocations, so PMF queries also cover indices past the scene.
constexpr uint32 TRIANGLE_COUNT = 1500;
constexpr usize SURFACE_COUNT = 8;
constexpr uint32 SAMPLE_INDEX = 3;
constexpr math::RandomKey RANDOM_KEY{0x0123456789abcdefULL};
// Metal compiles with fast math, so the GPU's binary32 division may differ from the
// correctly rounded C++ quotient by a few units in the last place.
constexpr float32 PMF_RELATIVE_TOLERANCE = 1.0e-6F;

template <typename Handle>
void WaitReady(rhi::DeviceHandle device, Handle handle)
{
    const uint64 start = time::NowTicks();
    S status = rhi::GetStatus(device, handle);
    while (status == S::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
    {
        REQUIRE(rhi::PollLifetime(device) == S::Ready);
        status = rhi::GetStatus(device, handle);
    }
    REQUIRE(status == S::Ready);
}

bool IsAccepted(S status) noexcept
{
    return status == S::Ready || status == S::Pending;
}

float32 GetFloat(uint32 bits) noexcept
{
    float32 value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Emitting surfaces span six orders of magnitude, a colored one, a powerless negative one
// and one without emission; triangle areas include a zero and a huge one, so faint emitters
// fall below one quantization unit and keep the floor of one.
void BuildScene(bool isPowerless,
                StaticArray<internal::ReferenceSurfaceData, SURFACE_COUNT>& outSurfaces,
                internal::ReferenceSceneData& outScene)
{
    const StaticArray<math::Vector3, SURFACE_COUNT> emissions{{
        {1000, 1000, 1000},
        {4, 4, 4},
        {0, 0, 0},
        {.001F, .002F, .003F},
        {-4, 0, 0},
        {10, 0, 2},
        {0, 0, 0},
        {.5F, .25F, 1},
    }};
    for (usize i = 0; i < SURFACE_COUNT; ++i)
    {
        outSurfaces[i].Emission = isPowerless == true ? math::Vector3{-1, 0, -1} : emissions[i];
    }
    REQUIRE(outScene.Instances.TryResize(SURFACE_COUNT) == true);
    for (usize i = 0; i < SURFACE_COUNT; ++i)
    {
        outScene.Instances[i].Surface = &outSurfaces[i];
    }
    outScene.InstanceCount = SURFACE_COUNT;
    REQUIRE(outScene.Triangles.TryResize(TRIANGLE_COUNT) == true);
    for (uint32 i = 0; i < TRIANGLE_COUNT; ++i)
    {
        internal::ReferenceTriangle& triangle = outScene.Triangles[i];
        triangle.Instance = (i * 5 + i / 7) % SURFACE_COUNT;
        triangle.Area = i == 11 ? 0.0F : i == 400 ? 1.0e6F : .001F + static_cast<float32>(i % 13) * .37F;
        // The proposal list holds every surface with a nonzero channel, including powerless ones.
        const math::Vector3 emission = emissions[triangle.Instance];
        if (emission.X != 0 || emission.Y != 0 || emission.Z != 0)
        {
            REQUIRE(outScene.Emitters.TryAdd(i) == true);
        }
    }
    REQUIRE(internal::BuildReferenceEmitterSelection(outScene) == S::Ready);
}

struct ParityConfiguration final
{
    uint32 EmitterOffset = 0;
    uint32 EmitterCount = 0;
    format::SceneDataValue SceneData = format::SCENE_DATA_ALL;
};

// One dispatch of the parity kernel over a host-packed scene; returns the raw result words.
void RunParity(rhi::DeviceHandle device,
               rhi::SurfaceHandle surface,
               rhi::BufferHandle sceneBuffer,
               usize sceneBytes,
               const ParityConfiguration& configuration,
               Array<uint32>& outWords)
{
    internal::GpuPreviewUniform uniform{};
    uniform.Dimensions[0] = EXTENT_PIXELS;
    uniform.Dimensions[1] = EXTENT_PIXELS;
    uniform.Dimensions[2] = SAMPLE_INDEX;
    uniform.Offsets[2] = configuration.EmitterOffset;
    uniform.Counts[2] = configuration.EmitterCount;
    uniform.Sampling[0] = static_cast<uint32>(RANDOM_KEY.Value);
    uniform.Sampling[1] = static_cast<uint32>(RANDOM_KEY.Value >> 32);
    std::memcpy(&uniform.Options.W, &configuration.SceneData, sizeof(configuration.SceneData));

    rhi::BufferHandle uniformBuffer;
    rhi::BufferHandle resultBuffer;
    REQUIRE(outWords.TryResize(RESULT_BYTES / sizeof(uint32), uint32{0}) == true);
    REQUIRE(
        IsAccepted(rhi::CreateBuffer(device,
                                     { .Role = rhi::BufferRole::Uniform, .Size = sizeof(uniform) },
                                     std::span<const uint8>{reinterpret_cast<const uint8*>(&uniform), sizeof(uniform)},
                                     uniformBuffer)) == true);
    REQUIRE(IsAccepted(rhi::CreateBuffer(
                device,
                { .Role = rhi::BufferRole::Storage, .Size = RESULT_BYTES },
                std::span<const uint8>{reinterpret_cast<const uint8*>(outWords.GetData()), RESULT_BYTES},
                resultBuffer)) == true);
    const rhi::RasterShaderDescription shader = ludus::shaders::power_cdf_parity::Compute();
    rhi::RasterShaderHandle shaderHandle;
    rhi::BindingLayoutHandle layout;
    REQUIRE(IsAccepted(rhi::CreateRasterShader(device, shader, shaderHandle)) == true);
    REQUIRE(IsAccepted(rhi::CreateBindingLayout(device, shader.Bindings, layout)) == true);
    WaitReady(device, uniformBuffer);
    WaitReady(device, resultBuffer);
    WaitReady(device, shaderHandle);
    WaitReady(device, layout);
    const rhi::RasterBindingResource resources[] = {
        { .Binding = 0, .Buffer = uniformBuffer, .Size = sizeof(uniform) },
        { .Binding = 1, .Buffer = sceneBuffer, .Size = sceneBytes },
        { .Binding = LUDUS_POWER_CDF_PARITY_RESULT_BINDING, .Buffer = resultBuffer, .Size = RESULT_BYTES },
    };
    rhi::BindingSetHandle set;
    rhi::ComputePipelineHandle pipeline;
    REQUIRE(IsAccepted(rhi::CreateBindingSet(device, layout, resources, set)) == true);
    REQUIRE(IsAccepted(rhi::CreateComputePipeline(device, { .Shader = shaderHandle, .Layout = layout }, pipeline)) ==
            true);
    WaitReady(device, set);
    WaitReady(device, pipeline);

    rhi::OrderedGraph graph;
    REQUIRE(rhi::CreateOrderedGraph(device, graph) == S::Ready);
    rhi::GraphVersion uniformVersion;
    rhi::GraphVersion sceneVersion;
    rhi::GraphVersion initialResults;
    rhi::GraphVersion results;
    REQUIRE(rhi::ImportGraphBuffer(device, graph, uniformBuffer, uniformVersion) == S::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, sceneBuffer, sceneVersion) == S::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, resultBuffer, initialResults) == S::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, initialResults, results) == S::Ready);
    const rhi::GraphUse uses[] = {
        {uniformVersion, rhi::GraphAccessMode::Uniform, 0, sizeof(uniform), rhi::RasterVisibility::Compute},
        {sceneVersion, rhi::GraphAccessMode::StorageRead, 0, sceneBytes, rhi::RasterVisibility::Compute},
        {results, rhi::GraphAccessMode::StorageReadWrite, 0, RESULT_BYTES, rhi::RasterVisibility::Compute},
    };
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Groups = {INVOCATION_COUNT / LUDUS_POWER_CDF_PARITY_GROUP_SIZE, 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "power CDF parity";
    pass.Uses = uses;
    pass.UseCount = sizeof(uses) / sizeof(uses[0]);
    pass.SideEffect = true;
    pass.Dispatch = &dispatch;
    REQUIRE(rhi::AddGraphPass(device, graph, pass) == S::Ready);
    rhi::GraphReport report;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, report) == S::Ready);
    REQUIRE(rhi::SetFrameTarget(device, surface, {16, 16, 0, 0, 0, 1}) == rhi::DeviceStatus::Ready);
    rhi::SubmissionToken completion;
    const uint64 start = time::NowTicks();
    S executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    while (executed == S::NotReady && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
    {
        executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    }
    REQUIRE(executed == S::Ready);
    WaitReady(device, completion);
    uint8* bytes = reinterpret_cast<uint8*>(outWords.GetData());
    for (usize offset = 0; offset < RESULT_BYTES; offset += READBACK_CHUNK_BYTES)
    {
        const usize size = RESULT_BYTES - offset < READBACK_CHUNK_BYTES ? RESULT_BYTES - offset : READBACK_CHUNK_BYTES;
        rhi::ReadbackTicket ticket;
        S readback = rhi::RequestBufferReadback(device, resultBuffer, offset, size, ticket);
        const uint64 readbackStart = time::NowTicks();
        while (readback == S::Pending && time::NowTicks() - readbackStart < TIMEOUT_NANOSECONDS)
        {
            REQUIRE(rhi::PollLifetime(device) == S::Ready);
            readback = rhi::GetStatus(device, ticket);
        }
        REQUIRE(readback == S::Ready);
        REQUIRE(rhi::CopyReadback(device, ticket, bytes + offset, size) == S::Ready);
        REQUIRE(rhi::Release(device, ticket) == S::Ready);
    }
}

bool IsPmfClose(float32 actual, float32 expected) noexcept
{
    return expected == 0 ? actual == 0 : math::Abs(actual - expected) <= expected * PMF_RELATIVE_TOLERANCE;
}

// Compares invocation results with the CPU accessors. A missing lights group expects no sample.
void CheckParity(const internal::ReferenceSceneData& scene, bool hasLights, const Array<uint32>& words)
{
    uint32 sampleCount = 0;
    for (uint32 invocation = 0; invocation < INVOCATION_COUNT; ++invocation)
    {
        INFO("invocation " << invocation);
        const uint32 dimension = invocation % LUDUS_POWER_CDF_PARITY_INVALID_PERIOD == 0
                                     ? math::RandomMaxDimension + 1
                                     : LUDUS_POWER_CDF_PARITY_FIRST_TICKET_DIMENSION +
                                           2 * (invocation % LUDUS_POWER_CDF_PARITY_TICKET_COUNT);
        internal::ReferenceEmissiveTriangleSelection expected;
        float32 expectedPmf = 0;
        if (hasLights == true)
        {
            // The CPU address cannot spell the GPU's out-of-layout dimension, which selects nothing.
            if (dimension <= math::RandomMaxDimension)
            {
                expected = internal::SelectReferenceEmissiveTriangle(
                    scene,
                    RANDOM_KEY,
                    { .Scope = invocation, .Event = SAMPLE_INDEX, .Dimension = static_cast<uint16>(dimension) });
            }
            expectedPmf = internal::EvaluateReferenceEmissiveTriangleSelectionPmf(scene, invocation);
        }
        const uint32* result = words.GetData() + usize{invocation} * 4;
        REQUIRE(result[2] == (expected.HasSample == true ? 1U : 0U));
        REQUIRE(result[0] == expected.TriangleIndex);
        REQUIRE(IsPmfClose(GetFloat(result[1]), expected.SelectionPmf) == true);
        REQUIRE(IsPmfClose(GetFloat(result[3]), expectedPmf) == true);
        if (expected.HasSample == true)
        {
            ++sampleCount;
            // The selected triangle's PMF is the one EvaluateEmissiveTriangleSelectionPmf reports.
            REQUIRE(expected.SelectionPmf ==
                    internal::EvaluateReferenceEmissiveTriangleSelectionPmf(scene, expected.TriangleIndex));
        }
    }
    if (hasLights == true && scene.EmitterCdf[scene.EmitterCdf.GetSize() - 1] != 0)
    {
        REQUIRE(sampleCount == INVOCATION_COUNT - (INVOCATION_COUNT + LUDUS_POWER_CDF_PARITY_INVALID_PERIOD - 1) /
                                                      LUDUS_POWER_CDF_PARITY_INVALID_PERIOD);
    }
    else
    {
        REQUIRE(sampleCount == 0);
    }
}
} // namespace

TEST_CASE("GPU power-CDF scene accessors match the CPU reference over the shared packed CDF",
          "[renderer][gpu][power-cdf]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;

    StaticArray<internal::ReferenceSurfaceData, SURFACE_COUNT> surfaces;
    internal::ReferenceSceneData scene;
    BuildScene(false, surfaces, scene);
    StaticArray<internal::ReferenceSurfaceData, SURFACE_COUNT> powerlessSurfaces;
    internal::ReferenceSceneData powerless;
    BuildScene(true, powerlessSurfaces, powerless);
    REQUIRE(scene.Emitters.GetSize() > 0);
    REQUIRE(powerless.EmitterCdf[powerless.EmitterCdf.GetSize() - 1] == 0);

    // Both scenes' emitter words in one buffer, packed by the production packer.
    const usize emitterCount = scene.Emitters.GetSize();
    Array<math::Vector4> sceneWords;
    REQUIRE(sceneWords.TryResize(emitterCount * 2) == true);
    REQUIRE(internal::PackReferenceEmitterSelection(scene, {sceneWords.GetData(), emitterCount}) == true);
    REQUIRE(internal::PackReferenceEmitterSelection(powerless, {sceneWords.GetData() + emitterCount, emitterCount}) ==
            true);
    const usize sceneBytes = sceneWords.GetSize() * sizeof(math::Vector4);

    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.RequiredProfile.Raster.MinBufferSize = RESULT_BYTES;
    description.RequiredProfile.Compute.MinStorageRange = RESULT_BYTES;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No native GPU adapter");
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rhi::BufferHandle sceneBuffer;
    REQUIRE(IsAccepted(rhi::CreateBuffer(
                device,
                { .Role = rhi::BufferRole::Storage, .Size = sceneBytes },
                std::span<const uint8>{reinterpret_cast<const uint8*>(sceneWords.GetData()), sceneBytes},
                sceneBuffer)) == true);
    WaitReady(device, sceneBuffer);

    Array<uint32> words;
    SECTION("A scene with emitted power selects and evaluates exactly the CPU triangles")
    {
        RunParity(device,
                  surface,
                  sceneBuffer,
                  sceneBytes,
                  { .EmitterOffset = 0, .EmitterCount = static_cast<uint32>(emitterCount) },
                  words);
        CheckParity(scene, true, words);
    }
    SECTION("A Scene connection without lights has no emissive triangle CDF")
    {
        RunParity(device,
                  surface,
                  sceneBuffer,
                  sceneBytes,
                  {
                      .EmitterOffset = 0,
                      .EmitterCount = static_cast<uint32>(emitterCount),
                      .SceneData = format::SCENE_DATA_GEOMETRY | format::SCENE_DATA_MATERIALS,
                  },
                  words);
        CheckParity(scene, false, words);
    }
    SECTION("Emitters without emitted power return no sample and zero probability")
    {
        RunParity(
            device,
            surface,
            sceneBuffer,
            sceneBytes,
            { .EmitterOffset = static_cast<uint32>(emitterCount), .EmitterCount = static_cast<uint32>(emitterCount) },
            words);
        CheckParity(powerless, true, words);
    }
}
