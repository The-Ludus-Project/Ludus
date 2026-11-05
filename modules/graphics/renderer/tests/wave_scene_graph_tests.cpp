#include "../src/internal/scene_material_validation.h"
#include "renderer_composite.h"
#include <cstdio>
#include <cstdlib>
#include <ludus/content/content.h>
#include <ludus/content/scene_format/limits.h>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/runtime/resources/scene.h>

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/render_graph_capture.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

namespace
{
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
using rhi::RasterStatus;
constexpr uint32 EXTENT = 4;
constexpr usize QUAD_VERTICES = 4;
constexpr usize QUAD_INDICES = 6;
constexpr usize TABLE_COUNT = 2;
constexpr usize CAMERA_COUNT = 1;
constexpr uint64 COLD_CAPTURE_TIMEOUT_NANOSECONDS =
    LUDUS_RENDERER_TEST_COLD_GPU_TIMEOUT_SECONDS * time::NANOSECONDS_PER_SECOND;
constexpr std::chrono::milliseconds COMPLETION_POLL_INTERVAL{1};
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
void LoadPackage(renderer::RenderGraphPackage& outPackage)
{
    filesystem::Directory directory;
    REQUIRE(directory.Open(LUDUS_WAVE_SCENE_TEMPLATE_DIRECTORY).Succeeded() == true);
    filesystem::File file;
    REQUIRE(directory.OpenRead("wave_scene_graph.lurgrt", file).Succeeded() == true);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(static_cast<usize>(file.Size())) == true);
    REQUIRE(file.ReadAt(0, bytes.AsSpan()).BytesRead == bytes.GetSize());
    REQUIRE(outPackage.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
}
using ludus::content::scene_format::MaterialModel;
void CheckSceneWaveGraph(uint32 screen, MaterialModel model = MaterialModel::MetallicRoughness)
{
    Session session;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.Required.PortableRaster = true;
    const rhi::DeviceStatus created =
        rhi::CreateDevice({}, { .Width = EXTENT, .Height = EXTENT }, description, device, surface);
    if (created != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No compute adapter");
    }
    REQUIRE(created == rhi::DeviceStatus::Ready);
    renderer::RenderGraphPackage package;
    LoadPackage(package);
    const renderer::RenderGraphPlan plan = package.GetPlan();
    const renderer::RenderGraphPrograms programs = package.GetPrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                       ludus::shaders::renderer_composite::Fragment());
    REQUIRE(renderer::RequiresHardwareRayTracing(plan) == false);
    REQUIRE(plan.Passes.GetSize() == 4);
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
    renderer::SceneShaderMaterial material;
    if (model != MaterialModel::MetallicRoughness)
    {
        material.BaseColor = {0.8F, 0.7F, 0.6F};
        material.Scattering.Model = model;
        if (model == MaterialModel::ProjectBrdf)
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
        inputs.Materials = {&material, 1};
        items[0].MaterialOverrides = {&materialIndex, 1};
    }
    // Primary pass index zero only writes shared hits. The following passes export wave, geometric and aperture.
    for (uint32 pass = 1; pass < plan.Passes.GetSize(); ++pass)
    {
        renderer::RenderGraphCapture capture;
        renderer::RenderGraphCaptureSettings settings;
        settings.Output = plan.Passes[pass].Color;
        settings.TargetSamples = 1;
        const RasterStatus started = capture.Start(device, surface, plan, programs, EXTENT, EXTENT, settings);
        REQUIRE((started == RasterStatus::Ready || started == RasterStatus::Pending));
        const uint64 beginning = time::NowTicks();
        while (capture.GetPhase() == renderer::RenderGraphCapturePhase::Initializing &&
               time::NowTicks() - beginning < COLD_CAPTURE_TIMEOUT_NANOSECONDS)
        {
            capture.Service();
            std::this_thread::sleep_for(COMPLETION_POLL_INTERVAL);
        }
        REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::AwaitingScene);
        const usize itemCount = screen == 0 ? 1 : TABLE_COUNT;
        const uint64 captureBeginning = time::NowTicks();
        const RasterStatus submitted = capture.SubmitScene({meshes.GetData(), itemCount},
                                                           {items.GetData(), itemCount},
                                                           {cameras.GetData(), CAMERA_COUNT},
                                                           1,
                                                           &inputs);
        REQUIRE((submitted == RasterStatus::Ready || submitted == RasterStatus::Pending));
        while (capture.GetPhase() != renderer::RenderGraphCapturePhase::Complete &&
               capture.GetPhase() != renderer::RenderGraphCapturePhase::Failed &&
               time::NowTicks() - captureBeginning < COLD_CAPTURE_TIMEOUT_NANOSECONDS)
        {
            capture.Service();
            std::this_thread::sleep_for(COMPLETION_POLL_INTERVAL);
        }
        const uint64 elapsedNanoseconds = time::NowTicks() - captureBeginning;
        CAPTURE(pass,
                screen,
                capture.GetPhase(),
                capture.GetStatus(),
                capture.GetProblem(),
                elapsedNanoseconds,
                COLD_CAPTURE_TIMEOUT_NANOSECONDS);
        REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::Complete);
        const BorrowedView<const renderer::RenderGraphCapturePixel> pixels = capture.GetPixels();
        REQUIRE(pixels.GetSize() == usize{EXTENT} * EXTENT);
        for (usize index = 0; index < pixels.GetSize(); ++index)
        {
            const float32 value = pixels[index].Mean.X;
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

TEST_CASE("Scene wave graph blocks an opaque screen from the current World", "[renderer][gpu][wave-tracing][graph]")
{
    CheckSceneWaveGraph(1);
}

TEST_CASE("Scene wave graph diffracts beyond a half screen from the current World",
          "[renderer][gpu][wave-tracing][graph]")
{
    CheckSceneWaveGraph(2);
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
    LoadPackage(package);
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
        std::this_thread::sleep_for(COMPLETION_POLL_INTERVAL);
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
        std::this_thread::sleep_for(COMPLETION_POLL_INTERVAL);
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
