// Standalone installed-SDK consumer for the generated one-receiver glTF scenes.
#include "renderer_authored.h"
#include "renderer_authored_shadow.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_output.h"
#include "renderer_shadow.h"

#include <ludus/content/scene_cook/best_effort_import.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/runtime/resources/scene.h>

#include <fstream>
#include <iostream>

namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace cook = ludus::content::scene_cook;
namespace resources = ludus::resources;
namespace shaders = ludus::shaders;
using Status = rhi::RasterStatus;
constexpr uint32 EXTENT = 65; // Odd extent puts one pixel centre on the optical axis.
constexpr usize ROW_BYTES = EXTENT * 4;
constexpr usize ENVIRONMENT_BUDGET_BYTES = usize{64} * 1024 * 1024;
constexpr uint64 READY_TIMEOUT_TICKS = 120 * time::NANOSECONDS_PER_SECOND;
// Increasing exposures extend precision for dark probes in the RGBA8 transfer path.
constexpr float32 READBACK_EXPOSURES[]{.125F, 1, 8, 64, 512};

template <typename Poll>
bool WaitForReady(Status status, const Poll& poll)
{
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < READY_TIMEOUT_TICKS)
    {
        threading::YieldThread();
        status = poll();
    }
    if (status != Status::Ready)
    {
        std::cerr << "Raster status " << static_cast<int>(status) << '\n';
    }
    return status == Status::Ready;
}

bool CaptureLinearFrame(rr::Renderer& inoutRenderer,
                        rhi::DeviceHandle device,
                        rhi::SurfaceHandle surface,
                        rr::PreparedView view,
                        const rr::ViewMapping& mapping)
{
    rhi::RasterPassDescription hdr;
    rhi::RasterPassDescription linear;
    if (rhi::CreateTexture(device, {EXTENT, EXTENT, rhi::RasterFormat::Rgba16Float, true}, {}, hdr.Color) !=
            Status::Ready ||
        rhi::CreateTexture(device, {EXTENT, EXTENT, rhi::RasterFormat::Rgba8Unorm, true}, {}, linear.Color) !=
            Status::Ready)
    {
        return false;
    }
    if (rhi::BeginFrame(device, surface) != rhi::DeviceStatus::Ready)
    {
        return false;
    }
    const Status drawn = inoutRenderer.DrawView(view, hdr);
    rhi::SubmissionToken completion;
    if (rhi::EndFrame(device, completion) != Status::Ready || drawn != Status::Ready)
    {
        return false;
    }
    Array<float32> reconstructed(usize{EXTENT} * EXTENT * 3);
    for (const float32 exposure : READBACK_EXPOSURES)
    {
        rr::LightingOutput output;
        output.Exposure = exposure;
        output.ToneMap = false;
        output.Encode = false;
        rr::Presentation presentation;
        if (WaitForReady(inoutRenderer.PrepareLightingPresentation(hdr.Color, mapping, output, presentation),
                         [&] { return inoutRenderer.GetStatus(presentation); }) == false ||
            rhi::BeginFrame(device, surface) != rhi::DeviceStatus::Ready)
        {
            return false;
        }
        const Status composed = inoutRenderer.DrawLightingPresentation(presentation, linear);
        rhi::SubmissionToken submission;
        if (rhi::EndFrame(device, submission) != Status::Ready || composed != Status::Ready)
        {
            return false;
        }
        rhi::ReadbackTicket ticket;
        const Status read =
            rhi::RequestTextureReadback(device, linear.Color, { .Width = EXTENT, .Height = EXTENT }, ROW_BYTES, ticket);
        Array<uint8> image(ROW_BYTES * EXTENT);
        if (WaitForReady(read, [&] { return rhi::GetStatus(device, ticket); }) == false ||
            rhi::CopyReadback(device, ticket, image.GetData(), image.GetSize()) != Status::Ready ||
            rhi::Release(device, ticket) != Status::Ready || inoutRenderer.Release(presentation) != Status::Ready)
        {
            return false;
        }
        for (usize i = 0; i < reconstructed.GetSize(); ++i)
        {
            const uint8 code = image[(i / 3) * 4 + i % 3];
            if (code < 255)
            {
                reconstructed[i] = static_cast<float32>(code) / (255 * exposure);
            }
            else if (exposure == READBACK_EXPOSURES[0])
            {
                std::cerr << "Clipped HDR readback\n";
                return false;
            }
        }
    }
    std::ofstream file("linear.rgb", std::ios::binary);
    file.write(reinterpret_cast<const char*>(reconstructed.GetData()),
               static_cast<std::streamsize>(reconstructed.GetSize() * sizeof(float32)));
    file.close();
    return file.good() == true && rhi::Destroy(device, linear.Color) == Status::Ready &&
           rhi::Destroy(device, hdr.Color) == Status::Ready;
}

bool PrepareEnvironment(rhi::DeviceHandle device,
                        const resources::SceneEnvironment& source,
                        uint32 filterWidth,
                        uint32 samples,
                        rr::EnvironmentMap& outEnvironment)
{
    Array<math::Vector4> pixels;
    std::ofstream cooked("cooked.rgb", std::ios::binary);
    for (const math::Vector3 value : source.Pixels)
    {
        const math::Vector4 radiance{value.X * source.Rgb.X, value.Y * source.Rgb.Y, value.Z * source.Rgb.Z, 1};
        pixels.Add(radiance);
        const float32 rgb[]{radiance.X, radiance.Y, radiance.Z};
        cooked.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
    }
    cooked.close();
    std::ofstream dimensions("dimensions.txt");
    dimensions << source.Width << ' ' << source.Height << '\n';
    dimensions.close();
    if (cooked.fail() == true || dimensions.fail() == true)
    {
        return false;
    }
    rr::EnvironmentDescription description{pixels.GetData(),
                                           source.Width,
                                           source.Height,
                                           filterWidth,
                                           samples,
                                           ENVIRONMENT_BUDGET_BYTES};
    return WaitForReady(outEnvironment.Initialize(device, description), [&] { return outEnvironment.GetStatus(); });
}

bool Render(const resources::SceneDefinition& scene, uint32 filterWidth, uint32 samples)
{
    rhi::DeviceDescription request;
    request.Required.PortableRaster = true;
    request.RequiredProfile.Raster.MinBindings = 10;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    if (rhi::CreateDevice({}, { .Width = EXTENT, .Height = EXTENT }, request, device, surface) !=
        rhi::DeviceStatus::Ready)
    {
        std::cerr << "Headless device unavailable\n";
        return false;
    }
    rhi::DeviceInfo info;
    if (rhi::GetDeviceInfo(device, info) != rhi::DeviceStatus::Ready)
    {
        return false;
    }
    std::cout << "Authored Lit backend "
              << (info.Startup.SelectedBackend == rhi::Backend::Metal    ? "Metal"
                  : info.Startup.SelectedBackend == rhi::Backend::Vulkan ? "Vulkan"
                                                                         : "D3D12")
              << '\n';
    rr::Renderer renderer;
    if (renderer.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment()) !=
            Status::Ready ||
        renderer.InitializeViews(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()) !=
            Status::Ready ||
        renderer.InitializeLighting(shaders::renderer_output::Vertex(),
                                    shaders::renderer_output::Fragment(),
                                    shaders::renderer_shadow::Fragment()) != Status::Ready)
    {
        return false;
    }
    rr::MaterialProgram program;
    const Status warmed = renderer.PrewarmLitProgram(shaders::renderer_authored::Vertex(),
                                                     shaders::renderer_authored::Fragment(),
                                                     shaders::renderer_authored_shadow::Fragment(),
                                                     program);
    if (WaitForReady(warmed, [&] { return renderer.GetStatus(program); }) == false)
    {
        return false;
    }
    rr::EnvironmentMap environment;
    if (PrepareEnvironment(device, scene.Environment, filterWidth, samples, environment) == false)
    {
        return false;
    }
    const resources::SceneObject& object = scene.Objects[0];
    const resources::SceneMaterial& material = scene.Materials[object.Material];
    rr::LitMaterialDescription surfaceDescription;
    surfaceDescription.AreAuthoredInputsEnabled = true;
    surfaceDescription.EnvironmentOrNull = &environment;
    surfaceDescription.Metallic = material.Metallic;
    surfaceDescription.Roughness = material.Roughness;
    surfaceDescription.NormalScale = 0;
    // Every generated receiver has an achromatic uniform base colour. Use its cooked constant swatch.
    const resources::SceneTexture& swatch = scene.Textures[material.Texture];
    rhi::TextureHandle texture;
    if (rhi::CreateTexture(device,
                           {swatch.Width, swatch.Height, rhi::RasterFormat::Rgba8Srgb},
                           {{swatch.Pixels.GetData(), swatch.Pixels.GetSize()}, usize{swatch.Width} * 4},
                           texture) != Status::Ready)
    {
        return false;
    }
    surfaceDescription.Surface.Texture = texture;
    rr::MaterialVersion version;
    rr::LightSelection lights;
    if (WaitForReady(renderer.CreateLitMaterial(program, surfaceDescription, version, lights),
                     [&] { return renderer.GetStatus(version); }) == false ||
        rhi::Destroy(device, texture) != Status::Ready)
    {
        return false;
    }
    const resources::SceneMesh& sourceMesh = scene.Meshes[object.Mesh];
    rr::MeshDescription geometry{sourceMesh.Positions.GetData(),
                                 sourceMesh.Positions.GetSize(),
                                 sourceMesh.Indices.GetData(),
                                 sourceMesh.Indices.GetSize()};
    geometry.Normals = sourceMesh.Normals.GetData();
    geometry.Texcoords = sourceMesh.Texcoords.GetData();
    rr::Mesh mesh;
    if (WaitForReady(renderer.CreateMesh(geometry, mesh), [&] { return renderer.GetStatus(mesh); }) == false)
    {
        return false;
    }
    rr::SceneItem item;
    item.SourceId = object.SourceId;
    item.Geometry = mesh;
    item.Surface = version;
    item.Transform = object.Transform;
    rr::Snapshot snapshot;
    rr::ViewMapping mapping;
    rr::ProjectionDescription lens;
    lens.VerticalFovRadians = scene.VerticalFov;
    rr::ViewDescription camera;
    if (renderer.CreateSnapshot(&item, 1, snapshot) != Status::Ready ||
        rr::ResolveViewMapping({EXTENT, EXTENT, rr::ScreenFit::Fill},
                               {0, 0, EXTENT, EXTENT},
                               EXTENT,
                               EXTENT,
                               mapping) != Status::Ready ||
        math::TryLookAt(scene.Eye, scene.Target, scene.Up, lens.WorldToView) != math::MathStatus::Success ||
        rr::BuildViewDescription(lens, mapping, camera) != Status::Ready)
    {
        return false;
    }
    camera.CameraPosition = scene.Eye;
    rr::PreparedView view;
    rr::ViewReport report;
    if (WaitForReady(renderer.PrepareView(snapshot, camera, view, report), [&] { return renderer.GetStatus(view); }) ==
            false ||
        report.Visible != 1)
    {
        return false;
    }
    return CaptureLinearFrame(renderer, device, surface, view, mapping);
}
} // namespace
int main()
{
    uint32 width = 0;
    uint32 samples = 0;
    if ((std::cin >> width >> samples).fail() == true)
    {
        return 1;
    }
    cook::BestEffortOptions options;
    options.EnvironmentFile = "environment.hdr";
    cook::BestEffortResult result;
    cook::CookError error;
    resources::SceneDefinition scene;
    if (cook::ImportGltfBestEffort("receiver.gltf", options, result, error) == false ||
        resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), scene) !=
            resources::Status::Ok ||
        scene.Objects.GetSize() != 1 || scene.Materials[scene.Objects[0].Material].Texture >= scene.Textures.GetSize())
    {
        std::cerr << "Receiver import failed: " << error.Message.View() << '\n';
        return 1;
    }
    const bool isReady = Render(scene, width, samples);
    rhi::Shutdown();
    return isReady == true ? 0 : 1;
}
