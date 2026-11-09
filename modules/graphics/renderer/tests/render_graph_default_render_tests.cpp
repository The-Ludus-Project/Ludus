// Default-graph pixel suite: clustered deferred against forward shading, the forward-lit graph,
// bilinear and spatial upscaling (MetalFX where available) and deferred mesh area lights.

#include "../src/internal/raster_mesh_lights.h"
#include "../src/internal/render_graph_metalfx.h"
#include "../src/internal/render_graph_upscale.h"
#include "render_graph_gpu_fixture.h"
#include "renderer_composite.h"
#include "rg2_forward_lit.h"
#include "rg2_offscreen.h"

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/graphics/rhi/device.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <ludus/graphics/rhi/spatial_upscale.h>

#include <chrono>
#include <thread>

using namespace ludus::foundation;
using namespace ludus::graphics::renderer::testing;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace packages = ludus::render_graph_packages;
namespace shaders = ludus::shaders;
namespace format = ludus::content::render_graph_format;
TEST_CASE("default clustered deferred matches forward material shading and preserves optional prepass pixels",
          "[render-graph][clustered-deferred][gpu]")
{
    constexpr usize LIGHT_COUNT = 40;
    constexpr usize ENVIRONMENT_TEXEL_COUNT = 2;
    constexpr usize LIGHTING_CASE_COUNT = 4;
    constexpr usize PROFILE_COUNT = 3;
    StaticArray<StaticArray<uint8, PIXEL_BYTES>, LIGHTING_CASE_COUNT> forwardPixels{};
    StaticArray<StaticArray<uint8, PIXEL_BYTES>, LIGHTING_CASE_COUNT> forwardDepthPixels{};
    constexpr usize POLL_ATTEMPTS = 10000;
    StaticArray<StaticArray<uint8, PIXEL_BYTES>, LIGHTING_CASE_COUNT> ordinaryPixels{};
    for (usize profile = 0; profile < PROFILE_COUNT; ++profile)
    {
        const bool shouldUsePrepass = profile == 2;
        GraphGpuFixture fixture;
        const PackageLocation location =
            profile == 0
                ? PackageLocation{LUDUS_RG3_CLUSTERED_FORWARD_DIRECTORY, "rg2_clustered_forward_control.lurgrt"}
            : shouldUsePrepass == true
                ? PackageLocation{LUDUS_RG3_CLUSTERED_ZPREPASS_DIRECTORY, "rg2_clustered_ZPrepass.lurgrt"}
                : PackageLocation{LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"};
        const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(location);
        const renderer::RenderGraphPrograms programs =
            package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
        REQUIRE(programs.HasClusteredForward == true);
        REQUIRE(programs.HasClusteredDeferred == (profile != 0));
        CHECK(package->GetPlan().Passes.GetSize() == (shouldUsePrepass == true ? 2 : 1));
        if (shouldUsePrepass == true)
        {
            CHECK(package->GetPlan().Passes[0].AllowsRepeatedCoverage == 1);
            CHECK(package->GetPlan().Passes[1].AllowsRepeatedCoverage == 1);
        }
        const auto poll = [&](rhi::RasterStatus status) {
            for (usize attempt = 0; attempt < POLL_ATTEMPTS && status == rhi::RasterStatus::Pending; ++attempt)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                status = fixture.Graph.Poll();
            }
            return status;
        };
        REQUIRE(poll(fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
                rhi::RasterStatus::Ready);
        const StaticArray<math::Vector3, VERTEX_COUNT> normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
        fixture.Meshes[0].Geometry.Normals = normals.GetData();
        const uint32 materialIndex = 0;
        fixture.Meshes[0].MaterialBindings = {&materialIndex, 1};
        constexpr usize MATERIAL_COUNT = 2;
        StaticArray<renderer::SceneShaderMaterial, MATERIAL_COUNT> materials{};
        materials[1].BaseColor = {.2F, .6F, .8F};
        materials[1].Metallic = .5F;
        materials[1].Roughness = .3F;
        materials[1].Emission = {.1F, .02F, .03F};
        const StaticArray<uint8, TEXEL_BYTES> surfaceMap{{140, 120, 150, 200}};
        materials[1].SurfaceTexture =
            {surfaceMap.GetData(), surfaceMap.GetSize(), surfaceMap.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Unorm};
        const StaticArray<uint8, TEXEL_BYTES> emissionMap{{128, 255, 64, 255}};
        materials[1].EmissionTexture =
            {emissionMap.GetData(), emissionMap.GetSize(), emissionMap.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Unorm};
        StaticArray<renderer::RenderGraphItem, MATERIAL_COUNT> items{{fixture.Items[0], fixture.Items[0]}};
        items[1].SourceId = 2;
        items[1].MeshToWorld.Translation = {.2F, 0, -.1F};
        const uint32 secondMaterial = 1;
        items[1].MaterialOverrides = {&secondMaterial, 1};
        renderer::SceneShaderInputs inputs;
        inputs.Materials = {materials.GetData(), materials.GetSize()};
        inputs.Environment = {.02F, .03F, .01F};
        const StaticArray<math::Vector3, ENVIRONMENT_TEXEL_COUNT> environmentPixels{
            {{.01F, .04F, .02F}, {.04F, .01F, .03F}}};
        StaticArray<renderer::DirectLight, LIGHT_COUNT> lights{};
        inputs.Lights = {lights.GetData(), lights.GetSize()};
        uint64 revision = 0;
        uint64 contentRevision = 0;
        const auto update = [&]() {
            renderer::RenderGraphReport report;
            ++revision;
            ++contentRevision;
            return poll(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                             {items.GetData(), items.GetSize()},
                                             {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                             revision,
                                             renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                             report,
                                             &inputs,
                                             contentRevision));
        };
        const auto updateCamera = [&]() {
            renderer::RenderGraphReport report;
            ++revision;
            return poll(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                             {items.GetData(), items.GetSize()},
                                             {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                             revision,
                                             renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                             report,
                                             &inputs,
                                             contentRevision));
        };
        rhi::TextureHandle texture;
        rhi::TextureAspect aspect;
        REQUIRE(fixture.Graph.GetExportAllocation(0, texture, aspect) == rhi::RasterStatus::Ready);
        renderer::Renderer compositor;
        REQUIRE(compositor.Initialize(fixture.Device, programs.ColorVertex, programs.ColorFragment) ==
                rhi::RasterStatus::Ready);
        REQUIRE(compositor.InitializeViews(shaders::renderer_composite::Vertex(),
                                           shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
        renderer::ViewMapping mapping;
        REQUIRE(renderer::ResolveViewMapping({WIDTH, HEIGHT, renderer::ScreenFit::Stretch},
                                             {0, 0, WIDTH, HEIGHT},
                                             WIDTH,
                                             HEIGHT,
                                             mapping) == rhi::RasterStatus::Ready);
        DepthDisplay depthDisplay(fixture.Device);
        renderer::Presentation presentation;
        REQUIRE(compositor.PreparePresentation(texture, mapping, presentation) == rhi::RasterStatus::Ready);
        const auto render = [&]() {
            REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
            REQUIRE(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
            REQUIRE(compositor.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
            rhi::SubmissionToken completion;
            REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
            const rhi::RasterStatus confirmed = fixture.Graph.ConfirmSubmission(completion);
            REQUIRE((confirmed == rhi::RasterStatus::Ready || confirmed == rhi::RasterStatus::Pending));
            REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
        };
        for (usize lightingCase = 0; lightingCase < LIGHTING_CASE_COUNT; ++lightingCase)
        {
            const bool hasEnvironmentMap = lightingCase + 1 == LIGHTING_CASE_COUNT;
            inputs.Environment =
                hasEnvironmentMap == true ? math::Vector3{16, 16, 16} : math::Vector3{.02F, .03F, .01F};
            inputs.EnvironmentWidth = hasEnvironmentMap == true ? 2 : 0;
            inputs.EnvironmentHeight = hasEnvironmentMap == true ? 1 : 0;
            inputs.EnvironmentPixels =
                hasEnvironmentMap == true
                    ? BorrowedView<const math::Vector3>{environmentPixels.GetData(), environmentPixels.GetSize()}
                    : BorrowedView<const math::Vector3>{};
            const bool areDirectional = lightingCase % 2 == 0;
            // Compare the material paths with no inter-object occlusion in the directional cases.
            items[1].MeshToWorld.Translation.Z = areDirectional == true ? 0.0F : -0.1F;
            fixture.Cameras[0].OrthographicCamera = lightingCase >= 2;
            fixture.Cameras[0].WorldToClip.Columns[2].W = lightingCase >= 2 ? 0 : .2F;
            fixture.Cameras[0].WorldToClip.Columns[3].X = .1F;
            for (usize light = 0; light < LIGHT_COUNT; ++light)
            {
                lights[light].SourceId = light + 1;
                lights[light].Color = light + 1 == LIGHT_COUNT ? math::Vector3{0, 0, 1} : math::Vector3{1, 0, 0};
                lights[light].Direction = {0, 0, -1};
                lights[light].Intensity = light + 1 == LIGHT_COUNT ? 0.5F : 0.02F;
                lights[light].Kind =
                    areDirectional == true ? renderer::LightKind::Directional : renderer::LightKind::Point;
                lights[light].Position = {100 + static_cast<float32>(light), 0, 1};
                lights[light].Range = 0.1F;
            }
            lights[LIGHT_COUNT - 1].Position = {0, 0, 1};
            lights[LIGHT_COUNT - 1].Range = 2;
            fixture.Cameras[0].DirectLighting = false;
            REQUIRE(update() == rhi::RasterStatus::Ready);
            const renderer::RenderGraphSceneResourceReport report = fixture.Graph.GetReport().Resources;
            CHECK(report.DroppedLights == 0);
            CHECK(report.OverflowClusters == (areDirectional == true ? 512 : 0));
            render();
            CHECK(fixture.Pixels[GetPixel(WIDTH / 2, HEIGHT / 2) + 2] > 10);
            const StaticArray<uint8, PIXEL_BYTES> clustered = fixture.Pixels;
            StaticArray<uint8, PIXEL_BYTES> depthPixels{};
            depthDisplay.Show(fixture.Surface, texture, depthPixels);
            if (profile == 0)
            {
                forwardDepthPixels[lightingCase] = depthPixels;
            }
            else
            {
                CHECK(depthPixels == forwardDepthPixels[lightingCase]);
            }
            if (profile == 0)
            {
                forwardPixels[lightingCase] = clustered;
            }
            else
            {
                // RGBA16F attributes and depth reconstruction may move a final linear RGBA8 channel by two codes.
                for (usize channel = 0; channel < PIXEL_BYTES; ++channel)
                {
                    CHECK(math::Abs(static_cast<float32>(clustered[channel]) -
                                    static_cast<float32>(forwardPixels[lightingCase][channel])) <= 2);
                }
                const renderer::RenderGraphAllocationReport allocation = fixture.Graph.GetAllocationReport();
                CHECK(allocation.DeferredAllocations == 3);
                CHECK(allocation.DeferredAttachmentBytes == uint64{WIDTH} * HEIGHT * 36);
            }
            if (profile == 1)
            {
                ordinaryPixels[lightingCase] = clustered;
            }
            else if (shouldUsePrepass == true)
            {
                CHECK(clustered == ordinaryPixels[lightingCase]);
            }
            fixture.Cameras[0].DirectLighting = true;
            REQUIRE(updateCamera() == rhi::RasterStatus::Ready);
            render();
            for (usize channel = 0; channel < PIXEL_BYTES; ++channel)
            {
                CHECK(math::Abs(static_cast<float32>(clustered[channel]) -
                                static_cast<float32>(fixture.Pixels[channel])) <= 1);
            }
            // Failed replacement retains the last successfully published scene and pixels.
            const StaticArray<uint8, PIXEL_BYTES> lastDrawable = fixture.Pixels;
            const uint64 lastDrawableRevision = fixture.Graph.GetDrawableSceneRevision();
            if (profile != 0)
            {
                const math::Matrix4 validWorldToClip = fixture.Cameras[0].WorldToClip;
                fixture.Cameras[0].WorldToClip.Columns[0] = {};
                REQUIRE(updateCamera() != rhi::RasterStatus::Ready);
                CHECK(fixture.Graph.GetDrawableSceneRevision() == lastDrawableRevision);
                render();
                CHECK(fixture.Pixels == lastDrawable);
                fixture.Cameras[0].WorldToClip = validWorldToClip;
            }
            lights[LIGHT_COUNT - 1].Color = {0, 1, 0};
            lights[LIGHT_COUNT - 1].SourceId = lights[0].SourceId;
            REQUIRE(update() != rhi::RasterStatus::Ready);
            CHECK(fixture.Graph.GetDrawableSceneRevision() == lastDrawableRevision);
            render();
            CHECK(fixture.Pixels == lastDrawable);
        }
        if (profile == 1)
        {
            lights[LIGHT_COUNT - 1].SourceId = LIGHT_COUNT;
            lights[LIGHT_COUNT - 1].Color = {0, 0, 1};
            REQUIRE(update() == rhi::RasterStatus::Ready);
            render();
            const StaticArray<uint8, PIXEL_BYTES> beforeResize = fixture.Pixels;
            REQUIRE(fixture.Graph.Resize(WIDTH / 2, HEIGHT / 2) == rhi::RasterStatus::Pending);
            render();
            CHECK(fixture.Pixels == beforeResize);
            REQUIRE(update() == rhi::RasterStatus::Ready);
            CHECK(fixture.Graph.HasPendingResize() == false);
            REQUIRE(fixture.Graph.GetExportAllocation(0, texture, aspect) == rhi::RasterStatus::Ready);
            REQUIRE(compositor.Release(presentation) == rhi::RasterStatus::Ready);
            REQUIRE(renderer::ResolveViewMapping({WIDTH / 2, HEIGHT / 2, renderer::ScreenFit::Stretch},
                                                 {0, 0, WIDTH, HEIGHT},
                                                 WIDTH,
                                                 HEIGHT,
                                                 mapping) == rhi::RasterStatus::Ready);
            REQUIRE(compositor.PreparePresentation(texture, mapping, presentation) == rhi::RasterStatus::Ready);
            render();
            CHECK(fixture.Pixels[GetPixel(WIDTH / 2, HEIGHT / 2) + 2] > 10);
            CHECK(fixture.Graph.GetAllocationReport().DeferredAttachmentBytes == uint64{WIDTH / 2} * (HEIGHT / 2) * 36);
        }
        // Mask state must affect material/depth coverage together. The optional
        // opaque-only prepass rejects this table before it can publish solid holes.
        CAPTURE(profile);
        lights[LIGHT_COUNT - 1].SourceId = LIGHT_COUNT;
        lights[LIGHT_COUNT - 1].Color = {0, 0, 1};
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        const StaticArray<uint8, PIXEL_BYTES> retained = fixture.Pixels;
        const uint64 retainedRevision = fixture.Graph.GetDrawableSceneRevision();
        StaticArray<uint8, TEXEL_BYTES> coverage{{255, 255, 255, 0}};
        for (renderer::SceneShaderMaterial& material : materials)
        {
            material.IsAlphaMasked = true;
            material.AlphaCutoff = .5F;
            material.ColorTexture =
                {coverage.GetData(), coverage.GetSize(), coverage.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Srgb};
        }
        if (shouldUsePrepass == true)
        {
            CHECK(update() == rhi::RasterStatus::Unsupported);
            CHECK(fixture.Graph.GetDrawableSceneRevision() == retainedRevision);
            render();
            CHECK(fixture.Pixels == retained);
        }
        else
        {
            REQUIRE(update() == rhi::RasterStatus::Ready);
            render();
            for (usize channel = 0; channel < 3; ++channel)
            {
                CHECK(fixture.Pixels[GetPixel(WIDTH / 2, HEIGHT / 2) + channel] == fixture.Pixels[channel]);
            }
        }
        coverage[3] = 255;
        for (renderer::SceneShaderMaterial& material : materials)
        {
            material.IsAlphaMasked = false;
        }
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[GetPixel(WIDTH / 2, HEIGHT / 2) + 2] > 0);
    }
}

TEST_CASE("forward-lit reuses immutable assets and preserves the drawable scene on rejection",
          "[render-graph][forward-lit][gpu]")
{
    GraphGpuFixture fixture;
    const renderer::RenderGraphPrograms programs =
        packages::rg2_forward_lit::MakePrograms(shaders::renderer_composite::Vertex(),
                                                shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPlan plan = packages::rg2_forward_lit::MakePlan();
    rhi::RasterStatus initialized = fixture.Graph.Initialize(fixture.Device, plan, programs, nullptr, WIDTH, HEIGHT);
    for (usize attempt = 0; attempt < 10000 && initialized == rhi::RasterStatus::Pending; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        initialized = fixture.Graph.Poll();
    }
    REQUIRE(initialized == rhi::RasterStatus::Ready);
    const StaticArray<math::Vector3, VERTEX_COUNT> normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    fixture.Meshes[0].Geometry.Normals = normals.GetData();
    const uint32 materialIndex = 0;
    fixture.Meshes[0].MaterialBindings = {&materialIndex, 1};
    renderer::SceneShaderMaterial material;
    material.Emission = {1, 0, 0};
    const StaticArray<uint8, TEXEL_BYTES> white{{255, 255, 255, 255}};
    material.ColorTexture = {white.GetData(), white.GetSize(), white.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Unorm};
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&material, 1};
    renderer::RenderGraphReport report;
    const auto update = [&](uint64 revision) {
        rhi::RasterStatus status = fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                                        {fixture.Items.GetData(), fixture.Items.GetSize()},
                                                        {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                                        revision,
                                                        renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                        report,
                                                        &inputs,
                                                        revision);
        for (usize attempt = 0; attempt < 10000 && status == rhi::RasterStatus::Pending; ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            status = fixture.Graph.Poll();
        }
        return status;
    };
    REQUIRE(update(1) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetReport().Resources.CreatedMeshes == 1);
    CHECK(fixture.Graph.GetReport().Resources.CreatedTextures == 1);
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    REQUIRE(fixture.Graph.GetExportAllocation(0, texture, aspect) == rhi::RasterStatus::Ready);
    renderer::Renderer compositor;
    REQUIRE(compositor.Initialize(fixture.Device, programs.ColorVertex, programs.ColorFragment) ==
            rhi::RasterStatus::Ready);
    REQUIRE(compositor.InitializeViews(shaders::renderer_composite::Vertex(),
                                       shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    renderer::ViewMapping mapping;
    REQUIRE(renderer::ResolveViewMapping({WIDTH, HEIGHT, renderer::ScreenFit::Stretch},
                                         {0, 0, WIDTH, HEIGHT},
                                         WIDTH,
                                         HEIGHT,
                                         mapping) == rhi::RasterStatus::Ready);
    renderer::Presentation presentation;
    REQUIRE(compositor.PreparePresentation(texture, mapping, presentation) == rhi::RasterStatus::Ready);
    const auto render = [&]() {
        REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
        REQUIRE(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
        REQUIRE(compositor.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
        const rhi::RasterStatus confirmed = fixture.Graph.ConfirmSubmission(completion);
        REQUIRE((confirmed == rhi::RasterStatus::Ready || confirmed == rhi::RasterStatus::Pending));
        REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
    };
    render();
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    CHECK(fixture.Pixels[center] > 200);
    CHECK(fixture.Pixels[center + 1] < 10);
    material.Emission = {0, 1, 0};
    REQUIRE(update(2) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetReport().Resources.CreatedMeshes == 1);
    CHECK(fixture.Graph.GetReport().Resources.CreatedTextures == 1);
    CHECK(fixture.Graph.GetReport().Resources.ReusedMeshes == 1);
    CHECK(fixture.Graph.GetReport().Resources.ReusedTextures == 1);
    render();
    CHECK(fixture.Pixels[center] < 10);
    CHECK(fixture.Pixels[center + 1] > 200);
    fixture.Meshes[0].Geometry.Normals = nullptr;
    CHECK(update(3) != rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 2);
    render();
    CHECK(fixture.Pixels[center + 1] > 200);
    fixture.Meshes[0].Geometry.Normals = normals.GetData();
    material.Emission = {};
    renderer::DirectLight light;
    light.SourceId = 1;
    light.Color = {0, 0, 1};
    light.Direction = {0, 0, -1};
    light.Intensity = 1;
    inputs.Lights = {&light, 1};
    REQUIRE(update(4) == rhi::RasterStatus::Ready);
    render();
    CHECK(fixture.Pixels[center] < 10);
    CHECK(fixture.Pixels[center + 2] > 10);
    CHECK(fixture.Graph.GetReport().Resources.CreatedMeshes == 1);
    CHECK(fixture.Graph.GetReport().Resources.CreatedTextures == 1);
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_FORWARD_DIRECTORY, "rg2_forward_lit.lurgrt"});
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    CHECK(decoded.ForwardVertex.Inputs.size() == format::FORWARD_INPUT_COUNT);
    CHECK(decoded.ForwardFragment.Bindings.size() == programs.ForwardFragment.Bindings.size());
}

TEST_CASE("bilinear GPU sampling matches pixel centers and clamps source edges", "[render-graph][gpu][bilinear]")
{
    GraphGpuFixture fixture;
    constexpr usize SOURCE_BYTES = 16;
    constexpr usize CHANNEL_COUNT = 4;
    const StaticArray<uint8, SOURCE_BYTES> pixels{{255, 0, 0, 0, 0, 255, 0, 64, 0, 0, 255, 128, 255, 255, 255, 255}};
    rhi::TextureDescription sourceDescription;
    sourceDescription.Width = 2;
    sourceDescription.Height = 2;
    rhi::TextureHandle source;
    REQUIRE(rhi::CreateTexture(fixture.Device, sourceDescription, {pixels.AsSpan(), 2 * CHANNEL_COUNT}, source) ==
            rhi::RasterStatus::Ready);
    renderer::internal::RenderGraphUpscaleProgram program;
    REQUIRE(program.Initialize(fixture.Device,
                               shaders::renderer_composite::Vertex(),
                               shaders::renderer_composite::Fragment(),
                               true) == rhi::RasterStatus::Ready);
    renderer::internal::RenderGraphUpscalePass upscale;
    REQUIRE(upscale.Initialize(program, source) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(fixture.Device, source) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(upscale.Draw({}) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
    // Independent CPU reference: pixel centers map to source texel coordinates u*2-0.5,
    // with edge clamping. The four source colors make red/green/blue orientation observable.
    for (uint32 y = 0; y < HEIGHT; ++y)
    {
        const float32 positionY = (static_cast<float32>(y) + .5F) * 2 / static_cast<float32>(HEIGHT) - .5F;
        const float32 weightY = positionY < 0 ? 0 : positionY > 1 ? 1 : positionY;
        for (uint32 x = 0; x < WIDTH; ++x)
        {
            const float32 positionX = (static_cast<float32>(x) + .5F) * 2 / static_cast<float32>(WIDTH) - .5F;
            const float32 weightX = positionX < 0 ? 0 : positionX > 1 ? 1 : positionX;
            for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
            {
                const float32 top = static_cast<float32>(pixels[channel]) * (1 - weightX) +
                                    static_cast<float32>(pixels[CHANNEL_COUNT + channel]) * weightX;
                const float32 bottom = static_cast<float32>(pixels[2 * CHANNEL_COUNT + channel]) * (1 - weightX) +
                                       static_cast<float32>(pixels[3 * CHANNEL_COUNT + channel]) * weightX;
                const int32 expected = static_cast<int32>(top * (1 - weightY) + bottom * weightY + .5F);
                const int32 actual = fixture.Pixels[(static_cast<usize>(y) * WIDTH + x) * CHANNEL_COUNT + channel];
                REQUIRE(actual >= expected - 1);
                REQUIRE(actual <= expected + 1);
            }
        }
    }
}

namespace
{
void CheckSpatialGraphCameraEdits(format::Technique technique)
{
    GraphGpuFixture fixture;
    if (technique != format::Technique::BilinearUpscale)
    {
        rhi::SpatialUpscaleCapabilities capabilities;
        REQUIRE(rhi::GetSpatialUpscaleCapabilities(fixture.Device, capabilities) == rhi::RasterStatus::Ready);
        if (capabilities.IsMetalFxSpatialSupported == false)
        {
            SKIP("MetalFX Spatial unsupported on this device");
        }
    }
    constexpr usize UPSCALE_RESOURCE_COUNT = 3;
    constexpr usize UPSCALE_PASS_COUNT = 2;
    const StaticArray<format::Resource, UPSCALE_RESOURCE_COUNT> resources{
        {{WIDTH / 2, HEIGHT / 2, format::Aspect::Color, 0, 0, 0},
         {WIDTH / 2, HEIGHT / 2, format::Aspect::Depth, 0, 0, 0},
         {WIDTH, HEIGHT, format::Aspect::Color, 2, 1, 0}}};
    StaticArray<format::Pass, UPSCALE_PASS_COUNT> passes{{packages::rg2_offscreen::PASSES[0],
                                                          {technique,
                                                           0,
                                                           2,
                                                           format::ABSENT,
                                                           format::Load::Clear,
                                                           format::Store::Store,
                                                           format::Load::Discard,
                                                           format::Store::Discard,
                                                           0}}};
    passes[1].SceneData = 0;
    passes[1].SourceColor = 0;
    const StaticArray<format::Camera, TABLE_COUNT> cameras{{{2}}};
    renderer::RenderGraphPlan plan = packages::rg2_offscreen::MakePlan();
    plan.Description.ResourceCount = UPSCALE_RESOURCE_COUNT;
    plan.Description.PassCount = UPSCALE_PASS_COUNT;
    plan.Resources = {resources.GetData(), resources.GetSize()};
    plan.Passes = {passes.GetData(), passes.GetSize()};
    plan.Cameras = {cameras.GetData(), cameras.GetSize()};
    plan.PassNames = {};
    fixture.Items[0].Material.Color = {.25F, .5F, .75F, 1};
    REQUIRE(fixture.Graph.Initialize(fixture.Device,
                                     plan,
                                     packages::rg2_offscreen::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                           shaders::renderer_composite::Fragment())) ==
            rhi::RasterStatus::Ready);
    fixture.Update(1);
    fixture.Render();
    const StaticArray<uint8, PIXEL_BYTES> before = fixture.Pixels;
    bool hasInterpolatedEdge = false;
    for (usize pixel = 0; pixel < PIXEL_BYTES; pixel += TEXEL_BYTES)
    {
        hasInterpolatedEdge = hasInterpolatedEdge == true || (fixture.Pixels[pixel] > 0 && fixture.Pixels[pixel] < 64);
    }
    CHECK(hasInterpolatedEdge == true);
    fixture.Cameras[0].WorldToClip.Columns[3].X = .2F;
    fixture.Update(2);
    fixture.Render();
    bool hasChanged = false;
    for (usize index = 0; index < PIXEL_BYTES; ++index)
    {
        hasChanged = hasChanged == true || before[index] != fixture.Pixels[index];
    }
    CHECK(hasChanged == true);
}

} // namespace

TEST_CASE("spatial graph executes rendered scene color and responds to camera edits", "[render-graph][gpu][bilinear]")
{
    CheckSpatialGraphCameraEdits(format::Technique::BilinearUpscale);
}

// The trusted Vulkan validation workflow requires [render-graph] cases to execute without skips.
// Platform-specific MetalFX cases remain in the full GPU suite and macOS qualification.
TEST_CASE("MetalFX graph executes rendered scene color and responds to camera edits",
          "[render-graph-metalfx][gpu][metalfx]")
{
    const format::Technique technique =
        GENERATE(format::Technique::MetalFxSpatialUpscale, format::Technique::MetalFxSpatialPerceptualUpscale);
    CheckSpatialGraphCameraEdits(technique);
}

TEST_CASE("MetalFX preserves constant RGB through its private output and surface copy",
          "[render-graph-metalfx][gpu][metalfx]")
{
    const rhi::SpatialUpscaleColorProcessing mode =
        GENERATE(rhi::SpatialUpscaleColorProcessing::Linear, rhi::SpatialUpscaleColorProcessing::Perceptual);
    GraphGpuFixture fixture;
    rhi::SpatialUpscaleCapabilities capabilities;
    REQUIRE(rhi::GetSpatialUpscaleCapabilities(fixture.Device, capabilities) == rhi::RasterStatus::Ready);
    if (capabilities.IsMetalFxSpatialSupported == false)
    {
        SKIP("MetalFX Spatial unsupported on this device");
    }
    constexpr uint32 SOURCE_WIDTH = WIDTH / 2;
    constexpr uint32 SOURCE_HEIGHT = HEIGHT / 2;
    constexpr usize SOURCE_BYTES = static_cast<usize>(SOURCE_WIDTH) * SOURCE_HEIGHT * TEXEL_BYTES;
    StaticArray<uint8, SOURCE_BYTES> pixels{};
    for (usize pixel = 0; pixel < SOURCE_BYTES; pixel += TEXEL_BYTES)
    {
        pixels[pixel] = 64;
        pixels[pixel + 1] = 128;
        pixels[pixel + 2] = 192;
        pixels[pixel + TEXEL_BYTES - 1] = 255;
    }
    rhi::TextureDescription sourceDescription;
    sourceDescription.Width = SOURCE_WIDTH;
    sourceDescription.Height = SOURCE_HEIGHT;
    rhi::TextureHandle source;
    REQUIRE(
        rhi::CreateTexture(fixture.Device, sourceDescription, {pixels.AsSpan(), SOURCE_WIDTH * TEXEL_BYTES}, source) ==
        rhi::RasterStatus::Ready);
    renderer::internal::RenderGraphUpscaleProgram program;
    REQUIRE(program.Initialize(fixture.Device,
                               shaders::renderer_composite::Vertex(),
                               shaders::renderer_composite::Fragment(),
                               true) == rhi::RasterStatus::Ready);
    renderer::internal::RenderGraphMetalFxPass pass;
    REQUIRE(pass.Initialize(fixture.Device, {SOURCE_WIDTH, SOURCE_HEIGHT, WIDTH, HEIGHT, mode}, source, {}, &program) ==
            rhi::RasterStatus::Ready);
    REQUIRE(pass.Poll() == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(pass.Draw() == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
    for (usize pixel = 0; pixel < PIXEL_BYTES; pixel += TEXEL_BYTES)
    {
        for (usize channel = 0; channel < TEXEL_BYTES - 1; ++channel)
        {
            const int32 actual = fixture.Pixels[pixel + channel];
            const int32 expected = pixels[channel];
            CHECK(actual >= expected - 2);
            CHECK(actual <= expected + 2);
        }
    }
    REQUIRE(rhi::Destroy(fixture.Device, source) == rhi::RasterStatus::Ready);
}

TEST_CASE("default deferred mesh area lights illuminate a receiver with CPU checked radiance", "[mesh-area][gpu]")
{
    constexpr usize SCENE_COUNT = 2;
    constexpr usize POLL_ATTEMPTS = 10000;
    StaticArray<uint8, PIXEL_BYTES> ordinary{};
    for (const bool shouldUsePrepass : {false, true})
    {
        GraphGpuFixture fixture;
        const PackageLocation location =
            shouldUsePrepass == true
                ? PackageLocation{LUDUS_RG3_CLUSTERED_ZPREPASS_DIRECTORY, "rg2_clustered_ZPrepass.lurgrt"}
                : PackageLocation{LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"};
        const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(location);
        const renderer::RenderGraphPrograms programs =
            package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
        const auto poll = [&](rhi::RasterStatus status) {
            for (usize attempt = 0; attempt < POLL_ATTEMPTS && status == rhi::RasterStatus::Pending; ++attempt)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                status = fixture.Graph.Poll();
            }
            return status;
        };
        REQUIRE(poll(fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
                rhi::RasterStatus::Ready);
        // This package exports an offscreen target; explicitly present it before reading the surface.
        rhi::TextureHandle outputTexture;
        rhi::TextureAspect outputAspect;
        REQUIRE(fixture.Graph.GetExportAllocation(0, outputTexture, outputAspect) == rhi::RasterStatus::Ready);
        renderer::Renderer compositor;
        REQUIRE(compositor.Initialize(fixture.Device, programs.ColorVertex, programs.ColorFragment) ==
                rhi::RasterStatus::Ready);
        REQUIRE(compositor.InitializeViews(shaders::renderer_composite::Vertex(),
                                           shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
        renderer::ViewMapping mapping;
        REQUIRE(renderer::ResolveViewMapping({WIDTH, HEIGHT, renderer::ScreenFit::Stretch},
                                             {0, 0, WIDTH, HEIGHT},
                                             WIDTH,
                                             HEIGHT,
                                             mapping) == rhi::RasterStatus::Ready);
        renderer::Presentation presentation;
        REQUIRE(compositor.PreparePresentation(outputTexture, mapping, presentation) == rhi::RasterStatus::Ready);
        const auto render = [&]() {
            REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
            REQUIRE(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
            REQUIRE(compositor.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
            rhi::SubmissionToken completion;
            REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
            const rhi::RasterStatus status = fixture.Graph.ConfirmSubmission(completion);
            REQUIRE((status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending));
            REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
        };
        const StaticArray<math::Vector3, VERTEX_COUNT> normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
        const StaticArray<uint32, INDEX_COUNT> reversed{{0, 2, 1}};
        StaticArray<renderer::RenderGraphMesh, SCENE_COUNT> meshes{{fixture.Meshes[0], fixture.Meshes[0]}};
        const uint32 receiverMaterial = 0;
        const uint32 emitterMaterial = 1;
        meshes[0].Geometry.Normals = normals.GetData();
        meshes[1].Geometry.Normals = normals.GetData();
        meshes[1].Geometry.Indices = reversed.GetData();
        meshes[0].MaterialBindings = {&receiverMaterial, 1};
        meshes[1].MaterialBindings = {&emitterMaterial, 1};
        StaticArray<renderer::RenderGraphItem, SCENE_COUNT> items{{fixture.Items[0], fixture.Items[0]}};
        items[1].SourceId = 2;
        items[1].MeshIndex = 1;
        items[1].MeshToWorld.Translation = {.8F, 0, .3F};
        StaticArray<renderer::SceneShaderMaterial, SCENE_COUNT> materials{};
        materials[1].BaseColor = {};
        materials[1].Emission = {.7F, .2F, 0};
        renderer::SceneShaderInputs inputs;
        inputs.Materials = {materials.GetData(), materials.GetSize()};
        fixture.Cameras[0].OrthographicCamera = true;
        uint64 revision = 0;
        uint64 contentRevision = 0;
        const auto update = [&](bool hasSceneChanges = true) {
            renderer::RenderGraphReport report;
            ++revision;
            if (hasSceneChanges == true)
            {
                ++contentRevision;
            }
            return poll(fixture.Graph.Update({meshes.GetData(), meshes.GetSize()},
                                             {items.GetData(), items.GetSize()},
                                             {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                             revision,
                                             renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                             report,
                                             &inputs,
                                             contentRevision));
        };
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Graph.GetReport().Resources.MeshLightPolygons == 1);
        const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
        CHECK(fixture.Pixels[center] > 0);
        CHECK(fixture.Pixels[center + 1] > 0);
        CHECK(fixture.Pixels[center + 2] == 0);
        Array<math::Vector4> words;
        uint32 recordCount = 0;
        REQUIRE(renderer::internal::AppendRasterMeshLights(meshes[1].Geometry,
                                                           items[1].MeshToWorld,
                                                           materials[1],
                                                           words,
                                                           recordCount) == rhi::RasterStatus::Ready);
        const math::Vector3 positionWorld{1.F / WIDTH, -1.F / HEIGHT, .5F};
        const math::Vector3 expected = renderer::internal::EvaluateRasterMeshLight({words.GetData(), words.GetSize()},
                                                                                   positionWorld,
                                                                                   {0, 0, 1},
                                                                                   {0, 0, 1},
                                                                                   {.5F, .5F, .5F},
                                                                                   0,
                                                                                   1);
        CHECK(math::Abs(float32(fixture.Pixels[center]) - expected.X * 255) <= 2);
        CHECK(math::Abs(float32(fixture.Pixels[center + 1]) - expected.Y * 255) <= 2);
        if (shouldUsePrepass == true)
        {
            CHECK(fixture.Pixels == ordinary);
        }
        else
        {
            ordinary = fixture.Pixels;
        }
        // Exercise the specular table on rough and smooth metals, then change only the camera.
        for (const float32 roughness : {.2F, .8F})
        {
            materials[0].Metallic = 1;
            materials[0].Roughness = roughness;
            REQUIRE(update() == rhi::RasterStatus::Ready);
            render();
            const math::Vector3 metalExpected =
                renderer::internal::EvaluateRasterMeshLight({words.GetData(), words.GetSize()},
                                                            positionWorld,
                                                            {0, 0, 1},
                                                            {0, 0, 1},
                                                            {.5F, .5F, .5F},
                                                            1,
                                                            roughness);
            CHECK(math::Abs(float32(fixture.Pixels[center]) - metalExpected.X * 255) <= 2);
            CHECK(math::Abs(float32(fixture.Pixels[center + 1]) - metalExpected.Y * 255) <= 2);
        }
        materials[0] = {};
        REQUIRE(update() == rhi::RasterStatus::Ready);
        const renderer::RenderGraphSceneResourceReport beforeCamera = fixture.Graph.GetReport().Resources;
        fixture.Cameras[0].CameraForward = {.3F, 0, -1};
        REQUIRE(update(false) == rhi::RasterStatus::Ready);
        render();
        math::Vector3 viewWorld;
        REQUIRE(math::TryNormalize(-fixture.Cameras[0].CameraForward, viewWorld) == math::MathStatus::Success);
        const math::Vector3 cameraExpected =
            renderer::internal::EvaluateRasterMeshLight({words.GetData(), words.GetSize()},
                                                        positionWorld,
                                                        {0, 0, 1},
                                                        viewWorld,
                                                        {.5F, .5F, .5F},
                                                        0,
                                                        1);
        CHECK(math::Abs(float32(fixture.Pixels[center]) - cameraExpected.X * 255) <= 2);
        CHECK(fixture.Graph.GetReport().Resources.CreatedMeshes == beforeCamera.CreatedMeshes);
        CHECK(fixture.Graph.GetReport().Resources.MeshLightBytes == beforeCamera.MeshLightBytes);
        fixture.Cameras[0].CameraForward = {0, 0, -1};
        // A perspective camera hundreds of metres from the origin with a centimetre near plane makes WorldToClip
        // itself ill-conditioned, as in the Sample project's Bistro. The resolve must still reconstruct the receiver.
        constexpr float32 FAR_OFFSET_METERS = 300;
        constexpr float32 NEAR_PLANE_METERS = .01F;
        constexpr float32 FAR_PLANE_METERS = 100;
        // One metre above the receiver plane at z = 0.5, looking down -z.
        constexpr float32 EYE_HEIGHT_METERS = 1.5F;
        const math::Vector3 offsetWorld{FAR_OFFSET_METERS, 0, FAR_OFFSET_METERS};
        const renderer::ViewDescription originCamera = fixture.Cameras[0];
        const StaticArray<renderer::RenderGraphItem, SCENE_COUNT> originItems = items;
        // A brighter emitter keeps the checked signal well above the comparison tolerance.
        constexpr float32 FAR_EMISSION_SCALE = 8;
        const math::Vector3 originEmission = materials[1].Emission;
        materials[1].Emission = originEmission * FAR_EMISSION_SCALE;
        for (renderer::RenderGraphItem& item : items)
        {
            item.MeshToWorld.Translation = item.MeshToWorld.Translation + offsetWorld;
        }
        const math::Vector3 eyeWorld = offsetWorld + math::Vector3{0, 0, EYE_HEIGHT_METERS};
        // Unit x/y scale; w is the distance below the eye, and conventional depth maps near/far to 0/1.
        const float32 depthScale = FAR_PLANE_METERS / (FAR_PLANE_METERS - NEAR_PLANE_METERS);
        renderer::ViewDescription& farCamera = fixture.Cameras[0];
        farCamera.OrthographicCamera = false;
        farCamera.Depth = renderer::DepthConvention::Conventional;
        farCamera.CameraPosition = eyeWorld;
        farCamera.WorldToClip.Columns[0] = {1, 0, 0, 0};
        farCamera.WorldToClip.Columns[1] = {0, 1, 0, 0};
        farCamera.WorldToClip.Columns[2] = {0, 0, -depthScale, -1};
        farCamera.WorldToClip.Columns[3] = {-eyeWorld.X,
                                            -eyeWorld.Y,
                                            depthScale * (eyeWorld.Z - NEAR_PLANE_METERS),
                                            eyeWorld.Z};
        math::Matrix4 directInverse;
        CHECK(math::TryInverse(farCamera.WorldToClip, directInverse) == math::MathStatus::IllConditioned);
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        Array<math::Vector4> farWords;
        uint32 farRecords = 0;
        REQUIRE(renderer::internal::AppendRasterMeshLights(meshes[1].Geometry,
                                                           items[1].MeshToWorld,
                                                           materials[1],
                                                           farWords,
                                                           farRecords) == rhi::RasterStatus::Ready);
        const math::Vector3 farPositionWorld = offsetWorld + positionWorld;
        math::Vector3 farViewWorld;
        REQUIRE(math::TryNormalize(eyeWorld - farPositionWorld, farViewWorld) == math::MathStatus::Success);
        const math::Vector3 farExpected =
            renderer::internal::EvaluateRasterMeshLight({farWords.GetData(), farWords.GetSize()},
                                                        farPositionWorld,
                                                        {0, 0, 1},
                                                        farViewWorld,
                                                        {.5F, .5F, .5F},
                                                        0,
                                                        1);
        CHECK(farExpected.X * 255 > 16);
        CHECK(math::Abs(float32(fixture.Pixels[center]) - farExpected.X * 255) <= 2);
        CHECK(math::Abs(float32(fixture.Pixels[center + 1]) - farExpected.Y * 255) <= 2);
        materials[1].Emission = originEmission;
        items = originItems;
        fixture.Cameras[0] = originCamera;
        // A fanned 64-segment disk merges into one 64-corner record. The resolve streams its edges and must match
        // the CPU check, which fans the record into clip-safe pieces, on rough dielectrics and glossy metals.
        constexpr usize DISK_SEGMENTS = 64;
        // Centered 0.55 m from the checked receiver point in world space, close enough without covering it.
        constexpr float32 DISK_RADIUS = .45F;
        constexpr float32 DISK_CENTER_X = -.25F;
        StaticArray<math::Vector3, DISK_SEGMENTS + 1> diskPositions{};
        StaticArray<math::Vector3, DISK_SEGMENTS + 1> diskNormals{};
        StaticArray<uint32, DISK_SEGMENTS * INDEX_COUNT> diskIndices{};
        diskPositions[0] = {DISK_CENTER_X, 0, .5F};
        diskNormals[0] = {0, 0, 1};
        for (usize segment = 0; segment < DISK_SEGMENTS; ++segment)
        {
            const float32 angle = static_cast<float32>(segment) * 2 * math::kPiF / static_cast<float32>(DISK_SEGMENTS);
            diskPositions[segment + 1] = {DISK_CENTER_X + DISK_RADIUS * math::Cos(angle),
                                          DISK_RADIUS * math::Sin(angle),
                                          .5F};
            diskNormals[segment + 1] = {0, 0, 1};
            diskIndices[segment * INDEX_COUNT] = 0;
            diskIndices[segment * INDEX_COUNT + 1] = static_cast<uint32>(1 + (segment + 1) % DISK_SEGMENTS);
            diskIndices[segment * INDEX_COUNT + 2] = static_cast<uint32>(1 + segment);
        }
        const renderer::RenderGraphMesh triangleEmitter = meshes[1];
        meshes[1].Geometry = {diskPositions.GetData(),
                              diskPositions.GetSize(),
                              diskIndices.GetData(),
                              diskIndices.GetSize()};
        meshes[1].Geometry.Normals = diskNormals.GetData();
        Array<math::Vector4> diskWords;
        uint32 diskRecords = 0;
        REQUIRE(renderer::internal::AppendRasterMeshLights(meshes[1].Geometry,
                                                           items[1].MeshToWorld,
                                                           materials[1],
                                                           diskWords,
                                                           diskRecords) == rhi::RasterStatus::Ready);
        REQUIRE(diskRecords == 1);
        for (const float32 metallic : {0.F, 1.F})
        {
            materials[0].Metallic = metallic;
            materials[0].Roughness = metallic == 1 ? .3F : 1;
            REQUIRE(update() == rhi::RasterStatus::Ready);
            render();
            CHECK(fixture.Graph.GetReport().Resources.MeshLightPolygons == 1);
            const math::Vector3 diskExpected =
                renderer::internal::EvaluateRasterMeshLight({diskWords.GetData(), diskWords.GetSize()},
                                                            positionWorld,
                                                            {0, 0, 1},
                                                            {0, 0, 1},
                                                            {.5F, .5F, .5F},
                                                            metallic,
                                                            materials[0].Roughness);
            CHECK(diskExpected.X * 255 > 4);
            CHECK(math::Abs(float32(fixture.Pixels[center]) - diskExpected.X * 255) <= 2);
            CHECK(math::Abs(float32(fixture.Pixels[center + 1]) - diskExpected.Y * 255) <= 2);
        }
        materials[0] = {};
        meshes[1] = triangleEmitter;
        // Emission edits replace the immutable light generation, with no invented analytic source.
        materials[1].Emission = {};
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[center] == 0);
        CHECK(fixture.Pixels[center + 1] == 0);
        // Back-side emission contributes only when explicitly authored two-sided.
        materials[1].Emission = {.7F, .2F, 0};
        meshes[1].Geometry.Indices = fixture.Indices.GetData();
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[center] == 0);
        materials[1].IsEmissionTwoSided = true;
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[center] > 0);
        // Retain every emitter contribution at the Staircase2 triangle count. Splitting
        // constant radiance across coincident triangles leaves the receiver integral unchanged.
        constexpr usize EMITTER_TRIANGLES = 336;
        Array<uint32> repeatedIndices;
        REQUIRE(repeatedIndices.TryResize(EMITTER_TRIANGLES * INDEX_COUNT) == true);
        for (usize index = 0; index < repeatedIndices.GetSize(); ++index)
        {
            repeatedIndices[index] = fixture.Indices[index % INDEX_COUNT];
        }
        const StaticArray<uint8, PIXEL_BYTES> singleEmitter = fixture.Pixels;
        meshes[1].Geometry.Indices = repeatedIndices.GetData();
        meshes[1].Geometry.IndexCount = repeatedIndices.GetSize();
        materials[1].Emission = materials[1].Emission / static_cast<float32>(EMITTER_TRIANGLES);
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Graph.GetReport().Resources.MeshLightPolygons == EMITTER_TRIANGLES);
        CHECK(math::Abs(float32(fixture.Pixels[center]) - float32(singleEmitter[center])) <= 2);
        CHECK(math::Abs(float32(fixture.Pixels[center + 1]) - float32(singleEmitter[center + 1])) <= 2);
        meshes[1].Geometry.Indices = fixture.Indices.GetData();
        meshes[1].Geometry.IndexCount = fixture.Indices.GetSize();
        materials[1].Emission = {.7F, .2F, 0};
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        const StaticArray<uint8, PIXEL_BYTES> lastDrawable = fixture.Pixels;
        const uint64 lastRevision = fixture.Graph.GetDrawableSceneRevision();
        // Too many emitter triangles rejects preparation and preserves the drawable generation.
        constexpr usize EXCESS_TRIANGLES = renderer::MAX_RASTER_MESH_LIGHT_POLYGONS + 1;
        Array<uint32> excessIndices;
        REQUIRE(excessIndices.TryResize(EXCESS_TRIANGLES * INDEX_COUNT) == true);
        for (usize index = 0; index < excessIndices.GetSize(); ++index)
        {
            excessIndices[index] = reversed[index % INDEX_COUNT];
        }
        meshes[1].Geometry.Indices = excessIndices.GetData();
        meshes[1].Geometry.IndexCount = excessIndices.GetSize();
        CHECK(update() == rhi::RasterStatus::CapacityExceeded);
        CHECK(fixture.Graph.GetDrawableSceneRevision() == lastRevision);
        render();
        CHECK(fixture.Pixels == lastDrawable);
        meshes[1].Geometry.Indices = reversed.GetData();
        meshes[1].Geometry.IndexCount = reversed.GetSize();
        renderer::DirectLight proxy;
        proxy.SourceId = 1;
        proxy.Kind = renderer::LightKind::AreaOneSided;
        inputs.Lights = {&proxy, 1};
        CHECK(update() == rhi::RasterStatus::Unsupported);
        CHECK(fixture.Graph.GetDrawableSceneRevision() == lastRevision);
        render();
        CHECK(fixture.Pixels == lastDrawable);
    }
}

TEST_CASE("default deferred lighting casts directional and mesh-light shadows",
          "[mesh-area][mesh-light-shadows][directional-shadows][gpu]")
{
    constexpr usize SCENE_COUNT = 3;
    constexpr usize POLL_ATTEMPTS = 10000;
    constexpr usize WALL_VERTEX_COUNT = 4;
    constexpr usize WALL_INDEX_COUNT = 6;
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const auto poll = [&](rhi::RasterStatus status) {
        for (usize attempt = 0; attempt < POLL_ATTEMPTS && status == rhi::RasterStatus::Pending; ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            status = fixture.Graph.Poll();
        }
        return status;
    };
    REQUIRE(poll(fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
            rhi::RasterStatus::Ready);
    rhi::TextureHandle outputTexture;
    rhi::TextureAspect outputAspect;
    REQUIRE(fixture.Graph.GetExportAllocation(0, outputTexture, outputAspect) == rhi::RasterStatus::Ready);
    renderer::Renderer compositor;
    REQUIRE(compositor.Initialize(fixture.Device, programs.ColorVertex, programs.ColorFragment) ==
            rhi::RasterStatus::Ready);
    REQUIRE(compositor.InitializeViews(shaders::renderer_composite::Vertex(),
                                       shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    renderer::ViewMapping mapping;
    REQUIRE(renderer::ResolveViewMapping({WIDTH, HEIGHT, renderer::ScreenFit::Stretch},
                                         {0, 0, WIDTH, HEIGHT},
                                         WIDTH,
                                         HEIGHT,
                                         mapping) == rhi::RasterStatus::Ready);
    renderer::Presentation presentation;
    REQUIRE(compositor.PreparePresentation(outputTexture, mapping, presentation) == rhi::RasterStatus::Ready);
    const auto render = [&]() {
        REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
        REQUIRE(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
        REQUIRE(compositor.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
        const rhi::RasterStatus status = fixture.Graph.ConfirmSubmission(completion);
        REQUIRE((status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending));
        REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
    };
    // The receiver and the downward emitter of the CPU-checked test, plus a vertical wall at x = 0.15 between the
    // receiver's centre and the whole emitter. The orthographic camera looks straight down, so it sees the wall
    // edge-on and the centre pixel still shows the receiver.
    const StaticArray<math::Vector3, VERTEX_COUNT> normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    const StaticArray<uint32, INDEX_COUNT> reversed{{0, 2, 1}};
    const StaticArray<math::Vector3, WALL_VERTEX_COUNT> wallPositions{
        {{.15F, -1, .45F}, {.15F, 1, .45F}, {.15F, 1, 1}, {.15F, -1, 1}}};
    const StaticArray<math::Vector3, WALL_VERTEX_COUNT> wallNormals{{{-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}}};
    const StaticArray<uint32, WALL_INDEX_COUNT> wallIndices{{0, 2, 1, 0, 3, 2}};
    StaticArray<renderer::RenderGraphMesh, SCENE_COUNT> meshes{
        {fixture.Meshes[0], fixture.Meshes[0], fixture.Meshes[0]}};
    const uint32 receiverMaterial = 0;
    const uint32 emitterMaterial = 1;
    meshes[0].Geometry.Normals = normals.GetData();
    meshes[1].Geometry.Normals = normals.GetData();
    meshes[1].Geometry.Indices = reversed.GetData();
    meshes[2].Geometry = {wallPositions.GetData(),
                          wallPositions.GetSize(),
                          wallIndices.GetData(),
                          wallIndices.GetSize()};
    meshes[2].Geometry.Normals = wallNormals.GetData();
    meshes[0].MaterialBindings = {&receiverMaterial, 1};
    meshes[1].MaterialBindings = {&emitterMaterial, 1};
    meshes[2].MaterialBindings = {&receiverMaterial, 1};
    StaticArray<renderer::RenderGraphItem, SCENE_COUNT> items{{fixture.Items[0], fixture.Items[0], fixture.Items[0]}};
    items[1].SourceId = 2;
    items[1].MeshIndex = 1;
    items[1].MeshToWorld.Translation = {.8F, 0, .3F};
    items[2].SourceId = 3;
    items[2].MeshIndex = 2;
    StaticArray<renderer::SceneShaderMaterial, 2> materials{};
    materials[1].BaseColor = {};
    // Bright enough that the lit receiver stands well clear of the shadowed one in eight bits.
    materials[1].Emission = {5.6F, 1.6F, 0};
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    fixture.Cameras[0].OrthographicCamera = true;
    uint64 revision = 0;
    const auto update = [&](usize itemCount) {
        renderer::RenderGraphReport report;
        ++revision;
        return poll(fixture.Graph.Update({meshes.GetData(), meshes.GetSize()},
                                         {items.GetData(), itemCount},
                                         {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                         revision,
                                         renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                         report,
                                         &inputs,
                                         revision));
    };
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    SECTION("mesh lights cast shadows from cached cube faces")
    {
        // Without the wall the receiver is lit; the shadow lookup must not darken it.
        REQUIRE(update(SCENE_COUNT - 1) == rhi::RasterStatus::Ready);
        // Published faces wait for the first frame, which draws all five within its budgets.
        CHECK(fixture.Graph.GetReport().PendingMeshLightShadowFaces == 5);
        render();
        CHECK(fixture.Graph.GetReport().PendingMeshLightShadowFaces == 0);
        const uint8 litRed = fixture.Pixels[center];
        const uint8 litGreen = fixture.Pixels[center + 1];
        CHECK(litRed > 16);
        CHECK(fixture.Graph.GetReport().Resources.MeshLightShadowCasters == 1);
        CHECK(fixture.Graph.GetReport().Resources.UnshadowedMeshLights == 0);
        CHECK(fixture.Graph.GetReport().Resources.MeshLightShadowFaces == 5);
        // A second frame changes nothing: every face was drawn in the first.
        render();
        CHECK(fixture.Pixels[center] == litRed);
        // The wall hides the whole emitter from the centre.
        REQUIRE(update(SCENE_COUNT) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[center] <= 1);
        CHECK(fixture.Pixels[center + 1] <= 1);
        // Removing it restores the lit value.
        REQUIRE(update(SCENE_COUNT - 1) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[center] == litRed);
        CHECK(fixture.Pixels[center + 1] == litGreen);
    }
    SECTION("directional visibility stays separate from other lights")
    {
        const bool hasMeshLight = GENERATE(false, true);
        if (hasMeshLight == false)
        {
            materials[1].Emission = {};
        }
        StaticArray<renderer::DirectLight, 2> lights;
        // The later source has the same incoming direction. It remains visible even where the selected Sun is blocked.
        lights[0].SourceId = 2;
        lights[0].Kind = renderer::LightKind::Directional;
        lights[0].Color = {0, 1, 0};
        lights[0].Intensity = 0.25F;
        lights[0].Direction = {-1, 0, -1};
        lights[1] = lights[0];
        lights[1].SourceId = 1;
        lights[1].Color = {0, 0, 1};
        lights[1].Intensity = 2;
        inputs.Lights = {lights.GetData(), lights.GetSize()};

        REQUIRE(update(SCENE_COUNT - 1) == rhi::RasterStatus::Ready);
        render();
        const uint8 litBlue = fixture.Pixels[center + 2];
        REQUIRE(litBlue > 32);

        // Join the wall and receiver as one draw, like a terrain ridge casting onto its own heightfield.
        Array<math::Vector3> combinedPositions;
        REQUIRE(combinedPositions.TryAddRange({fixture.Positions.GetData(), fixture.Positions.GetSize()}) == true);
        REQUIRE(combinedPositions.TryAddRange({wallPositions.GetData(), wallPositions.GetSize()}) == true);
        Array<math::Vector3> combinedNormals;
        REQUIRE(combinedNormals.TryAddRange({normals.GetData(), normals.GetSize()}) == true);
        REQUIRE(combinedNormals.TryAddRange({wallNormals.GetData(), wallNormals.GetSize()}) == true);
        Array<uint32> combinedIndices;
        REQUIRE(combinedIndices.TryAddRange({fixture.Indices.GetData(), fixture.Indices.GetSize()}) == true);
        for (const uint32 index : wallIndices)
        {
            REQUIRE(combinedIndices.TryAdd(index + static_cast<uint32>(VERTEX_COUNT)) == true);
        }
        meshes[0].Geometry = {combinedPositions.GetData(),
                              combinedPositions.GetSize(),
                              combinedIndices.GetData(),
                              combinedIndices.GetSize()};
        meshes[0].Geometry.Normals = combinedNormals.GetData();

        REQUIRE(update(SCENE_COUNT - 1) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[center + 2] <= 1);
        const uint8 remainingGreen = fixture.Pixels[center + 1];
        CHECK(remainingGreen > 4);
        CHECK(fixture.Pixels[center] <= 1);
        const StaticArray<uint8, PIXEL_BYTES> shadowed = fixture.Pixels;
        render();
        CHECK(fixture.Pixels == shadowed);

        lights[1].Direction = {};
        CHECK(update(SCENE_COUNT - 1) == rhi::RasterStatus::InvalidDescription);
        render();
        CHECK(fixture.Pixels == shadowed);

        lights[1].Direction = {1, 0, -1};
        REQUIRE(update(SCENE_COUNT - 1) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[center + 2] == litBlue);
        CHECK(fixture.Pixels[center + 1] == remainingGreen);
        CHECK(fixture.Pixels[center] <= 1);
    }
}
