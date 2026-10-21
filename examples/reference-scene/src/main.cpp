#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/base/parse_number.hpp>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/time/time.hpp>

#include <ludus/content/content.h>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/viewport.hpp>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/measurement.h>
#include <ludus/runtime/resources/scene.h>

#include "ray_query.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_gpu_preview.h"
#include "renderer_gpu_preview_image.h"
#include "renderer_lit.h"
#include "renderer_lit_shadow.h"
#include "renderer_output.h"
#include "renderer_shadow.h"

#include <cstring>
#include <string_view>

namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
using Status = rhi::RasterStatus;
LUDUS_DEFINE_LOG_CATEGORY(LOG_SCENE, "ReferenceScene");

struct SceneEvidence final
{
    // Textures, materials, meshes, objects, vertices, stored and instanced triangles.
    uint64 Counts[7]{};
};

template <typename Handle>
Status Wait(rr::Renderer& renderer, const Handle& handle, Status status) noexcept
{
    const auto started = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - started < 10 * time::NANOSECONDS_PER_SECOND)
    {
        status = renderer.GetStatus(handle);
    }
    return status;
}

bool Load(const uint8* bytes,
          usize count,
          rr::Renderer& renderer,
          rhi::DeviceHandle device,
          rr::MaterialProgram program,
          rr::Snapshot& snapshot,
          rr::ReferenceCaptureDescription& capture,
          SceneEvidence& evidence,
          rr::Snapshot* identitySnapshot) noexcept
{
    ludus::resources::SceneDefinition scene;
    if (ludus::resources::SceneDefinition::Decode(bytes, count, scene) != ludus::resources::Status::Ok)
    {
        return false;
    }
    math::Matrix4 view, projection;
    if (math::TryLookAt(scene.Eye, scene.Target, scene.Up, view) != math::MathStatus::Success ||
        math::TryPerspectiveReverseZ(scene.VerticalFov, 1, .05F, 100, projection) != math::MathStatus::Success)
    {
        return false;
    }
    capture.WorldToClip = projection * view;
    capture.CameraPosition = scene.Eye;
    capture.ReverseZ = true;
    evidence.Counts[0] = scene.Textures.GetSize();
    evidence.Counts[1] = scene.Materials.GetSize();
    evidence.Counts[2] = scene.Meshes.GetSize();
    evidence.Counts[3] = scene.Objects.GetSize();
    rhi::TextureHandle textures[8];
    for (usize i = 0; i < scene.Textures.GetSize(); ++i)
    {
        const auto& source = scene.Textures[i];
        rhi::TextureDescription description;
        description.Width = source.Width;
        description.Height = source.Height;
        description.Format = rhi::RasterFormat::Rgba8Srgb;
        description.RetainCpuContent = true;
        if (rhi::CreateTexture(device,
                               description,
                               {{source.Pixels.GetData(), source.Pixels.GetSize()}, usize(source.Width) * 4},
                               textures[i]) != Status::Ready)
        {
            return false;
        }
    }
    rr::MaterialVersion materials[8];
    for (usize i = 0; i < scene.Materials.GetSize(); ++i)
    {
        const auto& source = scene.Materials[i];
        rr::LitMaterialDescription description;
        description.Metallic = source.Metallic;
        description.Roughness = source.Roughness;
        description.Emission = source.Emission;
        if (source.Texture != ~uint32{0})
        {
            description.Surface.Texture = textures[source.Texture];
            description.Surface.Sampler.Filter = rhi::RasterFilter::Linear;
            description.Surface.Sampler.U = description.Surface.Sampler.V =
                source.Repeat ? rhi::RasterAddress::Repeat : rhi::RasterAddress::Clamp;
        }
        else
        {
            description.Surface.Tint = {0, 0, 0, 1};
        }
        rr::LightSelection selection;
        if (Wait(renderer, materials[i], renderer.CreateLitMaterial(program, description, materials[i], selection)) !=
            Status::Ready)
        {
            return false;
        }
    }
    rr::Mesh meshes[6];
    for (usize i = 0; i < scene.Meshes.GetSize(); ++i)
    {
        const auto& source = scene.Meshes[i];
        evidence.Counts[4] += source.Positions.GetSize();
        evidence.Counts[5] += source.Indices.GetSize() / 3;
        rr::MeshDescription description{source.Positions.GetData(),
                                        source.Positions.GetSize(),
                                        source.Indices.GetData(),
                                        source.Indices.GetSize()};
        description.Normals = source.Normals.GetData();
        description.Texcoords = source.Texcoords.GetData();
        if (Wait(renderer, meshes[i], renderer.CreateMesh(description, meshes[i])) != Status::Ready)
        {
            return false;
        }
    }
    Array<rr::SceneItem> items;
    if (items.TryResize(scene.Objects.GetSize()) == false)
    {
        return false;
    }
    for (usize i = 0; i < scene.Objects.GetSize(); ++i)
    {
        const auto& source = scene.Objects[i];
        auto& item = items[i];
        item.Geometry = meshes[source.Mesh];
        item.Surface = materials[source.Material];
        item.Transform = source.Transform;
        item.SourceId = i + 1;
        evidence.Counts[6] += scene.Meshes[source.Mesh].Indices.GetSize() / 3;
    }
    if (renderer.CreateSnapshot(items.GetData(), items.GetSize(), snapshot) != Status::Ready)
    {
        return false;
    }
    if (identitySnapshot != nullptr)
    {
        for (usize i = 0; i < items.GetSize(); ++i)
        {
            auto& item = items[i];
            const auto id = static_cast<uint32>(item.SourceId);
            item.Surface = {};
            item.Material.Color =
            {
                .X = float32(id) / 255,
                .Y = float32((id * 73) % 255 + 1) / 255,
                .Z = float32((id * 151) % 255 + 1) / 255,
                .W = 1,
            };
        }
        if (renderer.CreateSnapshot(items.GetData(), items.GetSize(), *identitySnapshot) != Status::Ready)
        {
            return false;
        }
    }
    for (usize i = 0; i < scene.Meshes.GetSize(); ++i)
    {
        (void)renderer.Release(meshes[i]);
    }
    for (usize i = 0; i < scene.Materials.GetSize(); ++i)
    {
        (void)renderer.Release(materials[i]);
    }
    for (usize i = 0; i < scene.Textures.GetSize(); ++i)
    {
        if (rhi::Destroy(device, textures[i]) != Status::Ready)
        {
            return false;
        }
    }
    return true;
}

// Public-API acceptance: compare batching/culling with every authored object in
// source order. Raster/reference transport are different models; their images
// are deliberately not compared to one another.
bool RasterEvidence(rr::Renderer& renderer,
                    rhi::DeviceHandle device,
                    rhi::SurfaceHandle surface,
                    rr::Snapshot snapshot,
                    const rr::ReferenceCaptureDescription& capture,
                    const SceneEvidence& evidence,
                    filesystem::WriteDirectory& directory,
                    bool identity = false) noexcept
{
    rr::PreparedView views[2];
    rr::ViewReport reports[2];
    rhi::TextureHandle targets[2];
    for (usize i = 0; i < 2; ++i)
    {
        rr::ViewDescription description;
        description.WorldToClip = capture.WorldToClip;
        description.CameraPosition = capture.CameraPosition;
        description.Depth = rr::DepthConvention::ReverseZ;
        description.Reference = i == 0;
        if (Wait(renderer, views[i], renderer.PrepareView(snapshot, description, views[i], reports[i])) !=
            Status::Ready)
        {
            return false;
        }
        rhi::TextureDescription target{capture.Width, capture.Height, rhi::RasterFormat::Rgba8Unorm};
        target.Attachment = true;
        if (rhi::CreateTexture(device, target, {}, targets[i]) != Status::Ready)
        {
            return false;
        }
    }
    if (reports[0].Submitted != evidence.Counts[3] || reports[0].Visible != evidence.Counts[3] ||
        reports[0].Reported != evidence.Counts[3] || reports[0].Draws != evidence.Counts[3] ||
        reports[1].Submitted != evidence.Counts[3] || reports[1].Visible + reports[1].Culled != evidence.Counts[3] ||
        reports[1].Reported != reports[1].Visible || rhi::BeginFrame(device, surface) != rhi::DeviceStatus::Ready)
    {
        return false;
    }
    bool drawn = true;
    for (usize i = 0; i < 2; ++i)
    {
        rhi::RasterPassDescription pass;
        pass.Color = targets[i];
        drawn = renderer.DrawView(views[i], pass) == Status::Ready && drawn;
    }
    rhi::SubmissionToken completion;
    if (rhi::EndFrame(device, completion) != Status::Ready || drawn == false)
    {
        return false;
    }
    const auto frameStarted = time::NowTicks();
    auto frameStatus = rhi::GetStatus(device, completion);
    while (frameStatus == Status::Pending && time::NowTicks() - frameStarted < 30 * time::NANOSECONDS_PER_SECOND)
    {
        frameStatus = rhi::GetStatus(device, completion);
    }
    if (frameStatus != Status::Ready)
    {
        return false;
    }
    for (usize i = 0; i < 2; ++i)
    {
        const std::string_view name = identity ? (i == 0 ? "identity-reference.rgba" : "identity-optimized.rgba")
                                               : (i == 0 ? "raster-reference.rgba" : "raster-optimized.rgba");
        rhi::ReadbackTicket readback;
        auto status = rhi::RequestTextureReadback(device,
                                                  targets[i],
                                                  {0, 0, 0, capture.Width, capture.Height},
                                                  usize(capture.Width) * 4,
                                                  readback);
        const auto started = time::NowTicks();
        while (status == Status::Pending && time::NowTicks() - started < 30 * time::NANOSECONDS_PER_SECOND)
        {
            status = rhi::GetStatus(device, readback);
        }
        Array<uint8> pixels;
        if (status != Status::Ready || pixels.TryResize(usize(capture.Width) * capture.Height * 4) == false ||
            rhi::CopyReadback(device, readback, pixels.GetData(), pixels.GetSize()) != Status::Ready ||
            directory.Publish(name, {pixels.GetData(), pixels.GetSize()}).Outcome.Succeeded() == false ||
            rhi::Release(device, readback) != Status::Ready || renderer.Release(views[i]) != Status::Ready ||
            rhi::Destroy(device, targets[i]) != Status::Ready)
        {
            return false;
        }
    }
    // LUSAC01: seven scene counts, reference submitted/draws, optimized
    // submitted/visible/culled/draws, selected backend; then uint64 source IDs.
    Array<uint8> bytes;
    rhi::DeviceInfo deviceInfo;
    rhi::MeasurementAdapter adapter;
    if (rhi::GetDeviceInfo(device, deviceInfo) != rhi::DeviceStatus::Ready ||
        rhi::GetMeasurementAdapter(device, adapter) != Status::Ready ||
        bytes.TryResize(8 + 14 * 8 + usize(reports[0].Reported + reports[1].Reported) * 8) == false)
    {
        return false;
    }
    usize nameSize = 0;
    while (nameSize < 255 && adapter.Name[nameSize] != '\0')
    {
        ++nameSize;
    }
    if (directory.Publish("adapter-name.txt", {reinterpret_cast<const uint8*>(adapter.Name), nameSize})
            .Outcome.Succeeded() == false)
    {
        return false;
    }
    std::memcpy(bytes.GetData(), "LUSAC01\0", 8);
    usize offset = 8;
    const auto append = [&bytes, &offset](uint64 value) noexcept {
        if (core::TryWriteLittleEndian<uint64>(value, {bytes.GetData() + offset, 8}) == false)
        {
            return false;
        }
        offset += 8;
        return true;
    };
    for (const auto count : evidence.Counts)
    {
        if (append(count) == false)
        {
            return false;
        }
    }
    if (append(reports[0].Submitted) == false || append(reports[0].Draws) == false ||
        append(reports[1].Submitted) == false || append(reports[1].Visible) == false ||
        append(reports[1].Culled) == false || append(reports[1].Draws) == false)
    {
        return false;
    }
    if (append(static_cast<uint64>(deviceInfo.Startup.SelectedBackend)) == false)
    {
        return false;
    }
    for (const auto& report : reports)
    {
        for (usize i = 0; i < report.Reported; ++i)
        {
            if (append(report.SourceIds[i]) == false)
            {
                return false;
            }
        }
    }
    return directory
        .Publish(identity ? "identity-acceptance.bin" : "scene-acceptance.bin", {bytes.GetData(), bytes.GetSize()})
        .Outcome.Succeeded();
}

bool ViewportEvidence(rr::Renderer& renderer,
                      rhi::DeviceHandle device,
                      rhi::SurfaceHandle surface,
                      rr::Snapshot snapshot,
                      const rr::ReferenceCaptureDescription& camera,
                      filesystem::WriteDirectory& directory) noexcept
{
    rr::RasterViewport viewport;
    Status status = viewport.Initialize(renderer, device, camera.Width, camera.Height);
    const uint64 deadline = time::NowTicks() + 30 * time::NANOSECONDS_PER_SECOND;
    while (status == Status::Pending && time::NowTicks() < deadline)
    {
        status = viewport.Poll();
    }
    if (status != Status::Ready)
    {
        return false;
    }
    rr::ViewDescription view;
    view.WorldToClip = camera.WorldToClip;
    view.CameraPosition = camera.CameraPosition;
    view.Depth = camera.ReverseZ ? rr::DepthConvention::ReverseZ : rr::DepthConvention::Conventional;
    rr::ViewReport report;
    if (viewport.Update(snapshot, view, report) != Status::Ready)
    {
        return false;
    }
    // Repeated interactive frames have no tracing setup, sample target or history.
    for (uint32 frame = 0; frame < 2; ++frame)
    {
        rhi::SubmissionToken producer;
        if (viewport.Render(surface, producer) != Status::Ready)
        {
            return false;
        }
        status = rhi::GetStatus(device, producer);
        while (status == Status::Pending && time::NowTicks() < deadline)
        {
            status = rhi::GetStatus(device, producer);
        }
        if (status != Status::Ready)
        {
            return false;
        }
    }
    rr::ViewportFrame frame;
    rr::ViewportImage image;
    if (viewport.Acquire(frame, image) != Status::Ready)
    {
        return false;
    }
    // Offline evidence only; a Qt consumer imports this texture directly.
    rhi::ReadbackTicket ticket;
    status = rhi::RequestTextureReadback(device,
                                         image.Texture,
                                         {0, 0, 0, image.Width, image.Height},
                                         usize(image.Width) * 4,
                                         ticket);
    while (status == Status::Pending && time::NowTicks() < deadline)
    {
        status = rhi::GetStatus(device, ticket);
    }
    Array<uint8> bytes;
    if (status != Status::Ready || bytes.TryResize(usize(image.Width) * image.Height * 4) == false ||
        rhi::CopyReadback(device, ticket, bytes.GetData(), bytes.GetSize()) != Status::Ready ||
        directory.Publish("viewport.rgba", {bytes.GetData(), bytes.GetSize()}).Outcome.Succeeded() == false)
    {
        return false;
    }
    return rhi::Release(device, ticket) == Status::Ready && viewport.Release(frame) == Status::Ready;
}

bool GpuEvidence(rr::Renderer& renderer,
                 rhi::DeviceHandle device,
                 rhi::SurfaceHandle surface,
                 rr::Snapshot& snapshot,
                 const rr::ReferenceCaptureDescription& description,
                 filesystem::WriteDirectory& directory) noexcept
{
    rr::GpuPreview preview;
    Status status = renderer.BeginGpuPreview(snapshot, description, preview);
    if ((status != Status::Ready && status != Status::Pending) || renderer.Release(snapshot) != Status::Ready)
    {
        return false;
    }
    rr::GpuPreviewInfo info;
    const uint64 started = time::NowTicks();
    while (info.Complete == false && time::NowTicks() - started < 1800 * time::NANOSECONDS_PER_SECOND)
    {
        status = renderer.GetGpuPreviewInfo(preview, info);
        if (status == Status::Pending)
        {
            continue;
        }
        if (status != Status::Ready)
        {
            return false;
        }
        if (info.Complete == false)
        {
            rhi::SubmissionToken producer;
            if (renderer.AdvanceGpuPreview(preview, surface, producer) != Status::Ready)
            {
                return false;
            }
        }
    }
    if (info.Complete == false)
    {
        return false;
    }
    rr::GpuPreviewFrame frame;
    rr::GpuPreviewImage image;
    if (renderer.AcquireGpuPreviewFrame(preview, frame, image) != Status::Ready)
    {
        return false;
    }
    // Offline SDK acceptance exports evidence. Interactive presentation retains
    // this exact texture and producer dependency; it never performs this readback.
    rhi::ReadbackTicket ticket;
    status = rhi::RequestTextureReadback(device,
                                         image.Texture,
                                         {0, 0, 0, image.Width, image.Height},
                                         usize(image.Width) * 4,
                                         ticket);
    const uint64 readStarted = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - readStarted < 30 * time::NANOSECONDS_PER_SECOND)
    {
        status = rhi::GetStatus(device, ticket);
    }
    Array<uint8> bytes;
    if (status != Status::Ready || bytes.TryResize(usize(image.Width) * image.Height * 4) == false ||
        rhi::CopyReadback(device, ticket, bytes.GetData(), bytes.GetSize()) != Status::Ready ||
        directory.Publish("gpu-preview.rgba", {bytes.GetData(), bytes.GetSize()}).Outcome.Succeeded() == false)
    {
        return false;
    }
    uint8 metadata[32]{};
    if (core::TryWriteLittleEndian<uint32>(image.Width, {metadata, 4}) == false ||
        core::TryWriteLittleEndian<uint32>(image.Height, {metadata + 4, 4}) == false ||
        core::TryWriteLittleEndian<uint32>(image.Samples, {metadata + 8, 4}) == false ||
        core::TryWriteLittleEndian<uint64>(info.SceneFingerprintLow, {metadata + 16, 8}) == false ||
        core::TryWriteLittleEndian<uint64>(info.SceneFingerprintHigh, {metadata + 24, 8}) == false ||
        directory.Publish("gpu-preview.bin", {metadata, sizeof(metadata)}).Outcome.Succeeded() == false)
    {
        return false;
    }
    return rhi::Release(device, ticket) == Status::Ready && renderer.Release(preview) == Status::Ready &&
           renderer.Release(frame) == Status::Ready;
}

bool Number(const char* text, uint32 maximum, uint32& output) noexcept
{
    usize size = 0;
    while (size <= 1024 && text[size] != '\0')
    {
        ++size;
    }
    if (size > 1024)
    {
        return false;
    }
    uint64 value = 0;
    if (ParseUint64(text, size, value) != NumberParseStatus::Success || value == 0 || value > maximum)
    {
        return false;
    }
    return core::TryIntegerCast(value, output);
}
} // namespace

int Run(int argc, char** argv) noexcept
{
    if (argc != 5 && argc != 6)
    {
        return 1; // root-directory, output-directory, square extent, samples
    }
    const bool viewport = argc == 6 && std::string_view(argv[5]) == "viewport";
    const bool gpu = argc == 6 && std::string_view(argv[5]) == "gpu";
    const bool native = argc == 6 && std::string_view(argv[5]) == "native";
    const bool acceptance = argc == 6 && (native || std::string_view(argv[5]) == "acceptance");
    if (argc == 6 && acceptance == false && gpu == false && viewport == false)
    {
        return 1;
    }
    rr::ReferenceCaptureDescription description;
    if (Number(argv[3], 1024, description.Width) == false || Number(argv[4], 4096, description.TargetSamples) == false)
    {
        return 2;
    }
    description.Height = description.Width;
    description.MaxDepth = 4;
    description.RouletteDepth = 3;
    description.Seed = 871;
    ludus::content::Bytes bundle;
    if (ludus::content::ReadFile(argv[1], "scene.luscn", usize{64} * 1024 * 1024, bundle) != ludus::content::Status::Ok)
    {
        return 3;
    }
    rhi::DeviceDescription request;
    request.Required.PortableRaster = true;
    request.Required.RayQueries = native == true || gpu == true;
    request.Required.Compute = gpu;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    if (rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, request, device, surface) != rhi::DeviceStatus::Ready)
    {
        return 4;
    }
    struct Shutdown final
    {
        ~Shutdown() noexcept
        {
            rhi::Shutdown();
        }
    } shutdown;
    rr::Renderer renderer;
    if (renderer.Initialize(device,
                            ludus::shaders::renderer_flat::Vertex(),
                            ludus::shaders::renderer_flat::Fragment()) != Status::Ready ||
        renderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                 ludus::shaders::renderer_composite::Fragment()) != Status::Ready ||
        renderer.InitializeLighting(ludus::shaders::renderer_output::Vertex(),
                                    ludus::shaders::renderer_output::Fragment(),
                                    ludus::shaders::renderer_shadow::Fragment()) != Status::Ready)
    {
        return 5;
    }
    const rr::ReferenceRenderingOptions options{native ? rr::ReferenceTraversal::NativeQueries
                                                       : rr::ReferenceTraversal::CpuOracle,
                                                usize{128} * 1024 * 1024,
                                                usize{128} * 1024 * 1024,
                                                512};
    const Status enabled = viewport ? Status::Ready
                           : gpu    ? renderer.EnableGpuPreview(ludus::shaders::renderer_gpu_preview::Compute(),
                                                                ludus::shaders::renderer_gpu_preview_image::Vertex(),
                                                                ludus::shaders::renderer_gpu_preview_image::Fragment(),
                                                                {usize{128} * 1024 * 1024, usize{128} * 1024 * 1024, 1})
                                    : renderer.EnableReferenceRendering(ludus::shaders::ray_query::Query(), options);
    if (enabled != Status::Ready)
    {
        return 6;
    }
    rr::MaterialProgram program;
    if (Wait(renderer,
             program,
             renderer.PrewarmLitProgram(ludus::shaders::renderer_lit::Vertex(),
                                        ludus::shaders::renderer_lit::Fragment(),
                                        ludus::shaders::renderer_lit_shadow::Fragment(),
                                        program)) != Status::Ready)
    {
        return 7;
    }
    rr::Snapshot snapshot;
    rr::Snapshot identitySnapshot;
    SceneEvidence evidence;
    if (Load(bundle.Data().data(),
             bundle.Data().size(),
             renderer,
             device,
             program,
             snapshot,
             description,
             evidence,
             acceptance ? &identitySnapshot : nullptr) == false)
    {
        return 8;
    }
    filesystem::WriteDirectory directory;
    if (directory.Open(argv[2]).Succeeded() == false)
    {
        return 14;
    }
    if (acceptance && RasterEvidence(renderer, device, surface, snapshot, description, evidence, directory) == false)
    {
        return 15;
    }
    if (acceptance &&
        (RasterEvidence(renderer, device, surface, identitySnapshot, description, evidence, directory, true) == false ||
         renderer.Release(identitySnapshot) != Status::Ready))
    {
        return 15;
    }
    if (viewport)
    {
        return ViewportEvidence(renderer, device, surface, snapshot, description, directory) ? 0 : 17;
    }
    if (gpu)
    {
        return GpuEvidence(renderer, device, surface, snapshot, description, directory) ? 0 : 16;
    }
    rr::ReferenceCapture capture;
    auto status = renderer.BeginReferenceCapture(snapshot, description, capture);
    if (status != Status::Ready && status != Status::Pending)
    {
        return 9;
    }
    if (renderer.Release(snapshot) != Status::Ready)
    {
        return 9;
    }
    const auto started = time::NowTicks();
    status = Status::Pending;
    while (status == Status::Pending && time::NowTicks() - started < 1800 * time::NANOSECONDS_PER_SECOND)
    {
        status = renderer.AdvanceReferenceCapture(capture);
    }
    if (status != Status::Ready)
    {
        return 10;
    }
    rr::ReferenceCaptureInfo info;
    if (renderer.GetReferenceCaptureInfo(capture, info) != Status::Ready || info.Complete == false ||
        info.InvalidContributions != 0 || info.MinimumSamples != description.TargetSamples ||
        info.MaximumSamples != description.TargetSamples ||
        info.CommittedSamples != uint64(description.Width) * description.Height * description.TargetSamples)
    {
        return 11;
    }
    Array<uint8> bytes;
    if (bytes.TryResize(256 + usize(description.Width) * description.Height * 16) == false)
    {
        return 12;
    }
    usize written = 0;
    if (renderer.EncodeReferenceHdr(capture, bytes.GetData(), bytes.GetSize(), written) != Status::Ready)
    {
        return 13;
    }
    if (directory.Publish("capture.luhdr", {bytes.GetData(), written}).Outcome.Succeeded() == false)
    {
        return 14;
    }
    return 0;
}

int main(int argc, char** argv)
{
    using namespace ludus::foundation::logging;
    LogConfig config;
    config.EnableFile = false;
    config.EnableDebugger = false;
    LogSystem::Initialize(config);
    const int result = Run(argc, argv);
    if (result != 0)
    {
        LUDUS_LOG_ERROR(LOG_SCENE,
                        "Scene capture failed at step {}. Usage: reference_scene <cooked-directory> <output-directory> "
                        "<extent:1..1024> <samples:1..4096> [acceptance|native|gpu]. See the reference-scene guide for "
                        "step meanings.",
                        result);
    }
    else
    {
        LUDUS_LOG_INFO(LOG_SCENE, "Complete explicit-traversal capture written to capture.luhdr.");
    }
    LogSystem::Shutdown();
    return result;
}
