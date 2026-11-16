// Hair-card raster pixels: the forward-lit graph shades a two-sided hair card around its authored strand and matches
// the CPU evaluation of the same lobe (hair_card.hpp). The CPU lobe sweeps are in hair_card_tests.cpp.

#include "render_graph_gpu_fixture.h"
#include "renderer_composite.h"
#include "rg2_forward_lit.h"

#include <ludus/content/scene_format/material.h>
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
    rhi::RasterStatus initialized = fixture.Graph.Initialize(fixture.Device,
                                                             packages::rg2_forward_lit::MakePlan(),
                                                             programs,
                                                             nullptr,
                                                             WIDTH,
                                                             HEIGHT);
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
    constexpr float32 INCIDENT_RADIANS = 20 * math::kPiF / 180; // ludus-style: magic-number degrees in a half turn
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
    fixture.Meshes[0].Geometry.Normals = frontNormals.GetData();
    light.Direction.Z = -light.Direction.Z;
    REQUIRE(update() == rhi::RasterStatus::Ready);
    CHECK(renderRed() == 0);
    light.Direction.Z = -light.Direction.Z;
    // Masked coverage discards the card in colour like any masked material; the cleared background shows through.
    const StaticArray<uint8, TEXEL_BYTES> transparent{{255, 255, 255, 0}};
    material.ColorTexture =
        {transparent.GetData(), transparent.GetSize(), transparent.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Srgb};
    material.IsAlphaMasked = true;
    material.AlphaCutoff = .5F;
    REQUIRE(update() == rhi::RasterStatus::Ready);
    CHECK(renderRed() == static_cast<int32>(fixture.Pixels[0]));
    // A metallic hair card is not a valid material; the drawable scene stays published.
    material.IsAlphaMasked = false;
    REQUIRE(update() == rhi::RasterStatus::Ready);
    const uint64 drawable = fixture.Graph.GetDrawableSceneRevision();
    material.Metallic = .5F;
    CHECK(update() != rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == drawable);
}
