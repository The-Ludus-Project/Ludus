#include "internal/readback.h"
#include "l0_scene.h"
#include "l1_scene.h"
#include "l2_scene.h"
#include "l4_scene.h"
#include "l5_scene.h"
#include "renderer_clustered.h"
#include "renderer_clustered_shadow.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_grid.h"
#include "renderer_lit.h"
#include "renderer_lit_shadow.h"
#include "renderer_output.h"
#include "renderer_overlay.h"
#include "renderer_shadow.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>
namespace rhi = ludus::graphics::rhi;
namespace rr = ludus::graphics::renderer;
using namespace ludus::foundation;
TEST_CASE("L0 optimized packets match unsorted direct pixels with depth, errors and painter overlays",
          "[renderer][gpu]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const auto started = rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, description, device, surface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter on this host");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rr::Renderer renderer;
    REQUIRE(renderer.Initialize(device,
                                ludus::shaders::renderer_flat::Vertex(),
                                ludus::shaders::renderer_flat::Fragment()) == rhi::RasterStatus::Ready);
    rr::Mesh quad, triangle;
    REQUIRE(renderer.CreateMesh({ludus::qa::L0_QUAD, 4, ludus::qa::L0_INDICES, 6}, quad) == rhi::RasterStatus::Ready);
    REQUIRE(renderer.CreateMesh({ludus::qa::L0_TRIANGLE, 3, ludus::qa::L0_INDICES, 3}, triangle) ==
            rhi::RasterStatus::Ready);
    uint8 images[2][96 * 64 * 4]{};
    rr::ViewReport reports[2];
    for (usize variant = 0; variant < 2; ++variant)
    {
        rr::Snapshot snapshot;
        rr::PreparedView view;
        REQUIRE(ludus::qa::BuildL0Scene(renderer, quad, triangle, variant == 1, snapshot, view, reports[variant]) ==
                rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(renderer.Submit(view, surface, completion) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::backend::ReadHeadlessPixels(images[variant]));
        REQUIRE(renderer.Release(view) == rhi::RasterStatus::Ready);
        REQUIRE(renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    }
    CHECK(reports[0].Culled == 1);
    CHECK(reports[0].Draws < reports[1].Draws);
    CHECK(std::memcmp(images[0], images[1], sizeof(images[0])) == 0);
    // Independent interior samples prevent a blank/equally broken image from passing.
    const usize red = (usize{20} * 96 + 24) * 4;
    CHECK(images[0][red] == 255);
    CHECK(images[0][red + 1] == 0);
    const usize green = (usize{20} * 96 + 72) * 4;
    CHECK(images[0][green + 1] == 255);
    const usize background = (usize{32} * 96 + 12) * 4;
    CHECK(images[0][background] == 25);
    CHECK(images[0][background + 1] == 26);
    CHECK(images[0][background + 2] == 52);
    const usize overlay = (usize{32} * 96 + 48) * 4;
    CHECK(images[0][overlay] == 70);
    CHECK(images[0][overlay + 1] == 134);
    CHECK(images[0][overlay + 2] == 13);
    const usize error = (usize{45} * 96 + 48) * 4;
    CHECK(images[0][error] == 255);
    CHECK(images[0][error + 2] == 255);
}
TEST_CASE("L1 offscreen split views preserve neighbors, reverse-Z order and top-left UVs", "[renderer][views][gpu]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const auto started = rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, description, device, surface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter on this host");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rr::Renderer renderer;
    REQUIRE(renderer.Initialize(device,
                                ludus::shaders::renderer_flat::Vertex(),
                                ludus::shaders::renderer_flat::Fragment()) == rhi::RasterStatus::Ready);
    REQUIRE(renderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                     ludus::shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    rr::Mesh mesh;
    REQUIRE(renderer.CreateMesh({ludus::qa::L1_QUAD, 4, ludus::qa::L1_INDICES, 6}, mesh) == rhi::RasterStatus::Ready);
    rr::Snapshot snapshot;
    REQUIRE(ludus::qa::BuildL1Snapshot(renderer, mesh, snapshot) == rhi::RasterStatus::Ready);
    rhi::TextureHandle targets[2];
    for (auto& target : targets)
    {
        REQUIRE(rhi::CreateTexture(device, {48, 48, rhi::RasterFormat::Rgba8Unorm, true}, {}, target) ==
                rhi::RasterStatus::Ready);
    }
    rr::Presentation presentations[2];
    REQUIRE(ludus::qa::PrepareL1Presentations(renderer, targets, 96, 64, presentations) == rhi::RasterStatus::Ready);
    uint8 images[2][96 * 64 * 4]{};
    for (usize variant = 0; variant < 2; ++variant)
    {
        rr::PreparedView views[2];
        rr::ViewReport report;
        for (usize i = 0; i < 2; ++i)
        {
            REQUIRE(ludus::qa::BuildL1View(renderer, snapshot, i == 1, variant == 1, variant == 1, views[i], report) ==
                    rhi::RasterStatus::Ready);
        }
        REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
        REQUIRE(ludus::qa::DrawL1Frame(renderer, views, targets, presentations) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(rhi::EndFrame(device, completion) == rhi::RasterStatus::Ready);
        for (auto& view : views)
        {
            REQUIRE(renderer.Release(view) == rhi::RasterStatus::Ready);
        }
        REQUIRE(rhi::backend::ReadHeadlessPixels(images[variant]));
    }
    CHECK(std::memcmp(images[0], images[1], sizeof(images[0])) == 0);
    const auto color = [&](uint32 x, uint32 y, const core::StaticArray<uint8, 3>& rgb) {
        const auto offset = (static_cast<usize>(y) * 96 + static_cast<usize>(x)) * 4;
        CHECK(images[0][offset] == rgb[0]);
        CHECK(images[0][offset + 1] == rgb[1]);
        CHECK(images[0][offset + 2] == rgb[2]);
        CHECK(images[0][offset + 3] == 255);
    };
    color(24, 32, {255, 0, 0});
    color(72, 32, {255, 0, 0});
    color(4, 12, {0, 255, 0});
    color(52, 12, {0, 0, 255});
    color(42, 16, {255, 0, 255});
    color(90, 16, {255, 0, 255});
    color(42, 48, {0, 255, 0});
    color(90, 48, {0, 0, 255});
    color(24, 2, {0, 0, 0});
    color(72, 2, {0, 0, 0});
}
TEST_CASE("L1 explicit viewport/scissor preserves the outside surface and empty scissors draw nothing",
          "[renderer][views][gpu]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const auto started = rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, description, device, surface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter on this host");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rr::Renderer renderer;
    REQUIRE(renderer.Initialize(device,
                                ludus::shaders::renderer_flat::Vertex(),
                                ludus::shaders::renderer_flat::Fragment()) == rhi::RasterStatus::Ready);
    REQUIRE(renderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                     ludus::shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    rr::Mesh mesh;
    REQUIRE(renderer.CreateMesh({ludus::qa::L1_QUAD, 4, ludus::qa::L1_INDICES, 6}, mesh) == rhi::RasterStatus::Ready);
    rr::SceneItem item;
    item.Geometry = mesh;
    item.SourceId = 1;
    item.Transform.Translation.Z = .5F;
    rr::Snapshot snapshot;
    REQUIRE(renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::ViewDescription viewDescription;
    viewDescription.Depth = rr::DepthConvention::ReverseZ;
    rr::PreparedView view;
    rr::ViewReport report;
    REQUIRE(renderer.PrepareView(snapshot, viewDescription, view, report) == rhi::RasterStatus::Ready);
    for (const bool empty : {false, true})
    {
        REQUIRE(rhi::SetFrameTarget(device, surface, {96, 64, 1, 1, 0, 1}) == rhi::DeviceStatus::Ready);
        REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
        rhi::RasterPassDescription pass;
        pass.ColorLoad = rhi::RasterLoad::Load;
        pass.UseViewport = pass.UseScissor = true;
        pass.Viewport = {0, 0, 24, 24};
        pass.Scissor = {3, 3, empty ? 0U : 6U, 6};
        REQUIRE(renderer.DrawView(view, pass) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(rhi::EndFrame(device, completion) == rhi::RasterStatus::Ready);
        uint8 pixels[96 * 64 * 4];
        REQUIRE(rhi::backend::ReadHeadlessPixels(pixels));
        const auto inside = (usize{4} * 96 + 4) * 4;
        CHECK(pixels[inside] == 255);
        CHECK(pixels[inside + 1] == (empty ? 255 : 0));
        CHECK(pixels[inside + 2] == (empty ? 0 : 255));
        for (const auto offset : {(usize{12} * 96 + 12) * 4, (usize{50} * 96 + 50) * 4})
        {
            CHECK(pixels[offset] == 255);
            CHECK(pixels[offset + 1] == 255);
            CHECK(pixels[offset + 2] == 0);
        }
    }
}
TEST_CASE("L1 Fill crops logical offscreen pixels once and matches direct output sampling", "[renderer][views][gpu]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    rhi::DeviceDescription deviceDescription;
    deviceDescription.Required.PortableRaster = true;
    const auto started =
        rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, deviceDescription, device, surface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter on this host");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rr::Renderer renderer;
    REQUIRE(renderer.Initialize(device,
                                ludus::shaders::renderer_flat::Vertex(),
                                ludus::shaders::renderer_flat::Fragment()) == rhi::RasterStatus::Ready);
    REQUIRE(renderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                     ludus::shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    rr::Mesh mesh;
    REQUIRE(renderer.CreateMesh({ludus::qa::L1_QUAD, 4, ludus::qa::L1_INDICES, 6}, mesh) == rhi::RasterStatus::Ready);
    rr::Snapshot snapshot;
    REQUIRE(ludus::qa::BuildL1Snapshot(renderer, mesh, snapshot) == rhi::RasterStatus::Ready);
    rhi::TextureHandle target;
    REQUIRE(rhi::CreateTexture(device, {96, 48, rhi::RasterFormat::Rgba8Unorm, true}, {}, target) ==
            rhi::RasterStatus::Ready);
    rr::ViewMapping mapping;
    REQUIRE(rr::ResolveViewMapping({96, 48, rr::ScreenFit::Fill}, {0, 0, 96, 64}, 96, 64, mapping) ==
            rhi::RasterStatus::Ready);
    rr::Presentation presentation;
    REQUIRE(renderer.PreparePresentation(target, mapping, presentation) == rhi::RasterStatus::Ready);
    rr::ProjectionDescription lens;
    lens.Kind = rr::ProjectionKind::Orthographic;
    lens.OrthographicHeight = 4;
    lens.Near = 1;
    lens.Far = 10;
    for (const auto space : {rr::ProjectionSpace::LogicalScreen, rr::ProjectionSpace::OutputRegion})
    {
        rr::ViewDescription description;
        REQUIRE(rr::BuildViewDescription(lens, mapping, description, space) == rhi::RasterStatus::Ready);
        rr::PreparedView view;
        rr::ViewReport report;
        REQUIRE(renderer.PrepareView(snapshot, description, view, report) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
        rhi::RasterPassDescription pass;
        if (space == rr::ProjectionSpace::LogicalScreen)
        {
            pass.Color = target;
        }
        else
        {
            pass.UseViewport = pass.UseScissor = true;
            pass.Viewport = mapping.Viewport;
            pass.Scissor = mapping.Region;
        }
        REQUIRE(renderer.DrawView(view, pass) == rhi::RasterStatus::Ready);
        if (space == rr::ProjectionSpace::LogicalScreen)
        {
            REQUIRE(renderer.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
        }
        rhi::SubmissionToken completion;
        REQUIRE(rhi::EndFrame(device, completion) == rhi::RasterStatus::Ready);
        REQUIRE(renderer.Release(view) == rhi::RasterStatus::Ready);
        uint8 pixels[96 * 64 * 4]{};
        REQUIRE(rhi::backend::ReadHeadlessPixels(pixels));
        const auto sample = [&](uint32 x, uint32 y, const core::StaticArray<uint8, 3>& rgb) {
            const auto offset = (static_cast<usize>(y) * 96 + x) * 4;
            CHECK(pixels[offset] == rgb[0]);
            CHECK(pixels[offset + 1] == rgb[1]);
            CHECK(pixels[offset + 2] == rgb[2]);
        };
        sample(48, 32, {255, 0, 0});
        sample(28, 32, {0, 255, 0}); // A second crop would incorrectly widen red into this sample.
        sample(8, 32, {0, 0, 255});
        sample(89, 11, {255, 0, 255});
    }
}
TEST_CASE("L2 R8 coverage orientation, painter scissors and clipped thick lines have independent pixel oracles",
          "[renderer][l2][gpu]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const auto started = rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, description, device, surface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter on this host");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rr::OverlayRenderer overlays;
    const auto initialized = overlays.Initialize(device,
                                                 ludus::shaders::renderer_overlay::Vertex(),
                                                 ludus::shaders::renderer_overlay::Fragment());
    REQUIRE((initialized == rhi::RasterStatus::Ready || initialized == rhi::RasterStatus::Pending));
    auto setup = initialized;
    for (usize i = 0; i < 100000 && setup == rhi::RasterStatus::Pending; ++i)
    {
        setup = overlays.Poll();
    }
    REQUIRE(setup == rhi::RasterStatus::Ready);
    rr::OverlayList list;
    ludus::graphics::text::CoverageAtlas atlas;
    REQUIRE(ludus::qa::BuildL2List(list, atlas) == rhi::RasterStatus::Ready);
    rhi::TextureHandle texture;
    const auto published = atlas.Publish(device, texture);
    REQUIRE((published == rhi::RasterStatus::Ready || published == rhi::RasterStatus::Pending));
    bool textureReady = false;
    for (usize i = 0; i < 100000 && textureReady == false; ++i)
    {
        REQUIRE(overlays.Poll() == rhi::RasterStatus::Ready);
        const auto status = rhi::GetStatus(device, texture);
        REQUIRE((status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending));
        textureReady = status == rhi::RasterStatus::Ready;
    }
    REQUIRE(textureReady);
    rr::PreparedOverlay prepared;
    const auto preparedStatus = overlays.Prepare(list, texture, prepared);
    REQUIRE((preparedStatus == rhi::RasterStatus::Ready || preparedStatus == rhi::RasterStatus::Pending));
    // Bounded nonblocking poll; native transfer completion may lag submission.
    bool ready = false;
    for (usize i = 0; i < 100000 && ready == false; ++i)
    {
        REQUIRE(overlays.Poll() == rhi::RasterStatus::Ready);
        const auto status = overlays.GetStatus(prepared);
        REQUIRE((status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending));
        ready = status == rhi::RasterStatus::Ready;
    }
    REQUIRE(ready);
    // Logical atlas/CPU release cannot invalidate prepared version.
    REQUIRE(rhi::Destroy(device, texture) == rhi::RasterStatus::Ready);
    atlas.Reset();
    list.Clear();
    REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    rhi::RasterPassDescription pass;
    pass.DepthStore = rhi::RasterStore::Store;
    REQUIRE(rhi::BeginRasterPass(device, pass) == rhi::RasterStatus::Ready);
    REQUIRE(overlays.Draw(prepared, pass) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(device, completion) == rhi::RasterStatus::Ready);
    uint8 image[96 * 64 * 4];
    REQUIRE(rhi::backend::ReadHeadlessPixels(image));
    const auto color = [&](uint32 x, uint32 y, const uint8(&rgb)[3]) {
        const usize offset = (static_cast<usize>(y) * 96 + x) * 4;
        CHECK(image[offset] == rgb[0]);
        CHECK(image[offset + 1] == rgb[1]);
        CHECK(image[offset + 2] == rgb[2]);
    };
    color(8, 48, {255, 0, 0});
    // UNorm destination-alpha blend precision differs: llvmpipe gives red 127,
    // Metal/Chromium give 128. Bound only that observed one-code-value rounding.
    const usize blend = (usize{48} * 96 + 24) * 4;
    CHECK(image[blend] >= 127);
    CHECK(image[blend] <= 128);
    CHECK(image[blend + 1] == 128);
    CHECK(image[blend + 2] == 0);
    color(40, 48, {0, 0, 0});
    color(80, 4, {255, 255, 255});
    color(80, 28, {0, 0, 0});
    color(60, 16, {0, 0, 255});
    color(32, 16, {0, 0, 0});
    REQUIRE(overlays.Release(prepared) == rhi::RasterStatus::Ready);
}
#include "l3_color.h"
#include "l3_scene.h"
#include "renderer_material.h"
TEST_CASE("L3 native material reload, color mips, mask and prewarmed targets preserve independent pixels",
          "[renderer][gpu][l3]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    rhi::DeviceDescription required;
    required.Required.PortableRaster = true;
    const auto started = rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, required, device, surface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rr::Renderer renderer;
    REQUIRE(renderer.Initialize(device,
                                ludus::shaders::renderer_flat::Vertex(),
                                ludus::shaders::renderer_flat::Fragment()) == rhi::RasterStatus::Ready);
    REQUIRE(renderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                     ludus::shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    const auto wait = [&](auto handle) {
        auto status = renderer.GetStatus(handle);
        for (usize i = 0; i < 100000 && status == rhi::RasterStatus::Pending; ++i)
        {
            status = renderer.GetStatus(handle);
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
    };
    rr::MaterialProgram program;
    const auto warmed = renderer.PrewarmMaterialProgram(ludus::shaders::renderer_material::Vertex(),
                                                        ludus::shaders::renderer_material::Fragment(),
                                                        program);
    REQUIRE((warmed == rhi::RasterStatus::Pending || warmed == rhi::RasterStatus::Ready));
    wait(program);
    rhi::TextureHandle texture, mask;
    const auto uploaded = rhi::CreateTexture(device,
                                             ludus::textures::l3_color::Description(),
                                             ludus::textures::l3_color::Upload(),
                                             texture);
    REQUIRE((uploaded == rhi::RasterStatus::Pending || uploaded == rhi::RasterStatus::Ready));
    auto ready = rhi::GetStatus(device, texture);
    for (usize i = 0; i < 100000 && ready == rhi::RasterStatus::Pending; ++i)
    {
        REQUIRE(rhi::PollLifetime(device) == rhi::RasterStatus::Ready);
        ready = rhi::GetStatus(device, texture);
    }
    REQUIRE(ready == rhi::RasterStatus::Ready);
    const uint8 maskBytes[]{255, 255, 255, 0, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 0};
    REQUIRE(rhi::CreateTexture(device, {2, 2, rhi::RasterFormat::Rgba8Unorm}, {maskBytes, 8}, mask) ==
            rhi::RasterStatus::Ready);
    rr::MaterialVersion materials[ludus::qa::L3_MATERIALS];
    for (usize i = 0; i < ludus::qa::L3_MATERIALS; ++i)
    {
        const auto status =
            renderer.CreateMaterial(program, ludus::qa::L3Material(i, i == 4 ? mask : texture), materials[i]);
        REQUIRE((status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending));
        wait(materials[i]);
    }
    REQUIRE(rhi::Destroy(device, texture) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, mask) == rhi::RasterStatus::Ready);
    rr::Mesh mesh;
    REQUIRE(ludus::qa::L3Mesh(renderer, mesh) == rhi::RasterStatus::Ready);
    rr::SceneItem items[8];
    ludus::qa::L3Items(mesh, materials, items);
    rr::Snapshot snapshot;
    REQUIRE(renderer.CreateSnapshot(items, 8, snapshot) == rhi::RasterStatus::Ready);
    rr::PreparedView prior;
    rr::ViewReport report;
    REQUIRE(renderer.PrepareView(snapshot, {}, prior, report) == rhi::RasterStatus::Ready);
    auto changed = ludus::qa::L3Material(2, {});
    changed.Tint = {0, 1, 0, 1};
    rr::MaterialVersion candidate;
    REQUIRE(renderer.CreateMaterial(program, changed, candidate) == rhi::RasterStatus::Ready);
    REQUIRE(renderer.PublishMaterial(candidate, materials[2]) == rhi::RasterStatus::Ready);
    changed.Tint.X = 2;
    CHECK(renderer.CreateMaterial(program, changed, candidate) == rhi::RasterStatus::InvalidDescription);
    // Old view still resolves red despite publication and an invalid subsequent reload.
    REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    REQUIRE(renderer.DrawView(prior, {}) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(device, completion) == rhi::RasterStatus::Ready);
    uint8 oldImage[96 * 64 * 4];
    REQUIRE(rhi::backend::ReadHeadlessPixels(oldImage));
    CHECK(oldImage[(usize{48} * 96 + 16) * 4] == 255);
    CHECK(oldImage[(usize{48} * 96 + 16) * 4 + 1] == 0);
    REQUIRE(renderer.Release(prior) == rhi::RasterStatus::Ready);
    REQUIRE(renderer.Release(snapshot) == rhi::RasterStatus::Ready);
    // Four prewarmed opaque/depth/target states, with reference ordering equivalence.
    uint8 oracle[96 * 64 * 4]{};
    for (usize variant = 0; variant < 4; ++variant)
    {
        ludus::qa::L3Items(mesh, materials, items);
        const bool reverse = variant >= 2, offscreen = (variant % 2) != 0;
        if (reverse)
        {
            items[5].Transform.Translation.Z = .1F;
        }
        REQUIRE(renderer.CreateSnapshot(items, 8, snapshot) == rhi::RasterStatus::Ready);
        rr::PreparedView view;
        rr::ViewDescription description;
        description.Reference = offscreen;
        description.Depth = reverse ? rr::DepthConvention::ReverseZ : rr::DepthConvention::Conventional;
        REQUIRE(renderer.PrepareView(snapshot, description, view, report) == rhi::RasterStatus::Ready);
        rhi::TextureHandle target;
        rr::Presentation presentation;
        rhi::RasterPassDescription pass;
        if (offscreen)
        {
            REQUIRE(rhi::CreateTexture(device, {96, 64, rhi::RasterFormat::Rgba8Unorm, true}, {}, target) ==
                    rhi::RasterStatus::Ready);
            rr::ViewMapping mapping;
            REQUIRE(rr::ResolveViewMapping({96, 64, rr::ScreenFit::Stretch}, {0, 0, 96, 64}, 96, 64, mapping) ==
                    rhi::RasterStatus::Ready);
            const auto prepared = renderer.PreparePresentation(target, mapping, presentation);
            REQUIRE((prepared == rhi::RasterStatus::Ready || prepared == rhi::RasterStatus::Pending));
            wait(presentation);
            pass.Color = target;
        }
        REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
        REQUIRE(renderer.DrawView(view, pass) == rhi::RasterStatus::Ready);
        if (offscreen)
        {
            REQUIRE(renderer.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
        }
        completion = {};
        REQUIRE(rhi::EndFrame(device, completion) == rhi::RasterStatus::Ready);
        uint8 image[96 * 64 * 4];
        REQUIRE(rhi::backend::ReadHeadlessPixels(image));
        if (variant == 0)
        {
            std::memcpy(oracle, image, sizeof(image));
        }
        else
        {
            CHECK(std::memcmp(oracle, image, sizeof(image)) == 0);
        }
        const auto color = [&](usize x, usize y, const uint8(&rgb)[3]) {
            const auto offset = (y * 96 + x) * 4;
            CHECK(image[offset] == rgb[0]);
            CHECK(image[offset + 1] == rgb[1]);
            CHECK(image[offset + 2] == rgb[2]);
        };
        const auto mip = (usize{8} * 96 + 16) * 4;
        CHECK(image[mip] >= 127);
        CHECK(image[mip] <= 129);
        CHECK(image[mip + 1] == image[mip]);
        CHECK(image[mip + 2] == image[mip]);
        color(40, 8, {255, 255, 255});
        color(56, 8, {0, 0, 0});
        color(16, 48, {0, 255, 0});
        color(72, 8, {0, 0, 255});
        color(88, 8, {0, 0, 0});
        color(80, 48, {255, 0, 255});
        const auto blend = (usize{48} * 96 + 48) * 4;
        CHECK(image[blend] == 0);
        CHECK(image[blend + 1] >= 127);
        CHECK(image[blend + 1] <= 128);
        CHECK(image[blend + 2] == 128);
        REQUIRE(renderer.Release(view) == rhi::RasterStatus::Ready);
        REQUIRE(renderer.Release(snapshot) == rhi::RasterStatus::Ready);
        if (offscreen)
        {
            REQUIRE(renderer.Release(presentation) == rhi::RasterStatus::Ready);
            REQUIRE(rhi::Destroy(device, target) == rhi::RasterStatus::Ready);
        }
    }
}
TEST_CASE("L4 GGX lighting and linear HDR/LDR output match independent interior references", "[renderer][l4][gpu]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const auto started = rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, description, device, surface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter on this host");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rhi::RasterCapabilities caps;
    REQUIRE(rhi::GetRasterCapabilities(device, caps) == rhi::RasterStatus::Ready);
    const math::Vector3 positions[]{{-1, -1, .5F}, {1, -1, .5F}, {1, 1, .5F}, {-1, 1, .5F}};
    const math::Vector3 normals[]{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    const uint32 indices[]{0, 1, 2, 0, 2, 3};
    uint8 images[2][96 * 64 * 4];
    for (usize hdr = 0; hdr < (caps.HdrColor ? 2U : 1U); ++hdr)
    {
        for (usize scenario = 0; scenario < 7; ++scenario)
        {
            for (usize reference = 0; reference < 2; ++reference)
            {
                rr::Renderer renderer;
                auto wait = [&](auto handle) {
                    auto status = renderer.GetStatus(handle);
                    for (usize i = 0; i < 100000 && status == rhi::RasterStatus::Pending; ++i)
                    {
                        status = renderer.GetStatus(handle);
                    }
                    REQUIRE(status == rhi::RasterStatus::Ready);
                };
                REQUIRE(renderer.Initialize(device,
                                            ludus::shaders::renderer_flat::Vertex(),
                                            ludus::shaders::renderer_flat::Fragment()) == rhi::RasterStatus::Ready);
                REQUIRE(renderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                                 ludus::shaders::renderer_composite::Fragment()) ==
                        rhi::RasterStatus::Ready);
                REQUIRE(renderer.InitializeLighting(ludus::shaders::renderer_output::Vertex(),
                                                    ludus::shaders::renderer_output::Fragment(),
                                                    ludus::shaders::renderer_shadow::Fragment()) ==
                        rhi::RasterStatus::Ready);
                rr::MaterialProgram program;
                const auto warming = renderer.PrewarmLitProgram(ludus::shaders::renderer_lit::Vertex(),
                                                                ludus::shaders::renderer_lit::Fragment(),
                                                                ludus::shaders::renderer_lit_shadow::Fragment(),
                                                                program);
                REQUIRE((warming == rhi::RasterStatus::Ready || warming == rhi::RasterStatus::Pending));
                wait(program);
                rr::Mesh mesh;
                rr::MeshDescription geometry{positions, 4, indices, 6};
                geometry.Normals = normals;
                REQUIRE(renderer.CreateMesh(geometry, mesh) == rhi::RasterStatus::Ready);
                rr::DirectLight light;
                light.SourceId = 1;
                if (scenario == 4 || scenario == 5)
                {
                    light.Kind = scenario == 4 ? rr::LightKind::Point : rr::LightKind::Spot;
                    light.Position = {0, 0, 2.5F};
                    light.Intensity = 4;
                    light.Range = 100;
                }
                rr::LitMaterialDescription material;
                material.Surface.Tint = {.5F, .5F, .5F, 1};
                material.Roughness = 1;
                material.Lights = &light;
                material.LightCount = 1;
                if (scenario == 6)
                {
                    light.Intensity = 1000000;
                    material.Roughness = .045F;
                }
                if (scenario == 1)
                {
                    material.Emission = {4, 0, 0};
                    material.LightCount = 0;
                }
                if (scenario == 2)
                {
                    material.Metallic = 1;
                    material.Surface.Tint = {1, 1, 1, 1};
                    material.EnvironmentRadiance = {1, 1, 1};
                    material.LightCount = 0;
                }
                if (scenario == 3)
                {
                    material.Emission = {4, 0, 0};
                    material.LightCount = 0;
                    material.Surface.Alpha = rr::MaterialAlpha::Painter;
                    material.Surface.Tint.W = .5F;
                }
                rr::MaterialVersion version;
                rr::LightSelection lights;
                const auto created = renderer.CreateLitMaterial(program, material, version, lights);
                REQUIRE((created == rhi::RasterStatus::Ready || created == rhi::RasterStatus::Pending));
                wait(version);
                rr::SceneItem item;
                item.Geometry = mesh;
                item.Surface = version;
                item.SourceId = 1;
                if (scenario == 3)
                {
                    item.Pass = rr::Layer::Overlay;
                }
                rr::Snapshot snapshot;
                REQUIRE(renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
                rr::ViewDescription viewDescription;
                viewDescription.Reference = reference != 0;
                rr::PreparedView view;
                rr::ViewReport report;
                const auto prepared = renderer.PrepareView(snapshot, viewDescription, view, report);
                REQUIRE((prepared == rhi::RasterStatus::Ready || prepared == rhi::RasterStatus::Pending));
                wait(view);
                rhi::TextureHandle target;
                REQUIRE(rhi::CreateTexture(
                            device,
                            {96, 64, hdr != 0 ? rhi::RasterFormat::Rgba16Float : rhi::RasterFormat::Rgba8Unorm, true},
                            {},
                            target) == rhi::RasterStatus::Ready);
                rr::ViewMapping mapping;
                REQUIRE(rr::ResolveViewMapping({96, 64}, {0, 0, 96, 64}, 96, 64, mapping) == rhi::RasterStatus::Ready);
                rr::Presentation output;
                const auto composed = renderer.PrepareLightingPresentation(target, mapping, {}, output);
                REQUIRE((composed == rhi::RasterStatus::Ready || composed == rhi::RasterStatus::Pending));
                wait(output);
                REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
                rhi::RasterPassDescription pass;
                pass.Color = target;
                REQUIRE(renderer.DrawView(view, pass) == rhi::RasterStatus::Ready);
                REQUIRE(renderer.DrawLightingPresentation(output) == rhi::RasterStatus::Ready);
                rhi::SubmissionToken completion;
                REQUIRE(rhi::EndFrame(device, completion) == rhi::RasterStatus::Ready);
                REQUIRE(rhi::backend::ReadHeadlessPixels(images[reference]));
                const usize center = (usize{32} * 96 + 48) * 4;
                // Closed-form GGX/Lambert at N=V=L, white-metal furnace, explicit Reinhard+sRGB.
                const int32 expected = scenario == 6                      ? (hdr != 0 ? 255 : 188)
                                       : (scenario == 0 || scenario >= 4) ? 103
                                       : scenario == 1                    ? (hdr != 0 ? 231 : 188)
                                       : scenario == 2                    ? 188
                                                                          : (hdr != 0 ? 213 : 188);
                const auto difference = static_cast<int32>(images[reference][center]) - expected;
                CHECK(difference >= -2);
                CHECK(difference <= 2);
                CHECK(images[reference][center + 3] == 255);
                if (scenario == 1 || scenario == 3)
                {
                    CHECK(images[reference][center + 1] == 0);
                }
                REQUIRE(rhi::Destroy(device, target) == rhi::RasterStatus::Ready);
            }
            CHECK(std::memcmp(images[0], images[1], sizeof(images[0])) == 0);
        }
    }
}

TEST_CASE("L4 hemispheric constant environment scales irradiance by the normal's vertical component",
          "[renderer][l4][gpu][environment]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, description, device, surface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter on this host");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    const math::Vector3 positions[]{{-1, -1, .5F}, {1, -1, .5F}, {1, 1, .5F}, {-1, 1, .5F}};
    const uint32 indices[]{0, 1, 2, 0, 2, 3};
    constexpr float32 GROUND_SCALE = .25F;
    constexpr float32 SRGB_THRESHOLD = .04045F;
    constexpr float32 ONE_CODE = 1.0F / 255.0F;
    // Renders one lit quad with the given normal with no lights and returns the display-linear Reinhard input of its
    // centre pixel, which is proportional to the irradiance the shader used.
    const auto renderRadiance = [&](math::Vector3 normal, float32 groundScale) -> float32 {
        rr::Renderer renderer;
        auto wait = [&](auto handle) {
            rhi::RasterStatus status = renderer.GetStatus(handle);
            for (usize i = 0; i < 100000 && status == rhi::RasterStatus::Pending; ++i)
            {
                status = renderer.GetStatus(handle);
            }
            REQUIRE(status == rhi::RasterStatus::Ready);
        };
        REQUIRE(renderer.Initialize(device,
                                    ludus::shaders::renderer_flat::Vertex(),
                                    ludus::shaders::renderer_flat::Fragment()) == rhi::RasterStatus::Ready);
        REQUIRE(renderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                         ludus::shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
        REQUIRE(renderer.InitializeLighting(ludus::shaders::renderer_output::Vertex(),
                                            ludus::shaders::renderer_output::Fragment(),
                                            ludus::shaders::renderer_shadow::Fragment()) == rhi::RasterStatus::Ready);
        rr::MaterialProgram program;
        const rhi::RasterStatus warming = renderer.PrewarmLitProgram(ludus::shaders::renderer_lit::Vertex(),
                                                                     ludus::shaders::renderer_lit::Fragment(),
                                                                     ludus::shaders::renderer_lit_shadow::Fragment(),
                                                                     program);
        REQUIRE((warming == rhi::RasterStatus::Ready || warming == rhi::RasterStatus::Pending));
        wait(program);
        const math::Vector3 normals[]{normal, normal, normal, normal};
        rr::Mesh mesh;
        rr::MeshDescription geometry{positions, 4, indices, 6};
        geometry.Normals = normals;
        REQUIRE(renderer.CreateMesh(geometry, mesh) == rhi::RasterStatus::Ready);
        rr::LitMaterialDescription material;
        material.Surface.Tint = {.8F, .8F, .8F, 1};
        material.Roughness = 1;
        material.LightCount = 0;
        material.EnvironmentIrradiance = {2, 2, 2};
        material.EnvironmentGroundScale = groundScale;
        rr::MaterialVersion version;
        rr::LightSelection lights;
        const rhi::RasterStatus created = renderer.CreateLitMaterial(program, material, version, lights);
        REQUIRE((created == rhi::RasterStatus::Ready || created == rhi::RasterStatus::Pending));
        wait(version);
        rr::SceneItem item;
        item.Geometry = mesh;
        item.Surface = version;
        item.SourceId = 1;
        rr::Snapshot snapshot;
        REQUIRE(renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
        rr::PreparedView view;
        rr::ViewReport report;
        const rhi::RasterStatus prepared = renderer.PrepareView(snapshot, {}, view, report);
        REQUIRE((prepared == rhi::RasterStatus::Ready || prepared == rhi::RasterStatus::Pending));
        wait(view);
        rhi::TextureHandle target;
        REQUIRE(rhi::CreateTexture(device, {96, 64, rhi::RasterFormat::Rgba8Unorm, true}, {}, target) ==
                rhi::RasterStatus::Ready);
        rr::ViewMapping mapping;
        REQUIRE(rr::ResolveViewMapping({96, 64}, {0, 0, 96, 64}, 96, 64, mapping) == rhi::RasterStatus::Ready);
        rr::Presentation output;
        const rhi::RasterStatus composed = renderer.PrepareLightingPresentation(target, mapping, {}, output);
        REQUIRE((composed == rhi::RasterStatus::Ready || composed == rhi::RasterStatus::Pending));
        wait(output);
        REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
        rhi::RasterPassDescription pass;
        pass.Color = target;
        REQUIRE(renderer.DrawView(view, pass) == rhi::RasterStatus::Ready);
        REQUIRE(renderer.DrawLightingPresentation(output) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(rhi::EndFrame(device, completion) == rhi::RasterStatus::Ready);
        uint8 image[96 * 64 * 4];
        REQUIRE(rhi::backend::ReadHeadlessPixels(image));
        REQUIRE(rhi::Destroy(device, target) == rhi::RasterStatus::Ready);
        const float32 encoded = static_cast<float32>(image[(usize{32} * 96 + 48) * 4]) * ONE_CODE;
        const float32 display =
            encoded <= SRGB_THRESHOLD ? encoded / 12.92F : std::pow((encoded + .055F) / 1.055F, 2.4F);
        return display / (1 - display);
    };
    // The camera looks down -Z, so the normals tilt up or down while still facing it.
    const math::Vector3 tiltedUp{0, .6F, .8F};
    const math::Vector3 tiltedDown{0, -.6F, .8F};
    const math::Vector3 facing{0, 0, 1};
    const float32 upConstant = renderRadiance(tiltedUp, 1);
    const float32 downConstant = renderRadiance(tiltedDown, 1);
    CHECK(upConstant > .05F);
    // A scale of one keeps the old constant irradiance whatever the normal says.
    CHECK(downConstant == Catch::Approx(upConstant).epsilon(.03));
    // The irradiance factor is lerp(scale, 1, y / 2 + 1 / 2): 0.85 tilted up, 0.4 tilted down, 0.625 level.
    CHECK(renderRadiance(tiltedUp, GROUND_SCALE) == Catch::Approx(upConstant * .85F).epsilon(.03));
    CHECK(renderRadiance(tiltedDown, GROUND_SCALE) == Catch::Approx(upConstant * .4F).epsilon(.04));
    CHECK(renderRadiance(facing, GROUND_SCALE) == Catch::Approx(upConstant * .625F).epsilon(.03));
}

TEST_CASE("L4 PBR Neutral output matches the CPU curve and keeps hue where Reinhard does not",
          "[renderer][l4][gpu][tone-mapping]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, description, device, surface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter on this host");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rhi::RasterCapabilities caps;
    REQUIRE(rhi::GetRasterCapabilities(device, caps) == rhi::RasterStatus::Ready);
    const math::Vector3 positions[]{{-1, -1, .5F}, {1, -1, .5F}, {1, 1, .5F}, {-1, 1, .5F}};
    const math::Vector3 normals[]{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    const uint32 indices[]{0, 1, 2, 0, 2, 3};
    // An eight-bit target stores at most one, so the brighter colours need a half-float target.
    const math::Vector3 emissions[]{{.02F, .02F, .02F},
                                    {.3F, .3F, .3F},
                                    {.2F, .5F, .7F},
                                    {1, .5F, .1F},
                                    {4, 1, 0},
                                    {16, 8, 8},
                                    {1000, 1000, 1000}};
    constexpr usize SCENARIO_COUNT = sizeof(emissions) / sizeof(emissions[0]);
    constexpr usize EIGHT_BIT_SCENARIO_COUNT = 4;
    constexpr int32 CHANNEL_TOLERANCE = 2;
    for (const rr::ToneCurve curve : {rr::ToneCurve::Reinhard, rr::ToneCurve::PbrNeutral})
    {
        for (usize scenario = 0; scenario < (caps.HdrColor ? SCENARIO_COUNT : EIGHT_BIT_SCENARIO_COUNT); ++scenario)
        {
            rr::Renderer renderer;
            auto wait = [&](auto handle) {
                rhi::RasterStatus status = renderer.GetStatus(handle);
                for (usize i = 0; i < 100000 && status == rhi::RasterStatus::Pending; ++i)
                {
                    status = renderer.GetStatus(handle);
                }
                REQUIRE(status == rhi::RasterStatus::Ready);
            };
            REQUIRE(renderer.Initialize(device,
                                        ludus::shaders::renderer_flat::Vertex(),
                                        ludus::shaders::renderer_flat::Fragment()) == rhi::RasterStatus::Ready);
            REQUIRE(renderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                             ludus::shaders::renderer_composite::Fragment()) ==
                    rhi::RasterStatus::Ready);
            REQUIRE(renderer.InitializeLighting(ludus::shaders::renderer_output::Vertex(),
                                                ludus::shaders::renderer_output::Fragment(),
                                                ludus::shaders::renderer_shadow::Fragment()) ==
                    rhi::RasterStatus::Ready);
            rr::MaterialProgram program;
            const rhi::RasterStatus warming =
                renderer.PrewarmLitProgram(ludus::shaders::renderer_lit::Vertex(),
                                           ludus::shaders::renderer_lit::Fragment(),
                                           ludus::shaders::renderer_lit_shadow::Fragment(),
                                           program);
            REQUIRE((warming == rhi::RasterStatus::Ready || warming == rhi::RasterStatus::Pending));
            wait(program);
            rr::Mesh mesh;
            rr::MeshDescription geometry{positions, 4, indices, 6};
            geometry.Normals = normals;
            REQUIRE(renderer.CreateMesh(geometry, mesh) == rhi::RasterStatus::Ready);
            rr::LitMaterialDescription material;
            material.Surface.Tint = {0, 0, 0, 1};
            material.Roughness = 1;
            material.Emission = emissions[scenario];
            material.LightCount = 0;
            rr::MaterialVersion version;
            rr::LightSelection lights;
            const rhi::RasterStatus created = renderer.CreateLitMaterial(program, material, version, lights);
            REQUIRE((created == rhi::RasterStatus::Ready || created == rhi::RasterStatus::Pending));
            wait(version);
            rr::SceneItem item;
            item.Geometry = mesh;
            item.Surface = version;
            item.SourceId = 1;
            rr::Snapshot snapshot;
            REQUIRE(renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
            rr::PreparedView view;
            rr::ViewReport report;
            const rhi::RasterStatus prepared = renderer.PrepareView(snapshot, {}, view, report);
            REQUIRE((prepared == rhi::RasterStatus::Ready || prepared == rhi::RasterStatus::Pending));
            wait(view);
            rhi::TextureHandle target;
            const bool isHdr = caps.HdrColor == true;
            REQUIRE(rhi::CreateTexture(
                        device,
                        {96, 64, isHdr ? rhi::RasterFormat::Rgba16Float : rhi::RasterFormat::Rgba8Unorm, true},
                        {},
                        target) == rhi::RasterStatus::Ready);
            rr::ViewMapping mapping;
            REQUIRE(rr::ResolveViewMapping({96, 64}, {0, 0, 96, 64}, 96, 64, mapping) == rhi::RasterStatus::Ready);
            rr::Presentation output;
            const rhi::RasterStatus composed =
                renderer.PrepareLightingPresentation(target,
                                                     mapping,
                                                     { .ToneMap = true, .Curve = curve, .Encode = true },
                                                     output);
            REQUIRE((composed == rhi::RasterStatus::Ready || composed == rhi::RasterStatus::Pending));
            wait(output);
            REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
            rhi::RasterPassDescription pass;
            pass.Color = target;
            REQUIRE(renderer.DrawView(view, pass) == rhi::RasterStatus::Ready);
            REQUIRE(renderer.DrawLightingPresentation(output) == rhi::RasterStatus::Ready);
            rhi::SubmissionToken completion;
            REQUIRE(rhi::EndFrame(device, completion) == rhi::RasterStatus::Ready);
            uint8 image[96 * 64 * 4];
            REQUIRE(rhi::backend::ReadHeadlessPixels(image));
            const usize center = (usize{32} * 96 + 48) * 4;
            const math::Vector3 mapped = rr::ApplyToneCurve(curve, emissions[scenario]);
            for (usize channel = 0; channel < 3; ++channel)
            {
                const float32 linear = channel == 0 ? mapped.X : channel == 1 ? mapped.Y : mapped.Z;
                const int32 code = static_cast<int32>(math::EncodeSrgb(math::Clamp(linear, 0.0F, 1.0F)) * 255.0F + .5F);
                INFO("curve " << static_cast<int>(curve) << " scenario " << scenario << " channel " << channel);
                CHECK(std::abs(static_cast<int32>(image[center + channel]) - code) <= CHANNEL_TOLERANCE);
            }
            CHECK(image[center + 3] == 255);
            REQUIRE(rhi::Destroy(device, target) == rhi::RasterStatus::Ready);
        }
    }
}

TEST_CASE("L4 baked direct diffuse and immutable cascade/spot atlases survive caster reload", "[renderer][l4][gpu]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const auto started = rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, description, device, surface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter on this host");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rhi::RasterCapabilities caps;
    REQUIRE(rhi::GetRasterCapabilities(device, caps) == rhi::RasterStatus::Ready);
    for (usize hdr = 0; hdr < (caps.HdrColor ? 2U : 1U); ++hdr)
    {
        uint8 images[2][2][96 * 64 * 4];
        for (usize reference = 0; reference < 2; ++reference)
        {
            rr::Renderer renderer;
            ludus::qa::L4Scene scene;
            scene.Hdr = hdr != 0;
            scene.Reference = reference != 0;
            REQUIRE(renderer.Initialize(device,
                                        ludus::shaders::renderer_flat::Vertex(),
                                        ludus::shaders::renderer_flat::Fragment()) == rhi::RasterStatus::Ready);
            REQUIRE(renderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                             ludus::shaders::renderer_composite::Fragment()) ==
                    rhi::RasterStatus::Ready);
            REQUIRE(renderer.InitializeLighting(ludus::shaders::renderer_output::Vertex(),
                                                ludus::shaders::renderer_output::Fragment(),
                                                ludus::shaders::renderer_shadow::Fragment()) ==
                    rhi::RasterStatus::Ready);
            auto status = renderer.PrewarmLitProgram(ludus::shaders::renderer_lit::Vertex(),
                                                     ludus::shaders::renderer_lit::Fragment(),
                                                     ludus::shaders::renderer_lit_shadow::Fragment(),
                                                     scene.Program);
            REQUIRE(ludus::qa::L4Scene::Accepted(status));
            for (usize i = 0; i < 100000; ++i)
            {
                status = scene.Advance(renderer, device, surface);
                if (status != rhi::RasterStatus::Pending && status != rhi::RasterStatus::NotReady)
                {
                    break;
                }
            }
            REQUIRE(status == rhi::RasterStatus::Ready);
            for (usize moved = 0; moved < 2; ++moved)
            {
                if (moved != 0)
                {
                    status = scene.MoveCaster(renderer);
                    for (usize i = 0; i < 100000 && status == rhi::RasterStatus::Pending; ++i)
                    {
                        status = scene.MoveCaster(renderer);
                    }
                    REQUIRE(status == rhi::RasterStatus::Ready);
                }
                REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
                REQUIRE(scene.Draw(renderer) == rhi::RasterStatus::Ready);
                rhi::SubmissionToken completion;
                REQUIRE(rhi::EndFrame(device, completion) == rhi::RasterStatus::Ready);
                REQUIRE(rhi::backend::ReadHeadlessPixels(images[reference][moved]));
                const auto color = [&](const usize(&position)[2], int32 expected) {
                    const auto offset = (position[1] * 96 + position[0]) * 4;
                    const auto difference = static_cast<int32>(images[reference][moved][offset]) - expected;
                    CHECK(difference >= -2);
                    CHECK(difference <= 2);
                    CHECK(images[reference][moved][offset + 3] == 255);
                };
                color({16, 18}, 102);
                color({48, 18}, 102);
                color({80, 18}, 102);
                color({16, 46}, hdr != 0 ? 231 : 188);
                color({48, 46}, 188);
                color({80, 46}, hdr != 0 ? 213 : 188);
                color({48, 32}, moved != 0 ? 102 : 0);
                color({70, 32}, 102);
                CHECK(scene.Report.BakeFallbacks == 1);
                CHECK(scene.Report.ShadowFallbacks == moved);
            }
            renderer.Reset();
            REQUIRE(rhi::Destroy(device, scene.Target) == rhi::RasterStatus::Ready);
            REQUIRE(rhi::Destroy(device, scene.Display) == rhi::RasterStatus::Ready);
        }
        CHECK(std::memcmp(images[0], images[1], sizeof(images[0])) == 0);
    }
}

TEST_CASE("L5 clustered lighting matches full-list pixels with many objects and overflow", "[renderer][l5][gpu]")
{
    const bool benchmark = std::getenv("LUDUS_RENDERER_BENCHMARK") != nullptr;
    const uint32 width = benchmark ? 640U : 96U, height = benchmark ? 360U : 64U;
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const auto started =
        rhi::CreateDevice({}, { .Width = width, .Height = height }, description, device, surface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter on this host");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rhi::RasterCapabilities caps;
    REQUIRE(rhi::GetRasterCapabilities(device, caps) == rhi::RasterStatus::Ready);
    if (benchmark)
    {
        rhi::DeviceInfo info;
        REQUIRE(rhi::GetDeviceInfo(device, info) == rhi::DeviceStatus::Ready);
        std::printf("L5BACKEND,%u\n", static_cast<uint32>(info.Startup.SelectedBackend));
    }
    using Clock = std::chrono::steady_clock;
    const auto ns = [](Clock::time_point start) {
        return static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    };
    for (uint32 scenario = 0; scenario < 3; ++scenario)
    {
        Array<uint8> images[3];
        for (usize reference = 0; reference < 3; ++reference)
        {
            rr::Renderer renderer;
            REQUIRE(renderer.Initialize(device,
                                        ludus::shaders::renderer_flat::Vertex(),
                                        ludus::shaders::renderer_flat::Fragment()) == rhi::RasterStatus::Ready);
            REQUIRE(renderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                             ludus::shaders::renderer_composite::Fragment()) ==
                    rhi::RasterStatus::Ready);
            REQUIRE(renderer.InitializeLighting(ludus::shaders::renderer_output::Vertex(),
                                                ludus::shaders::renderer_output::Fragment(),
                                                ludus::shaders::renderer_shadow::Fragment()) ==
                    rhi::RasterStatus::Ready);
            ludus::qa::L5Scene scene;
            scene.Reference = reference == 1;
            scene.Scenario = scenario;
            scene.Width = width;
            scene.Height = height;
            auto warming = renderer.PrewarmLitProgram(ludus::shaders::renderer_clustered::Vertex(),
                                                      ludus::shaders::renderer_clustered::Fragment(),
                                                      ludus::shaders::renderer_clustered_shadow::Fragment(),
                                                      scene.Program);
            REQUIRE(ludus::qa::L5Scene::Accepted(warming));
            for (usize i = 0; i < 1000000 && warming == rhi::RasterStatus::Pending; ++i)
            {
                warming = renderer.GetStatus(scene.Program);
            }
            REQUIRE(warming == rhi::RasterStatus::Ready);
            const auto setupStart = Clock::now();
            auto advanced = scene.Advance(renderer, device, surface);
            for (usize i = 0; i < 1000000 && advanced == rhi::RasterStatus::Pending; ++i)
            {
                advanced = scene.Advance(renderer, device, surface);
            }
            INFO("L5 setup stage " << scene.Stage << " operation " << scene.Operation);
            REQUIRE(advanced == rhi::RasterStatus::Ready);
            const auto setupNs = ns(setupStart);
            CHECK(scene.Report.Visible == (reference == 1 ? 256 : 128));
            CHECK(scene.Report.CulledGroups == (reference == 1 ? 0 : 8));
            CHECK(scene.Report.BoundsTests == 0);
            images[reference].Resize(static_cast<usize>(width) * height * 4);
            const uint32 frames = benchmark ? 72U : 2U;
            for (uint32 frame = 0; frame < frames; ++frame)
            {
                const auto prepareStart = Clock::now();
                rr::PreparedView replacement;
                rr::ViewDescription view;
                view.Reference = reference == 1;
                view.DirectLighting = reference == 2;
                REQUIRE(renderer.PrepareView(scene.Current, view, replacement, scene.Report) ==
                        rhi::RasterStatus::Ready);
                REQUIRE(renderer.Release(scene.View) == rhi::RasterStatus::Ready);
                scene.View = replacement;
                const auto prepareNs = ns(prepareStart);
                const auto recordStart = Clock::now();
                REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
                REQUIRE(scene.Draw(renderer) == rhi::RasterStatus::Ready);
                rhi::SubmissionToken completion;
                REQUIRE(rhi::EndFrame(device, completion) == rhi::RasterStatus::Ready);
                const auto recordNs = ns(recordStart);
                const auto waitStart = Clock::now();
                REQUIRE(rhi::backend::ReadHeadlessPixels({images[reference].GetData(), images[reference].GetSize()}));
                const auto waitNs = ns(waitStart);
                if (benchmark && frame >= 8)
                {
                    const usize knownBytes = 160 + 24 + 16096 + 16384 + static_cast<usize>(scene.Report.Visible) * 192 +
                                             static_cast<usize>(width) * height * 4;
                    std::printf("L5,%u,%u,%u,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%zu\n",
                                scenario,
                                static_cast<uint32>(reference),
                                frame - 8,
                                static_cast<unsigned long long>(setupNs),
                                static_cast<unsigned long long>(prepareNs),
                                static_cast<unsigned long long>(recordNs),
                                static_cast<unsigned long long>(waitNs),
                                width,
                                height,
                                scene.Report.Visible,
                                scene.Report.Culled,
                                scene.Report.Draws,
                                scene.Report.BoundsTests,
                                knownBytes);
                }
            }
            renderer.Reset();
            REQUIRE(rhi::Destroy(device, scene.Target) == rhi::RasterStatus::Ready);
        }
        REQUIRE(images[0].GetSize() == images[1].GetSize());
        usize colored = 0;
        for (usize i = 0; i < images[0].GetSize(); ++i)
        {
            const auto difference = static_cast<int32>(images[0][i]) - static_cast<int32>(images[1][i]);
            const auto clusteredDifference = static_cast<int32>(images[0][i]) - static_cast<int32>(images[2][i]);
            REQUIRE(clusteredDifference >= -1);
            REQUIRE(clusteredDifference <= 1);
            REQUIRE(difference >= -1);
            REQUIRE(difference <= 1);
            if (i % 4 == 0 && images[0][i] > 20)
            {
                ++colored;
            }
        }
        CHECK(colored > 100); // Independent nonblank requirement, including overflow/outside-grid cases.
        if (scenario == 1)
        {
            uint8 maximum = 0;
            for (usize i = 0; i < images[0].GetSize(); i += 4)
            {
                maximum = images[0][i] > maximum ? images[0][i] : maximum;
            }
            CHECK(maximum > 130); // 128 directional lights; an accidental eight-light loop is much darker.
            CHECK(maximum < 170);
        }
    }
}

namespace
{
constexpr uint32 GRID_WIDTH = 256;
constexpr uint32 GRID_HEIGHT = 192;
constexpr float32 GRID_HALF_HEIGHT = 6;
constexpr float32 GRID_PIXELS_PER_UNIT = static_cast<float32>(GRID_HEIGHT) / (2 * GRID_HALF_HEIGHT);
constexpr uint32 GRID_CHANNELS = 4;
constexpr uint32 GRID_BENCHMARK_FRAMES = 40;

/// Top-down orthographic camera over the XZ plane; world +X is right and +Z is down the image.
/// Clip depth falls with height, so anything above the plane is in front of the grid.
ludus::graphics::renderer::ViewDescription MakeGridView(uint32 width, uint32 height, bool isGridVisible) noexcept
{
    rr::ViewDescription view;
    const float32 halfWidth = GRID_HALF_HEIGHT * static_cast<float32>(width) / static_cast<float32>(height);
    // Matrix4 lists columns: x scales clip x, z flips into clip y, height lowers clip depth from one half.
    view.WorldToClip = {{{1 / halfWidth, 0, 0, 0}, {0, 0, -.04F, 0}, {0, -1 / GRID_HALF_HEIGHT, 0, 0}, {0, 0, .5F, 1}}};
    view.OrthographicCamera = true;
    view.CameraPosition = {0, 10, 0};
    view.CameraForward = {0, -1, 0};
    view.WorldGrid = { .IsVisible = isGridVisible, .FocusDistance = 5, .Width = width, .Height = height };
    return view;
}

usize GetGridPixel(uint32 width, uint32 column, uint32 row) noexcept
{
    return (static_cast<usize>(row) * width + column) * GRID_CHANNELS;
}
} // namespace

TEST_CASE("Analytic world grids draw antialiased lines, axes and fades behind nearer geometry", "[renderer][grid][gpu]")
{
    const bool isBenchmark = std::getenv("LUDUS_RENDERER_BENCHMARK") != nullptr;
    const uint32 width = isBenchmark ? 1920U : GRID_WIDTH;
    const uint32 height = isBenchmark ? 1080U : GRID_HEIGHT;
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = width, .Height = height }, description, device, surface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter on this host");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    // One opaque quad above the plane at world (3, 2, 0), twice the L0 quad size, covers x 2.6..3.4 and z -.4..+.4.
    rr::SceneItem item;
    rr::Renderer ribbons;
    rr::Renderer analytic;
    rr::Mesh meshes[2];
    rr::Snapshot snapshots[2];
    rr::PreparedView views[2];
    rr::Renderer* renderers[2]{&ribbons, &analytic};
    for (usize variant = 0; variant < 2; ++variant)
    {
        rr::Renderer& renderer = *renderers[variant];
        REQUIRE(renderer.Initialize(device,
                                    ludus::shaders::renderer_flat::Vertex(),
                                    ludus::shaders::renderer_flat::Fragment()) == rhi::RasterStatus::Ready);
        if (variant == 1)
        {
            REQUIRE(renderer.InitializeGrid(ludus::shaders::renderer_grid::Vertex(),
                                            ludus::shaders::renderer_grid::Fragment()) == rhi::RasterStatus::Ready);
        }
        REQUIRE(renderer.CreateMesh({ludus::qa::L0_QUAD, 4, ludus::qa::L0_INDICES, 6}, meshes[variant]) ==
                rhi::RasterStatus::Ready);
        item.Geometry = meshes[variant];
        item.SourceId = 1;
        item.Transform.Columns[0] = {2, 0, 0};
        item.Transform.Columns[1] = {0, 0, 2};
        item.Transform.Columns[2] = {0, 1, 0};
        item.Transform.Translation = {3, 2, 0};
        item.Material.Color = {1, 0, 0, 1};
        REQUIRE(renderer.CreateSnapshot(&item, 1, snapshots[variant]) == rhi::RasterStatus::Ready);
        rr::ViewReport report;
        REQUIRE(renderer.PrepareView(snapshots[variant], MakeGridView(width, height, true), views[variant], report) ==
                rhi::RasterStatus::Ready);
        CHECK(report.Draws == (variant == 0 ? 2U : 3U));
    }
    Array<uint8> images[2];
    for (usize variant = 0; variant < 2; ++variant)
    {
        REQUIRE(images[variant].TryResize(static_cast<usize>(width) * height * GRID_CHANNELS) == true);
        rhi::SubmissionToken completion;
        REQUIRE(renderers[variant]->Submit(views[variant], surface, completion) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::backend::ReadHeadlessPixels({images[variant].GetData(), images[variant].GetSize()}));
    }
    if (isBenchmark)
    {
        using Clock = std::chrono::steady_clock;
        const auto ns = [](Clock::time_point start) {
            return static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        };
        // The pass cost is the difference from the same frame without a grid; both include clear and readback.
        rr::PreparedView hidden;
        rr::ViewReport hiddenReport;
        REQUIRE(analytic.PrepareView(snapshots[1], MakeGridView(width, height, false), hidden, hiddenReport) ==
                rhi::RasterStatus::Ready);
        struct Variant final
        {
            const char* Name;
            rr::Renderer* Renderer;
            rr::PreparedView View;
        };
        const Variant variants[]{{"none", &analytic, hidden},
                                 {"ribbons", &ribbons, views[0]},
                                 {"analytic", &analytic, views[1]}};
        for (const Variant& variant : variants)
        {
            uint64 total = 0;
            for (uint32 frame = 0; frame < GRID_BENCHMARK_FRAMES; ++frame)
            {
                const Clock::time_point start = Clock::now();
                rhi::SubmissionToken completion;
                REQUIRE(variant.Renderer->Submit(variant.View, surface, completion) == rhi::RasterStatus::Ready);
                REQUIRE(rhi::backend::ReadHeadlessPixels({images[0].GetData(), images[0].GetSize()}));
                total += ns(start);
            }
            std::printf("GRIDBENCH,%s,%ux%u,%u,%llu\n",
                        variant.Name,
                        width,
                        height,
                        GRID_BENCHMARK_FRAMES,
                        static_cast<unsigned long long>(total / GRID_BENCHMARK_FRAMES));
        }
        if (const char* path = std::getenv("LUDUS_GRID_IMAGE"))
        {
            for (usize variant = 0; variant < 2; ++variant)
            {
                rhi::SubmissionToken completion;
                REQUIRE(renderers[variant]->Submit(views[variant], surface, completion) == rhi::RasterStatus::Ready);
                REQUIRE(rhi::backend::ReadHeadlessPixels({images[variant].GetData(), images[variant].GetSize()}));
            }
            for (usize variant = 0; variant < 2; ++variant)
            {
                char name[512];
                std::snprintf(name, sizeof(name), "%s-%s.ppm", path, variant == 0 ? "ribbons" : "analytic");
                if (std::FILE* file = std::fopen(name, "wb"))
                {
                    std::fprintf(file, "P6\n%u %u\n255\n", width, height);
                    for (usize pixel = 0; pixel < static_cast<usize>(width) * height; ++pixel)
                    {
                        std::fwrite(&images[variant][pixel * GRID_CHANNELS], 1, 3, file);
                    }
                    std::fclose(file);
                }
            }
        }
        return;
    }
    const uint8* image = images[1].GetData();
    const uint32 centerColumn = width / 2;
    const uint32 centerRow = height / 2;
    const uint32 unit = static_cast<uint32>(GRID_PIXELS_PER_UNIT);
    const usize background = GetGridPixel(width, centerColumn + unit / 2, centerRow + unit / 2);
    // Mid-cell pixels keep the cleared colour, so the grid does not tint the surface.
    const usize farCell = GetGridPixel(width, centerColumn - 5 * unit + unit / 2, centerRow - 3 * unit + unit / 2);
    CHECK(image[background] == image[farCell]);
    // The X axis is red, the Z axis blue, and both are exact lines through the origin.
    const usize xAxis = GetGridPixel(width, centerColumn + 2 * unit + unit / 2, centerRow);
    CHECK(image[xAxis] > image[xAxis + 2]);
    CHECK(image[xAxis] > image[background] + 40);
    const usize zAxis = GetGridPixel(width, centerColumn, centerRow + 3 * unit + unit / 2);
    CHECK(image[zAxis + 2] > image[zAxis]);
    CHECK(image[zAxis + 2] > image[background + 2] + 40);
    // A minor line at x = 2 sits on a pixel boundary: both neighbours are partly covered, and
    // coverage is gone two pixels away, so the edge is antialiased rather than a hard two-level step.
    const uint32 lineColumn = centerColumn + 2 * unit;
    const uint32 row = centerRow + unit / 2 + unit;
    const uint8 base = image[GetGridPixel(width, lineColumn + 3, row) + 1];
    const uint8 left = image[GetGridPixel(width, lineColumn - 1, row) + 1];
    const uint8 right = image[GetGridPixel(width, lineColumn, row) + 1];
    CHECK(left > base);
    CHECK(right > base);
    CHECK(image[GetGridPixel(width, lineColumn - 3, row) + 1] == base);
    // Geometry above the plane hides the grid: inside the quad the line at x = 3 is pure item red,
    // and outside the quad the same line is drawn.
    const uint32 occludedColumn = centerColumn + 3 * unit - 1;
    const usize occluded = GetGridPixel(width, occludedColumn, centerRow + unit / 4);
    CHECK(image[occluded] == 255);
    CHECK(image[occluded + 1] == 0);
    CHECK(image[occluded + 2] == 0);
    const usize visible = GetGridPixel(width, occludedColumn, centerRow + 3 * unit + unit / 2);
    CHECK(image[visible + 1] > image[background + 1]);
    // The analytic and ribbon grids agree on where the lines are: the same pixels differ from the background.
    uint32 differing = 0;
    uint32 ribbonOnly = 0;
    uint32 analyticOnly = 0;
    for (usize pixel = 0; pixel < static_cast<usize>(width) * height; ++pixel)
    {
        const bool isRibbon = images[0][pixel * GRID_CHANNELS + 1] != images[0][background + 1];
        const bool isAnalytic = images[1][pixel * GRID_CHANNELS + 1] != image[background + 1];
        differing += isRibbon && isAnalytic ? 1U : 0U;
        ribbonOnly += isRibbon && isAnalytic == false ? 1U : 0U;
        analyticOnly += isRibbon == false && isAnalytic ? 1U : 0U;
    }
    CHECK(differing > 0);
    CHECK(differing > ribbonOnly * 4);
    CHECK(differing > analyticOnly / 2);
}

#if defined(LUDUS_TEST_RAY)
#    include "l6_scene.h"
#    include "ray_query.h"
TEST_CASE("RT1 native and CPU captures agree on MIS emitter, environment and indirect transport",
          "[renderer][gpu][rt1]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard() noexcept
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Preferred.RayQueries = true;
    const auto started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
#    if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#    endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rhi::RayCapabilities capabilities;
    const bool native = rhi::GetRayCapabilities(device, capabilities) == rhi::RasterStatus::Ready;
    // The shifted primary plane lies at z=0: affine-only error was zero in
    // its normal component and native secondary rays could hit it again.
    // Both light-sampling modes; a resampled native tile submits up to three queries per path.
    constexpr float32 SHIFTED_PLANE_OFFSET = -0.7F;
    struct NativeCase final
    {
        rr::ReferenceLightSampling LightSampling = rr::ReferenceLightSampling::UniformArea;
        float32 WorldZOffset = 0;
    };
    const NativeCase nativeCases[]{
        {rr::ReferenceLightSampling::UniformArea, 0},
        {rr::ReferenceLightSampling::UniformArea, SHIFTED_PLANE_OFFSET},
        {rr::ReferenceLightSampling::ResampledProjectedSolidAngle, 0},
        {rr::ReferenceLightSampling::ResampledProjectedSolidAngle, SHIFTED_PLANE_OFFSET},
    };
    for (const NativeCase& nativeCase : nativeCases)
    {
        const float32 worldZOffset = nativeCase.WorldZOffset;
        CAPTURE(static_cast<uint32>(nativeCase.LightSampling), worldZOffset);
        rr::ReferencePixel images[2][4];
        for (usize variant = 0; variant < (native ? 2U : 1U); ++variant)
        {
            rr::Renderer renderer;
            REQUIRE(renderer.Initialize(device,
                                        ludus::shaders::renderer_flat::Vertex(),
                                        ludus::shaders::renderer_flat::Fragment()) == rhi::RasterStatus::Ready);
            REQUIRE(renderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                             ludus::shaders::renderer_composite::Fragment()) ==
                    rhi::RasterStatus::Ready);
            REQUIRE(renderer.InitializeLighting(ludus::shaders::renderer_output::Vertex(),
                                                ludus::shaders::renderer_output::Fragment(),
                                                ludus::shaders::renderer_shadow::Fragment()) ==
                    rhi::RasterStatus::Ready);
            rr::ReferenceRenderingOptions options{variant == 0 ? rr::ReferenceTraversal::CpuOracle
                                                               : rr::ReferenceTraversal::NativeQueries,
                                                  usize{16} * 1024 * 1024,
                                                  usize{64} * 1024 * 1024,
                                                  4};
            REQUIRE(renderer.EnableReferenceRendering(ludus::shaders::ray_query::Query(), options) ==
                    rhi::RasterStatus::Ready);
            rr::MaterialProgram program;
            auto programStatus = renderer.PrewarmLitProgram(ludus::shaders::renderer_lit::Vertex(),
                                                            ludus::shaders::renderer_lit::Fragment(),
                                                            ludus::shaders::renderer_lit_shadow::Fragment(),
                                                            program);
            const auto programStarted = time::NowTicks();
            while (programStatus == rhi::RasterStatus::Pending &&
                   time::NowTicks() - programStarted < 10 * time::NANOSECONDS_PER_SECOND)
            {
                programStatus = renderer.GetStatus(program);
            }
            REQUIRE(programStatus == rhi::RasterStatus::Ready);
            rr::Snapshot snapshot;
            REQUIRE(ludus::qa::BuildL6Snapshot(renderer, program, snapshot, worldZOffset) == rhi::RasterStatus::Ready);
            rr::ReferenceCaptureDescription captureDescription;
            captureDescription.OrthographicCamera = true;
            captureDescription.WorldToClip.Columns[3].Z = -worldZOffset;
            captureDescription.CameraPosition.Z += worldZOffset;
            captureDescription.Width = captureDescription.Height = 2;
            captureDescription.TargetSamples = 128;
            captureDescription.MaxDepth = 4;
            captureDescription.Seed = 871;
            captureDescription.Environment = {.3F, .4F, .5F};
            captureDescription.LightSampling = nativeCase.LightSampling;
            rr::ReferenceCapture capture;
            auto status = renderer.BeginReferenceCapture(snapshot, captureDescription, capture);
            REQUIRE((status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending));
            // Creating an accepted job starts pending accumulation even when setup is ready.
            status = rhi::RasterStatus::Pending;
            const auto captureStarted = time::NowTicks();
            while (status == rhi::RasterStatus::Pending &&
                   time::NowTicks() - captureStarted < 30 * time::NANOSECONDS_PER_SECOND)
            {
                status = renderer.AdvanceReferenceCapture(capture);
            }
            REQUIRE(status == rhi::RasterStatus::Ready);
            REQUIRE(renderer.CopyReferencePixels(capture, images[variant], 4) == rhi::RasterStatus::Ready);
            rr::ReferenceCaptureInfo info;
            REQUIRE(renderer.GetReferenceCaptureInfo(capture, info) == rhi::RasterStatus::Ready);
            CHECK(info.InvalidContributions == 0);
            CHECK(info.VisibilityQueries > 0);
            CHECK(info.ClosestQueries > 512);
            float32 indirect = 0;
            for (auto pixel : images[variant])
            {
                CHECK(pixel.Samples == 128);
                CHECK(math::IsFinite(pixel.Radiance));
                CHECK(pixel.Direct.X > 0);
                indirect += pixel.Indirect.X;
            }
            CHECK(indirect > .01F);
            REQUIRE(renderer.Release(capture) == rhi::RasterStatus::Ready);
            REQUIRE(renderer.Release(snapshot) == rhi::RasterStatus::Ready);
            REQUIRE(renderer.Release(program) == rhi::RasterStatus::Ready);
        }
        if (native)
        {
            for (usize i = 0; i < 4; ++i)
            {
                CHECK(math::Length(images[0][i].Radiance - images[1][i].Radiance) < .002F);
                CHECK(math::Length(images[0][i].Direct - images[1][i].Direct) < .002F);
                CHECK(math::Length(images[0][i].Indirect - images[1][i].Indirect) < .002F);
            }
        }
    }
}
#endif
