#include "l6_scene.h"
#include "renderer_authored.h"
#include "renderer_authored_shadow.h"
#include "renderer_clustered_surface.h"
#include "renderer_clustered_surface_shadow.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_gpu_preview.h"
#include "renderer_gpu_preview_image.h"
#include "renderer_lit.h"
#include "renderer_lit_shadow.h"
#include "renderer_output.h"
#include "renderer_shadow.h"
#include "renderer_surface.h"
#include "renderer_surface_shadow.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/viewport.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace shaders = ludus::shaders;
using S = rhi::RasterStatus;
void WaitPreview(rr::Renderer& renderer, rr::GpuPreview preview, uint32 samples)
{
    rr::GpuPreviewInfo info;
    const uint64 start = time::NowTicks();
    S status = S::Pending;
    do
    {
        status = renderer.GetGpuPreviewInfo(preview, info);
        REQUIRE((status == S::Ready || status == S::Pending));
    } while (info.CompletedSamples < samples && time::NowTicks() - start < 10 * time::NANOSECONDS_PER_SECOND);
    REQUIRE(info.CompletedSamples == samples);
}
void ReadImage(rhi::DeviceHandle device, rr::GpuPreviewImage image, uint8* output, usize size)
{
    // Acceptance only: production preview never calls either readback service.
    rhi::ReadbackTicket ticket;
    S status = rhi::RequestTextureReadback(device,
                                           image.Texture,
                                           {0, 0, 0, image.Width, image.Height},
                                           usize(image.Width) * 4,
                                           ticket);
    const uint64 start = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - start < 10 * time::NANOSECONDS_PER_SECOND)
    {
        status = rhi::GetStatus(device, ticket);
    }
    REQUIRE(status == S::Ready);
    REQUIRE(rhi::CopyReadback(device, ticket, output, size) == S::Ready);
    REQUIRE(rhi::Release(device, ticket) == S::Ready);
}
uint8 Display(float32 value)
{
    value /= 1 + value;
    const float32 encoded = value <= .0031308F ? value * 12.92F : 1.055F * std::pow(value, 1.0F / 2.4F) - .055F;
    return static_cast<uint8>(std::lround(encoded * 255));
}
} // namespace
TEST_CASE("GPU preview transports emissive RGB on GPU and preserves leased output", "[renderer][gpu][gpu-preview]")
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
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Required.Compute = true;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rr::Renderer renderer;
    REQUIRE(renderer.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment()) ==
            S::Ready);
    rhi::RasterShaderDescription oldKernel = shaders::renderer_gpu_preview::Compute();
    constexpr usize COMPUTE_BINDING_COUNT = 5;
    rhi::RasterBinding oldBindings[COMPUTE_BINDING_COUNT];
    for (usize index = 0; index < COMPUTE_BINDING_COUNT; ++index)
    {
        oldBindings[index] = oldKernel.Bindings[index];
    }
    oldBindings[0].MinSize = 208;
    oldKernel.Bindings = oldBindings;
    CHECK(renderer.EnableGpuPreview(oldKernel,
                                    shaders::renderer_gpu_preview_image::Vertex(),
                                    shaders::renderer_gpu_preview_image::Fragment(),
                                    {usize{32} * 1024 * 1024, usize{32} * 1024 * 1024, 1}) == S::InvalidDescription);
    REQUIRE(renderer.EnableGpuPreview(shaders::renderer_gpu_preview::Compute(),
                                      shaders::renderer_gpu_preview_image::Vertex(),
                                      shaders::renderer_gpu_preview_image::Fragment(),
                                      {usize{32} * 1024 * 1024, usize{32} * 1024 * 1024, 1}) == S::Ready);
    CHECK(renderer.DisableReferenceRendering() == S::InvalidState);
    REQUIRE(renderer.InitializeViews(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()) ==
            S::Ready);
    REQUIRE(renderer.InitializeLighting(shaders::renderer_output::Vertex(),
                                        shaders::renderer_output::Fragment(),
                                        shaders::renderer_shadow::Fragment()) == S::Ready);
    rr::MaterialProgram program;
    S status = renderer.PrewarmLitProgram(shaders::renderer_lit::Vertex(),
                                          shaders::renderer_lit::Fragment(),
                                          shaders::renderer_lit_shadow::Fragment(),
                                          program);
    const uint64 setupStart = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - setupStart < 10 * time::NANOSECONDS_PER_SECOND)
    {
        status = renderer.GetStatus(program);
    }
    REQUIRE(status == S::Ready);
    rr::Snapshot snapshot;
    REQUIRE(ludus::qa::BuildL6Snapshot(renderer, program, snapshot, -.7F) == S::Ready);
    rr::ReferenceCaptureDescription capture;
    capture.OrthographicCamera = true;
    capture.WorldToClip.Columns[3].Z = .7F;
    capture.CameraPosition.Z -= .7F;
    capture.Width = capture.Height = 2;
    capture.TargetSamples = 128;
    capture.MaxDepth = 4;
    capture.Seed = 871;
    // Authored constant environment is permitted; the renderer invents no lights.
    capture.Environment = {.3F, .4F, .5F};
    rr::DirectLight authoredLight;
    authoredLight.SourceId = 41;
    authoredLight.Direction = {0, 0, 1};
    authoredLight.Intensity = .2F;
    capture.Lights = &authoredLight;
    capture.LightCount = 1;
    rr::ReferenceCapture oracle;
    REQUIRE(renderer.BeginReferenceCapture(snapshot, capture, oracle) == S::Ready);
    status = S::Pending;
    const uint64 oracleStart = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - oracleStart < 10 * time::NANOSECONDS_PER_SECOND)
    {
        status = renderer.AdvanceReferenceCapture(oracle);
    }
    REQUIRE(status == S::Ready);
    rr::ReferencePixel reference[4];
    REQUIRE(renderer.CopyReferencePixels(oracle, reference, 4) == S::Ready);
    float32 indirect = 0;
    for (const rr::ReferencePixel& pixel : reference)
    {
        indirect += pixel.Indirect.X;
    }
    REQUIRE(indirect > .01F);
    REQUIRE(renderer.Release(oracle) == S::Ready);
    rr::GpuPreview preview;
    rr::ReferenceCaptureDescription rejected = capture;
    rr::DirectLight light;
    rejected.Lights = &light;
    rejected.LightCount = 1;
    CHECK(renderer.BeginGpuPreview(snapshot, rejected, preview) == S::InvalidDescription);
    rejected = capture;
    rejected.Width = rejected.Height = 4096;
    CHECK(renderer.BeginGpuPreview(snapshot, rejected, preview) == S::CapacityExceeded);
    REQUIRE(renderer.BeginGpuPreview(snapshot, capture, preview) == S::Ready);
    REQUIRE(renderer.Release(snapshot) == S::Ready);
    REQUIRE(renderer.Release(program) == S::Ready);
    rr::GpuPreviewFrame frames[2];
    rr::GpuPreviewImage images[2];
    uint8 early[16], retained[16], final[16];
    for (uint32 sample = 1; sample <= 2; ++sample)
    {
        rhi::SubmissionToken token;
        REQUIRE(renderer.AdvanceGpuPreview(preview, surface, token) == S::Ready);
        WaitPreview(renderer, preview, sample);
        REQUIRE(renderer.AcquireGpuPreviewFrame(preview, frames[sample - 1], images[sample - 1]) == S::Ready);
    }
    ReadImage(device, images[0], early, sizeof(early));
    rhi::SubmissionToken blocked;
    CHECK(renderer.AdvanceGpuPreview(preview, surface, blocked) == S::NotReady);
    rr::GpuPreviewFrame staleFrame = frames[1];
    REQUIRE(renderer.Release(frames[1]) == S::Ready);
    CHECK(renderer.Release(staleFrame) == S::InvalidHandle);
    for (uint32 sample = 3; sample <= capture.TargetSamples; ++sample)
    {
        rhi::SubmissionToken token;
        REQUIRE(renderer.AdvanceGpuPreview(preview, surface, token) == S::Ready);
        WaitPreview(renderer, preview, sample);
    }
    REQUIRE(renderer.AcquireGpuPreviewFrame(preview, frames[1], images[1]) == S::Ready);
    ReadImage(device, images[0], retained, sizeof(retained));
    CHECK(std::memcmp(early, retained, sizeof(early)) == 0);
    ReadImage(device, images[1], final, sizeof(final));
    for (usize pixel = 0; pixel < 4; ++pixel)
    {
        const float32 expected[]{reference[pixel].Radiance.X, reference[pixel].Radiance.Y, reference[pixel].Radiance.Z};
        for (usize channel = 0; channel < 3; ++channel)
        {
            CAPTURE(pixel, channel, final[pixel * 4 + channel], Display(expected[channel]));
            CHECK(math::Abs(float32(final[pixel * 4 + channel]) - float32(Display(expected[channel]))) <= 2);
        }
        CHECK(final[pixel * 4 + 3] == 255);
        CHECK_FALSE((final[pixel * 4] == 255 && final[pixel * 4 + 1] == 0 && final[pixel * 4 + 2] == 255));
    }
    rr::GpuPreview staleJob = preview;
    REQUIRE(renderer.Release(preview) == S::Ready);
    rr::GpuPreviewInfo info;
    CHECK(renderer.GetGpuPreviewInfo(staleJob, info) == S::InvalidHandle);
    // Parent cancellation retains acquired texture until the consumer releases it.
    ReadImage(device, images[1], retained, sizeof(retained));
    CHECK(std::memcmp(final, retained, sizeof(final)) == 0);
    REQUIRE(renderer.Release(frames[0]) == S::Ready);
    REQUIRE(renderer.Release(frames[1]) == S::Ready);
    CHECK(rhi::GetStatus(device, images[1].Texture) == S::InvalidHandle);
}

TEST_CASE("Interactive raster viewport updates World snapshots and camera without tracing", "[renderer][gpu][viewport]")
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
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rr::Renderer renderer;
    REQUIRE(renderer.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment()) ==
            S::Ready);
    REQUIRE(renderer.InitializeViews(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()) ==
            S::Ready);
    REQUIRE(renderer.InitializeLighting(shaders::renderer_output::Vertex(),
                                        shaders::renderer_output::Fragment(),
                                        shaders::renderer_shadow::Fragment()) == S::Ready);
    const math::Vector3 positions[]{{-1, -1, .5F}, {1, -1, .5F}, {1, 1, .5F}, {-1, 1, .5F}};
    const uint32 indices[]{0, 1, 2, 0, 2, 3};
    rr::Mesh mesh;
    REQUIRE(renderer.CreateMesh({positions, 4, indices, 6}, mesh) == S::Ready);
    rr::SceneItem item;
    item.Geometry = mesh;
    item.SourceId = 1;
    item.Material.Color = {1, 0, 0, 1};
    rr::Snapshot world;
    REQUIRE(renderer.CreateSnapshot(&item, 1, world) == S::Ready);
    rr::RasterViewport viewport;
    REQUIRE(viewport.Initialize(renderer, device, 8, 8, 1, rr::ViewportColorMode::PreferHdr, true) == S::Ready);
    rr::ViewDescription camera;
    rr::ViewReport report;
    REQUIRE(viewport.Update(world, camera, report) == S::Ready);
    rr::ViewportFrame frames[2];
    rr::ViewportImage images[2];
    const auto render = [&](uint32 slot) {
        rhi::SubmissionToken producer;
        REQUIRE(viewport.Render(surface, producer) == S::Ready);
        const uint64 timeout = time::NowTicks();
        S status = S::Pending;
        do
        {
            status = viewport.Acquire(frames[slot], images[slot]);
        } while (status == S::Pending && time::NowTicks() - timeout < 10 * time::NANOSECONDS_PER_SECOND);
        REQUIRE(status == S::Ready);
    };
    render(0);
    constexpr usize center = (usize{4} * 8 + 4) * 4;
    uint8 oldImage[256], newImage[256], retained[256];
    ReadImage(device, {images[0].Texture, images[0].Producer, 8, 8, 0}, oldImage, sizeof(oldImage));
    REQUIRE(oldImage[center] >= 126);
    REQUIRE(oldImage[center + 1] == 0);
    item.Material.Color = {0, 1, 0, 1};
    REQUIRE(renderer.ReplaceSnapshot(&item, 1, world) == S::Ready);
    REQUIRE(viewport.Update(world, camera, report) == S::Ready);
    render(1);
    ReadImage(device, {images[1].Texture, images[1].Producer, 8, 8, 0}, newImage, sizeof(newImage));
    CHECK(newImage[center] == 0);
    CHECK(newImage[center + 1] >= 126);
    rhi::SubmissionToken blocked;
    CHECK(viewport.Render(surface, blocked) == S::NotReady);
    ReadImage(device, {images[0].Texture, images[0].Producer, 8, 8, 0}, retained, sizeof(retained));
    CHECK(std::memcmp(oldImage, retained, sizeof(oldImage)) == 0);
    rr::ViewportFrame stale = frames[1];
    REQUIRE(viewport.Release(frames[1]) == S::Ready);
    CHECK(viewport.Release(stale) == S::InvalidHandle);
    camera.WorldToClip.Columns[3].X = 4;
    REQUIRE(viewport.Update(world, camera, report) == S::Ready);
    CHECK(report.Visible == 0);
    render(1);
    ReadImage(device, {images[1].Texture, images[1].Producer, 8, 8, 0}, newImage, sizeof(newImage));
    CHECK(newImage[center] == 0);
    CHECK(newImage[center + 1] == 0);
    REQUIRE(viewport.Release(frames[0]) == S::Ready);
    REQUIRE(viewport.Release(frames[1]) == S::Ready);
    REQUIRE(renderer.Release(world) == S::Ready);
    REQUIRE(renderer.Release(mesh) == S::Ready);
    viewport.Reset();
}

TEST_CASE("Surface maps and authored punctual/area lights agree across raster and GPU reference",
          "[renderer][gpu][fidelity]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription deviceDescription;
    deviceDescription.Required.PortableRaster = true;
    deviceDescription.Required.Compute = true;
    deviceDescription.Storage.ResourcesPerKind = 128;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, deviceDescription, device, surface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rr::Renderer renderer;
    rr::Limits limits;
    limits.Materials = 16;
    REQUIRE(renderer.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment(), limits) ==
            S::Ready);
    REQUIRE(renderer.EnableGpuPreview(shaders::renderer_gpu_preview::Compute(),
                                      shaders::renderer_gpu_preview_image::Vertex(),
                                      shaders::renderer_gpu_preview_image::Fragment(),
                                      {usize{32} * 1024 * 1024, usize{32} * 1024 * 1024, 1}) == S::Ready);
    REQUIRE(renderer.InitializeViews(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()) ==
            S::Ready);
    REQUIRE(renderer.InitializeLighting(shaders::renderer_output::Vertex(),
                                        shaders::renderer_output::Fragment(),
                                        shaders::renderer_shadow::Fragment()) == S::Ready);
    rr::MaterialProgram program;
    rr::MaterialProgram clusteredProgram;
    const auto waitProgram = [&renderer](const rr::MaterialProgram& value, S status) {
        const uint64 start = time::NowTicks();
        while (status == S::Pending && time::NowTicks() - start < 10 * time::NANOSECONDS_PER_SECOND)
        {
            status = renderer.GetStatus(value);
        }
        REQUIRE(status == S::Ready);
    };
    waitProgram(program,
                renderer.PrewarmLitProgram(shaders::renderer_surface::Vertex(),
                                           shaders::renderer_surface::Fragment(),
                                           shaders::renderer_surface_shadow::Fragment(),
                                           program));
    waitProgram(clusteredProgram,
                renderer.PrewarmLitProgram(shaders::renderer_clustered_surface::Vertex(),
                                           shaders::renderer_clustered_surface::Fragment(),
                                           shaders::renderer_clustered_surface_shadow::Fragment(),
                                           clusteredProgram));
    const math::Vector3 positions[]{{-2, -2, .7F}, {2, -2, .7F}, {2, 2, .7F}, {-2, 2, .7F}};
    const math::Vector3 normals[]{{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}};
    const math::Vector2 uvs[]{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const uint32 indices[]{0, 2, 1, 0, 3, 2};
    rr::MeshDescription meshDescription{positions, 4, indices, 6};
    meshDescription.Normals = normals;
    meshDescription.Texcoords = uvs;
    rr::Mesh mesh;
    REQUIRE(renderer.CreateMesh(meshDescription, mesh) == S::Ready);
    // Linear packed map tilts toward +U, modulates roughness and removes metallic.
    // The UV-derived bitangent follows +V even on this clockwise camera-facing quad.
    const uint8 mapBytes[]{204, 128, 128, 0};
    rhi::TextureHandle map;
    rhi::TextureDescription mapDescription{1, 1, rhi::RasterFormat::Rgba8Unorm};
    mapDescription.RetainCpuContent = true;
    REQUIRE(rhi::CreateTexture(device, mapDescription, {{mapBytes, 4}, 4}, map) == S::Ready);
    rr::DirectLight light;
    light.SourceId = 73;
    light.Intensity = 1;
    light.Direction = {-.6F, 0, .8F};
    light.Position = {.6F, 0, -.1F};
    light.Range = 10;
    light.InnerCos = .95F;
    light.OuterCos = .5F;
    rr::LitMaterialDescription material;
    material.SurfaceMap = map;
    material.Metallic = 1;
    material.Roughness = .8F;
    material.Surface.Tint = {.6F, .3F, .1F, 1};
    rr::LightSelection selection;
    rr::MaterialVersion retained;
    REQUIRE(renderer.CreateLitMaterial(program, material, retained, selection) == S::Ready);
    constexpr uint32 LIGHT_CASE_COUNT = 7;
    constexpr usize RASTER_MATERIAL_COUNT = usize{LIGHT_CASE_COUNT} * 2;
    rr::MaterialVersion rasterMaterials[RASTER_MATERIAL_COUNT];
    const auto selectLight = [&light](uint32 kind) {
        light.Kind = kind == 3                ? rr::LightKind::Point
                     : kind == 4 || kind == 6 ? rr::LightKind::AreaOneSided
                     : kind == 5              ? rr::LightKind::AreaTwoSided
                                              : static_cast<rr::LightKind>(kind);
        light.Direction = kind == 2 || kind == 4 ? math::Vector3{0, 0, 1}
                          : kind >= 5            ? math::Vector3{0, 0, -1}
                                                 : math::Vector3{-.6F, 0, .8F};
        light.Position = kind == 3 ? math::Vector3{.003F, 0, .696F} : math::Vector3{.6F, 0, -.1F};
        light.Intensity = kind == 3 ? .0001F : 1;
    };
    rr::LightClusterDescription cluster;
    for (uint32 kind = 0; kind < LIGHT_CASE_COUNT; ++kind)
    {
        selectLight(kind);
        material.Lights = &light;
        material.LightCount = 1;
        material.Clusters = nullptr;
        REQUIRE(renderer.CreateLitMaterial(program, material, rasterMaterials[usize(kind) * 2], selection) == S::Ready);
        material.Clusters = &cluster;
        REQUIRE(
            renderer.CreateLitMaterial(clusteredProgram, material, rasterMaterials[usize(kind) * 2 + 1], selection) ==
            S::Ready);
        CHECK(selection.Dropped == 0);
    }
    REQUIRE(rhi::Destroy(device, map) == S::Ready);
    // The material retains map backing after the public texture owner is destroyed.
    for (uint32 kind = 0; kind < LIGHT_CASE_COUNT; ++kind)
    {
        selectLight(kind);
        rr::SceneItem item;
        item.Geometry = mesh;
        item.Surface = retained;
        item.SourceId = 9;
        rr::Snapshot snapshot;
        REQUIRE(renderer.CreateSnapshot(&item, 1, snapshot) == S::Ready);
        rr::ReferenceCaptureDescription capture;
        capture.OrthographicCamera = true;
        capture.Width = capture.Height = 1;
        capture.CenterRays = true;
        capture.TargetSamples = 1;
        capture.MaxDepth = 1;
        capture.Lights = &light;
        capture.LightCount = 1;
        rr::ReferenceCapture oracle;
        REQUIRE(renderer.BeginReferenceCapture(snapshot, capture, oracle) == S::Ready);
        S status = S::Pending;
        while (status == S::Pending)
        {
            status = renderer.AdvanceReferenceCapture(oracle);
        }
        REQUIRE(status == S::Ready);
        rr::ReferencePixel reference;
        REQUIRE(renderer.CopyReferencePixels(oracle, &reference, 1) == S::Ready);
        REQUIRE(reference.Samples == 1);
        if (kind == 6)
        {
            REQUIRE(reference.Direct.X == 0);
        }
        else
        {
            REQUIRE(reference.Direct.X > .01F);
        }
        // Independent center-pixel expectation: linear data, reconstructed tangent
        // direction, clockwise mesh normal, texture-removed metallic and fixed BRDF.
        const float32 normalY = 1.0F / 255;
        const math::Vector3 expectedNormalWorld{.6F, normalY, -math::Sqrt(1 - .36F - normalY * normalY)};
        math::Vector3 expected;
        REQUIRE(rr::EvaluateLitBrdf({.6F, .3F, .1F},
                                    0,
                                    .8F * (128.0F / 255),
                                    expectedNormalWorld,
                                    {0, 0, -1},
                                    {.6F, 0, -.8F},
                                    expected) == S::Ready);
        if (kind != 0)
        {
            const float32 window = kind == 3 ? 1.0F : 1 - .0001F;
            // The near point is 0.005m away: intensity 1e-4 / the 1e-4 m²
            // attenuation floor equals one, rather than the unbounded value four.
            const float32 cone = (.8F - .5F) / (.95F - .5F);
            const float32 emitterCosine = kind == 6 ? 0.F : kind >= 4 ? .8F : 1.F;
            expected = expected * (window * window * (kind == 2 ? cone * cone : emitterCosine));
        }
        CHECK(math::Length(reference.Direct - expected) < .00001F);
        for (uint32 mode = 0; mode < 2; ++mode)
        {
            rr::SceneItem rasterItem = item;
            rasterItem.Surface = rasterMaterials[usize(kind) * 2 + mode];
            rr::Snapshot rasterSnapshot;
            REQUIRE(renderer.CreateSnapshot(&rasterItem, 1, rasterSnapshot) == S::Ready);
            rr::RasterViewport viewport;
            REQUIRE(viewport.Initialize(renderer, device, 1, 1) == S::Ready);
            rr::ViewDescription camera;
            camera.OrthographicCamera = true;
            camera.CameraForward = {0, 0, 1};
            rr::ViewReport report;
            REQUIRE(viewport.Update(rasterSnapshot, camera, report) == S::Ready);
            rhi::SubmissionToken producer;
            REQUIRE(viewport.Render(surface, producer) == S::Ready);
            rr::ViewportFrame rasterFrame;
            rr::ViewportImage rasterImage;
            S imageStatus = S::Pending;
            const uint64 start = time::NowTicks();
            do
            {
                imageStatus = viewport.Acquire(rasterFrame, rasterImage);
            } while (imageStatus == S::Pending && time::NowTicks() - start < 10 * time::NANOSECONDS_PER_SECOND);
            REQUIRE(imageStatus == S::Ready);
            uint8 rasterPixel[4]{};
            ReadImage(device, {rasterImage.Texture, rasterImage.Producer, 1, 1, 0}, rasterPixel, sizeof(rasterPixel));
            CHECK(std::abs(int(rasterPixel[0]) - int(Display(expected.X))) <= 1);
            CHECK(std::abs(int(rasterPixel[1]) - int(Display(expected.Y))) <= 1);
            CHECK(std::abs(int(rasterPixel[2]) - int(Display(expected.Z))) <= 1);
            REQUIRE(viewport.Release(rasterFrame) == S::Ready);
            viewport.Reset();
            REQUIRE(renderer.Release(rasterSnapshot) == S::Ready);
        }

        rr::GpuPreview preview;
        REQUIRE(renderer.BeginGpuPreview(snapshot, capture, preview) == S::Ready);
        rhi::SubmissionToken token;
        REQUIRE(renderer.AdvanceGpuPreview(preview, surface, token) == S::Ready);
        WaitPreview(renderer, preview, 1);
        rr::GpuPreviewFrame frame;
        rr::GpuPreviewImage image;
        REQUIRE(renderer.AcquireGpuPreviewFrame(preview, frame, image) == S::Ready);
        uint8 pixel[4]{};
        ReadImage(device, image, pixel, sizeof(pixel));
        CHECK(std::abs(int(pixel[0]) - int(Display(reference.Direct.X))) <= 1);
        CHECK(std::abs(int(pixel[1]) - int(Display(reference.Direct.Y))) <= 1);
        CHECK(std::abs(int(pixel[2]) - int(Display(reference.Direct.Z))) <= 1);
        CHECK(pixel[3] == 255);
        REQUIRE(renderer.Release(frame) == S::Ready);
        REQUIRE(renderer.Release(preview) == S::Ready);
        REQUIRE(renderer.Release(oracle) == S::Ready);
        REQUIRE(renderer.Release(snapshot) == S::Ready);
    }
    for (rr::MaterialVersion& value : rasterMaterials)
    {
        REQUIRE(renderer.Release(value) == S::Ready);
    }
    REQUIRE(renderer.Release(retained) == S::Ready);
    REQUIRE(renderer.Release(mesh) == S::Ready);
    REQUIRE(renderer.Release(clusteredProgram) == S::Ready);
    REQUIRE(renderer.Release(program) == S::Ready);
    renderer.Reset();
}

TEST_CASE("CPU and GPU reference transport retained HDR source radiance and authored vertex color",
          "[renderer][gpu][authored]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Required.Compute = true;
    REQUIRE(rhi::CreateDevice({},
                              {
                                  .Width = 8,
                                  .Height = 8,
                              },
                              description,
                              device,
                              surface) == rhi::DeviceStatus::Ready);
    rr::Renderer renderer;
    REQUIRE(renderer.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment()) ==
            S::Ready);
    REQUIRE(renderer.EnableGpuPreview(shaders::renderer_gpu_preview::Compute(),
                                      shaders::renderer_gpu_preview_image::Vertex(),
                                      shaders::renderer_gpu_preview_image::Fragment(),
                                      {usize{64} * 1024 * 1024, usize{64} * 1024 * 1024, 1}) == S::Ready);
    REQUIRE(renderer.InitializeViews(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()) ==
            S::Ready);
    REQUIRE(renderer.InitializeLighting(shaders::renderer_output::Vertex(),
                                        shaders::renderer_output::Fragment(),
                                        shaders::renderer_shadow::Fragment()) == S::Ready);
    rr::MaterialProgram program;
    S status = renderer.PrewarmLitProgram(shaders::renderer_authored::Vertex(),
                                          shaders::renderer_authored::Fragment(),
                                          shaders::renderer_authored_shadow::Fragment(),
                                          program);
    const uint64 setupStart = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - setupStart < 10 * time::NANOSECONDS_PER_SECOND)
    {
        status = renderer.GetStatus(program);
    }
    REQUIRE(status == S::Ready);
    const math::Vector3 positions[]{{-1, -1, .5F}, {1, -1, .5F}, {1, 1, .5F}, {-1, 1, .5F}};
    const math::Vector3 normals[]{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    const math::Vector4 colors[]{{.25F, 1, .5F, 1}, {.25F, 1, .5F, 1}, {.25F, 1, .5F, 1}, {.25F, 1, .5F, 1}};
    const uint32 indices[]{0, 1, 2, 0, 2, 3};
    rr::MeshDescription geometry{positions, 4, indices, 6};
    geometry.Normals = normals;
    geometry.Colors = colors;
    rr::Mesh mesh;
    REQUIRE(renderer.CreateMesh(geometry, mesh) == S::Ready);
    rr::LitMaterialDescription material;
    material.AreAuthoredInputsEnabled = true;
    material.Roughness = 1;
    rr::MaterialVersion version;
    rr::LightSelection selected;
    REQUIRE(renderer.CreateLitMaterial(program, material, version, selected) == S::Ready);
    rr::SceneItem item;
    item.Geometry = mesh;
    item.Surface = version;
    item.SourceId = 1;
    rr::Snapshot snapshot;
    REQUIRE(renderer.CreateSnapshot(&item, 1, snapshot) == S::Ready);
    constexpr usize ENVIRONMENT_PIXELS = usize{32} * 16;
    math::Vector4 pixels[ENVIRONMENT_PIXELS];
    for (math::Vector4& pixel : pixels)
    {
        pixel = {.02F, .02F, .02F, 1};
    }
    pixels[4 * 32 + 9] = {64, 8, 2, 1};
    rr::EnvironmentMap environment;
    rr::EnvironmentDescription hdr;
    hdr.PixelsOrNull = pixels;
    hdr.Width = 32;
    hdr.Height = 16;
    hdr.FilterWidth = 16;
    hdr.FilterSamples = 16;
    hdr.CpuByteBudget = usize{1024} * 1024;
    status = environment.Initialize(device, hdr);
    const uint64 environmentStart = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - environmentStart < 10 * time::NANOSECONDS_PER_SECOND)
    {
        status = environment.GetStatus();
    }
    REQUIRE(status == S::Ready);
    rr::ReferenceCaptureDescription capture;
    capture.OrthographicCamera = true;
    capture.Width = capture.Height = 8;
    capture.TargetSamples = 1024;
    capture.MaxDepth = 1;
    capture.CenterRays = true;
    capture.WorldToClip.Columns[2].Z = -1;
    capture.WorldToClip.Columns[3].Z = 1;
    capture.EnvironmentTextureOrNull = &environment;
    rr::ReferenceCapture oracle;
    REQUIRE(renderer.BeginReferenceCapture(snapshot, capture, oracle) == S::Ready);
    rr::GpuPreview preview;
    REQUIRE(renderer.BeginGpuPreview(snapshot, capture, preview) == S::Ready);
    environment.Reset(); // Accepted captures own the source, not the public wrapper.
    const uint64 oracleStart = time::NowTicks();
    status = S::Pending;
    while (status == S::Pending && time::NowTicks() - oracleStart < 20 * time::NANOSECONDS_PER_SECOND)
    {
        status = renderer.AdvanceReferenceCapture(oracle);
    }
    REQUIRE(status == S::Ready);
    constexpr usize CAPTURE_PIXELS = usize{8} * 8;
    rr::ReferencePixel reference[CAPTURE_PIXELS];
    REQUIRE(renderer.CopyReferencePixels(oracle, reference, CAPTURE_PIXELS) == S::Ready);
    rr::ReferenceCaptureInfo information;
    REQUIRE(renderer.GetReferenceCaptureInfo(oracle, information) == S::Ready);
    CHECK(information.ProfileVersion == 4);
    CHECK(information.InvalidContributions == 0);
    rhi::SubmissionToken completion;
    for (uint32 sample = 1; sample <= capture.TargetSamples; ++sample)
    {
        completion = {};
        REQUIRE(renderer.AdvanceGpuPreview(preview, surface, completion) == S::Ready);
        WaitPreview(renderer, preview, sample);
    }
    rr::GpuPreviewFrame frame;
    rr::GpuPreviewImage image;
    REQUIRE(renderer.AcquireGpuPreviewFrame(preview, frame, image) == S::Ready);
    uint8 result[CAPTURE_PIXELS * 4]{};
    ReadImage(device, image, result, sizeof(result));
    int32 totalError = 0;
    for (usize pixel = 0; pixel < CAPTURE_PIXELS; ++pixel)
    {
        CHECK(reference[pixel].Direct.X > .01F);
        const uint8 expected[]{Display(reference[pixel].Direct.X),
                               Display(reference[pixel].Direct.Y),
                               Display(reference[pixel].Direct.Z)};
        for (usize channel = 0; channel < 3; ++channel)
        {
            const int32 difference = static_cast<int32>(result[pixel * 4 + channel]) - expected[channel];
            totalError += difference < 0 ? -difference : difference;
        }
        CHECK(result[pixel * 4 + 3] == 255);
    }
    // Independent importance distributions differ in FP32 CDF quantization; compare
    // aggregate convergence, not a byte-identical random trajectory.
    CHECK(totalError < static_cast<int32>(CAPTURE_PIXELS * 3 * 3));
    REQUIRE(renderer.Release(frame) == S::Ready);
}

TEST_CASE("Ray admission distinguishes GPU, RHI resource and CPU scratch ceilings", "[renderer][gpu][ray-memory]")
{
    rhi::Shutdown();
    struct DeviceGuard final
    {
        ~DeviceGuard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceDescription deviceDescription;
    deviceDescription.Required.PortableRaster = true;
    deviceDescription.Required.Compute = true;
    rr::GpuPreviewOptions options;
    options.CpuByteBudget = usize{1} * 1024 * 1024;
    options.GpuByteBudget = usize{1} * 1024 * 1024;
    rr::ReferenceCaptureDescription capture;
    capture.Width = capture.Height = 8;
    capture.OrthographicCamera = true;
    rr::SceneMemoryBudget expected = rr::SceneMemoryBudget::GpuPreview;
    SECTION("GPU budget counts pixel resources")
    {
        options.GpuByteBudget = 1024;
    }
    SECTION("CPU scratch counts pixel initialization independently of geometry")
    {
        capture.Width = capture.Height = 256;
        options.GpuByteBudget = usize{8} * 1024 * 1024;
        expected = rr::SceneMemoryBudget::CpuScratch;
    }
    SECTION("RHI resource budget includes other live device resources")
    {
        capture.Width = capture.Height = 256;
        options.CpuByteBudget = options.GpuByteBudget = usize{8} * 1024 * 1024;
        deviceDescription.Storage.ResourceBytes = uint64{1} * 1024 * 1024;
        expected = rr::SceneMemoryBudget::RhiResources;
    }
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, deviceDescription, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No GPU adapter on this host");
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rr::Renderer context;
    REQUIRE(context.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment()) ==
            S::Ready);
    REQUIRE(context.EnableTriangleRayPipeline(shaders::renderer_gpu_preview::Compute(),
                                              shaders::renderer_gpu_preview_image::Vertex(),
                                              shaders::renderer_gpu_preview_image::Fragment(),
                                              options) == S::Ready);
    constexpr usize VERTEX_COUNT = 3;
    StaticArray<math::Vector3, VERTEX_COUNT> positions{{{0, 0, .5F}, {1, 0, .5F}, {0, 1, .5F}}};
    StaticArray<uint32, VERTEX_COUNT> indices{{0, 1, 2}};
    rr::RenderGraphMesh mesh;
    mesh.Geometry = {positions.GetData(), positions.GetSize(), indices.GetData(), indices.GetSize()};
    rr::RenderGraphItem item;
    item.SourceId = 1;
    rr::SceneShaderDiagnostics diagnostics;
    rr::RenderGraphSceneDescription scene;
    scene.Meshes = {&mesh, 1};
    scene.Items = {&item, 1};
    scene.DiagnosticsOrNull = &diagnostics;
    rr::GpuPreview output;
    REQUIRE(context.BeginGpuPreview({}, capture, output, &scene) == S::CapacityExceeded);
    CHECK(diagnostics.ExceededBudget == expected);
    CHECK(diagnostics.RequiredBytes > diagnostics.AvailableBytes);
    if (expected == rr::SceneMemoryBudget::GpuPreview)
    {
        CHECK(diagnostics.AvailableBytes > 0);
        CHECK(diagnostics.AvailableBytes < options.GpuByteBudget);
    }
}
