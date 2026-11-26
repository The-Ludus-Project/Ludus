// Raster skin diffusion pixels: the clustered deferred default graph spreads a skin material's diffuse lighting over
// its scattering distance (renderer_skin_diffusion.luslh) and leaves flat lighting, other materials and surfaces at
// another depth as they were. Each scene is compared with the same scene at zero scattering distance, which takes the
// ordinary resolve. The kernel itself is checked on the CPU in skin_diffusion_tests.cpp. The LUSL diffusion recipes
// must draw their Slang originals' pixels (LUSL L5d wave 2).

#include "../shaders/skin_diffusion_layout.h"
#include "../src/internal/render_graph_deferred.h"
#include "baseline_renderer_skin_blur.h"
#include "baseline_renderer_skin_composite.h"
#include "recipe_ab_timing.h"
#include "render_graph_gpu_fixture.h"
#include "renderer_composite.h"
#include "renderer_skin_blur.h"
#include "renderer_skin_blur_slots.h"
#include "renderer_skin_composite.h"
#include "renderer_skin_composite_slots.h"

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
    GlossyBlackSkin, ///< Glossy black skin beside rough black skin: only specular light, with a sharp edge.
};
// Roughness of the glossy black skin; the other quads are fully rough.
constexpr float32 GLOSSY_ROUGHNESS = .3F;
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

// The two quads drawn through the clustered deferred default graph on a borrowed device, the left one as `left` and
// the right one white skin, both with the scattering distance times `distanceScale`, lit head-on by one directional
// light. Construction prepares the scene and the presentation; Draw encodes one frame into the surface.
/// Pixel extent of a skin graph's target.
struct SkinExtent final
{
    uint32 Width = WIDTH;
    uint32 Height = HEIGHT;
};
struct SkinGraph final
{
    SkinGraph(rhi::DeviceHandle device, LeftQuad left, float32 distanceScale, SkinExtent extent)
        : Device(device), Width(extent.Width), Height(extent.Height)
    {
        Package = LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
        Programs = Package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
        REQUIRE(Programs.HasClusteredDeferred == true);
        REQUIRE(Poll(Graph.Initialize(Device, Package->GetPlan(), Programs, nullptr, Width, Height)) ==
                rhi::RasterStatus::Ready);
        Meshes[0].Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Meshes[0].Geometry.Normals = Normals.GetData();
        Meshes[0].MaterialBindings = {&MaterialIndex, 1};
        Cameras[0].OrthographicCamera = true;

        const math::Vector3 distance = math::Vector3{RED_DISTANCE, GREEN_DISTANCE, BLUE_DISTANCE} * distanceScale;
        for (renderer::SceneShaderMaterial& material : Materials)
        {
            material.Roughness = 1;
            material.Scattering.Model = scene_format::MaterialModel::Skin;
            material.Scattering.K = {{distance.X, distance.Y, distance.Z}};
        }
        Materials[0].BaseColor = {DARK, DARK, DARK};
        if (left == LeftQuad::DarkMetallic)
        {
            Materials[0].Scattering = {};
        }
        if (left == LeftQuad::ShortDarkSkin)
        {
            Materials[0].Scattering.K = {{distance.X * SHORT_DISTANCE_SCALE,
                                          distance.Y * SHORT_DISTANCE_SCALE,
                                          distance.Z * SHORT_DISTANCE_SCALE}};
        }
        Materials[1].BaseColor = {WHITE, WHITE, WHITE};
        if (left == LeftQuad::GlossyBlackSkin)
        {
            Materials[0].BaseColor = {};
            Materials[0].Roughness = GLOSSY_ROUGHNESS;
            Materials[1].BaseColor = {};
        }
        for (usize quad = 0; quad < QUAD_COUNT; ++quad)
        {
            Items[quad].SourceId = quad + 1;
            Items[quad].Material.Color = {1, 1, 1, 1};
            Items[quad].MaterialOverrides = {&Overrides[quad], 1};
        }
        Items[0].MeshToWorld.Translation = {-QUAD_HALF_WIDTH, 0, left == LeftQuad::DistantDarkSkin ? .3F : 0};
        Items[1].MeshToWorld.Translation = {QUAD_HALF_WIDTH, 0, 0};
        Light.SourceId = 1;
        Light.Color = {1, 1, 1};
        Light.Direction = {0, 0, -1};
        Light.Intensity = LIGHT_INTENSITY;
        Light.Kind = renderer::LightKind::Directional;
        Inputs.Materials = {Materials.GetData(), Materials.GetSize()};
        Inputs.Lights = {&Light, 1};
        REQUIRE(Update() == rhi::RasterStatus::Ready);

        rhi::TextureHandle texture;
        rhi::TextureAspect aspect;
        REQUIRE(Graph.GetExportAllocation(0, texture, aspect) == rhi::RasterStatus::Ready);
        REQUIRE(Compositor.Initialize(Device, Programs.ColorVertex, Programs.ColorFragment) ==
                rhi::RasterStatus::Ready);
        REQUIRE(Compositor.InitializeViews(shaders::renderer_composite::Vertex(),
                                           shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
        renderer::ViewMapping mapping;
        REQUIRE(renderer::ResolveViewMapping(
                    {static_cast<float32>(Width), static_cast<float32>(Height), renderer::ScreenFit::Stretch},
                    {0, 0, Width, Height},
                    Width,
                    Height,
                    mapping) == rhi::RasterStatus::Ready);
        REQUIRE(Compositor.PreparePresentation(texture, mapping, Presentation) == rhi::RasterStatus::Ready);
    }
    SkinGraph(const SkinGraph&) = delete;
    SkinGraph& operator=(const SkinGraph&) = delete;

    rhi::RasterStatus Poll(rhi::RasterStatus status)
    {
        for (usize attempt = 0; attempt < POLL_ATTEMPTS && status == rhi::RasterStatus::Pending; ++attempt)
        {
            threading::SleepFor(timing::Milliseconds(1));
            status = Graph.Poll();
        }
        return status;
    }
    // Publish the current materials as the next scene revision.
    rhi::RasterStatus Update()
    {
        renderer::RenderGraphReport report;
        ++Revision;
        return Poll(Graph.Update({Meshes.GetData(), Meshes.GetSize()},
                                 {Items.GetData(), Items.GetSize()},
                                 {Cameras.GetData(), Cameras.GetSize()},
                                 Revision,
                                 renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                 report,
                                 &Inputs,
                                 Revision));
    }
    // Encode one frame of the graph and its presentation into `surface`, which the caller reads back.
    void Draw(rhi::SurfaceHandle surface)
    {
        REQUIRE(rhi::BeginFrame(Device, surface) == rhi::DeviceStatus::Ready);
        REQUIRE(Graph.Encode(surface) == rhi::RasterStatus::Ready);
        REQUIRE(Compositor.DrawPresentation(Presentation) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(rhi::EndFrame(Device, completion) == rhi::RasterStatus::Ready);
        const rhi::RasterStatus confirmed = Graph.ConfirmSubmission(completion);
        REQUIRE((confirmed == rhi::RasterStatus::Ready || confirmed == rhi::RasterStatus::Pending));
    }

    rhi::DeviceHandle Device{};
    uint32 Width = 0;
    uint32 Height = 0;
    core::UniquePtr<renderer::RenderGraphPackage> Package;
    renderer::RenderGraphPrograms Programs{};
    renderer::RenderGraph Graph;
    StaticArray<math::Vector3, QUAD_VERTEX_COUNT> Positions{{{-QUAD_HALF_WIDTH, -QUAD_HALF_HEIGHT, QUAD_DEPTH},
                                                             {QUAD_HALF_WIDTH, -QUAD_HALF_HEIGHT, QUAD_DEPTH},
                                                             {QUAD_HALF_WIDTH, QUAD_HALF_HEIGHT, QUAD_DEPTH},
                                                             {-QUAD_HALF_WIDTH, QUAD_HALF_HEIGHT, QUAD_DEPTH}}};
    StaticArray<math::Vector3, QUAD_VERTEX_COUNT> Normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    StaticArray<uint32, QUAD_INDEX_COUNT> Indices{{0, 1, 2, 0, 2, 3}};
    uint32 MaterialIndex = 0;
    StaticArray<renderer::RenderGraphMesh, 1> Meshes{};
    StaticArray<renderer::ViewDescription, 1> Cameras{};
    StaticArray<renderer::SceneShaderMaterial, QUAD_COUNT> Materials{};
    StaticArray<renderer::RenderGraphItem, QUAD_COUNT> Items{};
    StaticArray<uint32, QUAD_COUNT> Overrides{{0, 1}};
    renderer::DirectLight Light;
    renderer::SceneShaderInputs Inputs;
    uint64 Revision = 0;
    renderer::Renderer Compositor;
    renderer::Presentation Presentation;
};

// Draw the skin scene (SkinGraph) at the fixture's extent. With shouldCheckRejection, then edit the skin to emit,
// which is drawn, and to be metallic, which is rejected while the drawn scene stays published.
SkinScene RenderSkinScene(LeftQuad left, float32 distanceScale, bool shouldCheckRejection = false)
{
    GraphGpuFixture fixture;
    SkinGraph skin(fixture.Device, left, distanceScale, {});
    skin.Draw(fixture.Surface);
    SkinScene scene;
    REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(scene.Pixels)) == true);
    scene.Allocation = skin.Graph.GetAllocationReport();
    if (shouldCheckRejection == true)
    {
        // Skin may emit like any surface; the G-buffer keeps its profile in the metallic channel, since skin is a
        // dielectric.
        skin.Materials[1].Emission = {1, 1, 1};
        CHECK(skin.Update() == rhi::RasterStatus::Ready);
        const uint64 drawable = skin.Graph.GetDrawableSceneRevision();
        skin.Materials[1].Emission = {};
        skin.Materials[1].Metallic = .5F;
        CHECK(skin.Update() != rhi::RasterStatus::Ready);
        CHECK(skin.Graph.GetDrawableSceneRevision() == drawable);
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

TEST_CASE("Deferred skin keeps specular detail sharp", "[render-graph][clustered-deferred][skin][gpu]")
{
    // Both quads are black skin, the left glossy and the right rough, so all their light is specular. The diffusion
    // spreads diffuse light only: the highlight's edge stays where it is, and each side keeps its own value.
    const SkinScene skin = RenderSkinScene(LeftQuad::GlossyBlackSkin, 1);
    const SkinScene control = RenderSkinScene(LeftQuad::GlossyBlackSkin, 0);
    constexpr float32 MIN_SPECULAR_EDGE_CODES = 20;
    CHECK(GetChannel(control.Pixels, LEFT_EDGE_COLUMN, RED) - GetChannel(control.Pixels, RIGHT_EDGE_COLUMN, RED) >
          MIN_SPECULAR_EDGE_CODES);
    RequireFlat(skin, control, LEFT_EDGE_COLUMN);
    RequireFlat(skin, control, RIGHT_EDGE_COLUMN);
    RequireFlat(skin, control, LEFT_INTERIOR_COLUMN);
    RequireFlat(skin, control, RIGHT_INTERIOR_COLUMN);
    // The diffusion ran for this view.
    CHECK(skin.Allocation.DeferredSkinAllocations == 3);
}

namespace
{
namespace blur_slots = shaders::renderer_skin_blur::slots;
namespace composite_slots = shaders::renderer_skin_composite::slots;
// Both recipes keep the Slang original's manual layout, so the renderer's one binding table also binds the original.
static_assert(composite_slots::SKIN_SOURCE_TEXTURE == LUDUS_SKIN_SOURCE_BINDING &&
              composite_slots::SKIN_RADIANCE_TEXTURE == LUDUS_SKIN_RADIANCE_BINDING &&
              composite_slots::SKIN_NORMAL_TEXTURE == LUDUS_SKIN_NORMAL_BINDING &&
              composite_slots::SKIN_BASE_TEXTURE == LUDUS_SKIN_BASE_BINDING &&
              composite_slots::SKIN_DEPTH_TEXTURE == LUDUS_SKIN_DEPTH_BINDING &&
              composite_slots::SKIN_VIEW == LUDUS_SKIN_VIEW_BINDING &&
              composite_slots::SKIN_KERNEL == LUDUS_SKIN_KERNEL_BINDING &&
              composite_slots::SKIN_PROFILES == LUDUS_SKIN_PROFILE_BINDING);
static_assert(blur_slots::SKIN_SOURCE_TEXTURE == LUDUS_SKIN_SOURCE_BINDING &&
              blur_slots::SKIN_NORMAL_TEXTURE == LUDUS_SKIN_NORMAL_BINDING &&
              blur_slots::SKIN_BASE_TEXTURE == LUDUS_SKIN_BASE_BINDING &&
              blur_slots::SKIN_DEPTH_TEXTURE == LUDUS_SKIN_DEPTH_BINDING &&
              blur_slots::SKIN_VIEW == LUDUS_SKIN_VIEW_BINDING &&
              blur_slots::SKIN_KERNEL == LUDUS_SKIN_KERNEL_BINDING &&
              blur_slots::SKIN_PROFILES == LUDUS_SKIN_PROFILE_BINDING);
// Red bleed, in RGBA8 codes, that shows the diffusion passes ran at the dark side's edge column.
constexpr float32 PARITY_BLEED_CODES = 6;
constexpr uint32 BENCHMARK_WIDTH = 1920;
constexpr uint32 BENCHMARK_HEIGHT = 1080;
constexpr SkinExtent BENCHMARK_EXTENT{BENCHMARK_WIDTH, BENCHMARK_HEIGHT};

// Draws later skin scenes with `programs` in place of the built-in recipes until destroyed or Reset.
struct SkinShaderOverride final
{
    explicit SkinShaderOverride(const renderer::internal::SkinDiffusionShaders& programs) noexcept
    {
        renderer::internal::SetSkinDiffusionShadersForTesting(&programs);
    }
    ~SkinShaderOverride() noexcept
    {
        Reset();
    }
    SkinShaderOverride(const SkinShaderOverride&) = delete;
    SkinShaderOverride& operator=(const SkinShaderOverride&) = delete;
    void Reset() noexcept
    {
        renderer::internal::SetSkinDiffusionShadersForTesting(nullptr);
    }
};

renderer::internal::SkinDiffusionShaders GetSlangSkinShaders() noexcept
{
    return {shaders::baseline_renderer_skin_blur::Vertex(),
            shaders::baseline_renderer_skin_blur::Fragment(),
            shaders::baseline_renderer_skin_composite::Fragment()};
}

// Whether every binding `original` reflects is declared at the same slot with the same kind by `port`.
bool HasBindingsOf(const rhi::RasterShaderDescription& port, const rhi::RasterShaderDescription& original) noexcept
{
    for (const rhi::RasterBinding& binding : original.Bindings)
    {
        bool isDeclared = false;
        for (const rhi::RasterBinding& candidate : port.Bindings)
        {
            isDeclared = isDeclared || (candidate.Binding == binding.Binding && candidate.Kind == binding.Kind);
        }
        if (isDeclared == false)
        {
            return false;
        }
    }
    return true;
}

usize CountDifferingBytes(const SkinScene& left, const SkinScene& right) noexcept
{
    usize differing = 0;
    for (usize i = 0; i < PIXEL_BYTES; ++i)
    {
        differing += left.Pixels[i] != right.Pixels[i] ? 1 : 0;
    }
    return differing;
}
} // namespace

TEST_CASE("The LUSL skin diffusion recipes render their Slang originals' pixels",
          "[render-graph][clustered-deferred][skin][gpu][lusl]")
{
    // LUSL L5d wave 2: every skin scene drawn with renderer_skin_blur and renderer_skin_composite and with their Slang
    // originals (shaders/renderer_skin_blur.slang, renderer_skin_composite.slang) on one device; every byte must
    // match. With LUDUS_RENDERER_BENCHMARK set it also times the two programs A/B in 1080p frames.
    CHECK(HasBindingsOf(shaders::renderer_skin_blur::Fragment(), shaders::baseline_renderer_skin_blur::Fragment()));
    CHECK(HasBindingsOf(shaders::renderer_skin_composite::Fragment(),
                        shaders::baseline_renderer_skin_composite::Fragment()));
    CHECK(HasBindingsOf(shaders::baseline_renderer_skin_composite::Fragment(),
                        shaders::renderer_skin_composite::Fragment()));
    const renderer::internal::SkinDiffusionShaders slangShaders = GetSlangSkinShaders();
    for (const LeftQuad left : {LeftQuad::DarkSkin,
                                LeftQuad::DarkMetallic,
                                LeftQuad::DistantDarkSkin,
                                LeftQuad::ShortDarkSkin,
                                LeftQuad::GlossyBlackSkin})
    {
        const SkinScene lusl = RenderSkinScene(left, 1);
        SkinScene slang;
        {
            const SkinShaderOverride slangOverride(slangShaders);
            slang = RenderSkinScene(left, 1);
        }
        CHECK(CountDifferingBytes(lusl, slang) == 0);
        // The diffusion passes ran for both programs.
        CHECK(lusl.Allocation.DeferredSkinAllocations == 3);
        CHECK(slang.Allocation.DeferredSkinAllocations == 3);
    }
    // Equality is not vacuous: the spread light reaches the dark side's edge column.
    const SkinScene lusl = RenderSkinScene(LeftQuad::DarkSkin, 1);
    const SkinScene control = RenderSkinScene(LeftQuad::DarkSkin, 0);
    CHECK(GetChannel(lusl.Pixels, LEFT_EDGE_COLUMN, RED) - GetChannel(control.Pixels, LEFT_EDGE_COLUMN, RED) >
          PARITY_BLEED_CODES);
    // And the override reaches the composite stage: drawing it with the horizontal blur's fragment changes the image.
    renderer::internal::SkinDiffusionShaders mutant = slangShaders;
    mutant.Composite = shaders::baseline_renderer_skin_blur::Fragment();
    SkinScene mutated;
    {
        const SkinShaderOverride mutantOverride(mutant);
        mutated = RenderSkinScene(LeftQuad::DarkSkin, 1);
    }
    CHECK(CountDifferingBytes(lusl, mutated) > 0);

    if (IsRecipeBenchmark() == false)
    {
        return;
    }
    RhiGuard session;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.EnableGpuTiming = true;
    rhi::StartupInfo failure;
    REQUIRE(rhi::CreateDevice({},
                              { .Width = BENCHMARK_WIDTH, .Height = BENCHMARK_HEIGHT },
                              description,
                              device,
                              surface,
                              &failure) == rhi::DeviceStatus::Ready);
    // Each graph creates its skin programs on first use, so the Slang graph draws once under the override.
    SkinShaderOverride slangOverride(slangShaders);
    SkinGraph slangGraph(device, LeftQuad::DarkSkin, 1, BENCHMARK_EXTENT);
    Array<uint8> pixels;
    REQUIRE(pixels.TryResize(static_cast<usize>(BENCHMARK_WIDTH) * BENCHMARK_HEIGHT * TEXEL_BYTES) == true);
    slangGraph.Draw(surface);
    REQUIRE(rhi::backend::ReadHeadlessPixels({pixels.GetData(), pixels.GetSize()}) == true);
    slangOverride.Reset();
    SkinGraph luslGraph(device, LeftQuad::DarkSkin, 1, BENCHMARK_EXTENT);
    StaticArray<SkinGraph*, RECIPE_AB_VARIANTS> graphs{};
    graphs[RECIPE_AB_SLANG] = &slangGraph;
    graphs[RECIPE_AB_LUSL] = &luslGraph;
    RunRecipeAbTiming(device, "skin_diffusion", BENCHMARK_WIDTH, BENCHMARK_HEIGHT, [&](usize variant) {
        graphs[variant]->Draw(surface);
        REQUIRE(rhi::backend::ReadHeadlessPixels({pixels.GetData(), pixels.GetSize()}) == true);
    });
}
