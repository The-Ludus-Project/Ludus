// Default-graph pixel suite: clustered deferred against forward shading, the forward-lit graph,
// bilinear and spatial upscaling (MetalFX where available) and deferred mesh area lights.

#include "../shaders/probe_volume_layout.h"
#include "../src/internal/atmosphere_sky_tables.h"
#include "../src/internal/camera_inverse.h"
#include "../src/internal/raster_mesh_lights.h"
#include "../src/internal/render_graph_metalfx.h"
#include "../src/internal/render_graph_upscale.h"
#include "earth_sky_quadrature.hpp"
#include "hair_card_coverage_fixture.h"
#include "orbital_atmosphere_probes.hpp"
#include "render_graph_readback_fixture.h"
#include "renderer_composite.h"
#include "rg2_forward_lit.h"
#include "rg2_offscreen.h"

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/atmosphere.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/graphics/rhi/device.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/foundation/time/timers.hpp>
#include <ludus/graphics/rhi/spatial_upscale.h>

#include <cmath>

using namespace ludus::foundation;
using namespace ludus::graphics::renderer::testing;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace packages = ludus::render_graph_packages;
namespace shaders = ludus::shaders;
namespace format = ludus::content::render_graph_format;
namespace threading = ludus::foundation::threading;
namespace timing = ludus::foundation::time;
namespace
{
/// Direct3D 12 publishes a texture upload as Pending and completes it through the queue fence;
/// the other backends return Ready at once. Accept either and wait for Ready.
void RequireTextureReady(rhi::DeviceHandle device, rhi::TextureHandle texture, rhi::RasterStatus created)
{
    REQUIRE((created == rhi::RasterStatus::Ready || created == rhi::RasterStatus::Pending));
    constexpr usize POLL_ATTEMPTS = 10000;
    rhi::RasterStatus status = created;
    for (usize attempt = 0; attempt < POLL_ATTEMPTS && status == rhi::RasterStatus::Pending; ++attempt)
    {
        threading::SleepFor(timing::Milliseconds(1));
        status = rhi::GetStatus(device, texture);
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
}
/// The Sun's default apparent disk radius, for directional penumbra filtering checks.
constexpr float32 SUN_ANGULAR_RADIUS_RADIANS = renderer::AtmosphereDescription::DEFAULT_SOLAR_RADIUS_RADIANS;
/// Projection of the far oblique cameras: the World viewport's centimetre near plane and a narrow field of view.
constexpr float32 EDITOR_NEAR_PLANE_METERS = .01F;
constexpr float32 EDITOR_FAR_PLANE_METERS = 400;
constexpr float32 EDITOR_VERTICAL_FOV_RADIANS = .2F;
/// The World viewport's perspective camera at eyeWorld looking at targetWorld: its finite reverse-Z projection
/// rewritten to conventional depth (clip z becomes w - z), as RenderGraph draws graphs with ray passes. The default
/// graph keeps reverse-Z; this exercises the conventional depth allowance with a centimetre near plane.
void BuildConventionalEditorCamera(math::Vector3 eyeWorld,
                                   math::Vector3 targetWorld,
                                   renderer::ViewDescription& outCamera)
{
    renderer::ViewMapping mapping;
    REQUIRE(renderer::ResolveViewMapping({WIDTH, HEIGHT, renderer::ScreenFit::Stretch},
                                         {0, 0, WIDTH, HEIGHT},
                                         WIDTH,
                                         HEIGHT,
                                         mapping) == rhi::RasterStatus::Ready);
    renderer::ProjectionDescription projection;
    projection.Kind = renderer::ProjectionKind::Perspective;
    REQUIRE(math::TryLookAt(eyeWorld, targetWorld, {0, 0, 1}, projection.WorldToView) == math::MathStatus::Success);
    projection.VerticalFovRadians = EDITOR_VERTICAL_FOV_RADIANS;
    projection.Near = EDITOR_NEAR_PLANE_METERS;
    projection.Far = EDITOR_FAR_PLANE_METERS;
    REQUIRE(renderer::BuildViewDescription(projection, mapping, outCamera) == rhi::RasterStatus::Ready);
    for (math::Vector4& column : outCamera.WorldToClip.Columns)
    {
        column.Z = column.W - column.Z;
    }
    outCamera.Depth = renderer::DepthConvention::Conventional;
    outCamera.InfiniteFar = false;
    outCamera.OrthographicCamera = false;
    outCamera.CameraPosition = eyeWorld;
    REQUIRE(math::TryNormalize(targetWorld - eyeWorld, outCamera.CameraForward) == math::MathStatus::Success);
}
} // namespace
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
        CHECK(package->GetPlan().Passes.GetSize() == (shouldUsePrepass == true ? usize{2} : usize{1}));
        if (shouldUsePrepass == true)
        {
            CHECK(package->GetPlan().Passes[0].AllowsRepeatedCoverage == 1);
            CHECK(package->GetPlan().Passes[1].AllowsRepeatedCoverage == 1);
        }
        const auto poll = [&](rhi::RasterStatus status) {
            for (usize attempt = 0; attempt < POLL_ATTEMPTS && status == rhi::RasterStatus::Pending; ++attempt)
            {
                threading::SleepFor(timing::Milliseconds(1));
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
            REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(fixture.Pixels)) == true);
        };
        {
            // Two times the half-vector tilt gives the light tilt when the view points along the normal.
            constexpr float64 HALF_VECTOR_TILT_DEGREES = .0135;
            constexpr float64 DEGREES_PER_HALF_TURN = 180;
            const float64 halfAngleRadians = HALF_VECTOR_TILT_DEGREES * math::kPi / DEGREES_PER_HALF_TURN;
            const float64 lightAngleRadians = 2 * halfAngleRadians;
            const float64 lightCosine = math::Cos(lightAngleRadians);
            // The sharp metallic lobe needs a low illuminance to keep the linear target below saturation.
            constexpr float32 LIGHT_ILLUMINANCE_LUX = .00004F;
            const renderer::DirectLight highlightLight
            {
                .SourceId = 1,
                .Color = {1, 1, 1},
                .Intensity = LIGHT_ILLUMINANCE_LUX,
                .Direction = {-static_cast<float32>(math::Sin(lightAngleRadians)),
                              0,
                              -static_cast<float32>(lightCosine)},
                .ShouldCastRasterShadows = false,
            };
            const renderer::SceneShaderMaterial originalMaterial = materials[0];
            const math::Vector3 secondItemTranslation = items[1].MeshToWorld.Translation;
            materials[0].BaseColor = {1, 1, 1};
            materials[0].Metallic = 1;
            materials[0].Roughness = ludus::content::scene_format::MIN_ROUGHNESS;
            items[1].MeshToWorld.Translation.X = 2;
            fixture.Cameras[0].OrthographicCamera = true;
            fixture.Cameras[0].CameraForward = {0, 0, -1};
            inputs.Lights = {&highlightLight, 1};
            inputs.Environment = {};
            REQUIRE(update() == rhi::RasterStatus::Ready);
            render();

            const float64 roughness = static_cast<float64>(materials[0].Roughness);
            const float64 alpha = roughness * roughness;
            const float64 alphaSquared = alpha * alpha;
            const float64 halfSine = math::Sin(halfAngleRadians);
            const float64 halfCosine = math::Cos(halfAngleRadians);
            const float64 denominator = halfSine * halfSine + halfCosine * halfCosine * alphaSquared;
            const float64 distribution = alphaSquared / (math::kPi * denominator * denominator);
            const float64 visibility =
                0.5 / (lightCosine + math::Sqrt(lightCosine * lightCosine * (1 - alphaSquared) + alphaSquared));
            // Unit metallic Fresnel and no diffuse. Allow RGBA8 rounding and the deferred roughness storage error.
            constexpr float64 UNORM_MAXIMUM = 255;
            const float64 expectedReflectance = lightCosine * distribution * visibility;
            const float64 expectedCode =
                UNORM_MAXIMUM * static_cast<float64>(LIGHT_ILLUMINANCE_LUX) * expectedReflectance;
            if (profile == 0)
            {
                math::Vector3 reflectance;
                REQUIRE(renderer::EvaluateLitBrdf(materials[0].BaseColor,
                                                  materials[0].Metallic,
                                                  materials[0].Roughness,
                                                  {0, 0, 1},
                                                  {0, 0, 1},
                                                  -highlightLight.Direction,
                                                  reflectance) == rhi::RasterStatus::Ready);
                constexpr float64 CPU_RELATIVE_TOLERANCE = 0.0001;
                CHECK(math::Abs(static_cast<float64>(reflectance.X) / expectedReflectance - 1) <=
                      CPU_RELATIVE_TOLERANCE);
            }
            const float64 centerCode = static_cast<float64>(fixture.Pixels[GetPixel(WIDTH / 2, HEIGHT / 2)]);
            CAPTURE(profile, centerCode, expectedCode);
            CHECK(math::Abs(centerCode - expectedCode) <= 2);

            inputs.Lights = {lights.GetData(), lights.GetSize()};
            materials[0] = originalMaterial;
            items[1].MeshToWorld.Translation = secondItemTranslation;
        }
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
            // MSVC C4389: compare the unsigned report field with unsigned operands.
            CHECK(report.OverflowClusters == (areDirectional == true ? uint32{512} : uint32{0}));
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
                // In the point-light cases the second quad sits behind the first, which blocks part of its sky:
                // only the deferred resolve applies sky visibility, so there it may only be darker.
                for (usize channel = 0; channel < PIXEL_BYTES; ++channel)
                {
                    INFO("profile " << profile << " lighting case " << lightingCase << " byte " << channel);
                    const float32 difference = static_cast<float32>(clustered[channel]) -
                                               static_cast<float32>(forwardPixels[lightingCase][channel]);
                    if (areDirectional == true)
                    {
                        CHECK(math::Abs(difference) <= 2);
                    }
                    else
                    {
                        CHECK(difference <= 2);
                    }
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
        threading::SleepFor(timing::Milliseconds(1));
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
            threading::SleepFor(timing::Milliseconds(1));
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
        REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(fixture.Pixels)) == true);
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
    CHECK(decoded.ForwardVertex.Inputs.GetSize() == format::FORWARD_INPUT_COUNT);
    CHECK(decoded.ForwardFragment.Bindings.GetSize() == programs.ForwardFragment.Bindings.GetSize());
    CHECK(decoded.ForwardSurfaceChartFragment.Bindings.GetSize() == decoded.ForwardFragment.Bindings.GetSize() + 1);
    for (const rhi::RasterBinding& binding : decoded.ForwardFragment.Bindings)
    {
        CHECK(binding.Binding != renderer::BAKE_TEXTURE_BINDING);
    }

    namespace scene_format = ludus::content::scene_format;
    scene_format::BakeChartLayout layout;
    REQUIRE(scene_format::TryMakeBakeChartLayout(1, layout) == true);
    constexpr usize CHART_BYTES =
        scene_format::BAKE_CHART_TILE_PIXELS * scene_format::BAKE_CHART_TILE_PIXELS * scene_format::TEXEL_BYTES;
    StaticArray<uint8, CHART_BYTES> chartPixels{};
    for (usize offset = 0; offset < chartPixels.GetSize(); offset += scene_format::TEXEL_BYTES)
    {
        chartPixels[offset] = 255;
    }
    renderer::SceneSurfaceChart chart;
    chart.SourceId = fixture.Items[0].SourceId;
    chart.MeshToWorld = fixture.Items[0].MeshToWorld;
    chart.Scale = {3, 0, 0};
    chart.Irradiance = {chartPixels.GetData(),
                        chartPixels.GetSize(),
                        layout.Width * scene_format::TEXEL_BYTES,
                        layout.Width,
                        layout.Height,
                        rhi::RasterFormat::Rgba8Unorm};
    inputs.SurfaceCharts = {&chart, 1};
    inputs.BakedLightIds = {&light.SourceId, 1};
    REQUIRE(update(5) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetReport().Resources.SurfaceChartDraws == 1);
    render();
    CHECK(fixture.Pixels[center] > 64);
    CHECK(fixture.Pixels[center] > fixture.Pixels[center + 2]);
    const uint8 bakedBlue = fixture.Pixels[center + 2];

    // A mismatched placement must switch the draw to the realtime program, even with a chart bound.
    chart.MeshToWorld.Translation.X += 1;
    REQUIRE(update(6) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetReport().Resources.SurfaceChartDraws == 0);
    render();
    CHECK(fixture.Pixels[center] < 10);
    CHECK(fixture.Pixels[center + 2] > bakedBlue);
}

TEST_CASE("default clustered deferred adds the baked probe volume to GI Probe Receivers only",
          "[render-graph][clustered-deferred][probe-gi][gpu]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(programs.HasClusteredDeferred == true);
    // The engine default connects Baked Lighting, so its package carries the probe G-buffer.
    const rhi::ShaderDescription& probeArtifact = programs.DeferredProbeGBufferFragment.Artifact;
    REQUIRE((probeArtifact.Msl.empty() == false || probeArtifact.Spirv.IsEmpty() == false ||
             probeArtifact.Wgsl.empty() == false || probeArtifact.Dxil.IsEmpty() == false));
    constexpr usize POLL_ATTEMPTS = 10000;
    const auto poll = [&](rhi::RasterStatus status) {
        for (usize attempt = 0; attempt < POLL_ATTEMPTS && status == rhi::RasterStatus::Pending; ++attempt)
        {
            threading::SleepFor(timing::Milliseconds(1));
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
    fixture.Items[0].Material.Color = {1, 1, 1, 1};
    fixture.Cameras[0].CameraPosition = {0, 0, 2};
    renderer::SceneShaderMaterial material;
    material.BaseColor = {1, 1, 1};
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&material, 1};

    // Eight valid probes with red irradiance on every face and unoccluded depth faces enclose the receiver.
    renderer::GiProbeGrid grid;
    grid.OriginWorld = {-10, -10, -10};
    grid.Spacing = 20;
    grid.Counts = {2, 2, 2};
    constexpr uint32 PROBE_COUNT = 8;
    constexpr uint32 ATLAS_WIDTH = LUDUS_GI_PROBE_ATLAS_WIDTH;
    constexpr uint32 PROBE_TEXELS = LUDUS_GI_PROBE_TEXELS;
    constexpr uint32 ATLAS_HEIGHT = (PROBE_COUNT * PROBE_TEXELS + ATLAS_WIDTH - 1) / ATLAS_WIDTH;
    REQUIRE(renderer::HasGiProbeAtlasExtent(grid, ATLAS_WIDTH, ATLAS_HEIGHT) == true);
    Array<uint8> atlasPixels;
    REQUIRE(atlasPixels.TryResize(usize{ATLAS_WIDTH} * ATLAS_HEIGHT * TEXEL_BYTES, 0) == true);
    constexpr uint32 AMBIENT_CUBE_TEXELS = LUDUS_GI_PROBE_FACES;
    for (uint32 probe = 0; probe < PROBE_COUNT; ++probe)
    {
        for (uint32 texel = 0; texel < PROBE_TEXELS; ++texel)
        {
            const usize offset = (usize{probe} * PROBE_TEXELS + texel) * TEXEL_BYTES;
            constexpr uint8 FULL_UNORM = 255;
            atlasPixels[offset] = FULL_UNORM;
            atlasPixels[offset + 1] = texel < AMBIENT_CUBE_TEXELS ? 0 : FULL_UNORM;
            atlasPixels[offset + 2] = texel < AMBIENT_CUBE_TEXELS ? 0 : FULL_UNORM;
            atlasPixels[offset + TEXEL_BYTES - 1] = FULL_UNORM;
        }
    }
    inputs.ProbeVolume.Grid = grid;
    inputs.ProbeVolume.IrradianceScale = 2;
    inputs.ProbeVolume.Atlas = {atlasPixels.GetData(),
                                atlasPixels.GetSize(),
                                usize{ATLAS_WIDTH} * TEXEL_BYTES,
                                ATLAS_WIDTH,
                                ATLAS_HEIGHT,
                                rhi::RasterFormat::Rgba8Unorm};
    uint64 receiverId = fixture.Items[0].SourceId;
    inputs.ProbeReceiverIds = {&receiverId, 1};
    renderer::RenderGraphReport report;
    const auto update = [&](uint64 revision) {
        return poll(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                         {fixture.Items.GetData(), fixture.Items.GetSize()},
                                         {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                         revision,
                                         renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                         report,
                                         &inputs,
                                         revision));
    };
    REQUIRE(update(1) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetReport().Resources.ProbeReceiverDraws == 1);
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
        REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(fixture.Pixels)) == true);
    };
    render();
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    // Without lights, environment or emission, only the probe term lights the white receiver.
    CHECK(fixture.Pixels[center] > 100);
    CHECK(fixture.Pixels[center + 1] < 10);
    CHECK(fixture.Pixels[center + 2] < 10);

    // A draw that is not a receiver keeps the realtime G-buffer and stays dark.
    receiverId = fixture.Items[0].SourceId + 1;
    REQUIRE(update(2) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetReport().Resources.ProbeReceiverDraws == 0);
    render();
    CHECK(fixture.Pixels[center] < 10);

    // A receiver list with an atlas that does not match its grid is rejected.
    receiverId = fixture.Items[0].SourceId;
    inputs.ProbeVolume.Atlas.Height = ATLAS_HEIGHT + 1;
    CHECK(update(3) == rhi::RasterStatus::InvalidDescription);
    CHECK(fixture.Graph.GetReport().Resources.ProbeReceiverDraws == 0);
}

TEST_CASE("default clustered deferred draws baked surface charts and omits the baked lights' live diffuse once",
          "[render-graph][clustered-deferred][surface-chart][gpu]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(programs.HasClusteredDeferred == true);
    // The engine default connects Baked Lighting, so its package carries the chart G-buffer beside the probe one.
    const auto hasArtifact = [](const rhi::ShaderDescription& artifact) {
        return artifact.Msl.empty() == false || artifact.Spirv.IsEmpty() == false || artifact.Wgsl.empty() == false ||
               artifact.Dxil.IsEmpty() == false;
    };
    REQUIRE(hasArtifact(programs.DeferredSurfaceChartGBufferFragment.Artifact) == true);
    // The forward chart fragment belongs to the forward-lit profile only.
    CHECK(hasArtifact(programs.ForwardSurfaceChartFragment.Artifact) == false);
    constexpr usize POLL_ATTEMPTS = 10000;
    const auto poll = [&](rhi::RasterStatus status) {
        for (usize attempt = 0; attempt < POLL_ATTEMPTS && status == rhi::RasterStatus::Pending; ++attempt)
        {
            threading::SleepFor(timing::Milliseconds(1));
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
    fixture.Cameras[0].CameraPosition = {0, 0, 2};
    // The charted draw covers the centre; an uncharted copy beside it sees the same light in realtime. The copy is
    // drawn first, so the resolve must still find the chart material's baked-light flags.
    constexpr usize ITEM_COUNT = 2;
    constexpr float32 REALTIME_OFFSET_X = .6F;
    StaticArray<renderer::RenderGraphItem, ITEM_COUNT> items{{fixture.Items[0], fixture.Items[0]}};
    items[0].SourceId = fixture.Items[0].SourceId + 1;
    items[0].MeshToWorld.Translation.X = REALTIME_OFFSET_X;
    items[1].SourceId = fixture.Items[0].SourceId;
    for (renderer::RenderGraphItem& item : items)
    {
        item.Material.Color = {1, 1, 1, 1};
    }
    constexpr float32 ROUGHNESS = .5F;
    renderer::SceneShaderMaterial material;
    material.BaseColor = {1, 1, 1};
    material.Roughness = ROUGHNESS;
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&material, 1};
    // A blue directional light along the view axis: at the centre its half vector is the normal.
    renderer::DirectLight light;
    light.SourceId = 1;
    light.Color = {0, 0, 1};
    light.Direction = {0, 0, -1};
    light.Intensity = 1;
    inputs.Lights = {&light, 1};

    namespace scene_format = ludus::content::scene_format;
    scene_format::BakeChartLayout layout;
    REQUIRE(scene_format::TryMakeBakeChartLayout(1, layout) == true);
    constexpr usize CHART_BYTES =
        scene_format::BAKE_CHART_TILE_PIXELS * scene_format::BAKE_CHART_TILE_PIXELS * scene_format::TEXEL_BYTES;
    StaticArray<uint8, CHART_BYTES> chartPixels{};
    for (usize offset = 0; offset < chartPixels.GetSize(); offset += scene_format::TEXEL_BYTES)
    {
        chartPixels[offset] = 255;
    }
    // Red chart irradiance, distinct from the light's colour, so the two terms read in separate channels.
    constexpr float32 CHART_IRRADIANCE = 3;
    StaticArray<renderer::SceneSurfaceChart, ITEM_COUNT> charts{};
    charts[1].SourceId = items[1].SourceId;
    charts[1].MeshToWorld = items[1].MeshToWorld;
    charts[1].Scale = {CHART_IRRADIANCE, 0, 0};
    charts[1].Irradiance = {chartPixels.GetData(),
                            chartPixels.GetSize(),
                            layout.Width * scene_format::TEXEL_BYTES,
                            layout.Width,
                            layout.Height,
                            rhi::RasterFormat::Rgba8Unorm};
    inputs.SurfaceCharts = {charts.GetData(), charts.GetSize()};
    inputs.BakedLightIds = {&light.SourceId, 1};
    renderer::RenderGraphReport report;
    const auto update = [&](uint64 revision) {
        return poll(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                         {items.GetData(), items.GetSize()},
                                         {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                         revision,
                                         renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                         report,
                                         &inputs,
                                         revision));
    };
    REQUIRE(update(1) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetReport().Resources.SurfaceChartDraws == 1);
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
        REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(fixture.Pixels)) == true);
    };
    render();
    // The staged default writes clamped linear RGBA8. At normal incidence with a white dielectric, ShadeMaterial's
    // diffuse weight is (1 - F0) / pi and the GGX lobe at its peak is F0 / (4 pi alpha^2), alpha = roughness^2.
    constexpr float32 DIELECTRIC_F0 = .04F;
    constexpr float32 CODE_SCALE = 255;
    const float32 diffuseCodes = CODE_SCALE * (1 - DIELECTRIC_F0) / math::kPiF;
    const float32 alphaSquared = ROUGHNESS * ROUGHNESS * ROUGHNESS * ROUGHNESS;
    const float32 specularCodes = CODE_SCALE * DIELECTRIC_F0 / (4 * math::kPiF * alphaSquared);
    // Half-float G-buffer storage and the RGBA8 target may move a channel by a few codes.
    constexpr float32 CODE_TOLERANCE = 3;
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    CAPTURE(fixture.Pixels[center], fixture.Pixels[center + 2], diffuseCodes, specularCodes);
    // The chart supplies the diffuse once; the baked light keeps only its live specular there.
    CHECK(math::Abs(static_cast<float32>(fixture.Pixels[center]) - CHART_IRRADIANCE * diffuseCodes) <= CODE_TOLERANCE);
    CHECK(math::Abs(static_cast<float32>(fixture.Pixels[center + 2]) - specularCodes) <= CODE_TOLERANCE);
    // The uncharted draw shades the same light's live diffuse and has no chart term.
    constexpr uint32 REALTIME_PIXEL_X = static_cast<uint32>(WIDTH * (1 + REALTIME_OFFSET_X) / 2);
    const usize realtime = GetPixel(REALTIME_PIXEL_X, HEIGHT / 2);
    CAPTURE(fixture.Pixels[realtime], fixture.Pixels[realtime + 2]);
    CHECK(fixture.Pixels[realtime] < CODE_TOLERANCE);
    CHECK(static_cast<float32>(fixture.Pixels[realtime + 2]) > diffuseCodes / 2);

    // A mismatched placement returns the draw to the realtime G-buffer: no chart term and the live diffuse back.
    charts[1].MeshToWorld.Translation.Y += 1;
    REQUIRE(update(2) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetReport().Resources.SurfaceChartDraws == 0);
    render();
    CHECK(fixture.Pixels[center] < CODE_TOLERANCE);
    CHECK(math::Abs(static_cast<float32>(fixture.Pixels[center + 2]) - (diffuseCodes + specularCodes)) <=
          CODE_TOLERANCE);

    // A chart table that does not match the draw list is rejected and keeps the last drawable scene.
    charts[1].MeshToWorld = items[1].MeshToWorld;
    inputs.SurfaceCharts = {charts.GetData(), 1};
    CHECK(update(3) == rhi::RasterStatus::InvalidDescription);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 2);
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
    const rhi::RasterStatus sourceStatus = rhi::CreateTexture(fixture.Device,
                                                              sourceDescription,
                                                              {{pixels.GetData(), pixels.GetSize()}, 2 * CHANNEL_COUNT},
                                                              source);
    RequireTextureReady(fixture.Device, source, sourceStatus);
    renderer::RenderGraphProgramCache cache;
    REQUIRE(cache.Initialize(fixture.Device) == rhi::RasterStatus::Ready);
    renderer::internal::RenderGraphUpscaleProgram program;
    REQUIRE(program.Initialize(cache,
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
    REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(fixture.Pixels)) == true);
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

TEST_CASE("bilinear graphs sharing a program cache draw the private pass pixels with one upscale pipeline",
          "[render-graph][gpu][bilinear][managed-pipeline]")
{
    GraphGpuFixture fixture;
    constexpr usize UPSCALE_RESOURCE_COUNT = 3;
    constexpr usize UPSCALE_PASS_COUNT = 2;
    const StaticArray<format::Resource, UPSCALE_RESOURCE_COUNT> resources{
        {{WIDTH / 2, HEIGHT / 2, format::Aspect::Color, 0, 0, 0},
         {WIDTH / 2, HEIGHT / 2, format::Aspect::Depth, 0, 0, 0},
         {WIDTH, HEIGHT, format::Aspect::Color, 2, 1, 0}}};
    StaticArray<format::Pass, UPSCALE_PASS_COUNT> passes{{packages::rg2_offscreen::PASSES[0],
                                                          {format::Technique::BilinearUpscale,
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
    const renderer::RenderGraphPrograms programs =
        packages::rg2_offscreen::MakePrograms(shaders::renderer_composite::Vertex(),
                                              shaders::renderer_composite::Fragment());
    const auto draw = [&](renderer::RenderGraph& inoutGraph, StaticArray<uint8, PIXEL_BYTES>& outPixels) {
        renderer::RenderGraphReport report;
        REQUIRE(inoutGraph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                  {fixture.Items.GetData(), fixture.Items.GetSize()},
                                  {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                  1,
                                  renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                  report) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(inoutGraph.Render(fixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(outPixels)) == true);
    };
    const auto getReport = [](const renderer::RenderGraph& graph) {
        renderer::RenderGraphPipelineReport report;
        REQUIRE(graph.GetPipelineReport(report) == rhi::RasterStatus::Ready);
        return report;
    };
    // The private-cache graph is the unshared pass: its own shaders, layout and pipeline.
    StaticArray<uint8, PIXEL_BYTES> unshared{};
    REQUIRE(fixture.Graph.Initialize(fixture.Device, plan, programs) == rhi::RasterStatus::Ready);
    draw(fixture.Graph, unshared);
    CHECK(getReport(fixture.Graph).Upscale.SharedRequests == 0);
    fixture.Graph.Reset();

    renderer::RenderGraphProgramCache cache;
    REQUIRE(cache.Initialize(fixture.Device) == rhi::RasterStatus::Ready);
    renderer::RenderGraph active;
    REQUIRE(active.Initialize(fixture.Device, plan, programs, nullptr, 0, 0, nullptr, &cache) ==
            rhi::RasterStatus::Ready);
    StaticArray<uint8, PIXEL_BYTES> activePixels{};
    draw(active, activePixels);
    // A candidate generation prepared beside the active graph joins its upscale pipeline.
    renderer::RenderGraph candidate;
    REQUIRE(candidate.Initialize(fixture.Device, plan, programs, nullptr, 0, 0, nullptr, &cache) ==
            rhi::RasterStatus::Ready);
    StaticArray<uint8, PIXEL_BYTES> candidatePixels{};
    draw(candidate, candidatePixels);
    const renderer::RenderGraphPipelineReport activeReport = getReport(active);
    const renderer::RenderGraphPipelineReport candidateReport = getReport(candidate);
    CHECK(activeReport.Upscale.Requests == 1);
    CHECK(activeReport.Upscale.SharedRequests == 0);
    CHECK(activeReport.Upscale.ReadyRequests == 1);
    CHECK(candidateReport.Upscale.Requests == 1);
    CHECK(candidateReport.Upscale.SharedRequests == 1);
    CHECK(candidateReport.Upscale.ReadyRequests == 1);
    CHECK(activeReport.Upscale.PrewarmSeconds == candidateReport.Upscale.PrewarmSeconds);
    CHECK(cache.GetReport().CreatedEntries == 1);
    CHECK(cache.GetReport().SharedLeases == 1);
    // Sharing changes no pixel: the same artifacts, layout and fixed state draw the same image.
    bool isActiveEqual = true;
    bool isCandidateEqual = true;
    for (usize index = 0; index < PIXEL_BYTES; ++index)
    {
        isActiveEqual = isActiveEqual == true && activePixels[index] == unshared[index];
        isCandidateEqual = isCandidateEqual == true && candidatePixels[index] == unshared[index];
    }
    CHECK(isActiveEqual == true);
    CHECK(isCandidateEqual == true);
    candidate.Reset();
    active.Reset();
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
    const rhi::RasterStatus sourceStatus =
        rhi::CreateTexture(fixture.Device,
                           sourceDescription,
                           {{pixels.GetData(), pixels.GetSize()}, SOURCE_WIDTH * TEXEL_BYTES},
                           source);
    RequireTextureReady(fixture.Device, source, sourceStatus);
    renderer::RenderGraphProgramCache cache;
    REQUIRE(cache.Initialize(fixture.Device) == rhi::RasterStatus::Ready);
    renderer::internal::RenderGraphUpscaleProgram program;
    REQUIRE(program.Initialize(cache,
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
    REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(fixture.Pixels)) == true);
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
                threading::SleepFor(timing::Milliseconds(1));
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
            REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(fixture.Pixels)) == true);
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
            threading::SleepFor(timing::Milliseconds(1));
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
        REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(fixture.Pixels)) == true);
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
    SECTION("a textured hair card blocks a directional shadow only where its mask passes")
    {
        constexpr usize MATERIAL_COUNT = 3;
        constexpr uint32 CASTER_MATERIAL = MATERIAL_COUNT - 1;
        StaticArray<renderer::SceneShaderMaterial, MATERIAL_COUNT> shadowMaterials{};
        HairCardCoverageFixture coverage;
        coverage.SetMask(shadowMaterials[CASTER_MATERIAL]);
        meshes[2].MaterialBindings = {&CASTER_MATERIAL, 1};
        inputs.Materials = {shadowMaterials.GetData(), shadowMaterials.GetSize()};
        renderer::DirectLight light;
        light.SourceId = 1;
        light.Kind = renderer::LightKind::Directional;
        light.Direction = {-1, 0, -1};
        light.Color = {1, 1, 1};
        light.Intensity = 2;
        inputs.Lights = {&light, 1};
        REQUIRE(update(1) == rhi::RasterStatus::Ready);
        render();
        const uint8 lit = fixture.Pixels[center];
        REQUIRE(lit > 32);
        const auto measureShadow = [&](uint8 alpha, float32 cutoff, bool hasExpectedCoverage) {
            coverage.Texture[HairCardCoverageFixture::TEXEL_BYTES - 1] = alpha;
            shadowMaterials[CASTER_MATERIAL].AlphaCutoff = cutoff;
            REQUIRE(update(SCENE_COUNT) == rhi::RasterStatus::Ready);
            render();
            CAPTURE(alpha, cutoff);
            CHECK(fixture.Graph.GetReport().Resources.DirectionalShadowLights == 1);
            const uint8 red = fixture.Pixels[center];
            CHECK(red == (hasExpectedCoverage == true ? 0 : lit));
            return red != lit;
        };
        CHECK(measureShadow(0, HAIR_MASK_CUTOFF, false) == false);
        CHECK(measureShadow(HAIR_MASK_SOLID_ALPHA, HAIR_MASK_CUTOFF, true) == true);
        const bool hasMatchedShadow = measureShadow(HAIR_MASK_MID_ALPHA, HAIR_MASK_CUTOFF, true);
        const bool hasMismatchedShadow = measureShadow(HAIR_MASK_MID_ALPHA, HAIR_MASK_MISMATCHED_CUTOFF, false);
        CHECK(hasMatchedShadow != hasMismatchedShadow);
    }
    SECTION("every directional light up to the capacity keeps its own map")
    {
        const bool hasMeshLight = GENERATE(false, true);
        if (hasMeshLight == false)
        {
            materials[1].Emission = {};
        }
        // An Atmosphere's Sun and Moon, plus a third directional light past the two-map capacity. They share one
        // incoming direction. The dim Moon has the lowest source ID: when only that light received a map, the Sun
        // lit the receiver straight through every caster.
        constexpr usize SUN = 0;
        constexpr usize MOON = 1;
        constexpr usize EXTRA = 2;
        constexpr usize DIRECTIONAL_COUNT = 3;
        StaticArray<renderer::DirectLight, DIRECTIONAL_COUNT> lights;
        lights[SUN].SourceId = 9;
        lights[SUN].Kind = renderer::LightKind::Directional;
        lights[SUN].Color = {0, 0, 1};
        lights[SUN].Intensity = 2;
        lights[SUN].Direction = {-1, 0, -1};
        lights[MOON] = lights[SUN];
        lights[MOON].SourceId = 1;
        lights[MOON].Color = {0, 1, 0};
        lights[MOON].Intensity = 0.25F;
        lights[EXTRA] = lights[SUN];
        lights[EXTRA].SourceId = 5;
        lights[EXTRA].Color = {1, 0, 0};
        lights[EXTRA].Intensity = 0.2F;
        inputs.Lights = {lights.GetData(), lights.GetSize()};

        REQUIRE(update(SCENE_COUNT - 1) == rhi::RasterStatus::Ready);
        render();
        const uint8 litBlue = fixture.Pixels[center + 2];
        REQUIRE(litBlue > 32);
        CHECK(fixture.Graph.GetReport().Resources.DirectionalShadowLightSourceId == lights[SUN].SourceId);
        CHECK(fixture.Graph.GetReport().Resources.DirectionalShadowLights == 2);
        CHECK(fixture.Graph.GetReport().Resources.UnshadowedDirectionalLights == 1);

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

        // The wall blocks the Sun and the Moon. Only the light past the capacity still reaches the receiver.
        REQUIRE(update(SCENE_COUNT - 1) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[center + 2] <= 1);
        CHECK(fixture.Pixels[center + 1] <= 1);
        const uint8 unshadowedRed = fixture.Pixels[center];
        CHECK(unshadowedRed > 4);
        const StaticArray<uint8, PIXEL_BYTES> shadowed = fixture.Pixels;
        render();
        CHECK(fixture.Pixels == shadowed);

        lights[SUN].Direction = {};
        CHECK(update(SCENE_COUNT - 1) == rhi::RasterStatus::InvalidDescription);
        render();
        CHECK(fixture.Pixels == shadowed);

        // Excluding the Sun frees its map for the remaining light, and the Sun lights the receiver unshadowed.
        lights[SUN].Direction = {-1, 0, -1};
        lights[SUN].ShouldCastRasterShadows = false;
        REQUIRE(update(SCENE_COUNT - 1) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Graph.GetReport().Resources.DirectionalShadowLightSourceId == lights[MOON].SourceId);
        CHECK(fixture.Graph.GetReport().Resources.DirectionalShadowLights == 2);
        CHECK(fixture.Graph.GetReport().Resources.UnshadowedDirectionalLights == 0);
        CHECK(fixture.Pixels[center + 2] == litBlue);
        CHECK(fixture.Pixels[center + 1] <= 1);
        CHECK(fixture.Pixels[center] <= 1);

        for (renderer::DirectLight& light : lights)
        {
            light.ShouldCastRasterShadows = false;
        }
        REQUIRE(update(SCENE_COUNT - 1) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[center + 2] == litBlue);
        CHECK(fixture.Pixels[center + 1] > 4);
        CHECK(fixture.Pixels[center] == unshadowedRed);
        CHECK(fixture.Graph.GetReport().Resources.DirectionalShadowLightSourceId == 0);
        CHECK(fixture.Graph.GetReport().Resources.DirectionalShadowLights == 0);
    }
    SECTION("a close directional caster shadows a small scene")
    {
        // The orthographic camera sees the larger triangle at the nearer conventional depth. The directional
        // light sees the smaller triangle first. A metre of padding made the normalized comparison bias
        // exceed their separation, even though the caster covers the center of the receiver.
        constexpr float32 RECEIVER_SCALE = 0.16F;
        constexpr float32 CASTER_SCALE = 0.25F;
        constexpr float32 CASTER_HEIGHT_METRES = 0.0004F;
        StaticArray<math::Vector3, VERTEX_COUNT> receiverPositions;
        StaticArray<math::Vector3, VERTEX_COUNT> casterPositions;
        for (usize vertex = 0; vertex < VERTEX_COUNT; ++vertex)
        {
            const math::Vector3 position = fixture.Positions[vertex];
            receiverPositions[vertex] = {position.X * RECEIVER_SCALE, position.Y * RECEIVER_SCALE, position.Z};
            casterPositions[vertex] = {receiverPositions[vertex].X * CASTER_SCALE,
                                       receiverPositions[vertex].Y * CASTER_SCALE,
                                       position.Z + CASTER_HEIGHT_METRES};
        }
        meshes[0].Geometry.Positions = receiverPositions.GetData();
        meshes[1].Geometry = meshes[0].Geometry;
        meshes[1].Geometry.Positions = casterPositions.GetData();
        meshes[1].MaterialBindings = {&receiverMaterial, 1};
        items[1].MeshToWorld = items[0].MeshToWorld;
        fixture.Cameras[0].WorldToClip.Columns[0].X = 1 / RECEIVER_SCALE;
        fixture.Cameras[0].WorldToClip.Columns[1].Y = 1 / RECEIVER_SCALE;
        fixture.Cameras[0].LightingView = renderer::DeferredLightingView::ShadowCoverage;
        renderer::DirectLight light;
        light.SourceId = 1;
        light.Kind = renderer::LightKind::Directional;
        light.Direction = {0, 0, -1};
        light.Color = {1, 1, 1};
        light.Intensity = 1;
        inputs.Lights = {&light, 1};
        fixture.Cameras[0].LightingViewSourceId = light.SourceId;

        REQUIRE(update(SCENE_COUNT - 1) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Graph.GetReport().Resources.DirectionalShadowLights == 1);
        CHECK(static_cast<uint32>(fixture.Pixels[center]) <= 1);
        CHECK(static_cast<uint32>(fixture.Pixels[GetPixel(WIDTH / 2, HEIGHT / 2 + HEIGHT / 8)]) >= 254);
    }
    SECTION("a directional disk widens the shadow edge and keeps the lit level")
    {
        // A receiver quad and the wall at x = 0.15, whose top 0.5 m above the receiver ends the 45 degree light's
        // shadow at x = -0.35. A disk of angular radius r leaves the receivers whose wall-top elevation lies within r
        // of 45 degrees in penumbra: x in [-0.370, -0.330] for r = 0.02, about 19 pixels of the zoomed view.
        materials[1].Emission = {};
        constexpr float32 RECEIVER_HALF_EXTENT_METERS = .6F;
        constexpr float32 RECEIVER_HEIGHT_METERS = .5F;
        constexpr usize QUAD_VERTEX_COUNT = 4;
        constexpr usize QUAD_INDEX_COUNT = 6;
        const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> quadPositions{
            {{-RECEIVER_HALF_EXTENT_METERS, -RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
             {RECEIVER_HALF_EXTENT_METERS, -RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
             {RECEIVER_HALF_EXTENT_METERS, RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
             {-RECEIVER_HALF_EXTENT_METERS, RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS}}};
        const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> quadNormals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
        const StaticArray<uint32, QUAD_INDEX_COUNT> quadIndices{{0, 1, 2, 0, 2, 3}};
        meshes[0].Geometry = {quadPositions.GetData(),
                              quadPositions.GetSize(),
                              quadIndices.GetData(),
                              quadIndices.GetSize()};
        meshes[0].Geometry.Normals = quadNormals.GetData();
        items[1] = items[2];
        // The view spans x in [-0.45, -0.25] around the edge.
        constexpr float32 ZOOM = 10;
        constexpr float32 EDGE_METERS = -.35F;
        fixture.Cameras[0].WorldToClip.Columns[0].X = ZOOM;
        fixture.Cameras[0].WorldToClip.Columns[1].Y = ZOOM;
        fixture.Cameras[0].WorldToClip.Columns[3].X = -ZOOM * EDGE_METERS;
        // Only the blue light casts. Its otherwise identical green control shows the unoccluded contribution.
        StaticArray<renderer::DirectLight, 2> lights;
        lights[0].SourceId = 1;
        lights[0].Kind = renderer::LightKind::Directional;
        lights[0].Color = {0, 0, 1};
        lights[0].Intensity = 2;
        lights[0].Direction = {-1, 0, -1};
        lights[1] = lights[0];
        lights[1].SourceId = 2;
        lights[1].Color = {0, 1, 0};
        lights[1].ShouldCastRasterShadows = false;
        inputs.Lights = {lights.GetData(), lights.GetSize()};
        const auto countPenumbraColumns = [&]() {
            constexpr float32 PENUMBRA_LOWER_FRACTION = .2F;
            constexpr float32 PENUMBRA_UPPER_FRACTION = .8F;
            uint32 columns = 0;
            for (uint32 x = 0; x < WIDTH; ++x)
            {
                const usize pixel = GetPixel(x, HEIGHT / 2);
                const float32 control = fixture.Pixels[pixel + 1];
                const float32 shadowed = fixture.Pixels[pixel + 2];
                columns += shadowed > PENUMBRA_LOWER_FRACTION * control && shadowed < PENUMBRA_UPPER_FRACTION * control
                               ? 1U
                               : 0U;
            }
            return columns;
        };
        const usize lit = GetPixel(0, HEIGHT / 2);
        const usize shadowed = GetPixel(WIDTH - 1, HEIGHT / 2);
        constexpr float32 CHANNEL_TOLERANCE = 2;
        constexpr uint8 SATURATED_CHANNEL = 250;

        REQUIRE(update(SCENE_COUNT - 1) == rhi::RasterStatus::Ready);
        render();
        REQUIRE(fixture.Pixels[lit + 1] > 32);
        REQUIRE(fixture.Pixels[lit + 1] < SATURATED_CHANNEL);
        CHECK(math::Abs(float32(fixture.Pixels[lit + 2]) - float32(fixture.Pixels[lit + 1])) <= CHANNEL_TOLERANCE);
        CHECK(fixture.Pixels[shadowed + 2] <= 1);
        // A delta light keeps the hard edge of the bilinear comparison.
        constexpr uint32 HARD_EDGE_COLUMNS = 2;
        CHECK(countPenumbraColumns() <= HARD_EDGE_COLUMNS);

        constexpr float32 DISK_RADIUS_RADIANS = .02F;
        lights[0].AngularRadiusRadians = DISK_RADIUS_RADIANS;
        lights[1].AngularRadiusRadians = DISK_RADIUS_RADIANS;
        REQUIRE(update(SCENE_COUNT - 1) == rhi::RasterStatus::Ready);
        render();
        const uint32 penumbraColumns = countPenumbraColumns();
        CAPTURE(penumbraColumns);
        constexpr uint32 MINIMUM_PENUMBRA_COLUMNS = 6;
        CHECK(penumbraColumns >= MINIMUM_PENUMBRA_COLUMNS);
        // The disk keeps the light's illuminance where it is wholly visible and its umbra where it is wholly hidden.
        CHECK(math::Abs(float32(fixture.Pixels[lit + 2]) - float32(fixture.Pixels[lit + 1])) <= CHANNEL_TOLERANCE);
        CHECK(fixture.Pixels[shadowed + 2] <= 1);
        // Visibility falls monotonically across the edge.
        for (uint32 x = 1; x < WIDTH; ++x)
        {
            CAPTURE(x);
            CHECK(float32(fixture.Pixels[GetPixel(x, HEIGHT / 2) + 2]) <=
                  float32(fixture.Pixels[GetPixel(x - 1, HEIGHT / 2) + 2]) + CHANNEL_TOLERANCE);
        }
    }
    SECTION("grazing directional light illuminates its own receiver uniformly")
    {
        materials[1].Emission = {};
        StaticArray<renderer::DirectLight, 2> lights;
        lights[0].SourceId = 1;
        lights[0].Kind = renderer::LightKind::Directional;
        lights[0].Color = {1, 0, 0};
        lights[0].Intensity = 8;
        lights[0].Direction = {-1, -.6F, -.13F};
        // Only the first light casts. Its otherwise identical green control
        // shows the unoccluded contribution at each pixel.
        lights[1] = lights[0];
        lights[1].SourceId = 2;
        lights[1].Color = {0, 1, 0};
        lights[1].ShouldCastRasterShadows = false;
        inputs.Lights = {lights.GetData(), lights.GetSize()};

        REQUIRE(update(1) == rhi::RasterStatus::Ready);
        render();
        constexpr uint32 PROBE_RADIUS_PIXELS = 6;
        constexpr float32 MAX_CHANNEL_DIFFERENCE = 2;
        float32 maximumDifference = 0;
        for (uint32 y = HEIGHT / 2 - PROBE_RADIUS_PIXELS; y < HEIGHT / 2 + PROBE_RADIUS_PIXELS; ++y)
        {
            for (uint32 x = WIDTH / 2 - PROBE_RADIUS_PIXELS; x < WIDTH / 2 + PROBE_RADIUS_PIXELS; ++x)
            {
                const usize pixel = GetPixel(x, y);
                REQUIRE(fixture.Pixels[pixel + 1] > 32);
                const float32 difference =
                    math::Abs(float32(fixture.Pixels[pixel]) - float32(fixture.Pixels[pixel + 1]));
                maximumDifference = math::Max(maximumDifference, difference);
            }
        }
        CHECK(maximumDifference <= MAX_CHANNEL_DIFFERENCE);
    }
    SECTION("far perspective camera keeps steep receivers free of self-shadowing")
    {
        // A conventional camera with a centimetre near plane, as RenderGraph draws ray graphs. Tens of metres above a
        // scene, several texels of the whole-scene map fall in each pixel and the receiver position carries
        // the depth buffer's rounding. A flat receiver under one steep light must stay uniformly lit.
        materials[1].Emission = {};
        constexpr float32 RECEIVER_HALF_EXTENT_METERS = 20;
        constexpr float32 RECEIVER_HEIGHT_METERS = .5F;
        constexpr usize QUAD_VERTEX_COUNT = 4;
        constexpr usize QUAD_INDEX_COUNT = 6;
        const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> quadPositions{
            {{-RECEIVER_HALF_EXTENT_METERS, -RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
             {RECEIVER_HALF_EXTENT_METERS, -RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
             {RECEIVER_HALF_EXTENT_METERS, RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
             {-RECEIVER_HALF_EXTENT_METERS, RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS}}};
        const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> quadNormals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
        const StaticArray<uint32, QUAD_INDEX_COUNT> quadIndices{{0, 1, 2, 0, 2, 3}};
        meshes[0].Geometry = {quadPositions.GetData(),
                              quadPositions.GetSize(),
                              quadIndices.GetData(),
                              quadIndices.GetSize()};
        meshes[0].Geometry.Normals = quadNormals.GetData();

        // One prepared graph draws every combination. As GENERATE values, each re-ran the whole test case with its
        // cold graph preparation, about 19 s apiece on WARP.
        constexpr usize EYE_HEIGHT_COUNT = 2;
        constexpr usize LIGHT_ANGLE_COUNT = 3;
        constexpr usize DISK_RADIUS_COUNT = 2;
        const StaticArray<float32, EYE_HEIGHT_COUNT> eyeHeightsMeters{{50.0F, 150.0F}};
        const StaticArray<float32, LIGHT_ANGLE_COUNT> lightAnglesDegrees{{30.0F, 60.0F, 80.0F}};
        // The Sun's apparent disk widens the comparison filter; a flat receiver must stay free of acne with it too.
        const StaticArray<float32, DISK_RADIUS_COUNT> diskRadiiRadians{{0.0F, SUN_ANGULAR_RADIUS_RADIANS}};
        constexpr float32 NEAR_PLANE_METERS = .01F;
        constexpr float32 FAR_PLANE_METERS = 400;
        // The view spans the same ten metres of receiver around the centre at every eye height.
        constexpr float32 VIEW_HALF_EXTENT_METERS = 10;
        const float32 depthScale = FAR_PLANE_METERS / (FAR_PLANE_METERS - NEAR_PLANE_METERS);
        constexpr uint32 PROBE_HALF_WIDTH_PIXELS = 24;
        constexpr uint32 PROBE_HALF_HEIGHT_PIXELS = 16;
        constexpr float32 MAX_CHANNEL_DIFFERENCE = 2;
        renderer::ViewDescription& farCamera = fixture.Cameras[0];
        StaticArray<renderer::DirectLight, 2> lights;
        inputs.Lights = {lights.GetData(), lights.GetSize()};
        for (const float32 eyeHeightMeters : eyeHeightsMeters)
        {
            const float32 focal = eyeHeightMeters / VIEW_HALF_EXTENT_METERS;
            const math::Vector3 eyeWorld{0, 0, RECEIVER_HEIGHT_METERS + eyeHeightMeters};
            farCamera.OrthographicCamera = false;
            farCamera.Depth = renderer::DepthConvention::Conventional;
            farCamera.CameraPosition = eyeWorld;
            farCamera.CameraForward = {0, 0, -1};
            farCamera.WorldToClip.Columns[0] = {focal, 0, 0, 0};
            farCamera.WorldToClip.Columns[1] = {0, focal, 0, 0};
            farCamera.WorldToClip.Columns[2] = {0, 0, -depthScale, -1};
            farCamera.WorldToClip.Columns[3] = {0, 0, depthScale * (eyeWorld.Z - NEAR_PLANE_METERS), eyeWorld.Z};
            for (const float32 lightAngleDegrees : lightAnglesDegrees)
            {
                for (const float32 diskRadiusRadians : diskRadiiRadians)
                {
                    // Only the red light casts. Its otherwise identical green control shows the unoccluded
                    // contribution.
                    const float32 lightAngleRadians = math::DegreesToRadians(lightAngleDegrees);
                    lights[0] = {};
                    lights[0].SourceId = 1;
                    lights[0].Kind = renderer::LightKind::Directional;
                    lights[0].Color = {1, 0, 0};
                    lights[0].Intensity = 8;
                    lights[0].Direction = {-std::sin(lightAngleRadians), 0, -std::cos(lightAngleRadians)};
                    lights[0].AngularRadiusRadians = diskRadiusRadians;
                    lights[1] = lights[0];
                    lights[1].SourceId = 2;
                    lights[1].Color = {0, 1, 0};
                    lights[1].ShouldCastRasterShadows = false;

                    REQUIRE(update(1) == rhi::RasterStatus::Ready);
                    render();
                    REQUIRE(fixture.Graph.GetReport().Resources.DirectionalShadowLightSourceId == lights[0].SourceId);
                    float32 maximumDifference = 0;
                    uint32 darkenedPixels = 0;
                    for (uint32 y = HEIGHT / 2 - PROBE_HALF_HEIGHT_PIXELS; y < HEIGHT / 2 + PROBE_HALF_HEIGHT_PIXELS;
                         ++y)
                    {
                        for (uint32 x = WIDTH / 2 - PROBE_HALF_WIDTH_PIXELS; x < WIDTH / 2 + PROBE_HALF_WIDTH_PIXELS;
                             ++x)
                        {
                            const usize pixel = GetPixel(x, y);
                            REQUIRE(fixture.Pixels[pixel + 1] > 32);
                            const float32 difference =
                                math::Abs(float32(fixture.Pixels[pixel]) - float32(fixture.Pixels[pixel + 1]));
                            maximumDifference = math::Max(maximumDifference, difference);
                            darkenedPixels += difference > MAX_CHANNEL_DIFFERENCE ? 1 : 0;
                        }
                    }
                    CAPTURE(eyeHeightMeters, lightAngleDegrees, diskRadiusRadians, darkenedPixels);
                    CHECK(maximumDifference <= MAX_CHANNEL_DIFFERENCE);
                }
            }
        }
    }
    SECTION("oblique far cameras keep a grazing-lit receiver free of reconstruction acne")
    {
        // The resolve rebuilds each receiver from a conventional depth buffer. With a centimetre near plane the
        // rebuilt position is uncertain by centimetres tens of metres away, differently at every pixel and for
        // every camera pose. A grazing light turns that into dithered false shadows unless the comparison
        // allows for it, so sweep sub-pixel poses of a camera that sees the receiver at varying depth.
        materials[1].Emission = {};
        constexpr float32 RECEIVER_HALF_EXTENT_METERS = 40;
        constexpr float32 RECEIVER_HEIGHT_METERS = .5F;
        constexpr usize QUAD_VERTEX_COUNT = 4;
        constexpr usize QUAD_INDEX_COUNT = 6;
        const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> quadPositions{
            {{-RECEIVER_HALF_EXTENT_METERS, -RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
             {RECEIVER_HALF_EXTENT_METERS, -RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
             {RECEIVER_HALF_EXTENT_METERS, RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
             {-RECEIVER_HALF_EXTENT_METERS, RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS}}};
        const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> quadNormals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
        const StaticArray<uint32, QUAD_INDEX_COUNT> quadIndices{{0, 1, 2, 0, 2, 3}};
        meshes[0].Geometry = {quadPositions.GetData(),
                              quadPositions.GetSize(),
                              quadIndices.GetData(),
                              quadIndices.GetSize()};
        meshes[0].Geometry.Normals = quadNormals.GetData();

        constexpr float32 LIGHT_ANGLE_DEGREES = 75;
        const float32 lightAngleRadians = math::DegreesToRadians(LIGHT_ANGLE_DEGREES);
        // One prepared graph draws every combination, as in the far perspective section above.
        constexpr usize DISK_RADIUS_COUNT = 2;
        constexpr usize EYE_DISTANCE_COUNT = 2;
        // The Sun's apparent disk widens the comparison filter; a flat receiver must stay free of acne with it too.
        const StaticArray<float32, DISK_RADIUS_COUNT> diskRadiiRadians{{0.0F, SUN_ANGULAR_RADIUS_RADIANS}};
        const StaticArray<float32, EYE_DISTANCE_COUNT> eyeDistancesMeters{{50.0F, 120.0F}};
        StaticArray<renderer::DirectLight, 2> lights;
        inputs.Lights = {lights.GetData(), lights.GetSize()};
        constexpr int32 POSE_COUNT = 8;
        // A few millimetres between poses: far below a pixel, enough to change every rounding in the view.
        constexpr float32 POSE_STEP_METERS = .0037F;
        constexpr uint32 PROBE_HALF_WIDTH_PIXELS = 24;
        constexpr uint32 PROBE_HALF_HEIGHT_PIXELS = 16;
        constexpr float32 MAX_CHANNEL_DIFFERENCE = 2;
        for (const float32 diskRadiusRadians : diskRadiiRadians)
        {
            // Only the red light casts. Its otherwise identical green control shows the unoccluded contribution.
            lights[0] = {};
            lights[0].SourceId = 1;
            lights[0].Kind = renderer::LightKind::Directional;
            lights[0].Color = {1, 0, 0};
            lights[0].Intensity = 8;
            lights[0].Direction = {-std::sin(lightAngleRadians), 0, -std::cos(lightAngleRadians)};
            lights[0].AngularRadiusRadians = diskRadiusRadians;
            lights[1] = lights[0];
            lights[1].SourceId = 2;
            lights[1].Color = {0, 1, 0};
            lights[1].ShouldCastRasterShadows = false;
            for (const float32 eyeDistanceMeters : eyeDistancesMeters)
            {
                float32 maximumDifference = 0;
                uint32 darkenedPixels = 0;
                for (int32 pose = 0; pose < POSE_COUNT; ++pose)
                {
                    // Forty-five degrees above the receiver, so depth varies from pixel to pixel.
                    const float32 offset = eyeDistanceMeters * math::Sqrt(0.5F);
                    const math::Vector3 targetWorld{0, 0, RECEIVER_HEIGHT_METERS};
                    const math::Vector3 eyeWorld{static_cast<float32>(pose) * POSE_STEP_METERS,
                                                 -offset,
                                                 RECEIVER_HEIGHT_METERS + offset};
                    BuildConventionalEditorCamera(eyeWorld, targetWorld, fixture.Cameras[0]);

                    REQUIRE(update(1) == rhi::RasterStatus::Ready);
                    render();
                    REQUIRE(fixture.Graph.GetReport().Resources.DirectionalShadowLightSourceId == lights[0].SourceId);
                    for (uint32 y = HEIGHT / 2 - PROBE_HALF_HEIGHT_PIXELS; y < HEIGHT / 2 + PROBE_HALF_HEIGHT_PIXELS;
                         ++y)
                    {
                        for (uint32 x = WIDTH / 2 - PROBE_HALF_WIDTH_PIXELS; x < WIDTH / 2 + PROBE_HALF_WIDTH_PIXELS;
                             ++x)
                        {
                            const usize pixel = GetPixel(x, y);
                            REQUIRE(fixture.Pixels[pixel + 1] > 32);
                            const float32 difference =
                                math::Abs(float32(fixture.Pixels[pixel]) - float32(fixture.Pixels[pixel + 1]));
                            maximumDifference = math::Max(maximumDifference, difference);
                            darkenedPixels += difference > MAX_CHANNEL_DIFFERENCE ? 1 : 0;
                        }
                    }
                }
                CAPTURE(diskRadiusRadians, eyeDistanceMeters, darkenedPixels);
                CHECK(maximumDifference <= MAX_CHANNEL_DIFFERENCE);
            }
        }
    }
    SECTION("oblique far cameras keep an area-lit receiver free of reconstruction acne")
    {
        // The mesh-light cube faces store the receiver itself, and the resolve compares them with the receiver
        // rebuilt from conventional depth. Tens of metres from the camera that position lies centimetres off the
        // surface, differently at every pixel and camera pose, so a comparison that ignores it dithers false
        // shadows over the floor under an area light. Check each pixel against the CPU integral of the unshadowed
        // emitter where its ray meets the receiver, over sub-pixel poses.
        constexpr float32 RECEIVER_HALF_EXTENT_METERS = 40;
        constexpr float32 RECEIVER_HEIGHT_METERS = .5F;
        constexpr float32 EMITTER_HALF_EXTENT_METERS = 1;
        constexpr float32 EMITTER_HEIGHT_METERS = RECEIVER_HEIGHT_METERS + 3;
        // Bright enough that the floor under the emitter reaches most of the eight-bit range.
        constexpr float32 EMITTER_RADIANCE = 13;
        constexpr usize QUAD_VERTEX_COUNT = 4;
        constexpr usize QUAD_INDEX_COUNT = 6;
        const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> quadPositions{
            {{-RECEIVER_HALF_EXTENT_METERS, -RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
             {RECEIVER_HALF_EXTENT_METERS, -RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
             {RECEIVER_HALF_EXTENT_METERS, RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
             {-RECEIVER_HALF_EXTENT_METERS, RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS}}};
        const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> quadNormals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
        const StaticArray<uint32, QUAD_INDEX_COUNT> quadIndices{{0, 1, 2, 0, 2, 3}};
        meshes[0].Geometry = {quadPositions.GetData(),
                              quadPositions.GetSize(),
                              quadIndices.GetData(),
                              quadIndices.GetSize()};
        meshes[0].Geometry.Normals = quadNormals.GetData();
        // A square emitter facing down at the floor's centre.
        const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> emitterPositions{
            {{-EMITTER_HALF_EXTENT_METERS, -EMITTER_HALF_EXTENT_METERS, EMITTER_HEIGHT_METERS},
             {-EMITTER_HALF_EXTENT_METERS, EMITTER_HALF_EXTENT_METERS, EMITTER_HEIGHT_METERS},
             {EMITTER_HALF_EXTENT_METERS, EMITTER_HALF_EXTENT_METERS, EMITTER_HEIGHT_METERS},
             {EMITTER_HALF_EXTENT_METERS, -EMITTER_HALF_EXTENT_METERS, EMITTER_HEIGHT_METERS}}};
        const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> emitterNormals{
            {{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}}};
        meshes[1].Geometry = {emitterPositions.GetData(),
                              emitterPositions.GetSize(),
                              quadIndices.GetData(),
                              quadIndices.GetSize()};
        meshes[1].Geometry.Normals = emitterNormals.GetData();
        items[1].MeshToWorld.Translation = {};
        materials[1].Emission = {EMITTER_RADIANCE, 0, 0};
        Array<math::Vector4> words;
        uint32 recordCount = 0;
        REQUIRE(renderer::internal::AppendRasterMeshLights(meshes[1].Geometry,
                                                           items[1].MeshToWorld,
                                                           materials[1],
                                                           words,
                                                           recordCount) == rhi::RasterStatus::Ready);
        // The coplanar square merges into one polygon record, which the CPU check integrates whole.
        REQUIRE(recordCount == 1);
        const renderer::SceneShaderMaterial& receiver = materials[0];

        // One prepared graph checks both distances, as in the far perspective section above.
        constexpr usize EYE_DISTANCE_COUNT = 2;
        const StaticArray<float32, EYE_DISTANCE_COUNT> eyeDistancesMeters{{50.0F, 120.0F}};
        for (const float32 eyeDistanceMeters : eyeDistancesMeters)
        {
            constexpr int32 POSE_COUNT = 8;
            // A few millimetres between poses: far below a pixel, enough to change every rounding in the view.
            constexpr float32 POSE_STEP_METERS = .0037F;
            // Pixels whose ray passes this close to the emitter may show it instead of the floor.
            constexpr float32 EMITTER_MARGIN_METERS = .5F;
            // Checked pixels: well lit by the emitter and clear of eight-bit saturation.
            constexpr float32 MIN_CHECKED_CODE = 24;
            constexpr float32 MAX_CHECKED_CODE = 240;
            constexpr float32 CODE_SCALE = 255;
            // The resolve's float evaluation and eight-bit output differ from the CPU integral by a couple of codes;
            // one shadowed filter tap of nine already darkens a checked pixel by more.
            constexpr float32 MAX_DIFFERENCE_CODES = 3;
            constexpr uint32 MIN_CHECKED_PIXELS = 64;
            // Agreement of the CPU pixel rays with the camera's projection, in normalized device coordinates.
            constexpr float32 MAX_RAY_ERROR_NDC = 1e-3F;
            // The resolve's allowance for a rebuilt receiver: four units in the last place of the stored depth.
            constexpr float32 DEPTH_RELATIVE_UNCERTAINTY = 2.384185791015625e-07F;
            const auto evaluateCode = [&](math::Vector3 positionWorld) {
                math::Vector3 viewWorld;
                REQUIRE(math::TryNormalize(fixture.Cameras[0].CameraPosition - positionWorld, viewWorld) ==
                        math::MathStatus::Success);
                math::Vector3 radiance;
                if (renderer::internal::TryEvaluateRasterMeshLightFar({words.GetData(), words.GetSize()},
                                                                      positionWorld,
                                                                      {0, 0, 1},
                                                                      viewWorld,
                                                                      receiver.BaseColor,
                                                                      receiver.Metallic,
                                                                      receiver.Roughness,
                                                                      radiance) == false)
                {
                    radiance = renderer::internal::EvaluateRasterMeshLight({words.GetData(), words.GetSize()},
                                                                           positionWorld,
                                                                           {0, 0, 1},
                                                                           viewWorld,
                                                                           receiver.BaseColor,
                                                                           receiver.Metallic,
                                                                           receiver.Roughness);
                }
                return radiance.X * CODE_SCALE;
            };
            constexpr usize LATERAL_BOUND_COUNT = 5;
            const float32 tangentY = std::tan(EDITOR_VERTICAL_FOV_RADIANS / 2);
            const float32 tangentX = tangentY * static_cast<float32>(WIDTH) / static_cast<float32>(HEIGHT);
            float32 maximumDarkening = 0;
            // The unshadowed CPU integral bounds the image from above too, which shows the check itself is sound.
            float32 maximumBrightening = 0;
            float32 maximumRayError = 0;
            uint32 darkenedPixels = 0;
            uint32 checkedPixels = 0;
            for (int32 pose = 0; pose < POSE_COUNT; ++pose)
            {
                // Forty-five degrees above the receiver, so depth varies from pixel to pixel.
                const float32 offset = eyeDistanceMeters * math::Sqrt(0.5F);
                const math::Vector3 targetWorld{0, 0, RECEIVER_HEIGHT_METERS};
                const math::Vector3 eyeWorld{static_cast<float32>(pose) * POSE_STEP_METERS,
                                             -offset,
                                             RECEIVER_HEIGHT_METERS + offset};
                renderer::ViewDescription& obliqueCamera = fixture.Cameras[0];
                BuildConventionalEditorCamera(eyeWorld, targetWorld, obliqueCamera);
                // The view basis of math::TryLookAt, for the ray through each pixel centre.
                const math::Vector3 forwardWorld = obliqueCamera.CameraForward;
                math::Vector3 rightWorld;
                REQUIRE(math::TryNormalize(math::Cross(forwardWorld, {0, 0, 1}), rightWorld) ==
                        math::MathStatus::Success);
                const math::Vector3 upWorld = math::Cross(rightWorld, forwardWorld);
                const auto intersectReceiver = [&](math::Vector3 directionWorld) {
                    return eyeWorld + directionWorld * ((RECEIVER_HEIGHT_METERS - eyeWorld.Z) / directionWorld.Z);
                };

                REQUIRE(update(SCENE_COUNT - 1) == rhi::RasterStatus::Ready);
                render();
                REQUIRE(fixture.Graph.GetReport().Resources.MeshLightShadowCasters == 1);
                REQUIRE(fixture.Graph.GetReport().PendingMeshLightShadowFaces == 0);
                for (uint32 y = 0; y < HEIGHT; ++y)
                {
                    for (uint32 x = 0; x < WIDTH; ++x)
                    {
                        const float32 ndcX = (static_cast<float32>(x) + 0.5F) / static_cast<float32>(WIDTH) * 2 - 1;
                        const float32 ndcY = 1 - (static_cast<float32>(y) + 0.5F) / static_cast<float32>(HEIGHT) * 2;
                        const math::Vector3 directionWorld =
                            forwardWorld + rightWorld * (ndcX * tangentX) + upWorld * (ndcY * tangentY);
                        if (directionWorld.Z >= 0)
                        {
                            continue;
                        }
                        const math::Vector3 emitterHitWorld =
                            eyeWorld + directionWorld * ((EMITTER_HEIGHT_METERS - eyeWorld.Z) / directionWorld.Z);
                        if (math::Abs(emitterHitWorld.X) < EMITTER_HALF_EXTENT_METERS + EMITTER_MARGIN_METERS &&
                            math::Abs(emitterHitWorld.Y) < EMITTER_HALF_EXTENT_METERS + EMITTER_MARGIN_METERS)
                        {
                            continue;
                        }
                        const math::Vector3 positionWorld = intersectReceiver(directionWorld);
                        const math::Vector4 positionClip =
                            obliqueCamera.WorldToClip *
                            math::Vector4{positionWorld.X, positionWorld.Y, positionWorld.Z, 1};
                        maximumRayError = math::Max(maximumRayError,
                                                    math::Max(math::Abs(positionClip.X / positionClip.W - ndcX),
                                                              math::Abs(positionClip.Y / positionClip.W - ndcY)));
                        const float32 expectedCode = evaluateCode(positionWorld);
                        if (expectedCode < MIN_CHECKED_CODE || expectedCode > MAX_CHECKED_CODE)
                        {
                            continue;
                        }
                        // The resolve shades a receiver rebuilt from depth, which lies within the depth allowance along
                        // the ray and, tens of metres away, a fraction of a pixel to its side. The unshadowed radiance
                        // over half a pixel around the centre and that allowance bounds the pixel. One unit of view
                        // depth moves the position by directionWorld.
                        const float32 viewDepthMeters = math::Dot(positionWorld - eyeWorld, forwardWorld);
                        const float32 depth = EDITOR_FAR_PLANE_METERS * (viewDepthMeters - EDITOR_NEAR_PLANE_METERS) /
                                              (viewDepthMeters * (EDITOR_FAR_PLANE_METERS - EDITOR_NEAR_PLANE_METERS));
                        const float32 viewDepthPerDepth = viewDepthMeters * viewDepthMeters *
                                                          (EDITOR_FAR_PLANE_METERS - EDITOR_NEAR_PLANE_METERS) /
                                                          (EDITOR_FAR_PLANE_METERS * EDITOR_NEAR_PLANE_METERS);
                        const math::Vector3 uncertaintyWorld =
                            directionWorld * (viewDepthPerDepth * depth * DEPTH_RELATIVE_UNCERTAINTY);
                        const math::Vector3 halfPixelRightWorld = rightWorld * (tangentX / static_cast<float32>(WIDTH));
                        const math::Vector3 halfPixelUpWorld = upWorld * (tangentY / static_cast<float32>(HEIGHT));
                        const StaticArray<math::Vector3, LATERAL_BOUND_COUNT> lateralWorld{
                            {{}, -halfPixelRightWorld, halfPixelRightWorld, -halfPixelUpWorld, halfPixelUpWorld}};
                        float32 lowerCode = expectedCode;
                        float32 upperCode = expectedCode;
                        for (const math::Vector3& sideWorld : lateralWorld)
                        {
                            const math::Vector3 sidePositionWorld = intersectReceiver(directionWorld + sideWorld);
                            for (const float32 along : {-1.0F, 0.0F, 1.0F})
                            {
                                const float32 boundCode = evaluateCode(sidePositionWorld + uncertaintyWorld * along);
                                lowerCode = math::Min(lowerCode, boundCode);
                                upperCode = math::Max(upperCode, boundCode);
                            }
                        }
                        ++checkedPixels;
                        const float32 code = float32(fixture.Pixels[GetPixel(x, y)]);
                        const float32 darkening = lowerCode - code;
                        maximumDarkening = math::Max(maximumDarkening, darkening);
                        maximumBrightening = math::Max(maximumBrightening, code - upperCode);
                        darkenedPixels += darkening > MAX_DIFFERENCE_CODES ? 1 : 0;
                    }
                }
            }
            CAPTURE(eyeDistanceMeters, checkedPixels, darkenedPixels, maximumDarkening, maximumBrightening);
            REQUIRE(maximumRayError <= MAX_RAY_ERROR_NDC);
            REQUIRE(checkedPixels >= MIN_CHECKED_PIXELS * POSE_COUNT);
            CHECK(maximumBrightening <= MAX_DIFFERENCE_CODES);
            CHECK(maximumDarkening <= MAX_DIFFERENCE_CODES);
        }
    }
    SECTION("front-facing geometry stays lit at the shading-normal horizon")
    {
        materials[1].Emission = {};
        constexpr float32 DEPTH_SLOPE = 0.8F;
        const StaticArray<math::Vector3, VERTEX_COUNT> grazingNormals{
            {{-.98F, 0, -.2F}, {-.98F, 0, -.2F}, {-.98F, 0, -.2F}}};
        for (math::Vector3& position : fixture.Positions)
        {
            position.Z += position.X * DEPTH_SLOPE;
        }
        // This front-facing triangle has shading normals that turn away from
        // the camera. Its unoccluded light still reaches the visible surface.
        meshes[0].Geometry.Normals = grazingNormals.GetData();
        renderer::DirectLight light;
        light.SourceId = 1;
        light.Kind = renderer::LightKind::Directional;
        light.Direction = {1, 0, -1};
        light.Intensity = 2;
        inputs.Lights = {&light, 1};

        REQUIRE(update(1) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[center] > 32);
    }
}

TEST_CASE("default deferred sky visibility keeps environment light out of covered receivers",
          "[sky-visibility][lighting-views][gpu]")
{
    constexpr usize POLL_ATTEMPTS = 10000;
    constexpr usize QUAD_VERTEX_COUNT = 4;
    constexpr usize QUAD_INDEX_COUNT = 6;
    constexpr usize SCENE_COUNT = 2;
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const auto poll = [&](rhi::RasterStatus status) {
        for (usize attempt = 0; attempt < POLL_ATTEMPTS && status == rhi::RasterStatus::Pending; ++attempt)
        {
            threading::SleepFor(timing::Milliseconds(1));
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
        REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(fixture.Pixels)) == true);
    };
    // A wall facing the orthographic camera (+Z) under the +Y sky, and a horizontal roof at y = 0.1 jutting out
    // towards the camera over its lower half. The camera sees the roof edge-on. The sky above the roof reaches the
    // upper wall in full; under the roof the wall sees only the low sky past the roof's far edge.
    constexpr float32 ROOF_HEIGHT_METERS = .1F;
    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> wallPositions{
        {{-1, -1, .5F}, {1, -1, .5F}, {1, 1, .5F}, {-1, 1, .5F}}};
    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> wallNormals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> roofPositions{{{-1, ROOF_HEIGHT_METERS, .5F},
                                                                       {1, ROOF_HEIGHT_METERS, .5F},
                                                                       {1, ROOF_HEIGHT_METERS, 1.5F},
                                                                       {-1, ROOF_HEIGHT_METERS, 1.5F}}};
    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> roofNormals{{{0, -1, 0}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0}}};
    const StaticArray<uint32, QUAD_INDEX_COUNT> quadIndices{{0, 1, 2, 0, 2, 3}};
    StaticArray<renderer::RenderGraphMesh, SCENE_COUNT> meshes{{fixture.Meshes[0], fixture.Meshes[0]}};
    meshes[0].Geometry = {wallPositions.GetData(), wallPositions.GetSize(), quadIndices.GetData(), QUAD_INDEX_COUNT};
    meshes[0].Geometry.Normals = wallNormals.GetData();
    meshes[1].Geometry = {roofPositions.GetData(), roofPositions.GetSize(), quadIndices.GetData(), QUAD_INDEX_COUNT};
    meshes[1].Geometry.Normals = roofNormals.GetData();
    const uint32 material = 0;
    meshes[0].MaterialBindings = {&material, 1};
    meshes[1].MaterialBindings = {&material, 1};
    StaticArray<renderer::RenderGraphItem, SCENE_COUNT> items{{fixture.Items[0], fixture.Items[0]}};
    items[1].SourceId = 2;
    items[1].MeshIndex = 1;
    StaticArray<renderer::SceneShaderMaterial, 1> materials{};
    materials[0].BaseColor = {1, 1, 1};
    materials[0].Roughness = 1;
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    inputs.Environment = {.6F, .6F, .6F};
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
    // The identity camera maps world x and y in [-1, 1] across the target, y up.
    const auto getRowPixel = [](float32 worldY) {
        return GetPixel(WIDTH / 2, static_cast<uint32>((1 - worldY) * 0.5F * static_cast<float32>(HEIGHT)));
    };
    constexpr float32 ABOVE_ROOF_METERS = .5F;
    constexpr float32 UNDER_ROOF_METERS = 0;
    const usize above = getRowPixel(ABOVE_ROOF_METERS);
    const usize under = getRowPixel(UNDER_ROOF_METERS);

    REQUIRE(update(1) == rhi::RasterStatus::Ready);
    render();
    CHECK(fixture.Graph.GetReport().Resources.SkyVisibilityMaps == renderer::SKY_VISIBILITY_DIRECTION_COUNT);
    const uint8 openAbove = fixture.Pixels[above];
    const uint8 openUnder = fixture.Pixels[under];
    REQUIRE(openAbove > 32);
    // Nothing occludes the bare wall's sky, so every row receives the same environment light.
    CHECK(math::Abs(static_cast<float32>(openUnder) - static_cast<float32>(openAbove)) <= 1);

    REQUIRE(update(SCENE_COUNT) == rhi::RasterStatus::Ready);
    render();
    CHECK(math::Abs(static_cast<float32>(fixture.Pixels[above]) - static_cast<float32>(openAbove)) <= 1);
    // The roof hides the upper sky the wall faces; before sky visibility the deferred resolve lit it in full.
    const uint8 coveredUnder = fixture.Pixels[under];
    CHECK(static_cast<float32>(coveredUnder) < static_cast<float32>(openUnder) * 0.75F);
    CHECK(coveredUnder > 0);

    SECTION("the sky visibility view shows the covered fraction")
    {
        fixture.Cameras[0].LightingView = renderer::DeferredLightingView::SkyVisibility;
        REQUIRE(update(SCENE_COUNT) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[above] >= 254);
        CHECK(fixture.Pixels[under] < 192);
        CHECK(fixture.Pixels[under] == fixture.Pixels[under + 1]);
        CHECK(fixture.Pixels[under] == fixture.Pixels[under + 2]);
    }
    SECTION("term views isolate the environment from emission and lights")
    {
        materials[0].Emission = {0, .5F, 0};
        StaticArray<renderer::DirectLight, 2> lights;
        lights[0].SourceId = 7;
        lights[0].Kind = renderer::LightKind::Directional;
        lights[0].Color = {0, 0, 1};
        lights[0].Intensity = 2;
        lights[0].Direction = {0, 0, -1};
        lights[1] = lights[0];
        lights[1].SourceId = 9;
        lights[1].Color = {1, 0, 0};
        lights[1].ShouldCastRasterShadows = false;
        inputs.Lights = {lights.GetData(), lights.GetSize()};

        fixture.Cameras[0].LightingView = renderer::DeferredLightingView::SkyDiffuse;
        REQUIRE(update(1) == rhi::RasterStatus::Ready);
        render();
        // The grey environment alone, without the green emission or either light: its diffuse and specular views add
        // up to the environment-only image.
        const uint8 skyDiffuse = fixture.Pixels[above];
        CHECK(fixture.Pixels[above] == fixture.Pixels[above + 2]);
        CHECK(fixture.Pixels[above + 1] == fixture.Pixels[above + 2]);
        fixture.Cameras[0].LightingView = renderer::DeferredLightingView::SkySpecular;
        REQUIRE(update(1) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[above] > 0);
        CHECK(math::Abs(static_cast<float32>(skyDiffuse) + static_cast<float32>(fixture.Pixels[above]) -
                        static_cast<float32>(openAbove)) <= 2);

        fixture.Cameras[0].LightingView = renderer::DeferredLightingView::Light;
        fixture.Cameras[0].LightingViewSourceId = lights[0].SourceId;
        REQUIRE(update(1) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[above] <= 1);
        CHECK(fixture.Pixels[above + 1] <= 1);
        const uint8 isolatedBlue = fixture.Pixels[above + 2];
        CHECK(isolatedBlue > 32);

        fixture.Cameras[0].LightingView = renderer::DeferredLightingView::DirectLights;
        REQUIRE(update(1) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[above] > 32);
        CHECK(fixture.Pixels[above + 1] <= 1);
        CHECK(fixture.Pixels[above + 2] == isolatedBlue);

        // The mapped light covers the wall and nothing occludes it; the light without a map reads cyan.
        fixture.Cameras[0].LightingView = renderer::DeferredLightingView::ShadowCoverage;
        REQUIRE(update(1) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[above] >= 254);
        CHECK(fixture.Pixels[above + 1] >= 254);
        CHECK(fixture.Pixels[above + 2] >= 254);
        fixture.Cameras[0].LightingViewSourceId = lights[1].SourceId;
        REQUIRE(update(1) == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[above] <= 1);
        CHECK(fixture.Pixels[above + 1] >= 254);
        CHECK(fixture.Pixels[above + 2] >= 254);

        // A bright red light dominates: the saturated pixel takes the hue of its category, the second light in
        // source ID order.
        constexpr float32 DOMINANT_INTENSITY = 40;
        lights[1].Intensity = DOMINANT_INTENSITY;
        fixture.Cameras[0].LightingView = renderer::DeferredLightingView::DominantLight;
        REQUIRE(update(1) == rhi::RasterStatus::Ready);
        render();
        const math::Vector3 hue =
            renderer::GetDeferredLightingViewHue(renderer::DEFERRED_LIGHTING_VIEW_FIRST_LIGHT_CATEGORY + 1);
        constexpr float32 CODES_PER_UNIT = 255;
        constexpr float32 HUE_TOLERANCE_CODES = 2;
        CHECK(math::Abs(static_cast<float32>(fixture.Pixels[above]) - hue.X * CODES_PER_UNIT) <= HUE_TOLERANCE_CODES);
        CHECK(math::Abs(static_cast<float32>(fixture.Pixels[above + 1]) - hue.Y * CODES_PER_UNIT) <=
              HUE_TOLERANCE_CODES);
        CHECK(math::Abs(static_cast<float32>(fixture.Pixels[above + 2]) - hue.Z * CODES_PER_UNIT) <=
              HUE_TOLERANCE_CODES);

        // A lighting view outside the enumeration rejects the update and keeps the last image.
        const StaticArray<uint8, PIXEL_BYTES> shown = fixture.Pixels;
        constexpr uint8 INVALID_VIEW = 200;
        fixture.Cameras[0].LightingView = static_cast<renderer::DeferredLightingView>(INVALID_VIEW);
        CHECK(update(1) == rhi::RasterStatus::InvalidDescription);
        render();
        CHECK(fixture.Pixels == shown);
    }
}

TEST_CASE("default deferred rebuilds far receivers from reverse-Z depth to well under a millimetre",
          "[render-graph][clustered-deferred][depth-precision][gpu]")
{
    // The World viewport hands the default graph the editor's finite reverse-Z projection with a centimetre near
    // plane. Its stored float32 depth keeps a relative precision near 2^-24 at every distance, so the resolve rebuilds
    // a receiver tens of metres away within micrometres. The same camera rewritten to conventional depth (w - z) puts
    // it centimetres off its surface: the control below shows the measurement detects that loss. Read the stored
    // depth back exactly and rebuild each pixel as renderer_deferred_lighting.slang does, with and without the
    // prepass, whose depth the deferred pass loads and tests for equality.
    constexpr usize POLL_ATTEMPTS = 10000;
    constexpr float32 RECEIVER_HALF_EXTENT_METERS = 40;
    constexpr float32 RECEIVER_HEIGHT_METERS = .5F;
    constexpr usize QUAD_VERTEX_COUNT = 4;
    constexpr usize QUAD_INDEX_COUNT = 6;
    constexpr float32 NEAR_PLANE_METERS = .01F;
    // The editor bounds an infinite lens 1% beyond the farthest corner of the World's geometry (FINITE_FAR_MARGIN in
    // apps/editor/src/render_scene.cpp), here the receiver's: about 93 m at 50 m out and 158 m at 120 m.
    constexpr float32 FINITE_FAR_MARGIN = 1.01F;
    constexpr float32 VERTICAL_FOV_RADIANS = .2F;
    // Well under a millimetre: reverse-Z float32 depth keeps tens of micrometres even at 120 m.
    constexpr float64 MAX_POSITION_ERROR_METERS = 1e-4;
    const bool shouldUsePrepass = GENERATE(false, true);
    const float32 eyeDistanceMeters = GENERATE(50.0F, 120.0F);
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage(shouldUsePrepass == true
                        ? PackageLocation{LUDUS_RG3_CLUSTERED_ZPREPASS_DIRECTORY, "rg2_clustered_ZPrepass.lurgrt"}
                        : PackageLocation{LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(programs.HasClusteredDeferred == true);
    const auto poll = [&](rhi::RasterStatus status) {
        for (usize attempt = 0; attempt < POLL_ATTEMPTS && status == rhi::RasterStatus::Pending; ++attempt)
        {
            threading::SleepFor(timing::Milliseconds(1));
            status = fixture.Graph.Poll();
        }
        return status;
    };
    REQUIRE(poll(fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
            rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.CanUseReverseDepth() == true);
    uint32 depthResource = format::ABSENT;
    for (usize resource = 0; resource < package->GetPlan().Resources.GetSize(); ++resource)
    {
        if (package->GetPlan().Resources[resource].Kind == format::Aspect::Depth)
        {
            depthResource = static_cast<uint32>(resource);
        }
    }
    REQUIRE(depthResource != format::ABSENT);

    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> quadPositions{
        {{-RECEIVER_HALF_EXTENT_METERS, -RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
         {RECEIVER_HALF_EXTENT_METERS, -RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
         {RECEIVER_HALF_EXTENT_METERS, RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
         {-RECEIVER_HALF_EXTENT_METERS, RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS}}};
    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> quadNormals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    const StaticArray<uint32, QUAD_INDEX_COUNT> quadIndices{{0, 1, 2, 0, 2, 3}};
    fixture.Meshes[0].Geometry = {quadPositions.GetData(),
                                  quadPositions.GetSize(),
                                  quadIndices.GetData(),
                                  quadIndices.GetSize()};
    fixture.Meshes[0].Geometry.Normals = quadNormals.GetData();
    const uint32 materialIndex = 0;
    fixture.Meshes[0].MaterialBindings = {&materialIndex, 1};
    StaticArray<renderer::SceneShaderMaterial, 1> materials{};
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};

    // Forty-five degrees above the receiver, so depth varies from pixel to pixel, built as the editor builds it.
    const float32 offset = eyeDistanceMeters * math::Sqrt(0.5F);
    const math::Vector3 targetWorld{0, 0, RECEIVER_HEIGHT_METERS};
    const math::Vector3 eyeWorld{0, -offset, RECEIVER_HEIGHT_METERS + offset};
    renderer::ProjectionDescription projection;
    projection.Kind = renderer::ProjectionKind::Perspective;
    REQUIRE(math::TryLookAt(eyeWorld, targetWorld, {0, 0, 1}, projection.WorldToView) == math::MathStatus::Success);
    projection.VerticalFovRadians = VERTICAL_FOV_RADIANS;
    projection.Near = NEAR_PLANE_METERS;
    float32 farthestCornerMeters = 0;
    for (const math::Vector3& cornerWorld : quadPositions)
    {
        farthestCornerMeters = math::Max(farthestCornerMeters, math::Length(cornerWorld - eyeWorld));
    }
    projection.Far = farthestCornerMeters * FINITE_FAR_MARGIN;
    renderer::ViewMapping mapping;
    REQUIRE(renderer::ResolveViewMapping({WIDTH, HEIGHT, renderer::ScreenFit::Stretch},
                                         {0, 0, WIDTH, HEIGHT},
                                         WIDTH,
                                         HEIGHT,
                                         mapping) == rhi::RasterStatus::Ready);
    renderer::ViewDescription reverseCamera;
    REQUIRE(renderer::BuildViewDescription(projection, mapping, reverseCamera) == rhi::RasterStatus::Ready);
    REQUIRE(reverseCamera.Depth == renderer::DepthConvention::ReverseZ);
    REQUIRE(reverseCamera.InfiniteFar == false);
    reverseCamera.CameraPosition = eyeWorld;
    REQUIRE(math::TryNormalize(targetWorld - eyeWorld, reverseCamera.CameraForward) == math::MathStatus::Success);
    renderer::ViewDescription conventionalCamera = reverseCamera;
    for (math::Vector4& column : conventionalCamera.WorldToClip.Columns)
    {
        column.Z = column.W - column.Z;
    }
    conventionalCamera.Depth = renderer::DepthConvention::Conventional;

    DepthDisplay depthBits(fixture.Device, shaders::rg2_depth_bits::Vertex(), shaders::rg2_depth_bits::Fragment());
    uint64 revision = 0;
    const auto measure = [&](const renderer::ViewDescription& camera) {
        fixture.Cameras[0] = camera;
        renderer::RenderGraphReport report;
        ++revision;
        REQUIRE(poll(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                          {fixture.Items.GetData(), fixture.Items.GetSize()},
                                          {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                          revision,
                                          renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                          report,
                                          &inputs,
                                          revision)) == rhi::RasterStatus::Ready);
        fixture.Render();
        rhi::TextureHandle texture;
        rhi::TextureAspect aspect = rhi::TextureAspect::Color;
        rhi::RasterStatus allocation = rhi::RasterStatus::NotReady;
        for (usize attempt = 0; attempt < POLL_ATTEMPTS && allocation == rhi::RasterStatus::NotReady; ++attempt)
        {
            allocation = fixture.Graph.GetResourceAllocation(depthResource, texture, aspect);
            if (allocation == rhi::RasterStatus::NotReady)
            {
                threading::SleepFor(timing::Milliseconds(1));
            }
        }
        REQUIRE(allocation == rhi::RasterStatus::Ready);
        REQUIRE(aspect == rhi::TextureAspect::Depth);
        StaticArray<uint8, PIXEL_BYTES> depthPixels{};
        depthBits.Show(fixture.Surface, texture, depthPixels);

        // The resolve's arithmetic: clip-to-world about the eye, then a float32 homogeneous product per pixel centre.
        math::Matrix4 clipToWorld;
        REQUIRE(renderer::internal::TryMakeCameraClipToWorld(camera, clipToWorld) == true);
        const float32 clearDepth = camera.Depth == renderer::DepthConvention::ReverseZ ? 0.0F : 1.0F;
        float64 maximumErrorMeters = 0;
        usize coveredPixels = 0;
        for (uint32 y = 0; y < HEIGHT; ++y)
        {
            for (uint32 x = 0; x < WIDTH; ++x)
            {
                const float32 depth = ReadDepthBits(depthPixels, x, y);
                if (depth == clearDepth)
                {
                    continue;
                }
                ++coveredPixels;
                const math::Vector4 positionHomogeneousClip{
                    (static_cast<float32>(x) + 0.5F) / static_cast<float32>(WIDTH) * 2 - 1,
                    1 - (static_cast<float32>(y) + 0.5F) / static_cast<float32>(HEIGHT) * 2,
                    depth,
                    1};
                const math::Vector4 positionHomogeneousWorld = clipToWorld * positionHomogeneousClip;
                const math::Vector3d rebuiltWorld{
                    static_cast<float64>(positionHomogeneousWorld.X / positionHomogeneousWorld.W),
                    static_cast<float64>(positionHomogeneousWorld.Y / positionHomogeneousWorld.W),
                    static_cast<float64>(positionHomogeneousWorld.Z / positionHomogeneousWorld.W)};
                // The rebuilt point lies on its pixel's ray; its error is how far along that ray it misses the
                // receiver plane.
                const math::Vector3d eye{static_cast<float64>(eyeWorld.X),
                                         static_cast<float64>(eyeWorld.Y),
                                         static_cast<float64>(eyeWorld.Z)};
                const math::Vector3d ray = rebuiltWorld - eye;
                const float64 alongRay =
                    (static_cast<float64>(RECEIVER_HEIGHT_METERS) - eye.Z) / (rebuiltWorld.Z - eye.Z);
                maximumErrorMeters = math::Max(maximumErrorMeters, math::Abs(1 - alongRay) * math::Length(ray));
            }
        }
        // The receiver fills the view.
        CHECK(coveredPixels == static_cast<usize>(WIDTH) * HEIGHT);
        return maximumErrorMeters;
    };
    const float64 reverseErrorMeters = measure(reverseCamera);
    const float64 conventionalErrorMeters = measure(conventionalCamera);
    CAPTURE(shouldUsePrepass, eyeDistanceMeters, reverseErrorMeters, conventionalErrorMeters);
    CHECK(reverseErrorMeters < MAX_POSITION_ERROR_METERS);
    CHECK(conventionalErrorMeters > MAX_POSITION_ERROR_METERS);
}

TEST_CASE("default deferred samples minified material textures at their mean", "[texture-mips][gpu]")
{
    // A one-texel black and white checker tiled thousands of times across a receiver seen from fifty
    // metres has many checks per pixel. The chain's coarser levels hold the checker's linear mean, so
    // the render must match the same receiver carrying a uniform texture of that mean.
    constexpr usize POLL_ATTEMPTS = 10000;
    constexpr usize QUAD_VERTEX_COUNT = 4;
    constexpr usize QUAD_INDEX_COUNT = 6;
    constexpr usize CHANNEL_COUNT = 4;
    constexpr uint8 CODE_MAXIMUM = 255;
    constexpr uint32 CHECKER_EXTENT = 64;
    // Not a power of two: pixel centres then land at varying texel phases, so level zero alone cannot
    // average the checks by coincidence.
    constexpr float32 CHECKER_REPEATS = 61;
    constexpr float32 RECEIVER_HALF_EXTENT_METERS = 20;
    constexpr float32 RECEIVER_HEIGHT_METERS = .5F;
    constexpr float32 EYE_HEIGHT_METERS = 50;
    constexpr float32 VIEW_HALF_EXTENT_METERS = 10;
    constexpr float32 NEAR_PLANE_METERS = .01F;
    constexpr float32 FAR_PLANE_METERS = 400;
    constexpr uint32 PROBE_HALF_WIDTH_PIXELS = 24;
    constexpr uint32 PROBE_HALF_HEIGHT_PIXELS = 16;
    constexpr float32 MAX_CHANNEL_DIFFERENCE = 2;
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const auto poll = [&](rhi::RasterStatus status) {
        for (usize attempt = 0; attempt < POLL_ATTEMPTS && status == rhi::RasterStatus::Pending; ++attempt)
        {
            threading::SleepFor(timing::Milliseconds(1));
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
        REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(fixture.Pixels)) == true);
    };

    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> quadPositions{
        {{-RECEIVER_HALF_EXTENT_METERS, -RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
         {RECEIVER_HALF_EXTENT_METERS, -RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
         {RECEIVER_HALF_EXTENT_METERS, RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS},
         {-RECEIVER_HALF_EXTENT_METERS, RECEIVER_HALF_EXTENT_METERS, RECEIVER_HEIGHT_METERS}}};
    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> quadNormals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    const StaticArray<math::Vector2, QUAD_VERTEX_COUNT> quadUvs{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    const StaticArray<uint32, QUAD_INDEX_COUNT> quadIndices{{0, 1, 2, 0, 2, 3}};
    StaticArray<renderer::RenderGraphMesh, 1> meshes{{fixture.Meshes[0]}};
    meshes[0].Geometry = {quadPositions.GetData(),
                          quadPositions.GetSize(),
                          quadIndices.GetData(),
                          quadIndices.GetSize()};
    meshes[0].Geometry.Normals = quadNormals.GetData();
    meshes[0].Geometry.Texcoords = quadUvs.GetData();
    const uint32 receiverMaterial = 0;
    meshes[0].MaterialBindings = {&receiverMaterial, 1};
    StaticArray<renderer::RenderGraphItem, 1> items{{fixture.Items[0]}};

    Array<uint8> checker;
    REQUIRE(checker.TryResize(usize{CHECKER_EXTENT} * CHECKER_EXTENT * CHANNEL_COUNT) == true);
    for (uint32 y = 0; y < CHECKER_EXTENT; ++y)
    {
        for (uint32 x = 0; x < CHECKER_EXTENT; ++x)
        {
            const uint8 code = ((x + y) & 1) == 0 ? 0 : CODE_MAXIMUM;
            uint8* texel = checker.GetData() + (usize{y} * CHECKER_EXTENT + x) * CHANNEL_COUNT;
            texel[0] = code;
            texel[1] = code;
            texel[2] = code;
            texel[3] = CODE_MAXIMUM;
        }
    }
    const uint8 meanCode =
        static_cast<uint8>(math::Floor(static_cast<float64>(math::EncodeSrgb(0.5F)) * CODE_MAXIMUM + 0.5));
    const StaticArray<uint8, CHANNEL_COUNT> uniform{{meanCode, meanCode, meanCode, CODE_MAXIMUM}};
    StaticArray<renderer::SceneShaderMaterial, 1> materials{};
    materials[0].BaseColor = {1, 1, 1};
    materials[0].UvTransform = {CHECKER_REPEATS, CHECKER_REPEATS, 0, 0};
    materials[0].Sampler.Filter = rhi::RasterFilter::Linear;
    materials[0].Sampler.U = rhi::RasterAddress::Repeat;
    materials[0].Sampler.V = rhi::RasterAddress::Repeat;
    renderer::DirectLight light;
    light.SourceId = 1;
    light.Kind = renderer::LightKind::Directional;
    light.Direction = {0, 0, -1};
    light.Intensity = 2;
    light.ShouldCastRasterShadows = false;
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    inputs.Lights = {&light, 1};

    const math::Vector3 eyeWorld{0, 0, RECEIVER_HEIGHT_METERS + EYE_HEIGHT_METERS};
    const float32 focal = EYE_HEIGHT_METERS / VIEW_HALF_EXTENT_METERS;
    const float32 depthScale = FAR_PLANE_METERS / (FAR_PLANE_METERS - NEAR_PLANE_METERS);
    renderer::ViewDescription& camera = fixture.Cameras[0];
    camera.OrthographicCamera = false;
    camera.Depth = renderer::DepthConvention::Conventional;
    camera.CameraPosition = eyeWorld;
    camera.CameraForward = {0, 0, -1};
    camera.WorldToClip.Columns[0] = {focal, 0, 0, 0};
    camera.WorldToClip.Columns[1] = {0, focal, 0, 0};
    camera.WorldToClip.Columns[2] = {0, 0, -depthScale, -1};
    camera.WorldToClip.Columns[3] = {0, 0, depthScale * (eyeWorld.Z - NEAR_PLANE_METERS), eyeWorld.Z};

    uint64 revision = 0;
    const auto update = [&]() {
        renderer::RenderGraphReport report;
        ++revision;
        return poll(fixture.Graph.Update({meshes.GetData(), meshes.GetSize()},
                                         {items.GetData(), items.GetSize()},
                                         {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                         revision,
                                         renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                         report,
                                         &inputs,
                                         revision));
    };
    materials[0]
        .ColorTexture = {uniform.GetData(), uniform.GetSize(), CHANNEL_COUNT, 1, 1, rhi::RasterFormat::Rgba8Srgb};
    REQUIRE(update() == rhi::RasterStatus::Ready);
    render();
    const StaticArray<uint8, PIXEL_BYTES> uniformPixels = fixture.Pixels;
    REQUIRE(uniformPixels[GetPixel(WIDTH / 2, HEIGHT / 2)] > 32);

    materials[0].ColorTexture = {checker.GetData(),
                                 checker.GetSize(),
                                 usize{CHECKER_EXTENT} * CHANNEL_COUNT,
                                 CHECKER_EXTENT,
                                 CHECKER_EXTENT,
                                 rhi::RasterFormat::Rgba8Srgb};
    REQUIRE(update() == rhi::RasterStatus::Ready);
    render();
    float32 maximumDifference = 0;
    uint32 differingPixels = 0;
    for (uint32 y = HEIGHT / 2 - PROBE_HALF_HEIGHT_PIXELS; y < HEIGHT / 2 + PROBE_HALF_HEIGHT_PIXELS; ++y)
    {
        for (uint32 x = WIDTH / 2 - PROBE_HALF_WIDTH_PIXELS; x < WIDTH / 2 + PROBE_HALF_WIDTH_PIXELS; ++x)
        {
            const usize pixel = GetPixel(x, y);
            const float32 difference = math::Abs(float32(fixture.Pixels[pixel]) - float32(uniformPixels[pixel]));
            maximumDifference = math::Max(maximumDifference, difference);
            differingPixels += difference > MAX_CHANNEL_DIFFERENCE ? 1 : 0;
        }
    }
    CAPTURE(differingPixels);
    CHECK(maximumDifference <= MAX_CHANNEL_DIFFERENCE);
}

namespace
{
void SetOrbitalCamera(renderer::ViewDescription& inoutCamera,
                      math::Vector3 directionWorld,
                      math::Vector3 upWorld,
                      float32 fieldOfView,
                      bool shouldCenterOnPixel = false)
{
    constexpr float32 TARGET_OFFSET_METRES = 10000;
    constexpr float32 NEAR_METRES = 1000;
    constexpr float32 FAR_METRES = 2000000;
    math::Matrix4 worldToView;
    math::Matrix4 viewToClip;
    REQUIRE(math::TryLookAt(inoutCamera.CameraPosition,
                            inoutCamera.CameraPosition + directionWorld * TARGET_OFFSET_METRES,
                            upWorld,
                            worldToView) == math::MathStatus::Success);
    REQUIRE(math::TryPerspectiveReverseZ(fieldOfView,
                                         static_cast<float32>(WIDTH) / HEIGHT,
                                         NEAR_METRES,
                                         FAR_METRES,
                                         viewToClip) == math::MathStatus::Success);
    for (math::Vector4& column : viewToClip.Columns)
    {
        column.Z = column.W - column.Z;
    }
    if (shouldCenterOnPixel == true)
    {
        // Align the optical axis with the central pixel centre of the even-sized export.
        viewToClip.Columns[2].X = -1.0F / WIDTH;
        viewToClip.Columns[2].Y = 1.0F / HEIGHT;
    }
    inoutCamera.CameraForward = directionWorld;
    inoutCamera.WorldToClip = viewToClip * worldToView;
}

} // namespace

TEST_CASE("Clear-sky resolve matches scene-linear quadrature for sky and opaque transport",
          "[render-graph][atmosphere][gpu][reference-compute]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(PollAtmosphereGraph(
                fixture,
                fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
            rhi::RasterStatus::Ready);
    const StaticArray<math::Vector3, VERTEX_COUNT> normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    fixture.Meshes[0].Geometry.Normals = normals.GetData();
    const uint32 materialIndex = 0;
    fixture.Meshes[0].MaterialBindings = {&materialIndex, 1};
    renderer::SceneShaderMaterial material;
    material.BaseColor = {};
    material.Emission = {0.25F, 0.1F, 0.05F};
    material.ShouldRegisterRasterMeshLight = false;
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&material, 1};
    renderer::ViewDescription& camera = fixture.Cameras[0];
    camera.OrthographicCamera = true;
    const float32 altitude = GENERATE(0.0F, 2000.0F);
    const float32 solarElevation = GENERATE(1.0F, 0.10F, -0.04F);
    camera.CameraPosition = {0, altitude, 0};
    camera.WorldToClip.Columns[3].Y = -altitude;
    fixture.Items[0].MeshToWorld.Translation.Y = altitude;
    uint64 revision = 0;
    uint64 contentRevision = 1;
    const auto update = [&]() {
        renderer::RenderGraphReport report;
        return PollAtmosphereGraph(fixture,
                                   fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                                        {fixture.Items.GetData(), fixture.Items.GetSize()},
                                                        {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                                        ++revision,
                                                        renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                        report,
                                                        &inputs,
                                                        contentRevision));
    };
    REQUIRE(update() == rhi::RasterStatus::Ready);
    ReadAtmosphereFrame(fixture);
    const usize covered = GetPixel(WIDTH / 2, HEIGHT / 2);
    const usize sky = GetPixel(2, 2);
    const StaticArray<uint8, PIXEL_BYTES> original = fixture.Pixels;
    REQUIRE(original[covered] > 0);
    constexpr usize CHANNEL_COUNT = 3;
    camera.CameraForward = {0, 0, -1};
    camera.Atmosphere.IsEnabled = true;
    constexpr float32 SKY_ILLUMINANCE = 4;
    camera.Atmosphere.SolarIlluminance = {SKY_ILLUMINANCE, SKY_ILLUMINANCE, SKY_ILLUMINANCE};
    camera.Atmosphere.DirectionToSunWorld = {std::sqrt(1 - solarElevation * solarElevation), solarElevation, 0};
    REQUIRE(update() == rhi::RasterStatus::Ready);
    ReadAtmosphereFrame(fixture);
    // The RT1 reference evaluates the same tables on the CPU (ResolveAtmosphereEnvironment).
    const float64 heightKilometres = static_cast<float64>(altitude / 1000);
    renderer::internal::AtmosphereSkyTables cpuTables;
    REQUIRE(cpuTables.TryBuild(heightKilometres, static_cast<float64>(solarElevation), false) == true);
    for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        const math::Vector3d sunWorld{static_cast<float64>(camera.Atmosphere.DirectionToSunWorld.X),
                                      static_cast<float64>(solarElevation),
                                      0};
        const float64 expected =
            static_cast<float64>(SKY_ILLUMINANCE) *
            IntegrateEarthSky(static_cast<float64>(altitude) / 1000, {0, 0, -1}, sunWorld, channel).Radiance;
        const float64 actual = static_cast<float64>(fixture.Pixels[sky + channel]) / 255;
        INFO("altitude " << altitude << " solar cosine " << solarElevation << " channel " << channel);
        // RGBA8 fixture disables ToneMap, so only 8-bit quantization and LUT
        // interpolation separate this comparison from scene-linear transport.
        CHECK(math::Abs(actual - expected) <= 1.5 / 255 + 0.08 * expected);
        const math::Vector3d cpuRadiance = cpuTables.SampleSky({0, 0, -1}, {0, 1, 0}, sunWorld);
        const float64 cpu = static_cast<float64>(SKY_ILLUMINANCE) *
                            (channel == 0 ? cpuRadiance.X : (channel == 1 ? cpuRadiance.Y : cpuRadiance.Z));
        // Only 8-bit quantization and the GPU tables' half-precision texels separate the two.
        CHECK(math::Abs(actual - cpu) <= 1.5 / 255 + 0.02 * cpu);
        CHECK(fixture.Pixels[covered + channel] == original[covered + channel]);
    }
    if (altitude == 0 && solarElevation == 1)
    {
        // Zenith looking into the Sun. Independent optical depth integration
        // checks disk attenuation and the conversion from lux to disk radiance.
        constexpr float32 DISK_ILLUMINANCE = 0.02F;
        constexpr float32 DISK_RADIUS = 0.15F;
        constexpr float64 TOP_KILOMETRES = 100;
        constexpr usize STEPS = 4096;
        constexpr StaticArray<float64, CHANNEL_COUNT> LIMB_EXPONENT{{0.397, 0.503, 0.652}};
        camera.CameraForward = {0, 1, 0};
        camera.Atmosphere.SolarIlluminance = {DISK_ILLUMINANCE, DISK_ILLUMINANCE, DISK_ILLUMINANCE};
        camera.Atmosphere.SolarAngularRadiusRadians = DISK_RADIUS;
        REQUIRE(update() == rhi::RasterStatus::Ready);
        ReadAtmosphereFrame(fixture);
        const uint64 drawableRevision = fixture.Graph.GetDrawableSceneRevision();
        const StaticArray<uint8, PIXEL_BYTES> disk = fixture.Pixels;
        camera.Atmosphere.SolarAngularRadiusRadians = 0;
        CHECK(update() == rhi::RasterStatus::InvalidDescription);
        CHECK(fixture.Graph.GetDrawableSceneRevision() == drawableRevision);
        ReadAtmosphereFrame(fixture);
        CHECK(fixture.Pixels == disk);
        camera.Atmosphere.SolarAngularRadiusRadians = DISK_RADIUS;
        const float64 diskSine = std::sin(static_cast<float64>(DISK_RADIUS));
        const float64 step = TOP_KILOMETRES / STEPS;
        for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
        {
            float64 depth = 0;
            for (usize sample = 0; sample < STEPS; ++sample)
            {
                depth += ComputeEarthExtinction((static_cast<float64>(sample) + 0.5) * step, channel) * step;
            }
            const float64 peakRadiance = (1 + 0.5 * LIMB_EXPONENT[channel]) / (math::kPi * diskSine * diskSine);
            const float64 expected =
                static_cast<float64>(DISK_ILLUMINANCE) *
                (std::exp(-depth) * peakRadiance + IntegrateEarthSky(0, {0, 1, 0}, {0, 1, 0}, channel).Radiance);
            const float64 actual = static_cast<float64>(fixture.Pixels[sky + channel]) / 255;
            CHECK(math::Abs(actual - expected) <= 2.0 / 255);
        }
    }
    if (altitude == 0 && solarElevation == 1)
    {
        camera.CameraForward = {0, 0, -1};
        camera.Atmosphere.SolarIlluminance = {SKY_ILLUMINANCE, SKY_ILLUMINANCE, SKY_ILLUMINANCE};
        REQUIRE(update() == rhi::RasterStatus::Ready);
        ReadAtmosphereFrame(fixture);
        const uint8 singleScatteringBlue = fixture.Pixels[sky + 2];
        camera.Atmosphere.ShouldLightScene = true;
        REQUIRE(update() == rhi::RasterStatus::Ready);
        ReadAtmosphereFrame(fixture);
        constexpr usize BLUE_CHANNEL = 2;
        const float64 higherOrderBlue = static_cast<float64>(SKY_ILLUMINANCE) * IntegrateEarthBlueHigherOrders();
        const float64 observedBlue =
            (static_cast<float64>(fixture.Pixels[sky + BLUE_CHANNEL]) - singleScatteringBlue) / 255;
        CHECK(observedBlue > 0);
        CHECK(math::Abs(observedBlue - higherOrderBlue) <= 2.0 / 255 + 0.20 * higherOrderBlue);
        // The reference's multiple-scattered sky matches the raster's.
        renderer::internal::AtmosphereSkyTables cpuMultiple;
        REQUIRE(cpuMultiple.TryBuild(0, 1, true) == true);
        const math::Vector3d cpuSky = cpuMultiple.SampleSky({0, 0, -1}, {0, 1, 0}, {0, 1, 0});
        const StaticArray<float64, CHANNEL_COUNT> cpuChannels{{cpuSky.X, cpuSky.Y, cpuSky.Z}};
        for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
        {
            const float64 expectedSky = static_cast<float64>(SKY_ILLUMINANCE) * cpuChannels[channel];
            CHECK(math::Abs(static_cast<float64>(fixture.Pixels[sky + channel]) / 255 - expectedSky) <=
                  1.5 / 255 + 0.02 * expectedSky);
        }

        camera.Atmosphere.SolarIlluminance = {};
        constexpr float32 CAMERA_HEIGHT_METRES = 2;
        camera.CameraPosition.Y = CAMERA_HEIGHT_METRES;
        camera.WorldToClip.Columns[3].Y = -CAMERA_HEIGHT_METRES;
        fixture.Items[0].MeshToWorld.Translation.Y = CAMERA_HEIGHT_METRES;
        ++contentRevision;
        REQUIRE(update() == rhi::RasterStatus::Ready);
        ReadAtmosphereFrame(fixture);
        const StaticArray<uint8, PIXEL_BYTES> nearPixels = fixture.Pixels;
        CHECK(nearPixels[covered] == original[covered]);

        constexpr float32 DISTANCE_METRES = 5000;
        constexpr float32 FAR_PLANE_METRES = 10000;
        camera.CameraPosition.Z = DISTANCE_METRES + fixture.Positions[0].Z;
        camera.WorldToClip.Columns[2].Z = -1 / FAR_PLANE_METRES;
        camera.WorldToClip.Columns[3].Z = camera.CameraPosition.Z / FAR_PLANE_METRES;
        REQUIRE(update() == rhi::RasterStatus::Ready);
        ReadAtmosphereFrame(fixture);
        const StaticArray<uint8, PIXEL_BYTES> farPixels = fixture.Pixels;
        for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
        {
            constexpr usize STEPS = 1024;
            constexpr float64 GROUND = 6360;
            constexpr float64 STEP = static_cast<float64>(DISTANCE_METRES) / (1000 * STEPS);
            float64 depth = 0;
            for (usize sample = 0; sample < STEPS; ++sample)
            {
                const float64 distance = (static_cast<float64>(sample) + 0.5) * STEP;
                constexpr float64 RADIUS = GROUND + static_cast<float64>(CAMERA_HEIGHT_METRES) / 1000;
                const float64 height = std::sqrt(RADIUS * RADIUS + distance * distance) - GROUND;
                depth += ComputeEarthExtinction(height, channel) * STEP;
            }
            CHECK(farPixels[covered + channel] < nearPixels[covered + channel]);
            CHECK(math::Abs(farPixels[covered + channel] - nearPixels[covered + channel] * std::exp(-depth)) <= 1.5);
        }

        camera.Atmosphere.SolarIlluminance = {SKY_ILLUMINANCE, SKY_ILLUMINANCE, SKY_ILLUMINANCE};
        REQUIRE(update() == rhi::RasterStatus::Ready);
        ReadAtmosphereFrame(fixture);
        CHECK(fixture.Pixels[covered + 2] > farPixels[covered + 2]);

        // A second directional light comes before the Sun in the packed list.
        // Linking the Sun attenuates only its contribution and leaves the disk-free sky unchanged.
        constexpr usize LIGHT_COUNT = 2;
        constexpr float32 SOLAR_COSINE = 0.1F;
        constexpr float32 DIRECT_ILLUMINANCE = 0.4F;
        const math::Vector3 sunWorld{0, SOLAR_COSINE, std::sqrt(1 - SOLAR_COSINE * SOLAR_COSINE)};
        StaticArray<renderer::DirectLight, LIGHT_COUNT> lights{};
        lights[0].SourceId = 1;
        lights[0].Color = {0, 1, 0};
        lights[0].Intensity = DIRECT_ILLUMINANCE;
        lights[1].SourceId = 2;
        lights[1].Direction = -sunWorld;
        lights[1].Color = {1, 0, 0};
        lights[1].Intensity = DIRECT_ILLUMINANCE;
        inputs.Lights = {lights.GetData(), lights.GetSize()};
        camera.Atmosphere.DirectionToSunWorld = sunWorld;
        camera.Atmosphere.SolarIlluminance = {DIRECT_ILLUMINANCE, 0, 0};
        camera.Atmosphere.SunSourceId = 0;
        material.BaseColor = {0.5F, 0.5F, 0.5F};
        material.Emission = {};
        ++contentRevision;
        REQUIRE(update() == rhi::RasterStatus::Ready);
        ReadAtmosphereFrame(fixture);
        const StaticArray<uint8, PIXEL_BYTES> unlinked = fixture.Pixels;
        camera.Atmosphere.SunSourceId = lights[1].SourceId;
        REQUIRE(update() == rhi::RasterStatus::Ready);
        ReadAtmosphereFrame(fixture);
        CHECK(fixture.Pixels[covered] < unlinked[covered]);
        CHECK(fixture.Pixels[covered + 1] == unlinked[covered + 1]);
        CHECK(fixture.Pixels[covered + 1] > 0);
        CHECK(fixture.Pixels[sky] == unlinked[sky]);
        const StaticArray<uint8, PIXEL_BYTES> atmosphericLighting = fixture.Pixels;
        inputs.Environment = {1, 1, 1};
        ++contentRevision;
        REQUIRE(update() == rhi::RasterStatus::Ready);
        ReadAtmosphereFrame(fixture);
        CHECK(fixture.Pixels == atmosphericLighting);
        inputs.Lights = {};
        ++contentRevision;
    }
    camera.Atmosphere.IsEnabled = false;
    REQUIRE(update() == rhi::RasterStatus::Ready);
    ReadAtmosphereFrame(fixture);
    CHECK(fixture.Pixels[sky] == original[sky]);
    CHECK(fixture.Pixels[sky + 1] == original[sky + 1]);
    CHECK(fixture.Pixels[sky + 2] == original[sky + 2]);
}

TEST_CASE("default deferred sky visibility hides a blocked sunset horizon from Atmosphere reflections",
          "[sky-visibility][atmosphere][gpu]")
{
    constexpr usize QUAD_VERTEX_COUNT = 4;
    constexpr usize QUAD_INDEX_COUNT = 6;
    constexpr usize SCENE_COUNT = 2;
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(PollAtmosphereGraph(
                fixture,
                fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
            rhi::RasterStatus::Ready);
    // A glossy wall facing the orthographic camera (+Z), whose reflections leave horizontally towards +Z, where the
    // Sun sets. A low panel 1.1 m in front of it, beyond the camera's depth range, hides the horizon up to about 24
    // degrees but leaves the dim sky above open, as an arcade's opposite wall does.
    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> wallPositions{
        {{-1, -1, .5F}, {1, -1, .5F}, {1, 1, .5F}, {-1, 1, .5F}}};
    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> wallNormals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> panelPositions{
        {{-3, -.5F, 1.6F}, {3, -.5F, 1.6F}, {3, .5F, 1.6F}, {-3, .5F, 1.6F}}};
    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> panelNormals{{{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}}};
    const StaticArray<uint32, QUAD_INDEX_COUNT> quadIndices{{0, 1, 2, 0, 2, 3}};
    StaticArray<renderer::RenderGraphMesh, SCENE_COUNT> meshes{{fixture.Meshes[0], fixture.Meshes[0]}};
    meshes[0].Geometry = {wallPositions.GetData(), wallPositions.GetSize(), quadIndices.GetData(), QUAD_INDEX_COUNT};
    meshes[0].Geometry.Normals = wallNormals.GetData();
    meshes[1].Geometry = {panelPositions.GetData(), panelPositions.GetSize(), quadIndices.GetData(), QUAD_INDEX_COUNT};
    meshes[1].Geometry.Normals = panelNormals.GetData();
    const uint32 materialIndex = 0;
    meshes[0].MaterialBindings = {&materialIndex, 1};
    meshes[1].MaterialBindings = {&materialIndex, 1};
    StaticArray<renderer::RenderGraphItem, SCENE_COUNT> items{{fixture.Items[0], fixture.Items[0]}};
    items[1].SourceId = 2;
    items[1].MeshIndex = 1;
    renderer::SceneShaderMaterial material;
    material.BaseColor = {1, 1, 1};
    material.Metallic = 1;
    material.Roughness = .3F;
    material.ShouldRegisterRasterMeshLight = false;
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&material, 1};
    renderer::ViewDescription& camera = fixture.Cameras[0];
    camera.OrthographicCamera = true;
    camera.CameraPosition = {0, 0, 1};
    camera.CameraForward = {0, 0, -1};
    camera.Atmosphere.IsEnabled = true;
    camera.Atmosphere.ShouldLightScene = true;
    constexpr float32 SKY_ILLUMINANCE = 4;
    constexpr float32 SOLAR_ELEVATION_SINE = .05F;
    camera.Atmosphere.SolarIlluminance = {SKY_ILLUMINANCE, SKY_ILLUMINANCE, SKY_ILLUMINANCE};
    camera.Atmosphere.DirectionToSunWorld = {0,
                                             SOLAR_ELEVATION_SINE,
                                             std::sqrt(1 - SOLAR_ELEVATION_SINE * SOLAR_ELEVATION_SINE)};
    camera.LightingView = renderer::DeferredLightingView::SkySpecular;
    uint64 revision = 0;
    const auto update = [&](usize itemCount) {
        renderer::RenderGraphReport report;
        ++revision;
        return PollAtmosphereGraph(fixture,
                                   fixture.Graph.Update({meshes.GetData(), meshes.GetSize()},
                                                        {items.GetData(), itemCount},
                                                        {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                                        revision,
                                                        renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                        report,
                                                        &inputs,
                                                        revision));
    };
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    const auto readRed = [&]() {
        // The first frame draws the sky visibility maps; the second reads them.
        ReadAtmosphereFrame(fixture);
        ReadAtmosphereFrame(fixture);
        return static_cast<float32>(fixture.Pixels[center]);
    };
    REQUIRE(update(1) == rhi::RasterStatus::Ready);
    const float32 open = readRed();
    REQUIRE(open > 32);
    REQUIRE(update(SCENE_COUNT) == rhi::RasterStatus::Ready);
    const float32 blocked = readRed();
    CAPTURE(open, blocked);
    // The panel hides the bright horizon the reflection points at. Weighting the lobe's visibility by the sky's
    // brightness removes most of it (65 to 6 codes on Metal); counting the open dim sky above as much as the
    // horizon kept a quarter (65 to 16).
    constexpr float32 BLOCKED_FRACTION_LIMIT = .15F;
    CHECK(blocked < open * BLOCKED_FRACTION_LIMIT);
}

TEST_CASE("Orbital limb single scattering matches independent Earth shell quadrature",
          "[render-graph][atmosphere][gpu][reference-compute][orbital-limb]")
{
    GraphGpuFixture fixture;
    rhi::DeviceInfo deviceInfo;
    REQUIRE(rhi::GetDeviceInfo(fixture.Device, deviceInfo) == rhi::DeviceStatus::Ready);
    INFO("Native backend " << static_cast<uint32>(deviceInfo.Startup.SelectedBackend) << ", adapter "
                           << deviceInfo.Adapter.Name.GetData());
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(PollAtmosphereGraph(
                fixture,
                fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
            rhi::RasterStatus::Ready);
    renderer::ViewDescription& camera = fixture.Cameras[0];
    camera.CameraPosition = {0, ORBIT_HEIGHT_METRES, 0};
    camera.OrthographicCamera = false;
    camera.Atmosphere.IsEnabled = true;
    camera.Atmosphere.Celestial.IsEnabled = true;
    camera.Atmosphere.ShouldLightScene = false;
    const renderer::SceneShaderInputs inputs;
    uint64 revision = 0;
    constexpr usize VIEW_STEPS = 1536;
    constexpr usize SUN_STEPS = 384;
    constexpr usize RGB_CHANNELS = 3;
    constexpr float64 READBACK_STEP = 1.0 / 255;
    const usize pixel = GetPixel(WIDTH / 2, HEIGHT / 2);
    for (const bool isTwilight : {false, true})
    {
        const math::Vector3 sunWorld = GetOrbitalSunDirection(isTwilight);
        const math::Vector3d solarVectorWorld{static_cast<float64>(sunWorld.X),
                                              static_cast<float64>(sunWorld.Y),
                                              static_cast<float64>(sunWorld.Z)};
        const math::Vector3d sunDirectionWorld = solarVectorWorld * (1 / math::Length(solarVectorWorld));
        camera.Atmosphere.DirectionToSunWorld = sunWorld;
        for (const OrbitalProbe& probe : ORBITAL_PROBES)
        {
            const math::Vector3d rayWorld = GetOrbitalProbeDirection(probe.TangentAltitudeKilometres);
            SetOrbitalCamera(camera,
                             {0, static_cast<float32>(rayWorld.Y), static_cast<float32>(rayWorld.Z)},
                             {0, 1, 0},
                             ORBIT_VERTICAL_FOV_RADIANS,
                             true);
            const float32 illuminance = isTwilight == true ? probe.TwilightIlluminance : probe.DayIlluminance;
            camera.Atmosphere.SolarIlluminance = {illuminance, illuminance, illuminance};
            renderer::RenderGraphReport report;
            REQUIRE(PollAtmosphereGraph(fixture,
                                        fixture.Graph.Update({},
                                                             {},
                                                             {&camera, 1},
                                                             ++revision,
                                                             renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                             report,
                                                             &inputs)) == rhi::RasterStatus::Ready);
            ReadAtmosphereFrame(fixture);
            for (usize channel = 0; channel < RGB_CHANNELS; ++channel)
            {
                const EarthSkyIntegral integral = IntegrateEarthSky(ORBIT_ALTITUDE_KILOMETRES,
                                                                    rayWorld,
                                                                    sunDirectionWorld,
                                                                    channel,
                                                                    VIEW_STEPS,
                                                                    SUN_STEPS);
                const float64 expected = integral.Radiance * static_cast<float64>(illuminance);
                const float64 actual = fixture.Pixels[pixel + channel] * READBACK_STEP;
                const float64 absoluteError = math::Abs(actual - expected);
                CAPTURE(isTwilight, probe.TangentAltitudeKilometres, channel, expected, actual, absoluteError);
                if (isTwilight == true && probe.Ray == OrbitalRay::AboveGroundTangent)
                {
                    REQUIRE(integral.ShadowSamples > 0);
                    REQUIRE(integral.ShadowSamples < VIEW_STEPS);
                }
                if (probe.Ray == OrbitalRay::Miss || (isTwilight == true && probe.Ray == OrbitalRay::GroundHit))
                {
                    REQUIRE(expected == 0);
                    INFO("Relative error undefined for zero reference radiance");
                    CHECK(actual == 0);
                }
                else
                {
                    REQUIRE(expected > 0);
                    const float64 relativeError = absoluteError / expected;
                    CAPTURE(relativeError);
                    // Unmapped linear UNORM8 export, nearest code (half a code of quantization).
                    // Allow the orbital shader's 64-step integration and transmittance LUT interpolation.
                    CHECK(expected < 1);
                    CHECK(absoluteError <= 0.5 * READBACK_STEP + 0.04 * expected);
                }
            }
        }
    }
}

TEST_CASE("Celestial boundary composes through ground and orbital atmosphere rays",
          "[render-graph][atmosphere][gpu][reference-compute]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(PollAtmosphereGraph(
                fixture,
                fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
            rhi::RasterStatus::Ready);
    constexpr float32 LUNAR_PEAK = 0.5F;
    constexpr float32 TEST_DISK_RADIUS = 0.15F;
    constexpr usize BLUE_CHANNEL = 2;
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    renderer::ViewDescription& camera = fixture.Cameras[0];
    camera.Atmosphere.IsEnabled = true;
    camera.Atmosphere.Celestial.IsEnabled = true;
    camera.Atmosphere.Celestial.LunarPeakRadiance = {LUNAR_PEAK, LUNAR_PEAK, LUNAR_PEAK};
    camera.Atmosphere.Celestial.LunarAngularRadiusRadians = TEST_DISK_RADIUS;
    const renderer::SceneShaderInputs inputs;
    uint64 revision = 0;
    const auto render = [&]() {
        renderer::RenderGraphReport report;
        INFO("view revision " << (revision + 1) << ", height " << camera.CameraPosition.Y);
        REQUIRE(PollAtmosphereGraph(fixture,
                                    fixture.Graph.Update({},
                                                         {},
                                                         {&camera, 1},
                                                         ++revision,
                                                         renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                         report,
                                                         &inputs)) == rhi::RasterStatus::Ready);
        ReadAtmosphereFrame(fixture);
    };
    const auto pointAtMoon = [&](math::Vector3 directionWorld) {
        camera.CameraForward = directionWorld;
        camera.Atmosphere.DirectionToSunWorld = -directionWorld;
        camera.Atmosphere.Celestial.DirectionToMoonWorld = directionWorld;
    };
    const math::Matrix4 worldToOrthographicClip = camera.WorldToClip;
    camera.OrthographicCamera = true;
    pointAtMoon({0, 1, 0});
    render();
    const uint8 zenith = fixture.Pixels[center + BLUE_CHANNEL];
    CHECK(zenith > 0);
    CHECK(static_cast<float32>(zenith) / 255 < LUNAR_PEAK);

    camera.Atmosphere.DirectionToSunWorld = {0, 1, 0};
    render();
    CHECK(fixture.Pixels[center + BLUE_CHANNEL] == 0);
    camera.Atmosphere.Celestial.DirectionToSunAtMoonWorld = {0, -1, 0};
    render();
    CHECK(fixture.Pixels[center + BLUE_CHANNEL] == zenith);
    camera.Atmosphere.Celestial.DirectionToSunAtMoonWorld = {};
    pointAtMoon({1, 0, 0});
    render();
    CHECK(fixture.Pixels[center + BLUE_CHANNEL] < zenith);
    pointAtMoon({0, -1, 0});
    render();
    CHECK(fixture.Pixels[center + BLUE_CHANNEL] == 0);

    camera.OrthographicCamera = false;
    constexpr float64 GROUND_KILOMETRES = EARTH_GROUND_RADIUS_KILOMETRES;
    constexpr float64 TOP_KILOMETRES = EARTH_TOP_RADIUS_KILOMETRES;
    constexpr float64 ORBIT_RADIUS_KILOMETRES = GROUND_KILOMETRES + static_cast<float64>(ORBIT_HEIGHT_METRES) / 1000;
    camera.CameraPosition = {0, ORBIT_HEIGHT_METRES, 0};
    const auto makeOrbitRay = [&](float64 impactKilometres, float32 fieldOfView) {
        const math::Vector3 directionWorld{
            static_cast<float32>(impactKilometres / ORBIT_RADIUS_KILOMETRES),
            -static_cast<float32>(std::sqrt(1 - impactKilometres * impactKilometres /
                                                    (ORBIT_RADIUS_KILOMETRES * ORBIT_RADIUS_KILOMETRES))),
            0};
        SetOrbitalCamera(camera, directionWorld, {0, 0, 1}, fieldOfView);
        pointAtMoon(directionWorld);
    };
    constexpr float32 ORACLE_FIELD_OF_VIEW = 0.01F;
    makeOrbitRay(TOP_KILOMETRES + 20, ORACLE_FIELD_OF_VIEW);
    render();
    CHECK(math::Abs(static_cast<float64>(fixture.Pixels[center + BLUE_CHANNEL]) / 255 -
                    static_cast<float64>(LUNAR_PEAK)) <= 1.0 / 255);
    constexpr float64 LIMB_IMPACT_KILOMETRES = GROUND_KILOMETRES + 15;
    makeOrbitRay(LIMB_IMPACT_KILOMETRES, ORACLE_FIELD_OF_VIEW);
    render();
    constexpr usize STEPS = 2048;
    const float64 halfDistance =
        std::sqrt(TOP_KILOMETRES * TOP_KILOMETRES - LIMB_IMPACT_KILOMETRES * LIMB_IMPACT_KILOMETRES);
    const float64 step = 2 * halfDistance / STEPS;
    float64 depth = 0;
    for (usize i = 0; i < STEPS; ++i)
    {
        const float64 distance = (static_cast<float64>(i) + 0.5) * step - halfDistance;
        const float64 altitude =
            std::sqrt(LIMB_IMPACT_KILOMETRES * LIMB_IMPACT_KILOMETRES + distance * distance) - GROUND_KILOMETRES;
        depth += ComputeEarthExtinction(altitude, BLUE_CHANNEL) * step;
    }
    const float64 expected = static_cast<float64>(LUNAR_PEAK) * std::exp(-depth);
    const float64 observed = static_cast<float64>(fixture.Pixels[center + BLUE_CHANNEL]) / 255;
    CHECK(observed > 0);
    CHECK(math::Abs(observed - expected) <= 2.0 / 255 + 0.10 * expected);
    camera.Atmosphere.Celestial.LunarAngularRadiusRadians = 0;
    camera.Atmosphere.DirectionToSunWorld = {0, 1, 0};
    camera.Atmosphere.SolarIlluminance = {1, 1, 1};
    render();
    CHECK(fixture.Pixels[center + BLUE_CHANNEL] > 0);

    camera.Atmosphere.SolarIlluminance = {};
    camera.Atmosphere.Celestial.LunarAngularRadiusRadians = TEST_DISK_RADIUS;
    makeOrbitRay(GROUND_KILOMETRES - 20, ORACLE_FIELD_OF_VIEW);
    render();
    CHECK(fixture.Pixels[center + BLUE_CHANNEL] == 0);

    camera.Atmosphere.Celestial.LunarAngularRadiusRadians = 0;
    makeOrbitRay(TOP_KILOMETRES + 100, 0.5F);
    render();
    const StaticArray<uint8, PIXEL_BYTES> starless = fixture.Pixels;
    camera.Atmosphere.Celestial.StarRadiance = 1;
    render();
    const StaticArray<uint8, PIXEL_BYTES> stars = fixture.Pixels;
    CHECK(stars != starless);
    render();
    CHECK(fixture.Pixels == stars);

    camera.Atmosphere.Celestial.StarRadiance = 0;
    camera.OrthographicCamera = true;
    camera.CameraPosition = {};
    camera.WorldToClip = worldToOrthographicClip;
    camera.CameraForward = {0, 1, 0};
    camera.Atmosphere.DirectionToSunWorld = {0, -1, 0};
    camera.Atmosphere.Celestial.GalacticNorthWorld = {1, 0, 0};
    camera.Atmosphere.Celestial.GalacticCenterWorld = {0, 1, 0};
    camera.Atmosphere.Celestial.MilkyWayRadiance = 0.045F;
    render();
    const uint8 milkyWay = fixture.Pixels[center + BLUE_CHANNEL];
    CHECK(milkyWay > 0);

    // Pack a planet after a disabled slot and composite it over the galactic band.
    constexpr float32 TEST_BLUE_ILLUMINANCE_LUX = 2e-7F;
    camera.Atmosphere.Celestial.Planets[1] =
    {
        .DirectionWorld = {0, 1, 0},
        .Illuminance = {0, 0, TEST_BLUE_ILLUMINANCE_LUX},
    };
    render();
    CHECK(fixture.Pixels[center + BLUE_CHANNEL] > milkyWay);

    // A new Moon masks both the planet and the Milky Way.
    camera.Atmosphere.Celestial.DirectionToMoonWorld = camera.CameraForward;
    camera.Atmosphere.DirectionToSunWorld = {1, 1, 0};
    camera.Atmosphere.Celestial.LunarAngularRadiusRadians = TEST_DISK_RADIUS;
    render();
    CHECK(fixture.Pixels[center + BLUE_CHANNEL] == 0);

    camera.Atmosphere.Celestial.LunarAngularRadiusRadians = 0;
    camera.Atmosphere.DirectionToSunWorld = {0, -1, 0};
    camera.Atmosphere.Celestial.GalacticNorthWorld = camera.CameraForward;
    camera.Atmosphere.Celestial.GalacticCenterWorld = {1, 0, 0};
    camera.Atmosphere.Celestial.Planets[1].DirectionWorld = {1, 0, 0};
    render();
    CHECK(fixture.Pixels[center + BLUE_CHANNEL] == 0);

    camera.CameraForward = {0, -1, 0};
    camera.Atmosphere.DirectionToSunWorld = {0, 1, 0};
    camera.Atmosphere.Celestial.GalacticNorthWorld = {1, 0, 0};
    camera.Atmosphere.Celestial.GalacticCenterWorld = camera.CameraForward;
    camera.Atmosphere.Celestial.Planets[1].DirectionWorld = camera.CameraForward;
    render();
    CHECK(fixture.Pixels[center + BLUE_CHANNEL] == 0);
}

TEST_CASE("The resolved solar disk darkens toward its limb and preserves authored illuminance",
          "[render-graph][atmosphere][gpu][reference-compute]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(PollAtmosphereGraph(
                fixture,
                fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
            rhi::RasterStatus::Ready);

    constexpr float32 NEAR_METRES = 1000;
    constexpr float32 FAR_METRES = 2000000;
    constexpr float64 AVERAGE_RADIANCE = 0.4;
    constexpr float64 WIDE_ANGULAR_RADIUS = 0.5;
    const float64 radius =
        GENERATE_COPY(static_cast<float64>(renderer::AtmosphereDescription::DEFAULT_SOLAR_RADIUS_RADIANS),
                      WIDE_ANGULAR_RADIUS);
    const float64 diskSine = std::sin(radius);
    const float64 authoredIlluminance = AVERAGE_RADIANCE * math::kPi * diskSine * diskSine;
    const float32 illuminance = static_cast<float32>(authoredIlluminance);
    // Point away from the planet. Every ray sees the solar boundary in vacuum.
    renderer::ViewDescription& camera = fixture.Cameras[0];
    camera.CameraPosition = {0, ORBIT_HEIGHT_METRES, 0};
    camera.Atmosphere.IsEnabled = true;
    camera.Atmosphere.Celestial.IsEnabled = true;
    camera.Atmosphere.DirectionToSunWorld = {0, 1, 0};
    camera.Atmosphere.SolarIlluminance = {illuminance, illuminance, illuminance};
    camera.Atmosphere.SolarAngularRadiusRadians = static_cast<float32>(radius);
    camera.OrthographicCamera = false;
    const float64 halfHeight = 2 * std::tan(radius);
    math::Matrix4 worldToView;
    math::Matrix4 viewToClip;
    REQUIRE(math::TryLookAt(camera.CameraPosition,
                            camera.CameraPosition + math::Vector3{0, 1, 0},
                            {0, 0, 1},
                            worldToView) == math::MathStatus::Success);
    REQUIRE(math::TryPerspectiveReverseZ(static_cast<float32>(2 * std::atan(halfHeight)),
                                         static_cast<float32>(WIDTH) / HEIGHT,
                                         NEAR_METRES,
                                         FAR_METRES,
                                         viewToClip) == math::MathStatus::Success);
    for (math::Vector4& column : viewToClip.Columns)
    {
        column.Z = column.W - column.Z;
    }
    camera.WorldToClip = viewToClip * worldToView;
    const renderer::SceneShaderInputs inputs;
    renderer::RenderGraphReport report;
    REQUIRE(PollAtmosphereGraph(fixture,
                                fixture.Graph.Update({},
                                                     {},
                                                     {&camera, 1},
                                                     1,
                                                     renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                     report,
                                                     &inputs)) == rhi::RasterStatus::Ready);
    ReadAtmosphereFrame(fixture);

    constexpr usize CHANNEL_COUNT = 3;
    constexpr usize NEAR_LIMB_OFFSET_PIXELS = 13;
    constexpr float64 UNORM_SCALE = 255;
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    const usize nearLimb = GetPixel(WIDTH / 2 + NEAR_LIMB_OFFSET_PIXELS, HEIGHT / 2);
    const usize outside = GetPixel(WIDTH - 1, HEIGHT / 2);
    constexpr StaticArray<float64, CHANNEL_COUNT> LIMB_EXPONENT{{0.397, 0.503, 0.652}};
    const float64 pixelWidth = 2 * halfHeight / HEIGHT;
    const float64 limbX = (static_cast<float64>(NEAR_LIMB_OFFSET_PIXELS) + 0.5) * pixelWidth;
    const float64 limbY = 0.5 * pixelWidth;
    const float64 limbTangentSquared = limbX * limbX + limbY * limbY;
    const float64 limbSineSquared = limbTangentSquared / (1 + limbTangentSquared);
    const float64 limbMu = std::sqrt(1 - limbSineSquared / (diskSine * diskSine));
    for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        INFO("radius " << radius << " channel " << channel);
        CHECK(fixture.Pixels[center + channel] > fixture.Pixels[nearLimb + channel]);
        CHECK(fixture.Pixels[outside + channel] == 0);
        const float64 centerRadiance = fixture.Pixels[center + channel] / UNORM_SCALE;
        const float64 expectedCenter = AVERAGE_RADIANCE * (1 + 0.5 * LIMB_EXPONENT[channel]);
        CHECK(math::Abs(centerRadiance - expectedCenter) <= 1 / UNORM_SCALE);
        const float64 limbRadiance = fixture.Pixels[nearLimb + channel] / UNORM_SCALE;
        const float64 expectedLimb = expectedCenter * std::pow(limbMu, LIMB_EXPONENT[channel]);
        CHECK(math::Abs(limbRadiance - expectedLimb) <= 1 / UNORM_SCALE);

        // Integrate the readback over the projection plane. Each pixel's
        // solid angle times its cosine to the Sun gives the projected measure.
        float64 measuredIlluminance = 0;
        for (usize row = 0; row < HEIGHT; ++row)
        {
            const float64 y = (static_cast<float64>(row) + 0.5 - HEIGHT * 0.5) * pixelWidth;
            for (usize column = 0; column < WIDTH; ++column)
            {
                const float64 x = (static_cast<float64>(column) + 0.5 - WIDTH * 0.5) * pixelWidth;
                const float64 distanceSquared = 1 + x * x + y * y;
                const float64 radiance = fixture.Pixels[GetPixel(column, row) + channel] / UNORM_SCALE;
                measuredIlluminance += radiance * pixelWidth * pixelWidth / (distanceSquared * distanceSquared);
            }
        }
        // Allow discretization at the filtered limb and RGBA8 rounding.
        constexpr float64 ILLUMINANCE_RELATIVE_TOLERANCE = 0.025;
        CHECK(math::Abs(measuredIlluminance / authoredIlluminance - 1) <= ILLUMINANCE_RELATIVE_TOLERANCE);
    }
}

namespace
{
// Cloud layer fixtures (CloudLayerDescription). The Atmosphere anchors at the world origin with +Y up and +X as the
// layer's X axis, so the layer frame is the world frame.
constexpr float64 CLOUD_METRES_PER_KILOMETRE = 1000;
constexpr float32 CLOUD_BASE_METRES = 1000;
constexpr float32 CLOUD_THICKNESS_METRES = 500;
constexpr float32 CLOUD_SIZE_METRES = 40000;
constexpr float32 CLOUD_HALF_SIZE_METRES = CLOUD_SIZE_METRES / 2;
constexpr float32 CLOUD_CAMERA_HEIGHT_METRES = 2;

/// Aim the central pixel's centre along directionWorld, as SetOrbitalCamera does, with a near plane short enough
/// for surfaces a few hundred metres away and a far plane beyond the layer.
void SetCloudCamera(renderer::ViewDescription& inoutCamera, math::Vector3 directionWorld, math::Vector3 upWorld)
{
    constexpr float32 TARGET_OFFSET_METRES = 1000;
    constexpr float32 NEAR_METRES = 1;
    constexpr float32 FAR_METRES = 200000;
    constexpr float32 FIELD_OF_VIEW_RADIANS = 0.05F;
    math::Matrix4 worldToView;
    math::Matrix4 viewToClip;
    REQUIRE(math::TryLookAt(inoutCamera.CameraPosition,
                            inoutCamera.CameraPosition + directionWorld * TARGET_OFFSET_METRES,
                            upWorld,
                            worldToView) == math::MathStatus::Success);
    REQUIRE(math::TryPerspectiveReverseZ(FIELD_OF_VIEW_RADIANS,
                                         static_cast<float32>(WIDTH) / HEIGHT,
                                         NEAR_METRES,
                                         FAR_METRES,
                                         viewToClip) == math::MathStatus::Success);
    for (math::Vector4& column : viewToClip.Columns)
    {
        column.Z = column.W - column.Z;
    }
    viewToClip.Columns[2].X = -1.0F / WIDTH;
    viewToClip.Columns[2].Y = 1.0F / HEIGHT;
    inoutCamera.OrthographicCamera = false;
    inoutCamera.Depth = renderer::DepthConvention::Conventional;
    inoutCamera.InfiniteFar = false;
    inoutCamera.CameraForward = directionWorld;
    inoutCamera.WorldToClip = viewToClip * worldToView;
}

float64 GetCloudAxis(const math::Vector3d& value, usize axis)
{
    return axis == 0 ? value.X : (axis == 1 ? value.Y : value.Z);
}

/// Distances in metres along a reference ray.
struct CloudInterval final
{
    float64 Near = 0;
    float64 Far = 0;
};

/// Clip origin + t * direction, t in [0, maximumMetres], to the box [minimum, maximum] in metres.
bool ClipCloudReference(const math::Vector3d& originWorld,
                        const math::Vector3d& directionWorld,
                        const math::Vector3d& minimumWorld,
                        const math::Vector3d& maximumWorld,
                        float64 maximumMetres,
                        CloudInterval& outInterval)
{
    constexpr usize AXIS_COUNT = 3;
    constexpr float64 PARALLEL_EPSILON = 1e-12;
    float64 near = 0;
    float64 far = maximumMetres;
    for (usize axis = 0; axis < AXIS_COUNT; ++axis)
    {
        const float64 origin = GetCloudAxis(originWorld, axis);
        const float64 direction = GetCloudAxis(directionWorld, axis);
        const float64 minimum = GetCloudAxis(minimumWorld, axis);
        const float64 maximum = GetCloudAxis(maximumWorld, axis);
        if (math::Abs(direction) < PARALLEL_EPSILON)
        {
            if (origin < minimum || origin > maximum)
            {
                return false;
            }
            continue;
        }
        const float64 first = (minimum - origin) / direction;
        const float64 second = (maximum - origin) / direction;
        near = math::Max(near, math::Min(first, second));
        far = math::Min(far, math::Max(first, second));
    }
    outInterval = {near, far};
    return near < far;
}

struct CloudReference final
{
    /// Radiance scattered toward the camera at the layer's entry, per unit top-of-atmosphere illuminance.
    float64 Radiance = 0;
    float64 Transmittance = 1;
    float64 EntryMetres = 0;
    bool IsHit = false;
};

/// Independent binary64 single scattering of the Sun through a homogeneous slab with sharp sides: a dense midpoint
/// rule along the view ray, the slab's exact path toward the Sun, and the air's transmittance and the Earth's shadow
/// from the clear-sky quadrature at each point.
CloudReference IntegrateCloudReference(const renderer::CloudLayerDescription& clouds,
                                       math::Vector3d eyeWorld,
                                       math::Vector3d directionWorld,
                                       math::Vector3d sunWorld,
                                       usize channel)
{
    constexpr usize VIEW_STEPS = 4096;
    constexpr usize SOLAR_STEPS = 256;
    constexpr float64 UNBOUNDED_METRES = 1e7;
    const float64 halfX = static_cast<float64>(clouds.SizeXMetres) * 0.5;
    const float64 halfZ = static_cast<float64>(clouds.SizeZMetres) * 0.5;
    const float64 base = static_cast<float64>(clouds.BaseHeightMetres);
    const math::Vector3d minimumWorld{-halfX, base, -halfZ};
    const math::Vector3d maximumWorld{halfX, base + static_cast<float64>(clouds.ThicknessMetres), halfZ};
    CloudReference result;
    CloudInterval view;
    if (ClipCloudReference(eyeWorld, directionWorld, minimumWorld, maximumWorld, UNBOUNDED_METRES, view) == false)
    {
        return result;
    }
    const float64 near = view.Near;
    const float64 far = view.Far;
    const float64 extinction = static_cast<float64>(clouds.Density) * static_cast<float64>(clouds.ExtinctionPerMetre);
    const float64 scattering = extinction * static_cast<float64>(clouds.SingleScatteringAlbedo);
    const float64 asymmetry = static_cast<float64>(clouds.PhaseAsymmetry);
    const float64 phaseBase = 1 + asymmetry * asymmetry - 2 * asymmetry * math::Dot(directionWorld, sunWorld);
    const float64 phase = (1 - asymmetry * asymmetry) / (4 * math::kPi * phaseBase * std::sqrt(phaseBase));
    const float64 step = (far - near) / static_cast<float64>(VIEW_STEPS);
    for (usize sample = 0; sample < VIEW_STEPS; ++sample)
    {
        const float64 distance = near + (static_cast<float64>(sample) + 0.5) * step;
        const math::Vector3d pointWorld = eyeWorld + directionWorld * distance;
        CloudInterval sun;
        REQUIRE(ClipCloudReference(pointWorld, sunWorld, minimumWorld, maximumWorld, UNBOUNDED_METRES, sun) == true);
        const math::Vector3d pointKilometres{pointWorld.X / CLOUD_METRES_PER_KILOMETRE,
                                             EARTH_GROUND_RADIUS_KILOMETRES + pointWorld.Y / CLOUD_METRES_PER_KILOMETRE,
                                             pointWorld.Z / CLOUD_METRES_PER_KILOMETRE};
        float64 airTransmittance = 0;
        if (ComputeEarthSolarTransmittance(pointKilometres, sunWorld, channel, SOLAR_STEPS, airTransmittance) == true)
        {
            result.Radiance += std::exp(-extinction * (distance - near)) * scattering * phase * airTransmittance *
                               std::exp(-extinction * sun.Far) * step;
        }
    }
    result.Transmittance = std::exp(-extinction * (far - near));
    result.EntryMetres = near;
    result.IsHit = true;
    return result;
}

math::Vector3d ToDouble(math::Vector3 value)
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

renderer::CloudLayerDescription MakeCloudLayer(float32 extinctionPerMetre)
{
    renderer::CloudLayerDescription clouds;
    clouds.IsEnabled = true;
    clouds.BaseHeightMetres = CLOUD_BASE_METRES;
    clouds.ThicknessMetres = CLOUD_THICKNESS_METRES;
    clouds.SizeXMetres = CLOUD_SIZE_METRES;
    clouds.SizeZMetres = CLOUD_SIZE_METRES;
    clouds.EdgeFadeMetres = 0;
    clouds.Density = 1;
    clouds.ExtinctionPerMetre = extinctionPerMetre;
    return clouds;
}
} // namespace

TEST_CASE("Cloud layer composes analytic transmittance and single scattering with the clear sky",
          "[render-graph][atmosphere][cloud-layer][gpu][reference-compute]")
{
    GraphGpuFixture fixture;
    rhi::DeviceInfo deviceInfo;
    REQUIRE(rhi::GetDeviceInfo(fixture.Device, deviceInfo) == rhi::DeviceStatus::Ready);
    INFO("Native backend " << static_cast<uint32>(deviceInfo.Startup.SelectedBackend) << ", adapter "
                           << deviceInfo.Adapter.Name.GetData());
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(PollAtmosphereGraph(
                fixture,
                fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
            rhi::RasterStatus::Ready);
    struct CloudProbe final
    {
        const char* Name;
        math::Vector3 EyeWorld;
        math::Vector3 TargetWorld;
        float32 ExtinctionPerMetre;
    };
    constexpr float32 CLOUD_MIDDLE_METRES = CLOUD_BASE_METRES + CLOUD_THICKNESS_METRES / 2;
    constexpr float32 NEAR_HORIZON_METRES = 30000;
    constexpr float32 FAR_HORIZON_METRES = 60000;
    constexpr StaticArray<CloudProbe, 4> PROBES{{
        {"zenith", {0, CLOUD_CAMERA_HEIGHT_METRES, 0}, {0, CLOUD_MIDDLE_METRES, 0}, 0.001F},
        {"oblique", {0, CLOUD_CAMERA_HEIGHT_METRES, 0}, {0, CLOUD_MIDDLE_METRES, -CLOUD_MIDDLE_METRES}, 0.001F},
        {"horizon at 30 km",
         {0, CLOUD_CAMERA_HEIGHT_METRES, CLOUD_HALF_SIZE_METRES + NEAR_HORIZON_METRES},
         {0, CLOUD_MIDDLE_METRES, 0},
         0.0002F},
        {"horizon at 60 km",
         {0, CLOUD_CAMERA_HEIGHT_METRES, CLOUD_HALF_SIZE_METRES + FAR_HORIZON_METRES},
         {0, CLOUD_MIDDLE_METRES, 0},
         0.0002F},
    }};
    constexpr float32 DAY_SUN_COMPONENT = 0.70710678F;
    constexpr float32 SUNSET_ELEVATION_SINE = 0.1F;
    const StaticArray<math::Vector3, 2> suns{
        {{DAY_SUN_COMPONENT, DAY_SUN_COMPONENT, 0},
         {std::sqrt(1 - SUNSET_ELEVATION_SINE * SUNSET_ELEVATION_SINE), SUNSET_ELEVATION_SINE, 0}}};
    // The sunset Sun is brighter so that its dim, reddened probes stay well above the readback's quantization.
    constexpr StaticArray<float32, 2> SOLAR_ILLUMINANCE{{8, 24}};
    constexpr usize RGB_CHANNELS = 3;
    constexpr float64 READBACK_STEP = 1.0 / 255;
    constexpr usize AIR_STEPS = 2048;
    constexpr usize AIR_SOLAR_STEPS = 256;
    renderer::ViewDescription& camera = fixture.Cameras[0];
    camera.Atmosphere.IsEnabled = true;
    camera.Atmosphere.ShouldLightScene = false;
    const renderer::SceneShaderInputs inputs;
    uint64 revision = 0;
    const auto update = [&]() {
        renderer::RenderGraphReport report;
        return PollAtmosphereGraph(fixture,
                                   fixture.Graph.Update({},
                                                        {},
                                                        {&camera, 1},
                                                        ++revision,
                                                        renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                        report,
                                                        &inputs));
    };
    const usize pixel = GetPixel(WIDTH / 2, HEIGHT / 2);
    for (const CloudProbe& probe : PROBES)
    {
        for (usize sunIndex = 0; sunIndex < suns.GetSize(); ++sunIndex)
        {
            const math::Vector3 sunWorld = suns[sunIndex];
            camera.CameraPosition = probe.EyeWorld;
            math::Vector3 directionWorld;
            REQUIRE(math::TryNormalize(probe.TargetWorld - probe.EyeWorld, directionWorld) ==
                    math::MathStatus::Success);
            SetCloudCamera(camera,
                           directionWorld,
                           math::Abs(directionWorld.Y) > 0.9F ? math::Vector3{0, 0, -1} : math::Vector3{0, 1, 0});
            const float32 solarIlluminance = SOLAR_ILLUMINANCE[sunIndex];
            camera.Atmosphere.DirectionToSunWorld = sunWorld;
            camera.Atmosphere.SolarIlluminance = {solarIlluminance, solarIlluminance, solarIlluminance};
            camera.Atmosphere.Clouds = {};
            REQUIRE(update() == rhi::RasterStatus::Ready);
            ReadAtmosphereFrame(fixture);
            const StaticArray<uint8, PIXEL_BYTES> clear = fixture.Pixels;

            // An empty layer reproduces the clear sky exactly.
            camera.Atmosphere.Clouds = MakeCloudLayer(probe.ExtinctionPerMetre);
            camera.Atmosphere.Clouds.Density = 0;
            REQUIRE(update() == rhi::RasterStatus::Ready);
            ReadAtmosphereFrame(fixture);
            CHECK(fixture.Pixels == clear);

            for (const float32 albedo : {0.0F, 0.99F})
            {
                camera.Atmosphere.Clouds = MakeCloudLayer(probe.ExtinctionPerMetre);
                camera.Atmosphere.Clouds.SingleScatteringAlbedo = albedo;
                REQUIRE(update() == rhi::RasterStatus::Ready);
                ReadAtmosphereFrame(fixture);
                const math::Vector3d eyeWorld = ToDouble(probe.EyeWorld);
                const math::Vector3d rayWorld = ToDouble(directionWorld) * (1 / math::Length(ToDouble(directionWorld)));
                const math::Vector3d sunDirectionWorld = ToDouble(sunWorld) * (1 / math::Length(ToDouble(sunWorld)));
                const math::Vector3d eyeKilometres{eyeWorld.X / CLOUD_METRES_PER_KILOMETRE,
                                                   EARTH_GROUND_RADIUS_KILOMETRES +
                                                       eyeWorld.Y / CLOUD_METRES_PER_KILOMETRE,
                                                   eyeWorld.Z / CLOUD_METRES_PER_KILOMETRE};
                for (usize channel = 0; channel < RGB_CHANNELS; ++channel)
                {
                    const CloudReference cloud = IntegrateCloudReference(camera.Atmosphere.Clouds,
                                                                         eyeWorld,
                                                                         rayWorld,
                                                                         sunDirectionWorld,
                                                                         channel);
                    REQUIRE(cloud.IsHit == true);
                    const EarthSkyIntegral air =
                        IntegrateEarthSkySegment(eyeKilometres,
                                                 rayWorld,
                                                 sunDirectionWorld,
                                                 channel,
                                                 cloud.EntryMetres / CLOUD_METRES_PER_KILOMETRE,
                                                 AIR_STEPS,
                                                 AIR_SOLAR_STEPS);
                    const float64 illuminance = static_cast<float64>(solarIlluminance);
                    const float64 clearRadiance = clear[pixel + channel] * READBACK_STEP;
                    const float64 airRadiance = illuminance * air.Radiance;
                    const float64 cloudRadiance = illuminance * cloud.Radiance;
                    const float64 expected = airRadiance + air.Transmittance * cloudRadiance +
                                             cloud.Transmittance * math::Max(clearRadiance - airRadiance, 0.0);
                    const float64 actual = fixture.Pixels[pixel + channel] * READBACK_STEP;
                    const float64 absoluteError = math::Abs(actual - expected);
                    CAPTURE(probe.Name,
                            sunIndex,
                            albedo,
                            channel,
                            clearRadiance,
                            airRadiance,
                            air.Transmittance,
                            cloudRadiance,
                            cloud.Transmittance,
                            expected,
                            actual,
                            absoluteError);
                    REQUIRE(expected < 1);
                    // Half a code of rounding in each of the two readbacks, plus the shader's 32 view steps, the
                    // transmittance table's interpolation and its 16-step air segment in front of the layer.
                    CHECK(absoluteError <= READBACK_STEP + 0.03 * expected);
                }
            }
        }
    }
    // Invalid layer fields reject the update and keep the drawable scene.
    const uint64 drawableRevision = fixture.Graph.GetDrawableSceneRevision();
    camera.Atmosphere.Clouds.ThicknessMetres = 0;
    CHECK(update() == rhi::RasterStatus::InvalidDescription);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == drawableRevision);
    camera.Atmosphere.Clouds = MakeCloudLayer(1);
    camera.Atmosphere.Clouds.PhaseAsymmetry = 1;
    CHECK(update() == rhi::RasterStatus::InvalidDescription);
    camera.Atmosphere.Clouds = MakeCloudLayer(1);
    camera.Atmosphere.Clouds.AxisXWorld = {0, 1, 0};
    CHECK(update() == rhi::RasterStatus::InvalidDescription);
}

TEST_CASE("Cloud layer attenuates opaque surfaces from its entry to their depth",
          "[render-graph][atmosphere][cloud-layer][gpu][reference-compute]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(PollAtmosphereGraph(
                fixture,
                fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
            rhi::RasterStatus::Ready);
    // A large emissive triangle facing the camera; without sunlight the air and the cloud only attenuate it.
    constexpr float32 TRIANGLE_HALF_SIZE_METRES = 50;
    fixture.Positions = {{{-TRIANGLE_HALF_SIZE_METRES, -TRIANGLE_HALF_SIZE_METRES, 0},
                          {TRIANGLE_HALF_SIZE_METRES, -TRIANGLE_HALF_SIZE_METRES, 0},
                          {0, TRIANGLE_HALF_SIZE_METRES, 0}}};
    const StaticArray<math::Vector3, VERTEX_COUNT> normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    fixture.Meshes[0].Geometry.Normals = normals.GetData();
    const uint32 materialIndex = 0;
    fixture.Meshes[0].MaterialBindings = {&materialIndex, 1};
    renderer::SceneShaderMaterial material;
    material.BaseColor = {};
    material.Emission = {0.5F, 0.25F, 0.75F};
    material.ShouldRegisterRasterMeshLight = false;
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&material, 1};
    // Inside the layer's height range, 500 m outside its +Z side, looking at it.
    constexpr float32 EYE_HEIGHT_METRES = CLOUD_BASE_METRES + CLOUD_THICKNESS_METRES / 2;
    constexpr float32 OUTSIDE_METRES = 500;
    renderer::ViewDescription& camera = fixture.Cameras[0];
    camera.CameraPosition = {0, EYE_HEIGHT_METRES, CLOUD_HALF_SIZE_METRES + OUTSIDE_METRES};
    SetCloudCamera(camera, {0, 0, -1}, {0, 1, 0});
    camera.Atmosphere.IsEnabled = true;
    camera.Atmosphere.ShouldLightScene = true;
    camera.Atmosphere.SolarIlluminance = {};
    camera.Atmosphere.DirectionToSunWorld = {0, 1, 0};
    uint64 revision = 0;
    uint64 contentRevision = 1;
    const auto update = [&]() {
        renderer::RenderGraphReport report;
        return PollAtmosphereGraph(fixture,
                                   fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                                        {fixture.Items.GetData(), fixture.Items.GetSize()},
                                                        {&camera, 1},
                                                        ++revision,
                                                        renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                        report,
                                                        &inputs,
                                                        contentRevision));
    };
    const usize pixel = GetPixel(WIDTH / 2, HEIGHT / 2);
    constexpr usize RGB_CHANNELS = 3;
    // Half the light survives 500 m of cloud.
    constexpr float64 INSIDE_METRES = 500;
    const float32 extinction = static_cast<float32>(std::log(2.0) / INSIDE_METRES);
    constexpr float32 NEAR_SURFACE_METRES = 300;
    constexpr float32 INSIDE_SURFACE_METRES = OUTSIDE_METRES + static_cast<float32>(INSIDE_METRES);
    for (const float32 surfaceDistance : {NEAR_SURFACE_METRES, INSIDE_SURFACE_METRES})
    {
        fixture.Items[0].MeshToWorld.Translation = {0, EYE_HEIGHT_METRES, camera.CameraPosition.Z - surfaceDistance};
        ++contentRevision;
        camera.Atmosphere.Clouds = {};
        REQUIRE(update() == rhi::RasterStatus::Ready);
        ReadAtmosphereFrame(fixture);
        const StaticArray<uint8, PIXEL_BYTES> clear = fixture.Pixels;
        REQUIRE(clear[pixel] > 0);
        camera.Atmosphere.Clouds = MakeCloudLayer(extinction);
        REQUIRE(update() == rhi::RasterStatus::Ready);
        ReadAtmosphereFrame(fixture);
        const float64 cloudTransmittance =
            surfaceDistance > OUTSIDE_METRES
                ? std::exp(-static_cast<float64>(extinction) * static_cast<float64>(surfaceDistance - OUTSIDE_METRES))
                : 1;
        for (usize channel = 0; channel < RGB_CHANNELS; ++channel)
        {
            CAPTURE(surfaceDistance, channel, clear[pixel + channel], fixture.Pixels[pixel + channel]);
            if (cloudTransmittance == 1)
            {
                CHECK(fixture.Pixels[pixel + channel] == clear[pixel + channel]);
            }
            else
            {
                // One code: rounding of the two readbacks.
                CHECK(math::Abs(static_cast<float64>(fixture.Pixels[pixel + channel]) -
                                static_cast<float64>(clear[pixel + channel]) * cloudTransmittance) <= 1);
            }
        }
    }
}

namespace
{
// Two-region fixtures (CloudLayerTestField), the nonuniform-transmittance and two-region order controls of #1319.
// Thanks to William Dungan Jr., A Terrain and Cloud Computer Image Generation Model, SIGGRAPH '79, pp. 143-150,
// "Atmospheric Attenuation", for attenuation as the exponential of the summed optical depth along the eye ray
// (https://www.academia.edu/94793484/A_terrain_and_cloud_computer_image_generation_model), and to Robert A. Drebin,
// Loren Carpenter and Pat Hanrahan, Volume Rendering, SIGGRAPH '88, pp. 65-74, for front-to-back compositing of
// colored, absorbing material along the eye ray (https://www.cs.ucdavis.edu/~ma/ECS276/readings/papers/p65-drebin.pdf).
// The fixtures check the order-dependent consequence of that compositing in Ludus's layer.
// The slab is CLOUD_REGION_DEPTH_METRES deep along Z, split at z = 0:
// Regions[1] (z >= 0) faces a camera on the +Z side and Regions[0] lies behind it. The camera sits at the slab's
// middle height, CLOUD_REGION_GAP_METRES outside its +Z face, and the Sun is off so only the view ray's extinction
// and the test source terms act.
constexpr float32 CLOUD_REGION_HALF_DEPTH_METRES = 400;
constexpr float32 CLOUD_REGION_GAP_METRES = 500;
constexpr float32 CLOUD_REGION_EYE_HEIGHT_METRES = CLOUD_BASE_METRES + CLOUD_THICKNESS_METRES / 2;
constexpr usize CLOUD_FRONT_REGION = 1;
constexpr usize CLOUD_BACK_REGION = 0;

renderer::CloudLayerDescription MakeCloudRegions(float32 frontExtinction, float32 backExtinction)
{
    renderer::CloudLayerDescription clouds = MakeCloudLayer(0);
    clouds.SizeZMetres = 2 * CLOUD_REGION_HALF_DEPTH_METRES;
    clouds.SingleScatteringAlbedo = 0;
    clouds.TestField.IsEnabled = true;
    clouds.TestField.SplitNormalLocal = {0, 0, 1};
    clouds.TestField.SplitOffsetMetres = 0;
    clouds.TestField.Regions[CLOUD_FRONT_REGION].ExtinctionPerMetre = frontExtinction;
    clouds.TestField.Regions[CLOUD_BACK_REGION].ExtinctionPerMetre = backExtinction;
    return clouds;
}

float64 GetCloudChannel(math::Vector3 value, usize channel)
{
    return static_cast<float64>(channel == 0 ? value.X : (channel == 1 ? value.Y : value.Z));
}

/// Premultiplied source radiance and transmittance of a view segment through the layer.
struct CloudRegionsReference final
{
    float64 Radiance = 0;
    float64 Transmittance = 1;
    float64 EntryMetres = 0;
    bool IsHit = false;
};

/// Independent binary64 integral of a CloudLayerTestField along a view ray clipped at maximumMetres, without
/// sunlight: a dense midpoint rule of extinction * source * transmittance and of the optical depth. It does not
/// split steps at the regions' plane, so it checks the shader's exact split rule rather than repeating it.
CloudRegionsReference IntegrateCloudRegionsReference(const renderer::CloudLayerDescription& clouds,
                                                     math::Vector3d eyeWorld,
                                                     math::Vector3d directionWorld,
                                                     float64 maximumMetres,
                                                     usize channel)
{
    constexpr usize VIEW_STEPS = 65536;
    const float64 halfX = static_cast<float64>(clouds.SizeXMetres) * 0.5;
    const float64 halfZ = static_cast<float64>(clouds.SizeZMetres) * 0.5;
    const float64 base = static_cast<float64>(clouds.BaseHeightMetres);
    const math::Vector3d minimumWorld{-halfX, base, -halfZ};
    const math::Vector3d maximumWorld{halfX, base + static_cast<float64>(clouds.ThicknessMetres), halfZ};
    CloudRegionsReference result;
    CloudInterval view;
    if (ClipCloudReference(eyeWorld, directionWorld, minimumWorld, maximumWorld, maximumMetres, view) == false)
    {
        return result;
    }
    const renderer::CloudLayerTestField& field = clouds.TestField;
    const math::Vector3d normal =
        ToDouble(field.SplitNormalLocal) * (1 / math::Length(ToDouble(field.SplitNormalLocal)));
    const float64 density = static_cast<float64>(clouds.Density);
    const float64 step = (view.Far - view.Near) / static_cast<float64>(VIEW_STEPS);
    float64 opticalDepth = 0;
    for (usize sample = 0; sample < VIEW_STEPS; ++sample)
    {
        const float64 distance = view.Near + (static_cast<float64>(sample) + 0.5) * step;
        const math::Vector3d pointWorld = eyeWorld + directionWorld * distance;
        const bool isNegative = math::Dot(pointWorld, normal) < static_cast<float64>(field.SplitOffsetMetres);
        const renderer::CloudLayerTestRegion& region = field.Regions[isNegative == true ? 0 : 1];
        const float64 extinction = density * static_cast<float64>(region.ExtinctionPerMetre);
        const float64 middleDepth = opticalDepth + extinction * step * 0.5;
        result.Radiance += extinction * GetCloudChannel(region.SourceRadiance, channel) * std::exp(-middleDepth) * step;
        opticalDepth += extinction * step;
    }
    result.Transmittance = std::exp(-opticalDepth);
    result.EntryMetres = view.Near;
    result.IsHit = true;
    return result;
}

/// The clustered default graph with one emissive triangle facing +Z placed along the camera's central ray, no Sun
/// and no sky, so the air and the cloud layer only attenuate the backdrop and add the test source radiance.
struct CloudBackdropScene final
{
    GraphGpuFixture Fixture;
    core::UniquePtr<renderer::RenderGraphPackage> Package;
    StaticArray<math::Vector3, VERTEX_COUNT> Normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    uint32 MaterialIndex = 0;
    renderer::SceneShaderMaterial Material;
    renderer::SceneShaderInputs Inputs;
    uint64 Revision = 0;
    uint64 ContentRevision = 1;

    CloudBackdropScene() : Package(LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"}))
    {
        const renderer::RenderGraphPrograms programs =
            Package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
        REQUIRE(PollAtmosphereGraph(
                    Fixture,
                    Fixture.Graph.Initialize(Fixture.Device, Package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
                rhi::RasterStatus::Ready);
        constexpr float32 TRIANGLE_HALF_SIZE_METRES = 50;
        Fixture.Positions = {{{-TRIANGLE_HALF_SIZE_METRES, -TRIANGLE_HALF_SIZE_METRES, 0},
                              {TRIANGLE_HALF_SIZE_METRES, -TRIANGLE_HALF_SIZE_METRES, 0},
                              {0, TRIANGLE_HALF_SIZE_METRES, 0}}};
        Fixture.Meshes[0].Geometry.Normals = Normals.GetData();
        Fixture.Meshes[0].MaterialBindings = {&MaterialIndex, 1};
        Material.BaseColor = {};
        Material.ShouldRegisterRasterMeshLight = false;
        Inputs.Materials = {&Material, 1};
        renderer::ViewDescription& camera = Fixture.Cameras[0];
        camera.Atmosphere.IsEnabled = true;
        camera.Atmosphere.ShouldLightScene = true;
        camera.Atmosphere.SolarIlluminance = {};
        camera.Atmosphere.DirectionToSunWorld = {0, 1, 0};
    }

    renderer::ViewDescription& GetCamera()
    {
        return Fixture.Cameras[0];
    }

    /// Place the backdrop distanceMetres along the camera's central ray with the given emission.
    void SetBackdrop(math::Vector3 directionWorld, float32 distanceMetres, math::Vector3 emission)
    {
        Fixture.Items[0].MeshToWorld.Translation = GetCamera().CameraPosition + directionWorld * distanceMetres;
        Material.Emission = emission;
        ++ContentRevision;
    }

    /// Draw with the given layer and return the frame.
    const StaticArray<uint8, PIXEL_BYTES>& Draw(const renderer::CloudLayerDescription& clouds)
    {
        GetCamera().Atmosphere.Clouds = clouds;
        renderer::RenderGraphReport report;
        REQUIRE(PollAtmosphereGraph(Fixture,
                                    Fixture.Graph.Update({Fixture.Meshes.GetData(), Fixture.Meshes.GetSize()},
                                                         {Fixture.Items.GetData(), Fixture.Items.GetSize()},
                                                         {&GetCamera(), 1},
                                                         ++Revision,
                                                         renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                         report,
                                                         &Inputs,
                                                         ContentRevision)) == rhi::RasterStatus::Ready);
        ReadAtmosphereFrame(Fixture);
        return Fixture.Pixels;
    }
};

/// Air transmittance from the eye (world metres, anchor at the origin) to distanceMetres along a unit ray.
float64
ComputeCloudAirTransmittance(math::Vector3 eyeWorld, math::Vector3d rayWorld, float64 distanceMetres, usize channel)
{
    constexpr usize AIR_STEPS = 2048;
    constexpr usize AIR_SOLAR_STEPS = 16;
    const math::Vector3d eyeKilometres{static_cast<float64>(eyeWorld.X) / CLOUD_METRES_PER_KILOMETRE,
                                       EARTH_GROUND_RADIUS_KILOMETRES +
                                           static_cast<float64>(eyeWorld.Y) / CLOUD_METRES_PER_KILOMETRE,
                                       static_cast<float64>(eyeWorld.Z) / CLOUD_METRES_PER_KILOMETRE};
    return IntegrateEarthSkySegment(eyeKilometres,
                                    rayWorld,
                                    {0, 1, 0},
                                    channel,
                                    distanceMetres / CLOUD_METRES_PER_KILOMETRE,
                                    AIR_STEPS,
                                    AIR_SOLAR_STEPS)
        .Transmittance;
}
} // namespace

TEST_CASE("Cloud layer transmittance through two extinction regions is exp(-s1 d1 - s2 d2) at opaque depths",
          "[render-graph][atmosphere][cloud-layer][gpu][reference-compute]")
{
    CloudBackdropScene scene;
    renderer::ViewDescription& camera = scene.GetCamera();
    camera.CameraPosition = {0,
                             CLOUD_REGION_EYE_HEIGHT_METRES,
                             CLOUD_REGION_HALF_DEPTH_METRES + CLOUD_REGION_GAP_METRES};
    constexpr float32 FRONT_EXTINCTION = 0.001F;
    constexpr float32 BACK_EXTINCTION = 0.0025F;
    const renderer::CloudLayerDescription clouds = MakeCloudRegions(FRONT_EXTINCTION, BACK_EXTINCTION);
    renderer::CloudLayerDescription empty = clouds;
    empty.Density = 0;
    constexpr float32 BACKDROP_EMISSION = 0.95F;
    // Head-on, and 36.87 degrees off it in the XZ plane, which lengthens every path by 1.25.
    constexpr float32 OBLIQUE_SINE = 0.6F;
    constexpr float32 OBLIQUE_COSINE = 0.8F;
    struct CloudView final
    {
        const char* Name;
        math::Vector3 DirectionWorld;
    };
    const StaticArray<CloudView, 2> views{{{"head-on", {0, 0, -1}}, {"oblique", {OBLIQUE_SINE, 0, -OBLIQUE_COSINE}}}};
    // Backdrop depths along the view's Z, from the camera: in front of the layer, inside the front region, inside the
    // back region, and behind both.
    constexpr float32 REGION_METRES = CLOUD_REGION_HALF_DEPTH_METRES;
    constexpr float32 INSIDE_METRES = REGION_METRES / 2;
    constexpr StaticArray<float32, 4> DEPTHS_Z{{CLOUD_REGION_GAP_METRES - INSIDE_METRES,
                                                CLOUD_REGION_GAP_METRES + INSIDE_METRES,
                                                CLOUD_REGION_GAP_METRES + REGION_METRES + INSIDE_METRES,
                                                CLOUD_REGION_GAP_METRES + 2 * REGION_METRES + INSIDE_METRES}};
    constexpr usize RGB_CHANNELS = 3;
    const usize pixel = GetPixel(WIDTH / 2, HEIGHT / 2);
    float64 worstError = 0;
    for (const CloudView& view : views)
    {
        SetCloudCamera(camera, view.DirectionWorld, {0, 1, 0});
        const float64 zCosine = -static_cast<float64>(view.DirectionWorld.Z);
        for (const float32 depthZ : DEPTHS_Z)
        {
            const float32 distance = depthZ / static_cast<float32>(zCosine);
            scene.SetBackdrop(view.DirectionWorld, distance, {BACKDROP_EMISSION, BACKDROP_EMISSION, BACKDROP_EMISSION});
            const StaticArray<uint8, PIXEL_BYTES> clear = scene.Draw({});
            REQUIRE(clear[pixel] > 0);
            // An empty layer with the test field reproduces the clear frame exactly at every depth.
            CHECK(scene.Draw(empty) == clear);
            const StaticArray<uint8, PIXEL_BYTES>& clouded = scene.Draw(clouds);
            // Path lengths in each region along Z, clipped to the backdrop, then along the ray.
            const float64 frontZ = math::Clamp(static_cast<float64>(depthZ - CLOUD_REGION_GAP_METRES),
                                               0.0,
                                               static_cast<float64>(REGION_METRES));
            const float64 backZ = math::Clamp(static_cast<float64>(depthZ - CLOUD_REGION_GAP_METRES - REGION_METRES),
                                              0.0,
                                              static_cast<float64>(REGION_METRES));
            const float64 expected = std::exp(-static_cast<float64>(FRONT_EXTINCTION) * frontZ / zCosine -
                                              static_cast<float64>(BACK_EXTINCTION) * backZ / zCosine);
            const CloudRegionsReference reference = IntegrateCloudRegionsReference(
                clouds,
                ToDouble(camera.CameraPosition),
                ToDouble(view.DirectionWorld) * (1 / math::Length(ToDouble(view.DirectionWorld))),
                static_cast<float64>(distance),
                0);
            // The reference's midpoint rule misplaces at most half a step of optical depth where the ray crosses
            // the plane: |s1 - s2| * step / 2, about 1e-5 with 65536 steps.
            CHECK(math::Abs(reference.Transmittance - expected) <= 1e-5);
            for (usize channel = 0; channel < RGB_CHANNELS; ++channel)
            {
                const float64 clearCode = clear[pixel + channel];
                const float64 observed = static_cast<float64>(clouded[pixel + channel]) / clearCode;
                const float64 codeError =
                    math::Abs(static_cast<float64>(clouded[pixel + channel]) - clearCode * expected);
                worstError = math::Max(worstError, codeError);
                CAPTURE(view.Name, depthZ, distance, channel, clearCode, frontZ, backZ, expected, observed, codeError);
                if (expected == 1)
                {
                    CHECK(clouded[pixel + channel] == clear[pixel + channel]);
                }
                else
                {
                    // One code: half a code of rounding in each of the two readbacks. The shader's step split makes
                    // the piecewise-constant transmittance exact up to float32 rounding.
                    CHECK(codeError <= 1);
                }
            }
        }
    }
    UNSCOPED_INFO("Worst two-region transmittance error in readback codes: " << worstError);
    CHECK(worstError <= 1);

    // An invalid test field rejects the update.
    renderer::CloudLayerDescription invalid = clouds;
    invalid.TestField.SplitNormalLocal = {};
    camera.Atmosphere.Clouds = invalid;
    renderer::RenderGraphReport report;
    CHECK(PollAtmosphereGraph(
              scene.Fixture,
              scene.Fixture.Graph.Update({scene.Fixture.Meshes.GetData(), scene.Fixture.Meshes.GetSize()},
                                         {scene.Fixture.Items.GetData(), scene.Fixture.Items.GetSize()},
                                         {&camera, 1},
                                         ++scene.Revision,
                                         renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                         report,
                                         &scene.Inputs,
                                         scene.ContentRevision)) == rhi::RasterStatus::InvalidDescription);
}

TEST_CASE("Cloud layer composes two emitting regions front to back and keeps their total transmittance",
          "[render-graph][atmosphere][cloud-layer][gpu][reference-compute]")
{
    CloudBackdropScene scene;
    renderer::ViewDescription& camera = scene.GetCamera();
    camera.CameraPosition = {0,
                             CLOUD_REGION_EYE_HEIGHT_METRES,
                             CLOUD_REGION_HALF_DEPTH_METRES + CLOUD_REGION_GAP_METRES};
    const math::Vector3 directionWorld{0, 0, -1};
    SetCloudCamera(camera, directionWorld, {0, 1, 0});
    // Region A is dense and red, region B thin and blue; both regions are equally deep along the ray, so swapping
    // their coefficients reverses their order on the same ray.
    constexpr float32 A_EXTINCTION = 0.002F;
    constexpr float32 B_EXTINCTION = 0.0005F;
    const math::Vector3 sourceA{0.8F, 0.2F, 0.1F};
    const math::Vector3 sourceB{0.1F, 0.3F, 0.9F};
    constexpr float32 BACKDROP_EMISSION = 0.5F;
    const math::Vector3 backdrop{BACKDROP_EMISSION, BACKDROP_EMISSION, BACKDROP_EMISSION};
    // 300 m behind the layer's far face.
    constexpr float32 BEHIND_METRES = CLOUD_REGION_GAP_METRES + 3 * CLOUD_REGION_HALF_DEPTH_METRES + 300;
    const math::Vector3d eyeWorld = ToDouble(camera.CameraPosition);
    const math::Vector3d rayWorld = ToDouble(directionWorld);
    constexpr usize RGB_CHANNELS = 3;
    const usize pixel = GetPixel(WIDTH / 2, HEIGHT / 2);
    constexpr float64 READBACK_STEP = 1.0 / 255;

    const auto makeOrder = [&](bool isAFront, bool hasSources) {
        renderer::CloudLayerDescription clouds = isAFront == true ? MakeCloudRegions(A_EXTINCTION, B_EXTINCTION)
                                                                  : MakeCloudRegions(B_EXTINCTION, A_EXTINCTION);
        if (hasSources == true)
        {
            clouds.TestField.Regions[CLOUD_FRONT_REGION].SourceRadiance = isAFront == true ? sourceA : sourceB;
            clouds.TestField.Regions[CLOUD_BACK_REGION].SourceRadiance = isAFront == true ? sourceB : sourceA;
        }
        return clouds;
    };
    // The isolated region: the other region's extinction and source are removed.
    const auto isolate = [](renderer::CloudLayerDescription clouds, usize region) {
        clouds.TestField.Regions[1 - region] = {};
        return clouds;
    };

    scene.SetBackdrop(directionWorld, BEHIND_METRES, backdrop);
    const StaticArray<uint8, PIXEL_BYTES> clear = scene.Draw({});
    StaticArray<StaticArray<float64, RGB_CHANNELS>, 2> transmittances{};
    StaticArray<StaticArray<float64, RGB_CHANNELS>, 2> radiances{};
    float64 worstError = 0;
    for (const bool isAFront : {true, false})
    {
        const renderer::CloudLayerDescription clouds = makeOrder(isAFront, true);
        const renderer::CloudLayerDescription absorber = makeOrder(isAFront, false);
        // Background transmittance alone: the regions without their sources in front of the emissive backdrop.
        scene.SetBackdrop(directionWorld, BEHIND_METRES, backdrop);
        const StaticArray<uint8, PIXEL_BYTES> attenuated = scene.Draw(absorber);
        // Cloud radiance alone: the regions with their sources in front of a black backdrop.
        scene.SetBackdrop(directionWorld, BEHIND_METRES, {});
        const StaticArray<uint8, PIXEL_BYTES> emitted = scene.Draw(clouds);
        // Both together.
        scene.SetBackdrop(directionWorld, BEHIND_METRES, backdrop);
        const StaticArray<uint8, PIXEL_BYTES> composed = scene.Draw(clouds);
        const usize order = isAFront == true ? 0 : 1;
        for (usize channel = 0; channel < RGB_CHANNELS; ++channel)
        {
            const float64 maximum = static_cast<float64>(BEHIND_METRES);
            const CloudRegionsReference front = IntegrateCloudRegionsReference(isolate(clouds, CLOUD_FRONT_REGION),
                                                                               eyeWorld,
                                                                               rayWorld,
                                                                               maximum,
                                                                               channel);
            const CloudRegionsReference back = IntegrateCloudRegionsReference(isolate(clouds, CLOUD_BACK_REGION),
                                                                              eyeWorld,
                                                                              rayWorld,
                                                                              maximum,
                                                                              channel);
            const CloudRegionsReference whole =
                IntegrateCloudRegionsReference(clouds, eyeWorld, rayWorld, maximum, channel);
            REQUIRE(whole.IsHit == true);
            const float64 cloudRadiance = front.Radiance + front.Transmittance * back.Radiance;
            const float64 cloudTransmittance = front.Transmittance * back.Transmittance;
            // The independent whole-ray integral agrees with the order rule.
            // Half a reference step of optical depth at the plane, as in the transmittance case.
            CHECK(math::Abs(whole.Radiance - cloudRadiance) <= 1e-5);
            CHECK(math::Abs(whole.Transmittance - cloudTransmittance) <= 1e-5);
            const float64 airTransmittance =
                ComputeCloudAirTransmittance(camera.CameraPosition, rayWorld, whole.EntryMetres, channel);
            const float64 expectedEmitted = airTransmittance * cloudRadiance;
            const float64 clearRadiance = clear[pixel + channel] * READBACK_STEP;
            const float64 expectedAttenuated = clearRadiance * cloudTransmittance;
            const float64 expectedComposed = expectedAttenuated + expectedEmitted;
            const float64 actualEmitted = emitted[pixel + channel] * READBACK_STEP;
            const float64 actualAttenuated = attenuated[pixel + channel] * READBACK_STEP;
            const float64 actualComposed = composed[pixel + channel] * READBACK_STEP;
            const float64 emittedError = math::Abs(actualEmitted - expectedEmitted);
            const float64 attenuatedError = math::Abs(actualAttenuated - expectedAttenuated);
            const float64 composedError = math::Abs(actualComposed - expectedComposed);
            worstError = math::Max(worstError, math::Max(emittedError, math::Max(attenuatedError, composedError)));
            CAPTURE(isAFront,
                    channel,
                    front.Radiance,
                    front.Transmittance,
                    back.Radiance,
                    back.Transmittance,
                    airTransmittance,
                    clearRadiance,
                    expectedEmitted,
                    actualEmitted,
                    expectedAttenuated,
                    actualAttenuated,
                    expectedComposed,
                    actualComposed);
            REQUIRE(expectedComposed < 1);
            // Half a code of rounding in each readback compared: the cloud radiance has one, the attenuated and
            // composed backdrops also carry the clear frame's.
            CHECK(emittedError <= READBACK_STEP);
            CHECK(attenuatedError <= READBACK_STEP);
            CHECK(composedError <= 1.5 * READBACK_STEP);
            transmittances[order][channel] = actualAttenuated;
            radiances[order][channel] = actualEmitted;
        }
    }
    UNSCOPED_INFO("Worst two-region order error: " << worstError);
    // Reversing the regions changes the cloud color as predicted, red to blue, but not the background's attenuation.
    CHECK(radiances[0][0] > radiances[1][0]);
    CHECK(radiances[0][2] < radiances[1][2]);
    for (usize channel = 0; channel < RGB_CHANNELS; ++channel)
    {
        CHECK(math::Abs(transmittances[0][channel] - transmittances[1][channel]) <= READBACK_STEP);
    }
}

TEST_CASE("Auroral curtains animate, attenuate and respect opaque depth in the deferred atmosphere",
          "[render-graph][atmosphere][aurora][gpu][reference-compute]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(PollAtmosphereGraph(
                fixture,
                fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
            rhi::RasterStatus::Ready);

    renderer::ViewDescription& camera = fixture.Cameras[0];
    constexpr float32 EYE_HEIGHT_METRES = 2;
    constexpr float32 LOOK_SLOPE = 0.32F;
    camera.CameraPosition = {0, EYE_HEIGHT_METRES, 0};
    math::Vector3 directionWorld;
    REQUIRE(math::TryNormalize(math::Vector3{0, LOOK_SLOPE, -1}, directionWorld) == math::MathStatus::Success);
    SetCloudCamera(camera, directionWorld, {0, 1, 0});
    camera.Atmosphere.IsEnabled = true;
    camera.Atmosphere.ShouldLightScene = true;
    camera.Atmosphere.DirectionToSunWorld = {0, -1, 0};
    renderer::AuroraLayerDescription& aurora = camera.Atmosphere.Aurora;
    aurora.AxisXWorld = {-1, 0, 0};
    // Keep the narrower green band above quantization in the linear RGBA8 readback.
    constexpr float32 EMISSION_PER_METRE = 0.000005F;
    const StaticArray<math::Vector3, VERTEX_COUNT> normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    fixture.Meshes[0].Geometry.Normals = normals.GetData();
    const uint32 materialIndex = 0;
    fixture.Meshes[0].MaterialBindings = {&materialIndex, 1};
    renderer::SceneShaderMaterial material;
    material.BaseColor = {};
    material.Emission = {0.25F, 0, 0};
    material.ShouldRegisterRasterMeshLight = false;
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&material, 1};
    uint64 revision = 0;
    const auto capture = [&](bool hasOpaqueSurface) {
        renderer::RenderGraphReport report;
        const BorrowedView<const renderer::RenderGraphMesh> meshes =
            hasOpaqueSurface == true ? BorrowedView<const renderer::RenderGraphMesh>{fixture.Meshes.GetData(), 1}
                                     : BorrowedView<const renderer::RenderGraphMesh>{};
        const BorrowedView<const renderer::RenderGraphItem> items =
            hasOpaqueSurface == true ? BorrowedView<const renderer::RenderGraphItem>{fixture.Items.GetData(), 1}
                                     : BorrowedView<const renderer::RenderGraphItem>{};
        REQUIRE(PollAtmosphereGraph(fixture,
                                    fixture.Graph.Update(meshes,
                                                         items,
                                                         {&camera, 1},
                                                         ++revision,
                                                         renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                         report,
                                                         &inputs)) == rhi::RasterStatus::Ready);
        REQUIRE(report.UnrenderedWorldFeatures == 0);
        ReadAtmosphereFrame(fixture);
    };

    capture(false);
    const StaticArray<uint8, PIXEL_BYTES> clear = fixture.Pixels;
    aurora.IsEnabled = true;
    aurora.EmissionPerMetre = 0;
    capture(false);
    CHECK(fixture.Pixels == clear);

    aurora.EmissionPerMetre = EMISSION_PER_METRE;
    capture(false);
    const StaticArray<uint8, PIXEL_BYTES> first = fixture.Pixels;
    const usize middle = GetPixel(WIDTH / 2, HEIGHT / 2);
    CHECK(first[middle + 1] > clear[middle + 1]);
    CHECK(first[middle + 1] > first[middle]);
    CHECK(first[middle + 1] > first[middle + 2]);

    aurora.TimeSeconds = renderer::AuroraLayerDescription::LOOP_SECONDS / 4;
    capture(false);
    CHECK(fixture.Pixels != first);
    aurora.TimeSeconds = renderer::AuroraLayerDescription::LOOP_SECONDS;
    capture(false);
    CHECK(fixture.Pixels == first);

    // A black cloud cuts the view to the aurora. It is lit by neither the Sun nor the aurora.
    camera.Atmosphere.Clouds = MakeCloudLayer(renderer::CloudLayerDescription::DEFAULT_EXTINCTION_PER_METRE);
    camera.Atmosphere.Clouds.SingleScatteringAlbedo = 0;
    capture(false);
    CHECK(fixture.Pixels[middle + 1] < first[middle + 1]);
    camera.Atmosphere.Clouds = {};

    // Opaque geometry nearby overwrites only the pixels it covers.
    constexpr float32 SURFACE_DISTANCE_METRES = 10;
    fixture.Items[0].MeshToWorld.Translation = camera.CameraPosition + directionWorld * SURFACE_DISTANCE_METRES;
    capture(true);
    const StaticArray<uint8, PIXEL_BYTES> obscured = fixture.Pixels;
    aurora.IsEnabled = false;
    capture(true);
    CHECK(obscured[middle] > 0);
    CHECK(obscured[middle + 1] == fixture.Pixels[middle + 1]);
    CHECK(obscured[middle + 1] < first[middle + 1]);

    aurora.IsEnabled = true;
    SetCloudCamera(camera, {0, -LOOK_SLOPE, -1}, {0, 1, 0});
    capture(false);
    const StaticArray<uint8, PIXEL_BYTES> ground = fixture.Pixels;
    aurora.IsEnabled = false;
    capture(false);
    CHECK(fixture.Pixels == ground);
}
