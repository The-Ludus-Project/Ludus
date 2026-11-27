#include "../src/internal/scene_material_validation.h"
#include "material_program_fixtures.h"
#include "renderer_composite.h"
#include <cstdio>
#include <cstdlib>
#include <ludus/content/content.h>
#include <ludus/content/scene_format/limits.h>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/runtime/resources/scene.h>

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/config.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/strings/string.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/render_graph_capture.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace threading = ludus::foundation::threading;
using rhi::RasterStatus;
constexpr uint32 EXTENT = 4;
constexpr usize QUAD_VERTICES = 4;
constexpr usize QUAD_INDICES = 6;
constexpr usize TABLE_COUNT = 2;
constexpr usize CAMERA_COUNT = 1;
constexpr uint64 COLD_CAPTURE_TIMEOUT_NANOSECONDS =
    LUDUS_RENDERER_TEST_COLD_GPU_TIMEOUT_SECONDS * time::NANOSECONDS_PER_SECOND;
constexpr time::Duration COMPLETION_POLL_INTERVAL = time::Milliseconds(1);
struct Session final
{
    LUDUS_INLINE Session() noexcept
    {
        rhi::Shutdown();
    }
    LUDUS_INLINE ~Session() noexcept
    {
        rhi::Shutdown();
    }
};
/// Connection operator of the cooked template a test captures.
enum class WaveConnection : uint8
{
    /// The shipped default: the angular-spectrum lattice.
    AngularSpectrum,
    /// The same template cooked with WT_OPTICAL_CONNECTION 1.
    UtdEdgeDiffraction,
    /// The same template cooked with WT_OPTICAL_CONNECTION 2.
    FraunhoferEdgeDiffraction,
};
void LoadPackage(WaveConnection connection, renderer::RenderGraphPackage& outPackage)
{
    filesystem::Directory directory;
    const char* directoryPath = LUDUS_WAVE_SCENE_TEMPLATE_DIRECTORY;
    const char* packageName = "wave_scene_graph.lurgrt";
    if (connection == WaveConnection::UtdEdgeDiffraction)
    {
        directoryPath = LUDUS_WAVE_SCENE_UTD_TEMPLATE_DIRECTORY;
        packageName = "wave_scene_utd_graph.lurgrt";
    }
    else if (connection == WaveConnection::FraunhoferEdgeDiffraction)
    {
        directoryPath = LUDUS_WAVE_SCENE_FRAUNHOFER_TEMPLATE_DIRECTORY;
        packageName = "wave_scene_fraunhofer_graph.lurgrt";
    }
    REQUIRE(directory.Open(directoryPath).Succeeded() == true);
    filesystem::File file;
    REQUIRE(directory.OpenRead(packageName, file).Succeeded() == true);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(static_cast<usize>(file.Size())) == true);
    REQUIRE(file.ReadAt(0, MakeSpan(bytes)).BytesRead == bytes.GetSize());
    REQUIRE(outPackage.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
}
using ludus::content::scene_format::MaterialModel;
/// Passes after the primary pass: wave, geometric and aperture exports.
constexpr uint32 EXPORT_PASSES = 3;
/// The wave-optics export, which propagates a scalar field to its light sample.
constexpr uint32 WAVE_PASS = 1;
/// The geometric-optics export, which shadow-tests its light sample instead of propagating a wave field.
constexpr uint32 GEOMETRIC_PASS = 2;
#if defined(LUDUS_PLATFORM_MACOS)
// Stress the Metal compiler regression: each row is a workgroup, so a larger image exposes more SIMD groups.
constexpr uint32 REPEATED_CAPTURE_EXTENT = 64;
// With fast math, 16 pairs exposed the fault in each of 12 runs on an Apple M5, with at least 160 wrong pixels per run.
constexpr uint32 REPEATED_CAPTURE_ROUNDS = 16;
#else
// Repeat both exports once on the same portable fixture used by the wave transport checks.
constexpr uint32 REPEATED_CAPTURE_EXTENT = EXTENT;
constexpr uint32 REPEATED_CAPTURE_ROUNDS = 2;
#endif
/// Mean radiance of one exported pass's pixels.
using ExportedPixels = StaticArray<math::Vector3, usize{EXTENT} * EXTENT>;
/// One compute device with the decoded scene wave package; captures on it share the device's pipelines and memory.
struct WaveDevice final
{
    Session Owner;
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    renderer::RenderGraphPackage Package;
    renderer::RenderGraphPlan Plan;
    renderer::RenderGraphPrograms Programs;
};
/// Create the device and load the package, or skip the test without a compute adapter.
void CreateWaveDevice(WaveDevice& outDevice, WaveConnection connection = WaveConnection::AngularSpectrum)
{
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.Required.PortableRaster = true;
    const rhi::DeviceStatus created =
        rhi::CreateDevice({}, { .Width = EXTENT, .Height = EXTENT }, description, outDevice.Device, outDevice.Surface);
    if (created != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No compute adapter");
    }
    REQUIRE(created == rhi::DeviceStatus::Ready);
    LoadPackage(connection, outDevice.Package);
    outDevice.Plan = outDevice.Package.GetPlan();
    outDevice.Programs = outDevice.Package.GetPrograms(ludus::shaders::renderer_composite::Vertex(),
                                                       ludus::shaders::renderer_composite::Fragment());
    REQUIRE(renderer::RequiresHardwareRayTracing(outDevice.Plan) == false);
    REQUIRE(outDevice.Plan.Passes.GetSize() == EXPORT_PASSES + 1);
}
/// Capture one exported pass of a World and return each pixel's mean over `samples` samples per pixel.
void RunSceneWaveCapture(const WaveDevice& device,
                         BorrowedView<const renderer::RenderGraphMesh> meshes,
                         BorrowedView<const renderer::RenderGraphItem> items,
                         BorrowedView<const renderer::ViewDescription> cameras,
                         const renderer::SceneShaderInputs& inputs,
                         uint32 pass,
                         uint32 samples,
                         Array<math::Vector3>& outPixels,
                         uint32 width,
                         uint32 height)
{
    const renderer::RenderGraphPlan& plan = device.Plan;
    renderer::RenderGraphCapture capture;
    renderer::RenderGraphCaptureSettings settings;
    settings.Output = plan.Passes[pass].Color;
    settings.TargetSamples = samples;
    const RasterStatus started =
        capture.Start(device.Device, device.Surface, plan, device.Programs, width, height, settings);
    REQUIRE((started == RasterStatus::Ready || started == RasterStatus::Pending));
    const uint64 beginning = time::NowTicks();
    while (capture.GetPhase() == renderer::RenderGraphCapturePhase::Initializing &&
           time::NowTicks() - beginning < COLD_CAPTURE_TIMEOUT_NANOSECONDS)
    {
        capture.Service();
        threading::SleepFor(COMPLETION_POLL_INTERVAL);
    }
    REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::AwaitingScene);
    const uint64 captureBeginning = time::NowTicks();
    const RasterStatus submitted = capture.SubmitScene(meshes, items, cameras, 1, &inputs);
    CAPTURE(submitted);
    REQUIRE((submitted == RasterStatus::Ready || submitted == RasterStatus::Pending));
    while (capture.GetPhase() != renderer::RenderGraphCapturePhase::Complete &&
           capture.GetPhase() != renderer::RenderGraphCapturePhase::Failed &&
           time::NowTicks() - captureBeginning < COLD_CAPTURE_TIMEOUT_NANOSECONDS)
    {
        capture.Service();
        threading::SleepFor(COMPLETION_POLL_INTERVAL);
    }
    const uint64 elapsedNanoseconds = time::NowTicks() - captureBeginning;
    renderer::RenderGraphRayFailures failures;
    if (capture.GetProblem() == renderer::RenderGraphCaptureProblem::LatchedPixels)
    {
        RasterStatus polled = capture.PollRayFailures(pass, failures);
        while (polled == RasterStatus::Pending &&
               time::NowTicks() - captureBeginning < COLD_CAPTURE_TIMEOUT_NANOSECONDS)
        {
            capture.Service();
            threading::SleepFor(COMPLETION_POLL_INTERVAL);
            polled = capture.PollRayFailures(pass, failures);
        }
    }
    INFO("latched pixels " << failures.TotalPixels << ", main reason " << failures.MainReason << ": "
                           << renderer::DescribeRenderGraphFailureReason(failures.MainReason));
    CAPTURE(pass,
            capture.GetPhase(),
            capture.GetStatus(),
            capture.GetProblem(),
            elapsedNanoseconds,
            COLD_CAPTURE_TIMEOUT_NANOSECONDS);
    REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::Complete);
    const BorrowedView<const renderer::RenderGraphCapturePixel> pixels = capture.GetPixels();
    REQUIRE(pixels.GetSize() == usize{width} * height);
    REQUIRE(outPixels.TryResize(pixels.GetSize()) == true);
    for (usize index = 0; index < pixels.GetSize(); ++index)
    {
        outPixels[index] = pixels[index].Mean;
    }
}
/// Capture one exported pass of the receiver (and, for a nonzero screen, the screen) with one sample per pixel.
/// @param materialOrNull The receiver's material, or null for the neutral default.
/// Sum of a pixel's channels. The template renders spectrally: one sample at one wavelength can leave a channel dark
/// where the camera's sensor does not respond, but the sum is positive wherever light reaches the camera.
float32 SumChannels(math::Vector3 value) noexcept
{
    return value.X + value.Y + value.Z;
}
void CaptureSceneWavePass(const WaveDevice& device,
                          uint32 screen,
                          const renderer::SceneShaderMaterial* materialOrNull,
                          uint32 pass,
                          Array<math::Vector3>& outPixels,
                          uint32 width = EXTENT,
                          uint32 height = EXTENT)
{
    const StaticArray<math::Vector3, QUAD_VERTICES> receiverWorld{{{-1, -1, 1}, {1, -1, 1}, {-1, 1, 1}, {1, 1, 1}}};
    const float32 screenRightWorld = screen == 1 ? 1.0F : 0.0F;
    const StaticArray<math::Vector3, QUAD_VERTICES> screenWorld{
        {{-1, -1, 2}, {screenRightWorld, -1, 2}, {-1, 1, 2}, {screenRightWorld, 1, 2}}};
    const StaticArray<uint32, QUAD_INDICES> indices{{0, 1, 2, 1, 3, 2}};
    StaticArray<renderer::RenderGraphMesh, TABLE_COUNT> meshes;
    meshes[0].Geometry = {receiverWorld.GetData(), receiverWorld.GetSize(), indices.GetData(), indices.GetSize()};
    meshes[1].Geometry = {screenWorld.GetData(), screenWorld.GetSize(), indices.GetData(), indices.GetSize()};
    StaticArray<renderer::RenderGraphItem, TABLE_COUNT> items;
    items[0].SourceId = 1;
    items[1].SourceId = 2;
    items[1].MeshIndex = 1;
    items[0].Material.Color = {1, 1, 1, 1};
    items[1].Material.Color = {1, 1, 1, 1};
    StaticArray<renderer::ViewDescription, CAMERA_COUNT> cameras;
    cameras[0].OrthographicCamera = true;
    cameras[0].WorldToClip.Columns[0].X = 10000;
    cameras[0].WorldToClip.Columns[1].Y = 10000;
    cameras[0].WorldToClip.Columns[2].Z = -2;
    cameras[0].WorldToClip.Columns[3].Z = 3;
    renderer::DirectLight source;
    source.SourceId = 123;
    source.Kind = renderer::LightKind::Point;
    source.Position = {0, 0, 3};
    source.Range = 10;
    source.Intensity = 10;
    renderer::SceneShaderInputs inputs;
    inputs.Lights = {&source, 1};
    const uint32 materialIndex = 0;
    if (materialOrNull != nullptr)
    {
        inputs.Materials = {materialOrNull, 1};
        items[0].MaterialOverrides = {&materialIndex, 1};
    }
    const usize itemCount = screen == 0 ? 1 : TABLE_COUNT;
    RunSceneWaveCapture(device,
                        {meshes.GetData(), itemCount},
                        {items.GetData(), itemCount},
                        {cameras.GetData(), CAMERA_COUNT},
                        inputs,
                        pass,
                        1,
                        outPixels,
                        width,
                        height);
}
/// Capture the scene wave graph once per exported pass with an optional receiver material.
/// @param materialOrNull The receiver's material, or null for the neutral default.
/// @param outGeometricOrNull When non-null, captures only GEOMETRIC_PASS and receives its pixel means.
void RunSceneWaveGraph(uint32 screen,
                       const renderer::SceneShaderMaterial* materialOrNull,
                       ExportedPixels* outGeometricOrNull = nullptr,
                       WaveConnection connection = WaveConnection::AngularSpectrum)
{
    WaveDevice device;
    CreateWaveDevice(device, connection);
    // Primary pass index zero only writes shared hits. The following passes export wave, geometric and aperture.
    for (uint32 pass = 1; pass < device.Plan.Passes.GetSize(); ++pass)
    {
        if (outGeometricOrNull != nullptr && pass != GEOMETRIC_PASS)
        {
            continue;
        }
        Array<math::Vector3> pixels;
        CaptureSceneWavePass(device, screen, materialOrNull, pass, pixels);
        for (usize index = 0; index < pixels.GetSize(); ++index)
        {
            if (outGeometricOrNull != nullptr)
            {
                (*outGeometricOrNull)[index] = pixels[index];
            }
            const float32 value = SumChannels(pixels[index]);
            CAPTURE(pass, screen, index);
            REQUIRE(math::IsFinite(value) == true);
            if (screen == 1)
            {
                CHECK(value == 0);
            }
            else if (screen == 0 || pass != 2)
            {
                CHECK(value > 0);
            }
            else if (index % EXTENT < EXTENT / 2)
            {
                CHECK(value == 0);
            }
            else
            {
                CHECK(value > 0);
            }
        }
    }
}
void CheckSceneWaveGraph(uint32 screen, MaterialModel model = MaterialModel::MetallicRoughness)
{
    renderer::SceneShaderMaterial material;
    material.BaseColor = {0.8F, 0.7F, 0.6F};
    material.Scattering.Model = model;
    if (model == MaterialModel::Program)
    {
        material.Roughness = 0.35F;
        material.Metallic = 0.6F;
        material.Program = ludus::renderer_tests::MakeParityProgram();
    }
    else if (model == MaterialModel::ProjectBrdf)
    {
        material.Roughness = 0.35F;
        material.Scattering.Eta = {{0.25F, 0.5F, 0.25F}};
        material.Scattering.K = {{0, 4, 12}};
        material.Scattering.CoatingThickness = 0;
    }
    else
    {
        material.Metallic = 0.25F;
        material.Scattering.AlphaU = 0.2F;
        material.Scattering.AlphaV = material.Scattering.AlphaU;
        material.Scattering.K = {{3, 4, 5}};
    }
    RunSceneWaveGraph(screen, model == MaterialModel::MetallicRoughness ? nullptr : &material);
}
} // namespace

TEST_CASE("Scene wave graph lights an unobstructed beam from the current World", "[renderer][gpu][wave-tracing][graph]")
{
    CheckSceneWaveGraph(0);
}

TEST_CASE("Scene wave graph renders a diffuse conductor Mix through the transport interface",
          "[renderer][gpu][wave-tracing][graph][mix]")
{
    CheckSceneWaveGraph(0, MaterialModel::DiffuseConductorMix);
}

TEST_CASE("Scene wave graph renders a project BRDF without a fixed-dimension declaration",
          "[renderer][gpu][wave-tracing][graph][project-brdf]")
{
    CheckSceneWaveGraph(0, MaterialModel::ProjectBrdf);
}

TEST_CASE("Scene wave graph renders a closure program through the transport interface",
          "[renderer][gpu][wave-tracing][graph][program]")
{
    CheckSceneWaveGraph(0, MaterialModel::Program);
}

TEST_CASE("Scene wave graph mixes two distinct lobes by a grain mask without a graph edit",
          "[renderer][gpu][wave-tracing][graph][program]")
{
    namespace fixtures = ludus::renderer_tests;
    // The lone receiver is lit by one point light and its continuations escape, so each geometric-optics pixel is the
    // BSDF at one pair of directions times fixed factors: here the gold's mirror peak, at normal incidence. The CPU
    // oracle and the CPU/GPU parity case cover off-specular directions. The wave export propagates a field instead,
    // so the exact comparison uses the geometric export.
    renderer::SceneShaderMaterial grain;
    grain.Scattering.Model = MaterialModel::Program;
    grain.Roughness = fixtures::GRAIN_MIX_ROUGHNESS;
    grain.Program = fixtures::MakeGrainMixProgram();
    renderer::SceneShaderMaterial diffuse;
    diffuse.BaseColor = {1, 1, 1};
    diffuse.Scattering.Model = MaterialModel::Diffuse;
    renderer::SceneShaderMaterial conductor;
    conductor.Scattering.Model = MaterialModel::Conductor;
    conductor.Scattering.AlphaU = fixtures::GRAIN_MIX_ROUGHNESS * fixtures::GRAIN_MIX_ROUGHNESS;
    conductor.Scattering.AlphaV = conductor.Scattering.AlphaU;
    conductor.Scattering.Eta = fixtures::GOLD_ETA;
    conductor.Scattering.K = fixtures::GOLD_K;
    ExportedPixels matte{};
    ExportedPixels gold{};
    RunSceneWaveGraph(0, &diffuse, &matte);
    RunSceneWaveGraph(0, &conductor, &gold);
    usize distinctCount = 0;
    for (usize index = 0; index < matte.GetSize(); ++index)
    {
        distinctCount += math::Distance(matte[index], gold[index]) > 0.1F * math::Length(matte[index]) ? 1 : 0;
    }
    CHECK(distinctCount > 0);
    // The program evaluates the same lobes through the interpreter; Metal's fast math bounds the difference.
    constexpr float32 RELATIVE_TOLERANCE = 1e-3F;
    constexpr float32 INTERIOR_MASK = 0.35F;
    for (const float32 mask : {0.0F, 1.0F, INTERIOR_MASK})
    {
        CAPTURE(mask);
        grain.BaseColor = {mask, mask, mask};
        ExportedPixels mixed{};
        RunSceneWaveGraph(0, &grain, &mixed);
        for (usize index = 0; index < mixed.GetSize(); ++index)
        {
            CAPTURE(index);
            const math::Vector3 expected = matte[index] * (1 - mask) + gold[index] * mask;
            CHECK(math::Distance(mixed[index], expected) <= RELATIVE_TOLERANCE * (1 + math::Length(expected)));
        }
    }
}

TEST_CASE("Scene wave graph blocks an opaque screen from the current World", "[renderer][gpu][wave-tracing][graph]")
{
    CheckSceneWaveGraph(1);
}

TEST_CASE("Scene wave graph diffracts beyond a half screen from the current World",
          "[renderer][gpu][wave-tracing][graph]")
{
    CheckSceneWaveGraph(2);
}

namespace
{
/// Run the open beam, the full screen and the half screen through a template cooked with an edge connection, then
/// check the half screen's shadow-boundary powers.
void CheckEdgeConnectionGraph(WaveConnection connection)
{
    for (uint32 screen = 0; screen <= 2; ++screen)
    {
        CAPTURE(screen);
        RunSceneWaveGraph(screen, nullptr, nullptr, connection);
    }
    // The half screen's straight edge at x = 0 lies midway between the light at z = 3 and the receiver at z = 1.
    // Every receiver pixel is within 0.14 Fresnel units of the shadow boundary, where Fresnel knife-edge diffraction
    // gives between 0.19 and 0.25 of the unobstructed power on the shadowed side and 0.25 to 0.32 on the lit side.
    constexpr uint32 TRANSMISSION_PASS = 3;
    constexpr float32 BOUNDARY_POWER = 0.25F;
    constexpr float32 SHADOW_MINIMUM_POWER = 0.15F;
    constexpr float32 LIT_MAXIMUM_POWER = 0.4F;
    WaveDevice device;
    CreateWaveDevice(device, connection);
    Array<math::Vector3> pixels;
    CaptureSceneWavePass(device, 2, nullptr, TRANSMISSION_PASS, pixels);
    for (usize index = 0; index < pixels.GetSize(); ++index)
    {
        CAPTURE(index, pixels[index].X);
        if (index % EXTENT < EXTENT / 2)
        {
            CHECK(pixels[index].X >= SHADOW_MINIMUM_POWER);
            CHECK(pixels[index].X <= BOUNDARY_POWER);
        }
        else
        {
            CHECK(pixels[index].X >= BOUNDARY_POWER);
            CHECK(pixels[index].X <= LIT_MAXIMUM_POWER);
        }
    }
}
} // namespace

TEST_CASE("Scene wave graph with UTD edge diffraction lights, blocks and diffracts from the current World",
          "[renderer][gpu][wave-tracing][wave-utd][graph]")
{
    CheckEdgeConnectionGraph(WaveConnection::UtdEdgeDiffraction);
}

TEST_CASE("Scene wave graph with Fraunhofer edge diffraction lights, blocks and diffracts from the current World",
          "[renderer][gpu][wave-tracing][wave-fraunhofer][graph]")
{
    CheckEdgeConnectionGraph(WaveConnection::FraunhoferEdgeDiffraction);
}

namespace
{
/// Samples per pixel of the glass relay captures: each sample either passes the pane or reflects away from the lamp.
constexpr uint32 GLASS_RELAY_SAMPLES = 8;
/// Capture the optical and geometric exports of a lamp seen through a thin glass pane, optionally with a half screen
/// between the pane and the lamp, and return their pixel means.
void CaptureGlassRelay(WaveConnection connection,
                       bool hasScreen,
                       Array<math::Vector3>& outOptical,
                       Array<math::Vector3>& outGeometric)
{
    // The camera looks down from its near plane at z = 1.5 through a pane at z = 1.25 onto a lamp at z = 1. The half
    // screen at z = 1.1 covers x <= 0, so each lit pixel sees the lamp past its edge. The lamp and screen are black, so
    // nothing reaches the camera by scattering from them.
    constexpr usize GLASS_TABLE_COUNT = 3;
    constexpr float32 PANE_DEPTH_WORLD = 1.25F;
    constexpr float32 SCREEN_DEPTH_WORLD = 1.1F;
    constexpr float32 LAMP_DEPTH_WORLD = 1;
    constexpr float32 GLASS_ETA = 1.5F;
    constexpr float32 LAMP_RADIANCE = 4;
    const StaticArray<math::Vector3, QUAD_VERTICES> paneWorld{
        {{-1, -1, PANE_DEPTH_WORLD}, {1, -1, PANE_DEPTH_WORLD}, {-1, 1, PANE_DEPTH_WORLD}, {1, 1, PANE_DEPTH_WORLD}}};
    const StaticArray<math::Vector3, QUAD_VERTICES> screenWorld{{{-1, -1, SCREEN_DEPTH_WORLD},
                                                                 {0, -1, SCREEN_DEPTH_WORLD},
                                                                 {-1, 1, SCREEN_DEPTH_WORLD},
                                                                 {0, 1, SCREEN_DEPTH_WORLD}}};
    const StaticArray<math::Vector3, QUAD_VERTICES> lampWorld{
        {{-1, -1, LAMP_DEPTH_WORLD}, {1, -1, LAMP_DEPTH_WORLD}, {-1, 1, LAMP_DEPTH_WORLD}, {1, 1, LAMP_DEPTH_WORLD}}};
    const StaticArray<uint32, QUAD_INDICES> indices{{0, 1, 2, 1, 3, 2}};
    StaticArray<renderer::RenderGraphMesh, GLASS_TABLE_COUNT> meshes;
    meshes[0].Geometry = {paneWorld.GetData(), paneWorld.GetSize(), indices.GetData(), indices.GetSize()};
    meshes[1].Geometry = {lampWorld.GetData(), lampWorld.GetSize(), indices.GetData(), indices.GetSize()};
    meshes[2].Geometry = {screenWorld.GetData(), screenWorld.GetSize(), indices.GetData(), indices.GetSize()};
    StaticArray<renderer::SceneShaderMaterial, GLASS_TABLE_COUNT> materials;
    materials[0].Scattering.Model = MaterialModel::ThinDielectric;
    materials[0].Scattering.Eta = {{GLASS_ETA, GLASS_ETA, GLASS_ETA}};
    materials[0].Scattering.AlphaU = ludus::content::scene_format::MIN_MATERIAL_ALPHA;
    materials[0].Scattering.AlphaV = ludus::content::scene_format::MIN_MATERIAL_ALPHA;
    materials[1].BaseColor = {0, 0, 0};
    materials[1].Scattering.Model = MaterialModel::Diffuse;
    materials[1].Emission = {LAMP_RADIANCE, LAMP_RADIANCE, LAMP_RADIANCE};
    materials[2].BaseColor = {0, 0, 0};
    materials[2].Scattering.Model = MaterialModel::Diffuse;
    const StaticArray<uint32, GLASS_TABLE_COUNT> materialIndices{{0, 1, 2}};
    StaticArray<renderer::RenderGraphItem, GLASS_TABLE_COUNT> items;
    for (usize item = 0; item < GLASS_TABLE_COUNT; ++item)
    {
        items[item].SourceId = static_cast<uint32>(item + 1);
        items[item].MeshIndex = static_cast<uint32>(item);
        items[item].Material.Color = {1, 1, 1, 1};
        items[item].MaterialOverrides = {&materialIndices[item], 1};
    }
    StaticArray<renderer::ViewDescription, CAMERA_COUNT> cameras;
    cameras[0].OrthographicCamera = true;
    cameras[0].WorldToClip.Columns[0].X = 10000;
    cameras[0].WorldToClip.Columns[1].Y = 10000;
    cameras[0].WorldToClip.Columns[2].Z = -2;
    cameras[0].WorldToClip.Columns[3].Z = 3;
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    WaveDevice device;
    CreateWaveDevice(device, connection);
    const usize itemCount = hasScreen == true ? GLASS_TABLE_COUNT : GLASS_TABLE_COUNT - 1;
    RunSceneWaveCapture(device,
                        {meshes.GetData(), itemCount},
                        {items.GetData(), itemCount},
                        {cameras.GetData(), CAMERA_COUNT},
                        inputs,
                        WAVE_PASS,
                        GLASS_RELAY_SAMPLES,
                        outOptical,
                        EXTENT,
                        EXTENT);
    RunSceneWaveCapture(device,
                        {meshes.GetData(), itemCount},
                        {items.GetData(), itemCount},
                        {cameras.GetData(), CAMERA_COUNT},
                        inputs,
                        GEOMETRIC_PASS,
                        GLASS_RELAY_SAMPLES,
                        outGeometric,
                        EXTENT,
                        EXTENT);
}
/// The lamp behind the pane, which the template omitted before glass relays, and its diffraction past a half screen.
void CheckGlassRelayGraph(WaveConnection connection)
{
    // The thin pane transmits 1 - 0.0769 of each sample at normal incidence (two interfaces of index 1.5 and their
    // internal reflections), so the geometric export's mean over all pixels' samples lies between 0.6 and 1 of the
    // lamp's white radiance in each channel. Each sample reads one wavelength, so single pixels are noisier than that.
    constexpr float32 LAMP_RADIANCE = 4;
    constexpr float32 MINIMUM_TRANSMITTED_FRACTION = 0.6F;
    // Unobstructed, the relayed field equals the free beam's at every wavelength, so the exports, which sample the
    // same wavelengths, agree to float32 rounding.
    constexpr float32 OPEN_RATIO_TOLERANCE = 0.01F;
    Array<math::Vector3> optical;
    Array<math::Vector3> geometric;
    CaptureGlassRelay(connection, false, optical, geometric);
    math::Vector3 geometricMean{};
    for (usize index = 0; index < optical.GetSize(); ++index)
    {
        CAPTURE(index, SumChannels(optical[index]), SumChannels(geometric[index]));
        REQUIRE(SumChannels(geometric[index]) > 0);
        CHECK(math::Abs(SumChannels(optical[index]) - SumChannels(geometric[index])) <=
              OPEN_RATIO_TOLERANCE * SumChannels(geometric[index]));
        geometricMean = geometricMean + geometric[index] / static_cast<float32>(geometric.GetSize());
    }
    CAPTURE(geometricMean.X, geometricMean.Y, geometricMean.Z);
    for (const float32 channel : {geometricMean.X, geometricMean.Y, geometricMean.Z})
    {
        CHECK(channel >= MINIMUM_TRANSMITTED_FRACTION * LAMP_RADIANCE);
        CHECK(channel <= LAMP_RADIANCE);
    }
    // Past the half screen, each lit pixel lies 0 to 0.7 Fresnel units from the edge's shadow boundary (the lamp is
    // 0.1 m below the screen and the camera 0.4 m above it). Fresnel knife-edge diffraction gives 0.25 to 0.65 of the
    // unobstructed power there; classical scattering at the pane would see the whole lamp.
    constexpr float32 BOUNDARY_POWER = 0.25F;
    constexpr float32 LIT_MAXIMUM_POWER = 0.75F;
    CaptureGlassRelay(connection, true, optical, geometric);
    for (usize index = 0; index < optical.GetSize(); ++index)
    {
        CAPTURE(index, SumChannels(optical[index]), SumChannels(geometric[index]));
        REQUIRE(math::IsFinite(SumChannels(optical[index])) == true);
        if (index % EXTENT < EXTENT / 2)
        {
            // The screen hides the lamp from these pixels' camera paths.
            CHECK(SumChannels(geometric[index]) == 0);
            CHECK(SumChannels(optical[index]) == 0);
            continue;
        }
        REQUIRE(SumChannels(geometric[index]) > 0);
        // Both exports sample the same wavelengths, so the ratio of their sums averages the relayed power over them.
        const float32 ratio = SumChannels(optical[index]) / SumChannels(geometric[index]);
        CHECK(ratio >= BOUNDARY_POWER * (1 - OPEN_RATIO_TOLERANCE));
        CHECK(ratio <= LIT_MAXIMUM_POWER);
    }
}
} // namespace

TEST_CASE("Scene wave graph relays a lamp through glass and diffracts it past a screen",
          "[renderer][gpu][wave-tracing][wave-relay][graph]")
{
    CheckGlassRelayGraph(WaveConnection::AngularSpectrum);
}

TEST_CASE("Scene wave graph with UTD edge diffraction relays a lamp through glass",
          "[renderer][gpu][wave-tracing][wave-utd][wave-relay][graph]")
{
    CheckGlassRelayGraph(WaveConnection::UtdEdgeDiffraction);
}

TEST_CASE("Scene wave graph with Fraunhofer edge diffraction relays a lamp through glass",
          "[renderer][gpu][wave-tracing][wave-fraunhofer][wave-relay][graph]")
{
    CheckGlassRelayGraph(WaveConnection::FraunhoferEdgeDiffraction);
}

#if defined(LUDUS_PLATFORM_MACOS)
TEST_CASE("Scene wave graph reaches an unobstructed receiver without Metal multiply-add contraction",
          "[renderer][gpu][wave-tracing][graph][reference-compute]")
{
    // Keep the production safe-math policy and vary contraction only for this capture's compute shader.
    String uncontractedSource;
    WaveDevice device;
    CreateWaveDevice(device);
    rhi::ShaderDescription& shader = device.Programs.RayCompute.Artifact;
    REQUIRE(shader.Msl.empty() == false);
    REQUIRE(uncontractedSource.TryAppend("#pragma METAL fp contract(off)\n") == StringStatus::Ok);
    REQUIRE(uncontractedSource.TryAppend(shader.Msl) == StringStatus::Ok);
    shader.Msl = uncontractedSource.GetView();

    Array<math::Vector3> pixels;
    CaptureSceneWavePass(device, 0, nullptr, WAVE_PASS, pixels, REPEATED_CAPTURE_EXTENT, REPEATED_CAPTURE_EXTENT);
    for (usize index = 0; index < pixels.GetSize(); ++index)
    {
        CAPTURE(index);
        CHECK(math::IsFinite(SumChannels(pixels[index])) == true);
        CHECK(SumChannels(pixels[index]) > 0);
    }
}
#endif

TEST_CASE("Scene wave graph repeats its wave and closure-program exports bit for bit",
          "[renderer][gpu][wave-tracing][graph][program]")
{
    // With Metal fast math, the merged ray kernel lost private values of whole SIMD groups at random: the wave
    // export returned exact zeros and the grain program read its zeroed roughness register, giving a mirror spike
    // about 2000 times too bright. Alternating the two exports on one device changes the kernel's code path between
    // dispatches, which exposed it within a few captures. ConfigureMetalComputeMath now compiles compute kernels with
    // safe math; see docs/architecture/renderer-systems.md#metal-compute-floating-point.
    namespace fixtures = ludus::renderer_tests;
    renderer::SceneShaderMaterial grain;
    grain.Scattering.Model = MaterialModel::Program;
    grain.Roughness = fixtures::GRAIN_MIX_ROUGHNESS;
    grain.Program = fixtures::MakeGrainMixProgram();
    grain.BaseColor = {1, 1, 1};
    WaveDevice device;
    CreateWaveDevice(device);
    Array<math::Vector3> firstWave;
    Array<math::Vector3> firstProgram;
    Array<math::Vector3> pixels;
    usize mismatchCount = 0;
    const auto compare = [&](const Array<math::Vector3>& first, uint32 round, uint32 pass) {
        for (usize index = 0; index < pixels.GetSize(); ++index)
        {
            const math::Vector3& value = pixels[index];
            const math::Vector3& expected = first[index];
            const bool isEqual = value.X == expected.X && value.Y == expected.Y && value.Z == expected.Z;
            if (isEqual == false && mismatchCount == 0)
            {
                // Report the first difference only; a fault usually changes every pixel of a workgroup.
                CAPTURE(round, pass, index, value.X, expected.X);
                CHECK(isEqual == true);
            }
            mismatchCount += isEqual == true ? 0 : 1;
        }
    };
    for (uint32 round = 0; round < REPEATED_CAPTURE_ROUNDS; ++round)
    {
        CaptureSceneWavePass(device, 0, nullptr, WAVE_PASS, pixels, REPEATED_CAPTURE_EXTENT, REPEATED_CAPTURE_EXTENT);
        if (round == 0)
        {
            for (usize index = 0; index < pixels.GetSize(); ++index)
            {
                CAPTURE(index, REPEATED_CAPTURE_EXTENT);
                const math::Vector3& value = pixels[index];
                REQUIRE(math::IsFinite(SumChannels(value)) == true);
                REQUIRE(SumChannels(value) > 0);
            }
            REQUIRE(firstWave.TryResize(pixels.GetSize()) == true);
            for (usize index = 0; index < pixels.GetSize(); ++index)
            {
                firstWave[index] = pixels[index];
            }
        }
        compare(firstWave, round, WAVE_PASS);
        CaptureSceneWavePass(device,
                             0,
                             &grain,
                             GEOMETRIC_PASS,
                             pixels,
                             REPEATED_CAPTURE_EXTENT,
                             REPEATED_CAPTURE_EXTENT);
        if (round == 0)
        {
            REQUIRE(firstProgram.TryResize(pixels.GetSize()) == true);
            for (usize index = 0; index < pixels.GetSize(); ++index)
            {
                REQUIRE(math::IsFinite(pixels[index].X) == true);
                firstProgram[index] = pixels[index];
            }
        }
        compare(firstProgram, round, GEOMETRIC_PASS);
    }
    CHECK(mismatchCount == 0);
}

TEST_CASE("Scene optical template captures a supplied cooked test scene", "[.wave-supplied-scene]")
{
    const char* sceneDirectoryOrNull = std::getenv("LUDUS_WAVE_SCENE_DIRECTORY");
    if (sceneDirectoryOrNull == nullptr)
    {
        SKIP("Set LUDUS_WAVE_SCENE_DIRECTORY to an existing cooked scene directory.");
    }
    ludus::content::Bytes bytes;
    REQUIRE(ludus::content::ReadFile(sceneDirectoryOrNull,
                                     "scene.luscn",
                                     ludus::content::scene_format::IMPORT_LIMITS.BundleBytes,
                                     bytes) == ludus::content::Status::Ok);
    ludus::resources::SceneDefinition scene;
    REQUIRE(ludus::resources::SceneDefinition::Decode(bytes.Data().data(), bytes.Data().size(), scene) ==
            ludus::resources::Status::Ok);
    Session session;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription deviceDescription;
    deviceDescription.Required.Compute = true;
    deviceDescription.Required.PortableRaster = true;
    constexpr usize HOST_BYTES = usize{4096} * 1024 * 1024;
    constexpr uint64 RESOURCE_BYTES = uint64{4096} * 1024 * 1024;
    constexpr uint32 RESOURCE_COUNT = 1536;
    constexpr uint32 DESCRIPTOR_COUNT = 8192;
    deviceDescription.Storage.HostBytes = HOST_BYTES;
    deviceDescription.Storage.ResourceBytes = RESOURCE_BYTES;
    deviceDescription.Storage.ResourcesPerKind = RESOURCE_COUNT;
    deviceDescription.Storage.BindingDescriptors = DESCRIPTOR_COUNT;
    REQUIRE(rhi::CreateDevice({}, { .Width = EXTENT, .Height = EXTENT }, deviceDescription, device, surface) ==
            rhi::DeviceStatus::Ready);
    renderer::RenderGraphPackage package;
    LoadPackage(WaveConnection::AngularSpectrum, package);
    const renderer::RenderGraphPlan plan = package.GetPlan();
    const renderer::RenderGraphPrograms programs = package.GetPrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                       ludus::shaders::renderer_composite::Fragment());
    Array<renderer::RenderGraphMesh> meshes;
    Array<renderer::RenderGraphItem> items;
    Array<renderer::SceneShaderMaterial> materials;
    Array<renderer::DirectLight> lights;
    REQUIRE(meshes.TryResize(scene.Meshes.GetSize()) == true);
    REQUIRE(items.TryResize(scene.Objects.GetSize()) == true);
    REQUIRE(materials.TryResize(scene.Materials.GetSize()) == true);
    for (usize index = 0; index < meshes.GetSize(); ++index)
    {
        const ludus::resources::SceneMesh& source = scene.Meshes[index];
        renderer::MeshDescription& geometry = meshes[index].Geometry;
        geometry.Positions = source.Positions.GetData();
        geometry.VertexCount = source.Positions.GetSize();
        geometry.Indices = source.Indices.GetData();
        geometry.IndexCount = source.Indices.GetSize();
        geometry.Normals = source.Normals.GetData();
        geometry.Texcoords = source.Texcoords.GetData();
        geometry.Tangents = {source.Tangents.GetData(), source.Tangents.GetSize()};
    }
    const auto getTexture = [&](uint32 index) {
        rhi::TextureCpuContent result;
        if (index < scene.Textures.GetSize())
        {
            const ludus::resources::SceneTexture& source = scene.Textures[index];
            result.Bytes = source.Pixels.GetData();
            result.Size = source.Pixels.GetSize();
            result.RowPitch = usize(source.Width) * 4;
            result.Width = source.Width;
            result.Height = source.Height;
            result.Format = source.IsLinear == true ? rhi::RasterFormat::Rgba8Unorm : rhi::RasterFormat::Rgba8Srgb;
        }
        return result;
    };
    for (usize index = 0; index < materials.GetSize(); ++index)
    {
        const ludus::resources::SceneMaterial& source = scene.Materials[index];
        renderer::SceneShaderMaterial& material = materials[index];
        material.BaseColor =
            source.HasSourceMaterial == true || source.Texture != ~uint32{0} ? source.Reflectance : math::Vector3{};
        material.Metallic = source.Metallic;
        material.Roughness = source.Roughness;
        material.Scattering = source.Scattering;
        material.Emission = source.Emission;
        material.IsEmissionTwoSided = source.IsEmissionTwoSided;
        material.IsAlphaMasked = source.IsAlphaMasked;
        material.AlphaCutoff = source.AlphaCutoff;
        material.NormalScale = source.NormalScale;
        material.ColorTexture = getTexture(source.Texture);
        material.SurfaceTexture = getTexture(source.SurfaceTexture);
        material.EmissionTexture = getTexture(source.EmissionTexture);
        material.Sampler.U = source.Repeat == true ? rhi::RasterAddress::Repeat : rhi::RasterAddress::Clamp;
        material.Sampler.V = material.Sampler.U;
        INFO("material " << index << " roughness " << material.Roughness << " alpha " << material.Scattering.AlphaU
                         << "," << material.Scattering.AlphaV);
        REQUIRE(renderer::internal::IsSceneShaderMaterialValid(material) == true);
    }
    usize triangleCount = 0;
    for (usize index = 0; index < items.GetSize(); ++index)
    {
        const ludus::resources::SceneObject& source = scene.Objects[index];
        renderer::RenderGraphItem& item = items[index];
        item.SourceId = source.SourceId == 0 ? index + 1 : source.SourceId;
        item.MeshIndex = source.Mesh;
        item.MeshToWorld = source.Transform;
        item.MaterialOverrides = {&source.Material, 1};
        triangleCount += scene.Meshes[source.Mesh].Indices.GetSize() / 3;
    }
    for (const ludus::resources::SceneLight& source : scene.Lights)
    {
        if (source.IsEnabled == false)
        {
            continue;
        }
        renderer::DirectLight light;
        light.SourceId = source.SourceId;
        light.Kind = source.Kind == ludus::resources::SceneLightKind::Directional
                         ? renderer::LightKind::Directional
                         : (source.Kind == ludus::resources::SceneLightKind::Spot ? renderer::LightKind::Spot
                                                                                  : renderer::LightKind::Point);
        light.Color = source.IntensityRgb;
        light.Intensity = 1;
        light.Position = source.PositionWorld;
        light.Direction = source.DirectionWorld;
        light.Range = source.Range;
        light.InnerCos = source.InnerCos;
        light.OuterCos = source.OuterCos;
        REQUIRE(lights.TryAdd(light) == true);
    }
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    inputs.Lights = {lights.GetData(), lights.GetSize()};
    inputs.Environment = scene.Environment.Rgb;
    inputs.EnvironmentWidth = scene.Environment.Width;
    inputs.EnvironmentHeight = scene.Environment.Height;
    inputs.EnvironmentPixels = {scene.Environment.Pixels.GetData(), scene.Environment.Pixels.GetSize()};
    constexpr uint32 WIDTH = 64;
    constexpr uint32 HEIGHT = 32;
    constexpr uint32 SAMPLES = 8;
    math::Matrix4 worldToView;
    math::Matrix4 viewToClip;
    REQUIRE(math::TryLookAt(scene.Eye, scene.Target, scene.Up, worldToView) == math::MathStatus::Success);
    REQUIRE(math::TryPerspectiveReverseZ(scene.VerticalFov,
                                         static_cast<float32>(WIDTH) / HEIGHT,
                                         0.01F,
                                         1000,
                                         viewToClip) == math::MathStatus::Success);
    // The graph camera contract uses conventional depth. Convert z to w-z without changing the authored view.
    for (math::Vector4& column : viewToClip.Columns)
    {
        column.Z = column.W - column.Z;
    }
    renderer::ViewDescription view;
    view.CameraPosition = scene.Eye;
    view.WorldToClip = viewToClip * worldToView;
    view.Depth = renderer::DepthConvention::Conventional;
    renderer::RenderGraphCapture capture;
    renderer::RenderGraphCaptureSettings settings;
    settings.Output = plan.Passes[1].Color;
    settings.TargetSamples = SAMPLES;
    REQUIRE((capture.Start(device, surface, plan, programs, WIDTH, HEIGHT, settings) == RasterStatus::Ready ||
             capture.GetPhase() == renderer::RenderGraphCapturePhase::Initializing));
    const uint64 start = time::NowTicks();
    constexpr uint64 SCENE_TIMEOUT = 900 * time::NANOSECONDS_PER_SECOND;
    while (capture.GetPhase() == renderer::RenderGraphCapturePhase::Initializing &&
           time::NowTicks() - start < SCENE_TIMEOUT)
    {
        capture.Service();
        threading::SleepFor(COMPLETION_POLL_INTERVAL);
    }
    REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::AwaitingScene);
    const RasterStatus submitted = capture.SubmitScene({meshes.GetData(), meshes.GetSize()},
                                                       {items.GetData(), items.GetSize()},
                                                       {&view, 1},
                                                       1,
                                                       &inputs);
    INFO("submission status " << static_cast<uint32>(submitted) << " problem "
                              << static_cast<uint32>(capture.GetProblem()));
    REQUIRE((submitted == RasterStatus::Ready || submitted == RasterStatus::Pending));
    while (capture.GetPhase() != renderer::RenderGraphCapturePhase::Complete &&
           capture.GetPhase() != renderer::RenderGraphCapturePhase::Failed && time::NowTicks() - start < SCENE_TIMEOUT)
    {
        capture.Service();
        threading::SleepFor(COMPLETION_POLL_INTERVAL);
    }
    INFO("scene " << sceneDirectoryOrNull << " triangles " << triangleCount << " status "
                  << static_cast<uint32>(capture.GetStatus()) << " problem "
                  << static_cast<uint32>(capture.GetProblem()));
    if (capture.GetPhase() == renderer::RenderGraphCapturePhase::Failed)
    {
        for (uint32 pass = 0; pass < plan.Passes.GetSize(); ++pass)
        {
            renderer::RenderGraphRayFailures failures;
            RasterStatus status = capture.PollRayFailures(pass, failures);
            while (status == RasterStatus::Pending && time::NowTicks() - start < SCENE_TIMEOUT)
            {
                status = capture.PollRayFailures(pass, failures);
            }
            INFO("pass " << pass << " failures " << failures.TotalPixels << " reason " << failures.MainReason << ": "
                         << renderer::DescribeRenderGraphFailureReason(failures.MainReason));
            CHECK(failures.TotalPixels == 0);
        }
    }
    REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::Complete);
    const renderer::RenderGraphCaptureInfo info = capture.GetInfo();
    CHECK(info.FailedPixels == 0);
    const BorrowedView<const renderer::RenderGraphCapturePixel> pixels = capture.GetPixels();
    REQUIRE(pixels.GetSize() == usize{WIDTH} * HEIGHT);
    for (usize index = 0; index < pixels.GetSize(); ++index)
    {
        const renderer::RenderGraphCapturePixel& pixel = pixels[index];
        CHECK(pixel.Samples == SAMPLES);
        CHECK(math::IsFinite(pixel.Mean) == true);
    }
    std::printf("wave graph: %zu triangles, %ux%u, %u spp, no failed pixels\n", triangleCount, WIDTH, HEIGHT, SAMPLES);
}

TEST_CASE("Runtime graph admission covers the shipped scene import profile", "[renderer][graph][limits]")
{
    namespace graph = ludus::content::render_graph_format;
    namespace scene = ludus::content::scene_format;
    CHECK(graph::MAX_MESHES >= scene::IMPORT_LIMITS.Meshes);
    CHECK(graph::MAX_ITEMS >= scene::IMPORT_LIMITS.Objects);
    CHECK(graph::MAX_ITEMS >= scene::IMPORT_LIMITS.Materials);
}
