// Raster skin diffusion pixels: the clustered deferred default graph spreads a skin material's diffuse lighting over
// its scattering distance (renderer_skin_diffusion.slang) and leaves flat lighting, other materials and surfaces at
// another depth as they were. Each scene is compared with the same scene at zero scattering distance, which takes the
// ordinary resolve. The kernel itself is checked on the CPU in skin_diffusion_tests.cpp.

#include "render_graph_gpu_fixture.h"
#include "renderer_composite.h"

#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
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
namespace shaders = ludus::shaders;
namespace threading = ludus::foundation::threading;
namespace timing = ludus::foundation::time;

namespace
{
namespace scene_format = ludus::content::scene_format;
constexpr usize QUAD_VERTEX_COUNT = 4;
constexpr usize QUAD_INDEX_COUNT = 6;
constexpr usize QUAD_COUNT = 2;
constexpr usize POLL_ATTEMPTS = 10000;
// Two 0.9 m by 1.2 m quads meet at x = 0, between pixel columns 47 and 48 of the identity clip transform, which maps
// x in [-1, 1] onto the 96 columns (2.08 cm per column).
constexpr usize LEFT_EDGE_COLUMN = WIDTH / 2 - 1;
constexpr usize RIGHT_EDGE_COLUMN = WIDTH / 2;
constexpr usize LEFT_INTERIOR_COLUMN = WIDTH / 4;
constexpr usize RIGHT_INTERIOR_COLUMN = 3 * WIDTH / 4;
constexpr usize ROW = HEIGHT / 2;
constexpr float32 QUAD_HALF_WIDTH = .45F;
constexpr float32 QUAD_HALF_HEIGHT = .6F;
constexpr float32 QUAD_DEPTH = .5F;
// Red scatters four times as far as blue: about one, three and nine columns for red's three taps.
constexpr float32 RED_DISTANCE = .06F;
constexpr float32 GREEN_DISTANCE = .03F;
constexpr float32 BLUE_DISTANCE = .015F;
constexpr float32 WHITE = .8F;
// The left quad's albedo. Post-scatter texturing multiplies the spread light by the square root of the receiving
// pixel's albedo, so light crossing the edge needs a nonzero albedo to show.
constexpr float32 DARK = .1F;
constexpr float32 LIGHT_INTENSITY = 2;
constexpr usize RED = 0;
constexpr usize BLUE = 2;
constexpr usize COLOR_CHANNELS = 3;
// RGBA16F storage and the separate radiance and diffuse targets move a linear RGBA8 channel by at most two codes.
constexpr float32 FLAT_TOLERANCE_CODES = 2;

enum class LeftQuad : uint8
{
    DarkSkin,        ///< Dark skin beside the white skin: an albedo edge on one surface.
    DarkMetallic,    ///< A dark non-skin dielectric beside the skin: a material (profile) edge.
    DistantDarkSkin, ///< Dark skin 0.3 m behind the white skin: a depth edge.
    ShortDarkSkin,   ///< Dark skin a tenth as far-scattering as the white skin: its own skin profile.
};
// Scale of the short-scattering dark skin's distance relative to the white skin's.
constexpr float32 SHORT_DISTANCE_SCALE = .1F;

struct SkinScene final
{
    StaticArray<uint8, PIXEL_BYTES> Pixels{};
    renderer::RenderGraphAllocationReport Allocation{};
};

float32 GetChannel(const StaticArray<uint8, PIXEL_BYTES>& pixels, usize column, usize channel)
{
    return static_cast<float32>(pixels[GetPixel(column, ROW) + channel]);
}

// Draw the two quads through the clustered deferred default graph, the left one as `left` and the right one white
// skin, both with the scattering distance times `distanceScale`, lit head-on by one directional light. With
// shouldCheckRejection, then edit the skin to emit, which is drawn, and to be metallic, which is rejected while the
// drawn scene stays published.
SkinScene RenderSkinScene(LeftQuad left, float32 distanceScale, bool shouldCheckRejection = false)
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
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
    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> positions{{{-QUAD_HALF_WIDTH, -QUAD_HALF_HEIGHT, QUAD_DEPTH},
                                                                   {QUAD_HALF_WIDTH, -QUAD_HALF_HEIGHT, QUAD_DEPTH},
                                                                   {QUAD_HALF_WIDTH, QUAD_HALF_HEIGHT, QUAD_DEPTH},
                                                                   {-QUAD_HALF_WIDTH, QUAD_HALF_HEIGHT, QUAD_DEPTH}}};
    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    const StaticArray<uint32, QUAD_INDEX_COUNT> indices{{0, 1, 2, 0, 2, 3}};
    fixture.Meshes[0].Geometry = {positions.GetData(), positions.GetSize(), indices.GetData(), indices.GetSize()};
    fixture.Meshes[0].Geometry.Normals = normals.GetData();
    const uint32 materialIndex = 0;
    fixture.Meshes[0].MaterialBindings = {&materialIndex, 1};
    fixture.Cameras[0].OrthographicCamera = true;

    StaticArray<renderer::SceneShaderMaterial, QUAD_COUNT> materials{};
    const math::Vector3 distance = math::Vector3{RED_DISTANCE, GREEN_DISTANCE, BLUE_DISTANCE} * distanceScale;
    for (renderer::SceneShaderMaterial& material : materials)
    {
        material.Roughness = 1;
        material.Scattering.Model = scene_format::MaterialModel::Skin;
        material.Scattering.K = {{distance.X, distance.Y, distance.Z}};
    }
    materials[0].BaseColor = {DARK, DARK, DARK};
    if (left == LeftQuad::DarkMetallic)
    {
        materials[0].Scattering = {};
    }
    if (left == LeftQuad::ShortDarkSkin)
    {
        materials[0].Scattering.K = {
            {distance.X * SHORT_DISTANCE_SCALE, distance.Y * SHORT_DISTANCE_SCALE, distance.Z * SHORT_DISTANCE_SCALE}};
    }
    materials[1].BaseColor = {WHITE, WHITE, WHITE};
    StaticArray<renderer::RenderGraphItem, QUAD_COUNT> items{{fixture.Items[0], fixture.Items[0]}};
    StaticArray<uint32, QUAD_COUNT> overrides{{0, 1}};
    for (usize quad = 0; quad < QUAD_COUNT; ++quad)
    {
        items[quad].SourceId = quad + 1;
        items[quad].Material.Color = {1, 1, 1, 1};
        items[quad].MaterialOverrides = {&overrides[quad], 1};
    }
    items[0].MeshToWorld.Translation = {-QUAD_HALF_WIDTH, 0, left == LeftQuad::DistantDarkSkin ? .3F : 0};
    items[1].MeshToWorld.Translation = {QUAD_HALF_WIDTH, 0, 0};
    renderer::DirectLight light;
    light.SourceId = 1;
    light.Color = {1, 1, 1};
    light.Direction = {0, 0, -1};
    light.Intensity = LIGHT_INTENSITY;
    light.Kind = renderer::LightKind::Directional;
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    inputs.Lights = {&light, 1};
    uint64 revision = 0;
    const auto update = [&]() {
        renderer::RenderGraphReport report;
        ++revision;
        return poll(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                         {items.GetData(), items.GetSize()},
                                         {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                         revision,
                                         renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                         report,
                                         &inputs,
                                         revision));
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
    REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
    REQUIRE(compositor.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
    const rhi::RasterStatus confirmed = fixture.Graph.ConfirmSubmission(completion);
    REQUIRE((confirmed == rhi::RasterStatus::Ready || confirmed == rhi::RasterStatus::Pending));
    SkinScene scene;
    REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(scene.Pixels)) == true);
    scene.Allocation = fixture.Graph.GetAllocationReport();
    if (shouldCheckRejection == true)
    {
        // Skin may emit like any surface; the G-buffer keeps its profile in the metallic channel, since skin is a
        // dielectric.
        materials[1].Emission = {1, 1, 1};
        CHECK(update() == rhi::RasterStatus::Ready);
        const uint64 drawable = fixture.Graph.GetDrawableSceneRevision();
        materials[1].Emission = {};
        materials[1].Metallic = .5F;
        CHECK(update() != rhi::RasterStatus::Ready);
        CHECK(fixture.Graph.GetDrawableSceneRevision() == drawable);
    }
    return scene;
}

void RequireFlat(const SkinScene& skin, const SkinScene& control, usize column)
{
    for (usize channel = 0; channel < COLOR_CHANNELS; ++channel)
    {
        CHECK(math::Abs(GetChannel(skin.Pixels, column, channel) - GetChannel(control.Pixels, column, channel)) <=
              FLAT_TOLERANCE_CODES);
    }
}
} // namespace

TEST_CASE("Deferred skin spreads diffuse light across an albedo edge and conserves flat lighting",
          "[render-graph][clustered-deferred][skin][gpu]")
{
    const SkinScene skin = RenderSkinScene(LeftQuad::DarkSkin, 1, true);
    const SkinScene control = RenderSkinScene(LeftQuad::DarkSkin, 0);
    // Far from the edge every tap sees the same lighting, so the renormalized kernel returns it unchanged.
    RequireFlat(skin, control, LEFT_INTERIOR_COLUMN);
    RequireFlat(skin, control, RIGHT_INTERIOR_COLUMN);
    // Light from the white side reaches the dark side's first column, red furthest, and leaves the white side's. The
    // spread light carries the square root of each side's albedo, and the receiving column multiplies by its own.
    const float32 redBleed =
        GetChannel(skin.Pixels, LEFT_EDGE_COLUMN, RED) - GetChannel(control.Pixels, LEFT_EDGE_COLUMN, RED);
    const float32 blueBleed =
        GetChannel(skin.Pixels, LEFT_EDGE_COLUMN, BLUE) - GetChannel(control.Pixels, LEFT_EDGE_COLUMN, BLUE);
    constexpr float32 MIN_RED_BLEED_CODES = 6;
    CHECK(redBleed > MIN_RED_BLEED_CODES);
    CHECK(redBleed > blueBleed);
    CHECK(GetChannel(skin.Pixels, RIGHT_EDGE_COLUMN, RED) <
          GetChannel(control.Pixels, RIGHT_EDGE_COLUMN, RED) - MIN_RED_BLEED_CODES / 2);
    // The diffusion targets exist only while a view draws skin with a scattering distance.
    CHECK(skin.Allocation.DeferredSkinAllocations == 3);
    CHECK(skin.Allocation.DeferredSkinAttachmentBytes == uint64{WIDTH} * HEIGHT * 36);
    CHECK(control.Allocation.DeferredSkinAllocations == 0);
    CHECK(control.Allocation.DeferredSkinAttachmentBytes == 0);
    CHECK(skin.Allocation.DeferredAttachmentBytes == control.Allocation.DeferredAttachmentBytes);
}

TEST_CASE("Deferred skin diffusion stops at other materials and at depth discontinuities",
          "[render-graph][clustered-deferred][skin][gpu]")
{
    for (const LeftQuad left : {LeftQuad::DarkMetallic, LeftQuad::DistantDarkSkin})
    {
        // The dark quad's taps never reach the white skin and the white skin's never reach the dark quad, so both
        // edge columns keep their unspread values: nothing bleeds in and the skin edge does not darken.
        const SkinScene skin = RenderSkinScene(left, 1);
        const SkinScene control = RenderSkinScene(left, 0);
        RequireFlat(skin, control, LEFT_EDGE_COLUMN);
        RequireFlat(skin, control, RIGHT_EDGE_COLUMN);
        RequireFlat(skin, control, RIGHT_INTERIOR_COLUMN);
    }
}

TEST_CASE("Deferred skin spreads each material by its own scattering distance",
          "[render-graph][clustered-deferred][skin][gpu]")
{
    // The dark skin takes its own profile, a tenth of the white skin's distance, so far less light crosses into it
    // than when both share the white skin's profile; the white side spreads by its own profile either way.
    const SkinScene shared = RenderSkinScene(LeftQuad::DarkSkin, 1);
    const SkinScene own = RenderSkinScene(LeftQuad::ShortDarkSkin, 1);
    const SkinScene control = RenderSkinScene(LeftQuad::DarkSkin, 0);
    const float32 sharedBleed =
        GetChannel(shared.Pixels, LEFT_EDGE_COLUMN, RED) - GetChannel(control.Pixels, LEFT_EDGE_COLUMN, RED);
    const float32 ownBleed =
        GetChannel(own.Pixels, LEFT_EDGE_COLUMN, RED) - GetChannel(control.Pixels, LEFT_EDGE_COLUMN, RED);
    CHECK(ownBleed < sharedBleed / 2);
    RequireFlat(own, shared, RIGHT_EDGE_COLUMN);
    RequireFlat(own, control, LEFT_INTERIOR_COLUMN);
}
