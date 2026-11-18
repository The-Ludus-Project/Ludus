#include "hybrid_diffuse_scene.h"
#include "render_graph_gpu_fixture.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_lit.h"
#include "renderer_lit_shadow.h"
#include "renderer_output.h"
#include "renderer_reference.h"
#include "renderer_reference_reduce.h"
#include "renderer_shadow.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/scene_shader.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/gpu_scope.h>

#include <bit>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::graphics::renderer::testing;
using namespace ludus::tests::hybrid_diffuse;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace shaders = ludus::shaders;

namespace
{
using Status = rhi::RasterStatus;
constexpr uint64 TIMEOUT = LUDUS_RENDERER_TEST_COLD_GPU_TIMEOUT_SECONDS * time::NANOSECONDS_PER_SECOND;
constexpr usize PIXEL_COUNT = usize{WIDTH} * HEIGHT;
constexpr usize ACCUMULATION_WORD_COUNT = PIXEL_COUNT * renderer::RENDER_GRAPH_ACCUMULATION_WORDS;
using Image = StaticArray<math::Vector3, PIXEL_COUNT>;
constexpr PackageLocation INDIRECT_PACKAGE{LUDUS_RG3_HYBRID_INDIRECT_DIFFUSE_DIRECTORY,
                                           "rg2_hybrid_indirect_diffuse.lurgrt"};
constexpr PackageLocation RASTER_PACKAGE{LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"};
constexpr uint32 RAY_PASS_INDEX = 1;

struct NativeDevice final
{
    RhiGuard Guard;
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    bool HasRays = false;

    explicit NativeDevice(bool shouldRequestRays, bool shouldTimeGpu = false)
    {
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Required.Compute = true;
        description.Preferred.RayQueries = shouldRequestRays;
        description.Storage.ResourcesPerGraph = renderer::VISIBILITY_GRAPH_RESOURCE_CAPACITY;
        description.EnableGpuTiming = shouldTimeGpu;
        rhi::StartupInfo failure;
        const rhi::DeviceStatus status =
            rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, Device, Surface, &failure);
        if (status != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
        {
            SKIP("No GPU adapter");
        }
        REQUIRE(status == rhi::DeviceStatus::Ready);
        rhi::RayCapabilities rays;
        HasRays = rhi::GetRayCapabilities(Device, rays) == Status::Ready;
    }
};

template <typename Poll>
Status WaitForStatus(Status status, Poll poll)
{
    const uint64 beginning = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - beginning < TIMEOUT)
    {
        threading::YieldThread();
        status = poll();
    }
    return status;
}

Status StageRoom(renderer::RenderGraph& inoutGraph, const Room& room, uint64 revision)
{
    renderer::RenderGraphReport report;
    return inoutGraph.Update(room.Meshes,
                             room.Items,
                             {&room.Camera, 1},
                             revision,
                             renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                             report,
                             &room.Inputs,
                             0,
                             room.Revisions);
}

void PrepareGraph(const NativeDevice& device,
                  const renderer::RenderGraphPackage& package,
                  renderer::RenderGraph& outGraph)
{
    const renderer::RenderGraphPrograms programs =
        package.GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(WaitForStatus(outGraph.Initialize(device.Device, package.GetPlan(), programs, nullptr, WIDTH, HEIGHT),
                          [&outGraph] { return outGraph.Poll(); }) == Status::Ready);
}

void RenderFrame(const NativeDevice& device, renderer::RenderGraph& inoutGraph)
{
    rhi::SubmissionToken completion;
    REQUIRE(inoutGraph.Render(device.Surface, WIDTH, HEIGHT, completion) == Status::Ready);
    REQUIRE(WaitForStatus(Status::Pending, [&] { return rhi::GetStatus(device.Device, completion); }) == Status::Ready);
}

void ReadMean(const NativeDevice& device, const renderer::RenderGraph& graph, uint32 frame, Image& outImage)
{
    rhi::BufferHandle buffer;
    REQUIRE(graph.GetAccumulation(RAY_PASS_INDEX, buffer) == Status::Ready);
    Array<math::Vector4> records;
    REQUIRE(records.TryResize(ACCUMULATION_WORD_COUNT) == true);
    rhi::LifetimeCapabilities capabilities;
    REQUIRE(rhi::GetLifetimeCapabilities(device.Device, capabilities) == Status::Ready);
    constexpr usize totalBytes = ACCUMULATION_WORD_COUNT * sizeof(math::Vector4);
    for (usize offset = 0; offset < totalBytes; offset += capabilities.TransferBytes)
    {
        const usize bytes = math::Min(capabilities.TransferBytes, totalBytes - offset);
        rhi::ReadbackTicket ticket;
        const Status requested = rhi::RequestBufferReadback(device.Device, buffer, offset, bytes, ticket);
        REQUIRE(WaitForStatus(requested, [&] { return rhi::GetStatus(device.Device, ticket); }) == Status::Ready);
        REQUIRE(rhi::CopyReadback(device.Device, ticket, reinterpret_cast<uint8*>(records.GetData()) + offset, bytes) ==
                Status::Ready);
        REQUIRE(rhi::Release(device.Device, ticket) == Status::Ready);
    }
    for (usize pixel = 0; pixel < PIXEL_COUNT; ++pixel)
    {
        const math::Vector4& sum = records[pixel * renderer::RENDER_GRAPH_ACCUMULATION_WORDS];
        REQUIRE(std::bit_cast<uint32>(sum.W) == frame);
        outImage[pixel] = {sum.X / static_cast<float32>(frame),
                           sum.Y / static_cast<float32>(frame),
                           sum.Z / static_cast<float32>(frame)};
    }
}

void RenderReference(const NativeDevice& device,
                     renderer::ReferenceTraversal traversal,
                     uint32 samples,
                     StaticArray<Image, STATE_COUNT>& outImages)
{
    constexpr usize MEMORY_BUDGET_BYTES = 64 * 1024 * 1024;
    constexpr uint32 TILE_PIXELS = 2048;
    constexpr uint32 SCATTERING_EVENTS = 2;
    renderer::Renderer reference;
    REQUIRE(reference.Initialize(device.Device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment()) ==
            Status::Ready);
    const renderer::ReferenceRenderingOptions options{traversal,
                                                      MEMORY_BUDGET_BYTES,
                                                      0,
                                                      TILE_PIXELS,
                                                      MEMORY_BUDGET_BYTES};
    const Status enabled = traversal == renderer::ReferenceTraversal::GpuCompute
                               ? reference.EnableReferenceRendering(shaders::renderer_reference::Compute(),
                                                                    shaders::renderer_reference_reduce::Compute(),
                                                                    options)
                               : reference.EnableReferenceRendering({}, options);
    REQUIRE(enabled == Status::Ready);
    REQUIRE(reference.InitializeViews(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()) ==
            Status::Ready);
    REQUIRE(reference.InitializeLighting(shaders::renderer_output::Vertex(),
                                         shaders::renderer_output::Fragment(),
                                         shaders::renderer_shadow::Fragment()) == Status::Ready);
    renderer::MaterialProgram program;
    const Status warmed = reference.PrewarmLitProgram(shaders::renderer_lit::Vertex(),
                                                      shaders::renderer_lit::Fragment(),
                                                      shaders::renderer_lit_shadow::Fragment(),
                                                      program);
    REQUIRE(WaitForStatus(warmed, [&] { return reference.GetStatus(program); }) == Status::Ready);

    Room room;
    StaticArray<renderer::Mesh, QUAD_COUNT> meshes{};
    for (usize quad = 0; quad < QUAD_COUNT; ++quad)
    {
        const Status created = reference.CreateMesh(room.Meshes[quad].Geometry, meshes[quad]);
        REQUIRE(WaitForStatus(created, [&] { return reference.GetStatus(meshes[quad]); }) == Status::Ready);
    }

    renderer::ReferenceCaptureDescription capture;
    capture.Width = WIDTH;
    capture.Height = HEIGHT;
    capture.OrthographicCamera = true;
    capture.CenterRays = true;
    capture.TargetSamples = samples;
    capture.MaxDepth = SCATTERING_EVENTS;
    capture.RouletteDepth = SCATTERING_EVENTS;
    Array<renderer::ReferencePixel> pixels;
    REQUIRE(pixels.TryResize(PIXEL_COUNT) == true);
    for (usize state = 0; state < STATE_COUNT; ++state)
    {
        room.Apply(STATES[state]);
        StaticArray<renderer::MaterialVersion, QUAD_COUNT> materials{};
        StaticArray<renderer::SceneItem, QUAD_COUNT> items{};
        for (usize quad = 0; quad < QUAD_COUNT; ++quad)
        {
            const renderer::SceneShaderMaterial& material = room.Materials[quad];
            renderer::LitMaterialDescription description;
            description.Roughness = 1;
            description.Metallic = 0;
            description.Scattering = material.Scattering;
            description.Surface.Tint = {material.BaseColor.X, material.BaseColor.Y, material.BaseColor.Z, 1};
            description.Emission = material.Emission;
            description.IsEmissionTwoSided = material.IsEmissionTwoSided;
            renderer::LightSelection selection;
            const Status created = reference.CreateLitMaterial(program, description, materials[quad], selection);
            REQUIRE(WaitForStatus(created, [&] { return reference.GetStatus(materials[quad]); }) == Status::Ready);
            items[quad].Geometry = meshes[quad];
            items[quad].Surface = materials[quad];
            items[quad].SourceId = room.Items[quad].SourceId;
            items[quad].Transform = room.Items[quad].MeshToWorld;
        }
        renderer::Snapshot snapshot;
        REQUIRE(reference.CreateSnapshot(items.GetData(), items.GetSize(), snapshot) == Status::Ready);
        renderer::ReferenceCapture handle;
        const Status started = reference.BeginReferenceCapture(snapshot, capture, handle);
        REQUIRE((started == Status::Ready || started == Status::Pending));
        REQUIRE(WaitForStatus(Status::Pending, [&] {
                    return reference.AdvanceReferenceCapture(handle, device.Surface);
                }) == Status::Ready);
        REQUIRE(reference.CopyReferencePixels(handle, pixels.GetData(), pixels.GetSize()) == Status::Ready);
        for (usize pixel = 0; pixel < PIXEL_COUNT; ++pixel)
        {
            REQUIRE(pixels[pixel].Samples == samples);
            REQUIRE(pixels[pixel].Direct == math::Vector3{});
            REQUIRE(pixels[pixel].Emission == math::Vector3{});
            outImages[state][pixel] = pixels[pixel].Indirect;
        }
        REQUIRE(reference.Release(handle) == Status::Ready);
        REQUIRE(reference.Release(snapshot) == Status::Ready);
        for (renderer::MaterialVersion& material : materials)
        {
            REQUIRE(reference.Release(material) == Status::Ready);
        }
    }
}

struct Error final
{
    float64 SignedMean = 0;
    float64 Rms = 0;
    float32 Peak = 0;
    usize Count = 0;
};

// Red is the highest-energy channel. CheckDiffuseColor checks the other two against it.
Error MeasureRedError(const Image& image,
                      const Image& target,
                      const Image* priorOrNull = nullptr,
                      bool isChanged = true)
{
    constexpr float32 CHANGE_THRESHOLD = .015F;
    Error metric;
    for (usize pixel = 0; pixel < PIXEL_COUNT; ++pixel)
    {
        if (priorOrNull != nullptr &&
            (math::Abs(target[pixel].X - (*priorOrNull)[pixel].X) >= CHANGE_THRESHOLD) != isChanged)
        {
            continue;
        }
        const float64 error = static_cast<float64>(image[pixel].X) - static_cast<float64>(target[pixel].X);
        metric.SignedMean += error;
        metric.Rms += error * error;
        metric.Peak = math::Max(metric.Peak, static_cast<float32>(math::Abs(error)));
        ++metric.Count;
    }
    REQUIRE(metric.Count > 0);
    metric.SignedMean /= static_cast<float64>(metric.Count);
    metric.Rms = math::Sqrt(metric.Rms / static_cast<float64>(metric.Count));
    return metric;
}

void CheckDiffuseColor(const Image& image)
{
    constexpr float32 TOLERANCE = 1e-6F;
    for (const math::Vector3 pixel : image)
    {
        REQUIRE(math::Abs(pixel.Y - pixel.X * (REFLECTOR_ALBEDO.Y / REFLECTOR_ALBEDO.X)) <= TOLERANCE);
        REQUIRE(math::Abs(pixel.Z - pixel.X * (REFLECTOR_ALBEDO.Z / REFLECTOR_ALBEDO.X)) <= TOLERANCE);
    }
}

// Thanks to Alexander Keller, "Instant Radiosity", SIGGRAPH 1997, pp. 49–56, §§3.2–3.3,
// https://doi.org/10.1145/258734.258769, for rendering one-bounce diffuse transport as visible
// virtual point lights. The test traces one step from a uniform rectangular source and measures
// the loss from clamping the near-source denominator separately from the unclamped estimate.
math::Vector3d Widen(math::Vector3 value)
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

bool ContainsPoint(const Corners& quad, math::Vector3d positionWorld, float64 offsetWorld = 0)
{
    const math::Vector3d lowerWorld = Widen(quad[0]);
    const math::Vector3d upperWorld = Widen(quad[2]);
    return positionWorld.X >= lowerWorld.X + offsetWorld && positionWorld.X <= upperWorld.X + offsetWorld &&
           positionWorld.Y >= lowerWorld.Y && positionWorld.Y <= upperWorld.Y;
}

bool IsBlocked(const Room& room, math::Vector3d firstWorld, math::Vector3d secondWorld)
{
    const float64 blockerDepthWorld = static_cast<float64>(room.Positions[BLOCKER][0].Z);
    const float64 parameter = (blockerDepthWorld - firstWorld.Z) / (secondWorld.Z - firstWorld.Z);
    return parameter > 0 && parameter < 1 &&
           ContainsPoint(room.Positions[BLOCKER],
                         firstWorld + (secondWorld - firstWorld) * parameter,
                         static_cast<float64>(room.Items[BLOCKER].MeshToWorld.Translation.X));
}

math::Vector3d SampleEmitter(const Room& room, float64 horizontal, float64 vertical)
{
    const math::Vector3d lowerWorld = Widen(room.Positions[EMITTER][0]);
    const math::Vector3d extentWorld = Widen(room.Positions[EMITTER][2]) - lowerWorld;
    return lowerWorld + math::Vector3d{extentWorld.X * horizontal, extentWorld.Y * vertical, 0};
}

math::Vector3d SampleCosineDirection(float64 radiusSample, float64 angleSample)
{
    const float64 radius = math::Sqrt(radiusSample);
    const float64 angle = math::kTau * angleSample;
    return {radius * math::Cos(angle), radius * math::Sin(angle), -math::Sqrt(1 - radiusSample)};
}

float64 RadicalInverse(usize index, usize base)
{
    const float64 inverseBase = 1 / static_cast<float64>(base);
    float64 weight = inverseBase;
    float64 value = 0;
    while (index > 0)
    {
        value += static_cast<float64>(index % base) * weight;
        index /= base;
        weight *= inverseBase;
    }
    return value;
}

void TraceVirtualLights(const Room& room, usize budget, Array<math::Vector3d>& outLightsWorld)
{
    constexpr usize EMITTER_HORIZONTAL_BASE = 2;
    constexpr usize EMITTER_VERTICAL_BASE = 3;
    constexpr usize COSINE_RADIUS_BASE = 5;
    constexpr usize COSINE_ANGLE_BASE = 7;
    REQUIRE(outLightsWorld.TryEnsureCapacity(budget) == true);
    const Corners& reflector = room.Positions[REFLECTOR];
    const float64 reflectorDepthWorld = static_cast<float64>(reflector[0].Z);
    for (usize index = 1; index <= budget; ++index)
    {
        const math::Vector3d originWorld = SampleEmitter(room,
                                                         RadicalInverse(index, EMITTER_HORIZONTAL_BASE),
                                                         RadicalInverse(index, EMITTER_VERTICAL_BASE));
        const math::Vector3d directionWorld =
            SampleCosineDirection(RadicalInverse(index, COSINE_RADIUS_BASE), RadicalInverse(index, COSINE_ANGLE_BASE));
        const math::Vector3d hitWorld =
            originWorld + directionWorld * ((reflectorDepthWorld - originWorld.Z) / directionWorld.Z);
        if (ContainsPoint(reflector, hitWorld) == true && IsBlocked(room, originWorld, hitWorld) == false)
        {
            REQUIRE(outLightsWorld.TryAdd(hitWorld) == true);
        }
    }
}

float64 GetRedAreaScale(const Room& room)
{
    const math::Vector3d extentWorld = Widen(room.Positions[EMITTER][2]) - Widen(room.Positions[EMITTER][0]);
    return static_cast<float64>(room.Materials[EMITTER].Emission.X) * extentWorld.X * extentWorld.Y *
           static_cast<float64>(RECEIVER_ALBEDO.X * REFLECTOR_ALBEDO.X) / math::kPi;
}

float64 EstimateVirtualLightRadiance(const Room& room,
                                     const Array<math::Vector3d>& lightsWorld,
                                     usize budget,
                                     math::Vector3d receiverWorld,
                                     float64 minimumDistanceSquaredWorld)
{
    float64 sum = 0;
    for (const math::Vector3d lightWorld : lightsWorld)
    {
        if (IsBlocked(room, receiverWorld, lightWorld) == false)
        {
            const math::Vector3d offsetWorld = receiverWorld - lightWorld;
            const float64 distanceSquaredWorld = math::LengthSquared(offsetWorld);
            const float64 cosineProduct = offsetWorld.Z * offsetWorld.Z / distanceSquaredWorld;
            sum += cosineProduct / math::Max(distanceSquaredWorld, minimumDistanceSquaredWorld);
        }
    }
    return sum * GetRedAreaScale(room) / static_cast<float64>(budget);
}

// Independently integrate in receiver solid angle. This resolves the near-field geometry without
// relying on the VPL point approximation or tiny light-to-receiver distances in the denominator.
float64 EstimateNearReference(const Room& room, math::Vector3d receiverWorld)
{
    constexpr usize RECEIVER_STRATA_SIDE = 128;
    constexpr usize EMITTER_STRATA_SIDE = 8;
    const Corners& reflector = room.Positions[REFLECTOR];
    const float64 reflectorDepthWorld = static_cast<float64>(reflector[0].Z);
    float64 sum = 0;
    for (usize ray = 0; ray < RECEIVER_STRATA_SIDE * RECEIVER_STRATA_SIDE; ++ray)
    {
        const math::Vector3d directionWorld =
            SampleCosineDirection((static_cast<float64>(ray % RECEIVER_STRATA_SIDE) + .5) / RECEIVER_STRATA_SIDE,
                                  (static_cast<float64>(ray / RECEIVER_STRATA_SIDE) + .5) / RECEIVER_STRATA_SIDE);
        const math::Vector3d hitWorld =
            receiverWorld + directionWorld * ((reflectorDepthWorld - receiverWorld.Z) / directionWorld.Z);
        if (ContainsPoint(reflector, hitWorld) == false || IsBlocked(room, receiverWorld, hitWorld) == true)
        {
            continue;
        }
        for (usize sample = 0; sample < EMITTER_STRATA_SIDE * EMITTER_STRATA_SIDE; ++sample)
        {
            const math::Vector3d lightWorld =
                SampleEmitter(room,
                              (static_cast<float64>(sample % EMITTER_STRATA_SIDE) + .5) / EMITTER_STRATA_SIDE,
                              (static_cast<float64>(sample / EMITTER_STRATA_SIDE) + .5) / EMITTER_STRATA_SIDE);
            if (IsBlocked(room, hitWorld, lightWorld) == false)
            {
                const math::Vector3d offsetWorld = lightWorld - hitWorld;
                const float64 distanceSquaredWorld = math::LengthSquared(offsetWorld);
                sum += offsetWorld.Z * offsetWorld.Z / (distanceSquaredWorld * distanceSquaredWorld);
            }
        }
    }
    constexpr usize SAMPLE_COUNT =
        RECEIVER_STRATA_SIDE * RECEIVER_STRATA_SIDE * EMITTER_STRATA_SIDE * EMITTER_STRATA_SIDE;
    return sum * GetRedAreaScale(room) / SAMPLE_COUNT;
}

void CompareVirtualLights(const Room& room, const Image& reference)
{
    constexpr usize PROBE_STRIDE = 8;
    constexpr usize PROBE_COUNT = (WIDTH / PROBE_STRIDE) * (HEIGHT / PROBE_STRIDE);
    constexpr usize NEAR_PROBE_COUNT = 12;
    constexpr float64 RECEIVER_SEPARATION_METRES = .015;
    constexpr float64 CLAMP_DISTANCE_METRES = .08;
    const math::Vector3d emitterCenterWorld = SampleEmitter(room, .5, .5);
    const float64 nearDepthWorld = static_cast<float64>(room.Positions[REFLECTOR][0].Z) + RECEIVER_SEPARATION_METRES;
    StaticArray<math::Vector3d, NEAR_PROBE_COUNT> nearProbesWorld{};
    StaticArray<float64, NEAR_PROBE_COUNT> nearReference{};
    for (usize probe = 0; probe < NEAR_PROBE_COUNT; ++probe)
    {
        constexpr float64 PROBE_STEP_METRES = .04;
        nearProbesWorld[probe] = {emitterCenterWorld.X - PROBE_STEP_METRES * static_cast<float64>(probe),
                                  0,
                                  nearDepthWorld};
        nearReference[probe] = EstimateNearReference(room, nearProbesWorld[probe]);
    }

    constexpr usize BUDGET_COUNT = 3;
    constexpr StaticArray<usize, BUDGET_COUNT> BUDGETS{{64, 256, 1024}};
    for (const usize budget : BUDGETS)
    {
        Array<math::Vector3d> virtualLightsWorld;
        const uint64 beginning = time::NowTicks();
        TraceVirtualLights(room, budget, virtualLightsWorld);
        const uint64 buildTime = time::NowTicks() - beginning;
        float64 signedError = 0;
        for (usize y = PROBE_STRIDE / 2; y < HEIGHT; y += PROBE_STRIDE)
        {
            for (usize x = PROBE_STRIDE / 2; x < WIDTH; x += PROBE_STRIDE)
            {
                const math::Vector3d receiverWorld = {2 * (static_cast<float64>(x) + .5) / WIDTH - 1,
                                                      1 - 2 * (static_cast<float64>(y) + .5) / HEIGHT,
                                                      static_cast<float64>(room.Positions[RECEIVER][0].Z)};
                signedError += EstimateVirtualLightRadiance(room, virtualLightsWorld, budget, receiverWorld, 0) -
                               static_cast<float64>(reference[y * WIDTH + x].X);
            }
        }
        float64 peakError = 0;
        float64 clampedPeakError = 0;
        float64 clampLoss = 0;
        for (usize probe = 0; probe < NEAR_PROBE_COUNT; ++probe)
        {
            const math::Vector3d pointWorld = nearProbesWorld[probe];
            const float64 raw = EstimateVirtualLightRadiance(room, virtualLightsWorld, budget, pointWorld, 0);
            const float64 clamped = EstimateVirtualLightRadiance(room,
                                                                 virtualLightsWorld,
                                                                 budget,
                                                                 pointWorld,
                                                                 CLAMP_DISTANCE_METRES * CLAMP_DISTANCE_METRES);
            peakError = math::Max(peakError, math::Abs(raw - nearReference[probe]));
            clampedPeakError = math::Max(clampedPeakError, math::Abs(clamped - nearReference[probe]));
            clampLoss += raw - clamped;
        }
        REQUIRE(virtualLightsWorld.GetSize() > 0);
        WARN("VPL budget " << budget << ", retained " << virtualLightsWorld.GetSize() << ", visibility tests "
                           << virtualLightsWorld.GetSize() * (PROBE_COUNT + 2 * NEAR_PROBE_COUNT) << ", prepare ns "
                           << buildTime << ", total ns " << time::NowTicks() - beginning << ", signed image error "
                           << signedError / PROBE_COUNT << ", near peak error " << peakError
                           << ", clamped near peak error " << clampedPeakError << ", clamp loss "
                           << clampLoss / NEAR_PROBE_COUNT);
        if (budget == BUDGETS[BUDGET_COUNT - 1])
        {
            CHECK(math::Abs(signedError / PROBE_COUNT) < .02);
        }
    }
}
} // namespace

TEST_CASE("RT2 diffuse bounce matches RT1 and discards history across lighting and occluder edits",
          "[gpu][reference-compute][hybrid-indirect-diffuse]")
{
    const NativeDevice device(true);
    if (device.HasRays == false)
    {
        SKIP("Native ray queries unavailable");
    }

    StaticArray<Image, STATE_COUNT> cpu{};
    StaticArray<Image, STATE_COUNT> gpu{};
    RenderReference(device, renderer::ReferenceTraversal::CpuOracle, 256, cpu);
    RenderReference(device, renderer::ReferenceTraversal::GpuCompute, 1024, gpu);

    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(INDIRECT_PACKAGE);
    REQUIRE(renderer::RequiresHardwareRayTracing(package->GetPlan()) == true);
    renderer::RenderGraph graph;
    PrepareGraph(device, *package, graph);

    Room room;
    constexpr uint32 SAMPLE_COUNT = 256;
    constexpr usize CHECKPOINT_COUNT = 5;
    constexpr StaticArray<uint32, CHECKPOINT_COUNT> CHECKPOINTS{{1, 2, 16, 64, SAMPLE_COUNT}};
    constexpr float64 TEMPORAL_RMS_TOLERANCE = .02;
    Image image{};
    Image replay{};

    for (usize state = 0; state < STATE_COUNT; ++state)
    {
        const uint64 revision = state + 1;
        CAPTURE(revision);
        const Error parity = MeasureRedError(cpu[state], gpu[state]);
        WARN("RT1 CPU/GPU mean " << parity.SignedMean << ", RMS " << parity.Rms << ", peak " << parity.Peak);
        CHECK(math::Abs(parity.SignedMean) < .005);
        CHECK(parity.Rms < .035);

        rhi::SubmissionToken priorSubmission;
        if (state != 0)
        {
            // Submit a last frame from the prior generation; stage the edit before waiting for its completion.
            REQUIRE(graph.Render(device.Surface, WIDTH, HEIGHT, priorSubmission) == Status::Ready);
        }
        room.Apply(STATES[state]);
        const Status staged = StageRoom(graph, room, revision);
        REQUIRE(WaitForStatus(staged, [&] { return graph.Poll(); }) == Status::Ready);
        REQUIRE(graph.GetDrawableSceneRevision() == revision);
        CHECK(graph.GetSubmittedRaySamples() == 0);
        if (state != 0)
        {
            REQUIRE(WaitForStatus(Status::Pending, [&] { return rhi::GetStatus(device.Device, priorSubmission); }) ==
                    Status::Ready);
            CHECK(graph.GetOutputSceneRevision() == revision - 1);
            rhi::BufferHandle history{};
            CHECK(graph.GetAccumulation(RAY_PASS_INDEX, history) == Status::NotReady);
            CHECK(history == rhi::BufferHandle{});
        }

        renderer::RenderGraph fresh;
        PrepareGraph(device, *package, fresh);
        const Status freshStaged = StageRoom(fresh, room, revision);
        REQUIRE(WaitForStatus(freshStaged, [&] { return fresh.Poll(); }) == Status::Ready);

        uint32 frame = 0;
        uint32 firstWithinTolerance = 0;
        for (const uint32 checkpoint : CHECKPOINTS)
        {
            while (frame < checkpoint)
            {
                RenderFrame(device, graph);
                RenderFrame(device, fresh);
                ++frame;
            }
            REQUIRE(graph.GetOutputSceneRevision() == revision);
            CHECK(graph.GetSubmittedRaySamples() == frame);
            ReadMean(device, graph, frame, image);
            ReadMean(device, fresh, frame, replay);
            CHECK(image == replay);

            const Error error = MeasureRedError(image, gpu[state]);
            WARN("revision " << revision << ", material " << room.Revisions.Materials << ", instances "
                             << room.Revisions.Instances << ", frame " << frame << ": signed " << error.SignedMean
                             << ", RMS " << error.Rms << ", peak " << error.Peak);
            if (state != 0)
            {
                const Error changed = MeasureRedError(image, gpu[state], &gpu[state - 1]);
                const Error stable = MeasureRedError(image, gpu[state], &gpu[state - 1], false);
                if (firstWithinTolerance == 0 && changed.Rms <= TEMPORAL_RMS_TOLERANCE)
                {
                    firstWithinTolerance = frame;
                }
                WARN("revision " << revision << ", frame " << frame << ", changed RMS " << changed.Rms << " over "
                                 << changed.Count << " pixels, stable RMS " << stable.Rms << " over " << stable.Count);
            }
        }
        CheckDiffuseColor(image);
        if (state == STATE_COUNT - 1)
        {
            CompareVirtualLights(room, gpu[state]);
        }
        const Error reconstructed = MeasureRedError(image, gpu[state]);
        CHECK(math::Abs(reconstructed.SignedMean) < .005);
        CHECK(reconstructed.Rms < .02);
        if (state != 0)
        {
            WARN("revision " << revision << ": first checkpoint within tolerance " << firstWithinTolerance);
            CHECK(firstWithinTolerance != 0);
        }
    }
}

TEST_CASE("RT2 diffuse bounce selects the default raster fallback", "[gpu][reference-compute][hybrid-indirect-diffuse]")
{
    const NativeDevice device(false);
    const renderer::RenderGraphPreference preference = device.HasRays == true
                                                           ? renderer::RenderGraphPreference::ForceRaster
                                                           : renderer::RenderGraphPreference::Automatic;
    core::UniquePtr<renderer::RenderGraphPackage> raster = LoadPackage(RASTER_PACKAGE);
    renderer::RenderGraphActivation activation;
    REQUIRE(activation.Initialize(device.Device,
                                  raster->GetInterfaceIdentity(),
                                  shaders::renderer_composite::Vertex(),
                                  shaders::renderer_composite::Fragment()) == Status::Ready);
    REQUIRE(activation.Resize(WIDTH, HEIGHT) == Status::Ready);
    const Status staged = activation.StageSelected(core::Move(raster), LoadPackage(INDIRECT_PACKAGE), preference);
    REQUIRE((staged == Status::Ready || staged == Status::Pending));
    REQUIRE(WaitForStatus(Status::Pending, [&] { return activation.Poll(); }) == Status::Ready);
    REQUIRE(activation.GetReport().ActiveRenderer == renderer::RenderGraphRenderer::Raster);
    rhi::DeviceInfo details;
    REQUIRE(rhi::GetDeviceInfo(device.Device, details) == rhi::DeviceStatus::Ready);
    if (details.Startup.SelectedBackend == rhi::Backend::Vulkan)
    {
        REQUIRE(preference == renderer::RenderGraphPreference::Automatic);
    }
    WARN("RT2 fallback " << (device.HasRays == true ? "ForceRaster" : "Automatic"));
    Room room;
    room.Apply(STATES[1]);
    renderer::RenderGraphReport report;
    const Status updated = activation.Update(room.Meshes,
                                             room.Items,
                                             {&room.Camera, 1},
                                             1,
                                             renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                             report,
                                             &room.Inputs);
    REQUIRE(WaitForStatus(updated, [&] { return activation.Poll(); }) == Status::Ready);
    renderer::RenderGraph* graphOrNull = activation.GetActiveOrNull();
    REQUIRE(graphOrNull != nullptr);
    RenderFrame(device, *graphOrNull);
    CHECK(graphOrNull->GetOutputSceneRevision() == 1);
    CHECK(graphOrNull->GetSubmittedRaySamples() == 0);
}

namespace
{
constexpr usize TIMING_COUNT = 21;
using Timings = StaticArray<uint64, TIMING_COUNT>;

void ReportTiming(std::string_view description, Timings& inoutValues)
{
    Sort(inoutValues.GetData(), inoutValues.GetSize(), [](uint64 left, uint64 right) noexcept { return left < right; });
    constexpr usize PERCENTILE = 95;
    constexpr usize PERCENT = 100;
    constexpr usize TAIL_INDEX = (TIMING_COUNT * PERCENTILE + PERCENT - 1) / PERCENT - 1;
    WARN(description << ": median "
                     << static_cast<float64>(inoutValues[TIMING_COUNT / 2]) / time::NANOSECONDS_PER_MILLISECOND
                     << " ms, p95 " << static_cast<float64>(inoutValues[TAIL_INDEX]) / time::NANOSECONDS_PER_MILLISECOND
                     << " ms");
}
} // namespace

TEST_CASE("Measure RT2 diffuse bounce preparation and GPU shade with reconstruction",
          "[.benchmark][hybrid-indirect-diffuse]")
{
    const NativeDevice device(true, true);
    if (device.HasRays == false)
    {
        SKIP("Native ray queries unavailable");
    }
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(INDIRECT_PACKAGE);
    renderer::RenderGraph graph;
    PrepareGraph(device, *package, graph);
    Room room;
    SceneState state = STATES[1];
    room.Apply(state);
    const Status initial = StageRoom(graph, room, 1);
    REQUIRE(WaitForStatus(initial, [&] { return graph.Poll(); }) == Status::Ready);
    RenderFrame(device, graph);
    Timings preparation{};
    Timings shade{};
    for (usize trial = 0; trial < TIMING_COUNT; ++trial)
    {
        constexpr float32 EDIT_STEP_NITS = .01F;
        state.EmitterRadiance += EDIT_STEP_NITS;
        room.Apply(state);
        const uint64 beginning = time::NowTicks();
        const Status updated = StageRoom(graph, room, trial + 2);
        REQUIRE(WaitForStatus(updated, [&] { return graph.Poll(); }) == Status::Ready);
        preparation[trial] = time::NowTicks() - beginning;
        const Status armed = rhi::ArmGpuPassTimings(device.Device);
        if (armed == Status::Unsupported)
        {
            SKIP("Per-pass GPU timing is unavailable");
        }
        REQUIRE(armed == Status::Ready);
        RenderFrame(device, graph);
        rhi::GpuPassTimings timings;
        const auto poll = [&]() {
            const Status status = rhi::GetGpuPassTimings(device.Device, timings);
            return status == Status::Ready && timings.State == rhi::GpuTimingState::Pending ? Status::Pending : status;
        };
        REQUIRE(WaitForStatus(Status::Pending, poll) == Status::Ready);
        REQUIRE(timings.State == rhi::GpuTimingState::Available);
        REQUIRE(timings.DroppedPasses == 0);
        usize computePasses = 0;
        for (usize index = 0; index < timings.Count; ++index)
        {
            const rhi::GpuPassTiming& pass = timings.Passes[index];
            if (pass.Kind == rhi::GpuPassKind::Compute)
            {
                REQUIRE(pass.Timing.State == rhi::GpuTimingState::Available);
                REQUIRE(math::IsFinite(pass.Timing.DurationNanoseconds) == true);
                REQUIRE(pass.Timing.DurationNanoseconds > 0);
                shade[trial] += static_cast<uint64>(pass.Timing.DurationNanoseconds);
                ++computePasses;
            }
        }
        REQUIRE(computePasses == 1);
    }
    ReportTiming("Emitter edit to accepted drawable revision", preparation);
    ReportTiming("Native GPU shade and fused running mean", shade);
    WARN("History storage " << ACCUMULATION_WORD_COUNT * sizeof(math::Vector4) << " bytes per revision");
}
