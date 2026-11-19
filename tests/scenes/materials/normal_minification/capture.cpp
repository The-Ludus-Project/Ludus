// Installed-SDK fixture runner. Requests contain the camera position and the direction
// towards a unit white light. Results contain GPU nanoseconds and the exported RGBA8 rows.
#include <ludus/content/content.h>
#include <ludus/content/render_graph_format/runtime_package.h>
#include <ludus/content/scene_format/limits.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/measurement.h>
#include <ludus/runtime/resources/scene.h>

#include "renderer_composite.h"
#include "sweep.h"

#include <cstdio>
#include <cstring>

namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace content = ludus::content;
namespace resources = ludus::resources;
using Status = rhi::RasterStatus;
using namespace ludus::qa::normal_minification;
constexpr usize BINDINGS = 3;
constexpr uint64 WAIT_NANOSECONDS = 120 * time::NANOSECONDS_PER_SECOND;
constexpr usize RGBA_BYTES = 4;
template <class Poll>
bool WaitForReady(Poll poll) noexcept
{
    const uint64 start = time::NowTicks();
    do
    {
        const Status status = poll();
        if (status == Status::Ready)
        {
            return true;
        }
        if (status != Status::NotReady && status != Status::Pending)
        {
            return false;
        }
        threading::YieldThread();
    } while (time::NowTicks() - start < WAIT_NANOSECONDS);
    return false;
}

rhi::TextureCpuContent GetTexture(const resources::SceneTexture& texture) noexcept
{
    return {texture.Pixels.GetData(),
            texture.Pixels.GetSize(),
            texture.Width * RGBA_BYTES,
            texture.Width,
            texture.Height,
            texture.IsLinear == true ? rhi::RasterFormat::Rgba8Unorm : rhi::RasterFormat::Rgba8Srgb};
}

struct Extraction final
{
    StaticArray<rr::SceneShaderMaterial, BINDINGS> Materials{};
    StaticArray<rr::RenderGraphMesh, BINDINGS> Meshes{};
    StaticArray<rr::RenderGraphItem, BINDINGS> Items{};
    bool Load(const resources::SceneDefinition& scene) noexcept
    {
        if (scene.Objects.GetSize() != BINDINGS || scene.Materials.GetSize() != BINDINGS || scene.Lights.GetSize() != 1)
        {
            return false;
        }
        for (usize i = 0; i < BINDINGS; ++i)
        {
            const resources::SceneObject& object = scene.Objects[i];
            const resources::SceneMesh& mesh = scene.Meshes[object.Mesh];
            Meshes[i].Geometry = {mesh.Positions.GetData(),
                                  mesh.Positions.GetSize(),
                                  mesh.Indices.GetData(),
                                  mesh.Indices.GetSize()};
            Meshes[i].Geometry.Normals = mesh.Normals.GetData();
            Meshes[i].Geometry.Texcoords = mesh.Texcoords.GetData();
            Meshes[i].Geometry.Tangents = {mesh.Tangents.GetData(), mesh.Tangents.GetSize()};
            Meshes[i].MaterialBindings = {&object.Material, 1};
            Items[i].MeshIndex = static_cast<uint32>(i);
            Items[i].SourceId = object.SourceId;
            Items[i].MeshToWorld = object.Transform;
            const resources::SceneMaterial& source = scene.Materials[i];
            if (source.Texture >= scene.Textures.GetSize() || source.SurfaceTexture >= scene.Textures.GetSize() ||
                source.SurfaceTexture != scene.Materials[0].SurfaceTexture ||
                source.NormalScale != static_cast<float32>(i) || source.Metallic != 1 || source.Roughness != 0.25F)
            {
                return false;
            }
            rr::SceneShaderMaterial& material = Materials[i];
            material.ColorTexture = GetTexture(scene.Textures[source.Texture]);
            material.SurfaceTexture = GetTexture(scene.Textures[source.SurfaceTexture]);
            material.BaseColor = source.Reflectance;
            material.Metallic = source.Metallic;
            material.Roughness = source.Roughness;
            material.NormalScale = source.NormalScale;
            material.Sampler.Filter = rhi::RasterFilter::Linear;
            material.Sampler.U = source.Repeat == true ? rhi::RasterAddress::Repeat : rhi::RasterAddress::Clamp;
            material.Sampler.V = material.Sampler.U;
        }
        return true;
    }
};

int ReportFailure(const char* message) noexcept
{
    std::fprintf(stderr, "minification_capture: %s\n", message);
    return 1;
}

bool ReadScene(const char* directory, resources::SceneDefinition& outScene)
{
    content::Bytes bundle;
    return content::ReadFile(directory, "scene.luscn", content::scene_format::IMPORT_LIMITS.BundleBytes, bundle) ==
               content::Status::Ok &&
           resources::SceneDefinition::Decode(bundle.Data().data(), bundle.Data().size(), outScene) ==
               resources::Status::Ok;
}

// Verify the actual material binding and decoded base level independently of shader output.
int WritePackedSurface(const char* directory, const char* destination)
{
    resources::SceneDefinition scene;
    Extraction extraction;
    if (ReadScene(directory, scene) == false || extraction.Load(scene) == false)
    {
        return ReportFailure("Regenerate the three bindings and their shared surface map with fixture.py.");
    }
    const resources::SceneTexture& texture = scene.Textures[scene.Materials[0].SurfaceTexture];
    if (texture.IsLinear == false || texture.Width != EXTENT || texture.Height != EXTENT)
    {
        return ReportFailure("Use the full-resolution runtime fixture to measure the packed base level.");
    }
    std::FILE* outputOrNull = std::fopen(destination, "wb");
    if (outputOrNull == nullptr)
    {
        return ReportFailure("Check the destination for the decoded surface map.");
    }
    const usize written = std::fwrite(texture.Pixels.GetData(), 1, texture.Pixels.GetSize(), outputOrNull);
    if (std::fclose(outputOrNull) != 0 || written != texture.Pixels.GetSize())
    {
        return ReportFailure("Writing the packed surface failed. Check available disk space.");
    }
    return 0;
}

int Capture(const char* sceneDirectory,
            const char* packageDirectory,
            const char* packageName,
            std::FILE& inoutRequests,
            std::FILE& outFrames)
{
    resources::SceneDefinition scene;
    Extraction extraction;
    if (ReadScene(sceneDirectory, scene) == false || extraction.Load(scene) == false)
    {
        return ReportFailure("Regenerate this cooked scene with fixture.py and the matching ludus-cook.");
    }
    content::Bytes bytes;
    rr::RenderGraphPackage package;
    if (content::ReadFile(packageDirectory, packageName, content::render_graph_format::MAX_PACKAGE_BYTES, bytes) !=
            content::Status::Ok ||
        package.Decode({bytes.Data().data(), bytes.Data().size()}) != rr::RenderGraphPackageStatus::Ready)
    {
        return ReportFailure("Rebuild the requested runtime package for the installed renderer.");
    }
    rhi::DeviceDescription request;
    request.Required.PortableRaster = true;
    request.EnableGpuTiming = true;
    request.Storage.ResourcesPerKind = DEVICE_RESOURCES;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    if (rhi::CreateDevice({}, { .Width = EXTENT, .Height = EXTENT }, request, device, surface) !=
        rhi::DeviceStatus::Ready)
    {
        return ReportFailure("Use a host with an available native raster device.");
    }
    rr::RenderGraph graph;
    const rr::RenderGraphPlan plan = package.GetPlan();
    uint32 exported = 0;
    while (exported < plan.Resources.GetSize() && plan.Resources[exported].IsExported == 0)
    {
        ++exported;
    }
    const Status initialized = graph.Initialize(device,
                                                plan,
                                                package.GetPrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                    ludus::shaders::renderer_composite::Fragment()),
                                                nullptr,
                                                EXTENT,
                                                EXTENT);
    if ((initialized != Status::Ready && initialized != Status::Pending) ||
        WaitForReady([&]() { return graph.Poll(); }) == false)
    {
        return ReportFailure("Check that the graph profile matches the active device.");
    }
    rr::DirectLight light;
    const resources::SceneLight& sourceLight = scene.Lights[0];
    if (sourceLight.IsEnabled == false || sourceLight.Kind != resources::SceneLightKind::Directional)
    {
        return ReportFailure("Regenerate this cooked scene with fixture.py and the matching ludus-cook.");
    }
    light.SourceId = sourceLight.SourceId;
    light.Color = sourceLight.IntensityRgb;
    light.Intensity = 1;
    light.ShouldCastRasterShadows = false;
    rr::SceneShaderInputs inputs;
    inputs.Materials = {extraction.Materials.GetData(), BINDINGS};
    inputs.Lights = {&light, 1};
    uint64 revision = 0;
    Frame frame{};
    usize count = 0;
    while ((count = std::fread(&frame, 1, sizeof(frame), &inoutRequests)) == sizeof(frame))
    {
        rr::ViewDescription camera;
        if (MakeView(frame, scene.VerticalFov, camera) == false)
        {
            return ReportFailure("Regenerate the invalid camera request with study.py.");
        }
        light.Direction = frame.TowardsLightWorld * -1.F;
        rr::RenderGraphReport report;
        const Status updated = graph.Update({extraction.Meshes.GetData(), BINDINGS},
                                            {extraction.Items.GetData(), BINDINGS},
                                            {&camera, 1},
                                            ++revision,
                                            rr::RenderGraphCoveragePolicy::RejectIncomplete,
                                            report,
                                            &inputs);
        if ((updated != Status::Ready && updated != Status::Pending) ||
            WaitForReady([&]() { return graph.Poll(); }) == false)
        {
            return ReportFailure(
                "The renderer rejected the scene inputs. Rebuild the scene and graph from the same fixture.");
        }
        rhi::SubmissionToken token;
        const bool hasTimer = rhi::ArmGpuTiming(device) == Status::Ready;
        if (graph.Render(surface, EXTENT, EXTENT, token) != Status::Ready)
        {
            return ReportFailure("The GPU draw failed. Check device availability and rerun the capture.");
        }
        rhi::TextureHandle target;
        rhi::TextureAspect aspect{};
        if (WaitForReady([&]() {
                static_cast<void>(rhi::PollLifetime(device));
                return graph.GetExport(exported, target, aspect);
            }) == false)
        {
            return ReportFailure("The graph did not publish its color export. Check the graph output and device.");
        }
        const float64 nanoseconds = GetFrameNanoseconds(device, hasTimer);
        rhi::ReadbackTicket ticket;
        const Status requested = rhi::RequestTextureReadback(device,
                                                             target,
                                                             { .Width = EXTENT, .Height = EXTENT },
                                                             EXTENT * RGBA_BYTES,
                                                             ticket);
        StaticArray<uint8, EXTENT * EXTENT * RGBA_BYTES> pixels{};
        if ((requested != Status::Ready && requested != Status::Pending) ||
            WaitForReady([&]() {
                static_cast<void>(rhi::PollLifetime(device));
                return rhi::CopyReadback(device, ticket, pixels.GetData(), pixels.GetSize());
            }) == false ||
            rhi::Release(device, ticket) != Status::Ready)
        {
            return ReportFailure("The exported texture could not be read back. Check the graph format and device.");
        }
        if (std::fwrite(&nanoseconds, sizeof(nanoseconds), 1, &outFrames) != 1 ||
            std::fwrite(pixels.GetData(), 1, pixels.GetSize(), &outFrames) != pixels.GetSize())
        {
            return ReportFailure("Writing the frame failed. Check the output path and available disk space.");
        }
    }
    if (revision == 0 || count != 0 || std::feof(&inoutRequests) == 0)
    {
        return ReportFailure("Incomplete frame requests. Regenerate them with study.py.");
    }
    return 0;
}
} // namespace

int main(int argumentCount, char** arguments)
{
    if (argumentCount == 4 && std::strcmp(arguments[1], "--read-packed") == 0)
    {
        return WritePackedSurface(arguments[2], arguments[3]);
    }
    if (argumentCount != 6)
    {
        return ReportFailure(
            "Expected scene directory, graph directory, graph filename, request file and output file.");
    }
    std::FILE* requestsOrNull = std::fopen(arguments[4], "rb");
    if (requestsOrNull == nullptr)
    {
        return ReportFailure("Check that the request file exists and is readable.");
    }
    std::FILE* framesOrNull = std::fopen(arguments[5], "wb");
    if (framesOrNull == nullptr)
    {
        std::fclose(requestsOrNull);
        return ReportFailure("Check that the frame output directory is writable.");
    }
    const int result = Capture(arguments[1], arguments[2], arguments[3], *requestsOrNull, *framesOrNull);
    ludus::graphics::rhi::Shutdown();
    std::fclose(requestsOrNull);
    if (std::fclose(framesOrNull) != 0)
    {
        return ReportFailure("Writing the frames failed. Check available disk space.");
    }
    return result;
}
