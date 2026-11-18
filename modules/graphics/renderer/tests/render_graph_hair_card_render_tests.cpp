// Hair-card raster pixels: the forward-lit graph shades a two-sided hair card around its authored strand and matches
// the CPU evaluation of the same lobe (hair_card.hpp). The CPU lobe sweeps are in hair_card_tests.cpp.

#include "hair_card_coverage_fixture.h"
#include "render_graph_gpu_fixture.h"
#include "renderer_composite.h"
#include "rg2_forward_lit.h"

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/hair_card.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/graphics/rhi/device.h>

#include <catch2/catch_test_macros.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>

using namespace ludus::foundation;
using namespace ludus::graphics::renderer::testing;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace packages = ludus::render_graph_packages;
namespace shaders = ludus::shaders;
namespace threading = ludus::foundation::threading;
namespace timing = ludus::foundation::time;

TEST_CASE("forward-lit hair cards follow the authored strand and shade both faces", "[render-graph][hair-card][gpu]")
{
    namespace scene_format = ludus::content::scene_format;
    GraphGpuFixture fixture;
    const renderer::RenderGraphPrograms programs =
        packages::rg2_forward_lit::MakePrograms(shaders::renderer_composite::Vertex(),
                                                shaders::renderer_composite::Fragment());
    renderer::RenderGraphPlan plan = packages::rg2_forward_lit::MakePlan();
    Array<ludus::content::render_graph_format::Resource> resources;
    REQUIRE(resources.TryAddRange(plan.Resources) == true);
    const uint32 depthResource = plan.Passes[0].Depth;
    resources[depthResource].IsExported = 1;
    plan.Resources = {resources.GetData(), resources.GetSize()};
    rhi::RasterStatus initialized = fixture.Graph.Initialize(fixture.Device, plan, programs, nullptr, WIDTH, HEIGHT);
    for (usize attempt = 0; attempt < 10000 && initialized == rhi::RasterStatus::Pending; ++attempt)
    {
        threading::SleepFor(timing::Milliseconds(1));
        initialized = fixture.Graph.Poll();
    }
    REQUIRE(initialized == rhi::RasterStatus::Ready);
    // The card faces +Z toward an orthographic viewer looking down -Z. It has no texture coordinates, so its strand
    // frame is the fallback one: tangent +X, bitangent +Y.
    const StaticArray<math::Vector3, VERTEX_COUNT> frontNormals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    const StaticArray<math::Vector3, VERTEX_COUNT> backNormals{{{0, 0, -1}, {0, 0, -1}, {0, 0, -1}}};
    fixture.Meshes[0].Geometry.Normals = frontNormals.GetData();
    const uint32 materialIndex = 0;
    fixture.Meshes[0].MaterialBindings = {&materialIndex, 1};
    fixture.Cameras[0].OrthographicCamera = true;
    fixture.Items[0].Material.Color = {1, 1, 1, 1};
    // The light lies in the plane of the vertical strand and the normal, twenty degrees from the normal. A primary
    // shift of minus ten degrees puts the highlight at the viewer (hair_card_tests.cpp derives the peak).
    constexpr float32 RADIANS_PER_DEGREE = math::kPiF / 180; // ludus-style: magic-number degrees in a half turn
    constexpr float32 FIRST_INCIDENT_DEGREES = 20;
    constexpr float32 INCIDENT_RADIANS = FIRST_INCIDENT_DEGREES * RADIANS_PER_DEGREE;
    scene_format::HairCardParameters hair;
    hair.StrandDirectionDegrees = 90;
    hair.PrimaryShiftDegrees = -10;
    hair.SecondaryWeight = 0;
    renderer::SceneShaderMaterial material;
    material.BaseColor = {.35F, .22F, .12F};
    material.Roughness = .35F;
    material.Scattering = scene_format::MakeHairCardScattering(hair);
    renderer::DirectLight light;
    light.SourceId = 1;
    light.Color = {1, 1, 1};
    light.Direction = {0, -math::Sin(INCIDENT_RADIANS), -math::Cos(INCIDENT_RADIANS)};
    light.Intensity = .2F;
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&material, 1};
    inputs.Lights = {&light, 1};
    renderer::RenderGraphReport report;
    uint64 revision = 0;
    const auto update = [&]() {
        ++revision;
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
    REQUIRE(update() == rhi::RasterStatus::Ready);
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
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    const auto renderRed = [&]() {
        REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
        REQUIRE(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
        REQUIRE(compositor.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
        const rhi::RasterStatus confirmed = fixture.Graph.ConfirmSubmission(completion);
        REQUIRE((confirmed == rhi::RasterStatus::Ready || confirmed == rhi::RasterStatus::Pending));
        REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(fixture.Pixels)) == true);
        return static_cast<int32>(fixture.Pixels[center]);
    };
    const int32 alongStrand = renderRed();
    // Turning the strand to the tangent moves the highlight off the viewer.
    hair.StrandDirectionDegrees = 0;
    material.Scattering = scene_format::MakeHairCardScattering(hair);
    REQUIRE(update() == rhi::RasterStatus::Ready);
    const int32 acrossStrand = renderRed();
    CAPTURE(alongStrand, acrossStrand);
    // The forward-lit target stores linear red; the CPU evaluation of the same lobe predicts both pixels.
    const auto predictRed = [&](float32 strandDegrees) {
        scene_format::HairCardParameters predicted = hair;
        predicted.StrandDirectionDegrees = strandDegrees;
        const renderer::HairCardShading shading = renderer::MakeHairCardShading(predicted);
        const math::Vector3 normalWorld{0, 0, 1};
        const math::Vector3 strandWorld =
            renderer::ComputeHairCardStrand(normalWorld, math::Vector4{0, 0, 0, 1}, shading.StrandRadians);
        const math::Vector3 reflectance =
            renderer::EvaluateHairCardReflectance(material.BaseColor,
                                                  material.Roughness,
                                                  shading,
                                                  {normalWorld, strandWorld, normalWorld, -light.Direction});
        constexpr float32 CODES = 255;
        return math::Clamp(reflectance.X * light.Intensity, 0.0F, 1.0F) * CODES;
    };
    constexpr float32 QUANTIZATION_CODES = 2;
    CHECK(math::Abs(static_cast<float32>(alongStrand) - predictRed(90)) <= QUANTIZATION_CODES);
    CHECK(math::Abs(static_cast<float32>(acrossStrand) - predictRed(0)) <= QUANTIZATION_CODES);
    constexpr int32 HIGHLIGHT_MARGIN_CODES = 40;
    CHECK(alongStrand > acrossStrand + HIGHLIGHT_MARGIN_CODES);
    // Seen from behind (its normals point away from the viewer and the light) the card turns toward the viewer and
    // keeps the physical strand, so it matches the front.
    hair.StrandDirectionDegrees = 90;
    material.Scattering = scene_format::MakeHairCardScattering(hair);
    const StaticArray<uint32, INDEX_COUNT> backIndices{{0, 2, 1}};
    fixture.Meshes[0].Geometry.Indices = backIndices.GetData();
    fixture.Meshes[0].Geometry.Normals = backNormals.GetData();
    REQUIRE(update() == rhi::RasterStatus::Ready);
    const int32 backFace = renderRed();
    CHECK(math::Abs(static_cast<float32>(backFace - alongStrand)) <= 1);
    // An ordinary metallic/roughness card is dark from behind under the same light.
    material.Scattering = {};
    REQUIRE(update() == rhi::RasterStatus::Ready);
    CHECK(renderRed() == 0);
    // A light behind the facing side contributes nothing to the hair card.
    material.Scattering = scene_format::MakeHairCardScattering(hair);
    fixture.Meshes[0].Geometry.Indices = fixture.Indices.GetData();
    fixture.Meshes[0].Geometry.Normals = frontNormals.GetData();
    light.Direction.Z = -light.Direction.Z;
    REQUIRE(update() == rhi::RasterStatus::Ready);
    CHECK(renderRed() == 0);
    light.Direction.Z = -light.Direction.Z;
    HairCardCoverageFixture coverage;
    coverage.SetMask(material);
    material.Scattering = scene_format::MakeHairCardScattering(hair);
    DepthDisplay display(fixture.Device, shaders::rg2_depth_bits::Vertex(), shaders::rg2_depth_bits::Fragment());
    const auto measureCoverage = [&](uint8 alpha, float32 cutoff, bool hasExpectedCoverage) {
        coverage.Texture[HairCardCoverageFixture::TEXEL_BYTES - 1] = alpha;
        material.AlphaCutoff = cutoff;
        REQUIRE(update() == rhi::RasterStatus::Ready);
        const int32 red = renderRed();
        const bool hasColor = red != static_cast<int32>(fixture.Pixels[0]);
        rhi::TextureHandle depth;
        rhi::TextureAspect depthAspect;
        REQUIRE(fixture.Graph.GetResourceAllocation(depthResource, depth, depthAspect) == rhi::RasterStatus::Ready);
        REQUIRE(depthAspect == rhi::TextureAspect::Depth);
        StaticArray<uint8, PIXEL_BYTES> depthPixels{};
        display.Show(fixture.Surface, depth, depthPixels);
        const bool hasDepth = ReadDepthBits(depthPixels, WIDTH / 2, HEIGHT / 2) != ReadDepthBits(depthPixels, 0, 0);
        CAPTURE(alpha, cutoff);
        CHECK(hasColor == hasDepth);
        CHECK(hasColor == hasExpectedCoverage);
        return hasColor;
    };
    CHECK(measureCoverage(0, HAIR_MASK_CUTOFF, false) == false);
    CHECK(measureCoverage(HAIR_MASK_SOLID_ALPHA, HAIR_MASK_CUTOFF, true) == true);
    const bool hasMatchedCoverage = measureCoverage(HAIR_MASK_MID_ALPHA, HAIR_MASK_CUTOFF, true);
    const bool hasMismatchedCoverage = measureCoverage(HAIR_MASK_MID_ALPHA, HAIR_MASK_MISMATCHED_CUTOFF, false);
    CHECK(hasMatchedCoverage != hasMismatchedCoverage);
    // Keep projection, mask and facing side fixed during the angular sweep. In the longitudinal plane,
    // the half angle is the mean of the incident and view angles, so the shifted lobe is cos^p(half + shift).
    // This scalar prediction is independent of the shared CPU vector lobe used above.
    coverage.Texture[HairCardCoverageFixture::TEXEL_BYTES - 1] = HAIR_MASK_SOLID_ALPHA;
    material.AlphaCutoff = HAIR_MASK_CUTOFF;
    constexpr float32 SWEEP_INTENSITY = 0.125F;
    constexpr float32 SECOND_INCIDENT_DEGREES = 35;
    constexpr int32 STEPS_PER_SIDE = 4;
    constexpr int32 SWEEP_STEP_DEGREES = 5;
    constexpr float32 MAXIMUM_COLOR_CODE = 255;
    light.Intensity = SWEEP_INTENSITY;
    const auto makeDirection = [](float32 degrees) -> math::Vector3 {
        const float32 radians = degrees * RADIANS_PER_DEGREE;
        return {0, math::Sin(radians), math::Cos(radians)};
    };
    const float64 roughness = static_cast<float64>(material.Roughness);
    const float64 alpha = roughness * roughness;
    const float64 exponent = 2 / (alpha * alpha) - 2;
    const float64 normalization = (exponent + 2) / (2 * math::kPi);
    const auto predictSweepRed = [&](float32 incidentDegrees, float32 viewDegrees) {
        const float32 shiftedHalfRadians =
            ((incidentDegrees + viewDegrees) * 0.5F + hair.PrimaryShiftDegrees) * RADIANS_PER_DEGREE;
        const float32 lobe = static_cast<float32>(
            normalization * math::Pow(math::Cos(static_cast<float64>(shiftedHalfRadians)), exponent));
        return (material.BaseColor.X / math::kPiF + lobe * hair.PrimaryWeight) * -light.Direction.Z * light.Intensity *
               MAXIMUM_COLOR_CODE;
    };
    for (const float32 incidentDegrees : {FIRST_INCIDENT_DEGREES, SECOND_INCIDENT_DEGREES})
    {
        light.Direction = -makeDirection(incidentDegrees);
        const float32 peakDegrees = -2 * hair.PrimaryShiftDegrees - incidentDegrees;
        int32 maximumRed = -1;
        int32 peakStep = STEPS_PER_SIDE;
        for (int32 step = -STEPS_PER_SIDE; step <= STEPS_PER_SIDE; ++step)
        {
            const float32 viewDegrees = peakDegrees + static_cast<float32>(step * SWEEP_STEP_DEGREES);
            fixture.Cameras[0].CameraForward = -makeDirection(viewDegrees);
            REQUIRE(update() == rhi::RasterStatus::Ready);
            const int32 red = renderRed();
            const float32 expected = predictSweepRed(incidentDegrees, viewDegrees);
            CAPTURE(incidentDegrees, viewDegrees, red, expected);
            CHECK(math::Abs(static_cast<float32>(red) - expected) <= QUANTIZATION_CODES);
            if (red > maximumRed)
            {
                maximumRed = red;
                peakStep = step;
            }
        }
        CHECK(peakStep == 0);
        fixture.Cameras[0].CameraForward = -makeDirection(peakDegrees);
        material.Scattering = {};
        REQUIRE(update() == rhi::RasterStatus::Ready);
        CHECK(renderRed() < maximumRed - HIGHLIGHT_MARGIN_CODES);
        material.Scattering = scene_format::MakeHairCardScattering(hair);
    }
    // Remain above the shading-normal clipping margin so this grazing probe keeps the same fixed normal.
    constexpr float32 NEAR_TANGENT_DEGREES = 89;
    constexpr float32 GRAZING_INTENSITY = 5;
    light.Intensity = GRAZING_INTENSITY;
    light.Direction = -makeDirection(NEAR_TANGENT_DEGREES);
    fixture.Cameras[0].CameraForward = -makeDirection(-NEAR_TANGENT_DEGREES);
    REQUIRE(update() == rhi::RasterStatus::Ready);
    const float32 grazingRed = predictSweepRed(NEAR_TANGENT_DEGREES, -NEAR_TANGENT_DEGREES);
    REQUIRE(grazingRed > QUANTIZATION_CODES);
    CHECK(math::Abs(static_cast<float32>(renderRed()) - grazingRed) <= QUANTIZATION_CODES);

    // A metallic hair card is not a valid material; the drawable scene stays published.
    material.IsAlphaMasked = false;
    REQUIRE(update() == rhi::RasterStatus::Ready);
    const uint64 drawable = fixture.Graph.GetDrawableSceneRevision();
    material.Metallic = .5F;
    CHECK(update() != rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == drawable);
}
