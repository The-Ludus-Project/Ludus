// A standalone installed-SDK application that draws ONE real, cooked character-bust World (its
// meshes, material slots, the hair opacity texture and the skin inputs) and renders it through the
// packaged forward-lit graph (for hair cards) or the clustered-deferred graph (for real-time skin),
// so verify.py can compare the clean package with the Editor on the same fixture (#1400).
//
// It links no Qt: it loads the cooked scene.luscn through Ludus::Resources, renders through the
// packaged render graph via Ludus::GraphicsRenderer, reads the frame back into raw RGBA8 and writes
// it with Ludus::Content. The Python driver converts the raw RGBA to PNG and compares.
//
// Usage:
//   hair_skin_capture --profile forward-lit         Draw the bust through the forward-lit graph (hair).
//   hair_skin_capture --profile clustered-deferred  Draw the bust through the clustered-deferred graph (skin).
//   hair_skin_capture --check-profile <feature> <profile>
//       Report, without rendering, whether <profile> can shade <feature> (hair|skin) and whether the
//       cooked scene actually carries that feature's typed material. A mismatch or a missing input
//       prints an actionable diagnostic and returns a distinct exit code, so packaging refuses an
//       incompatible selection or a missing dependency before it ships.
#include "renderer_composite.h"

#include <ludus/content/content.h>
#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/strings/status.hpp>
#include <ludus/foundation/strings/string.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/light_selection.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/scene_shader.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/runtime/resources/scene.h>

#include <string_view>

namespace
{
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace format = ludus::content::scene_format;
namespace resources = ludus::resources;
using Status = rhi::RasterStatus;
LUDUS_DEFINE_LOG_CATEGORY(LOG_HAIR_SKIN, "HairSkinBust");

/// Distinct process exit codes so the driver can tell a diagnostic refusal from a runtime failure.
constexpr int EXIT_OK = 0;
constexpr int EXIT_FAILURE_CODE = 1;
constexpr int EXIT_INCOMPATIBLE_PROFILE = 3;
constexpr int EXIT_MISSING_DEPENDENCY = 4;
/// Longest wait for a device operation to settle, in nanoseconds.
constexpr uint64 OPERATION_TIMEOUT_NANOSECONDS = 60 * time::NANOSECONDS_PER_SECOND;
/// Capture extent, matched with the Editor's bust viewport capture.
constexpr uint32 CAPTURE_WIDTH = 1600;
constexpr uint32 CAPTURE_HEIGHT = 1080;
/// Bytes of one RGBA8 pixel.
constexpr usize RGBA8_PIXEL_BYTES = 4;
/// Largest cooked scene bundle the app admits, matching the assembly-profile bound.
constexpr usize MAX_SCENE_BYTES = 576 * 1024 * 1024;
/// Near and far planes of the capture camera, in metres (the bust is about 17 cm wide).
constexpr float32 CAPTURE_NEAR_METRES = 0.02F;
constexpr float32 CAPTURE_FAR_METRES = 2.0F;
/// Startup probe extent; the real target is set from the capture extent before rendering.
constexpr uint32 PROBE_WIDTH = 96;
constexpr uint32 PROBE_HEIGHT = 64;

/// The two raster profiles this qualification selects. No single profile shades both features:
/// hair cards need the forward-lit profile's authored strand inputs, and real-time skin diffusion is
/// implemented only by the clustered-deferred default profile. See
/// docs/architecture/renderer-systems.md#hair-and-skin-rendering.
enum class Profile : uint32
{
    ForwardLit,        ///< Shades hair cards from their authored strand highlight (packaged forward_lit graph).
    ClusteredDeferred, ///< Spreads skin diffuse lighting (packaged clustered-deferred default graph).
};

enum class Feature : uint32
{
    Hair,
    Skin,
};

/// The feature each profile owns in this qualification. A forward-lit profile shades skin as its
/// metallic/roughness surface, and the clustered-deferred profile shades hair with ordinary lobes, so
/// each is paired with the one feature it renders faithfully.
Feature OwnedFeature(Profile profile) noexcept
{
    return profile == Profile::ForwardLit ? Feature::Hair : Feature::Skin;
}

/// The cooked render-graph package each profile draws through, staged beside the app by verify.py.
const char* ProfilePackageFile(Profile profile) noexcept
{
    return profile == Profile::ForwardLit ? "forward_lit.lurgrt" : "clustered_deferred.lurgrt";
}

const char* ProfileName(Profile profile) noexcept
{
    return profile == Profile::ForwardLit ? "forward-lit" : "clustered-deferred";
}

const char* CompatibleProfileName(Feature feature) noexcept
{
    return feature == Feature::Hair ? "forward-lit" : "clustered-deferred";
}

const char* FeatureName(Feature feature) noexcept
{
    return feature == Feature::Hair ? "hair" : "skin";
}

format::MaterialModel FeatureModel(Feature feature) noexcept
{
    return feature == Feature::Hair ? format::MaterialModel::HairCard : format::MaterialModel::Skin;
}

bool TryParseProfile(std::string_view text, Profile& outProfile) noexcept
{
    if (text == "forward-lit")
    {
        outProfile = Profile::ForwardLit;
        return true;
    }
    if (text == "clustered-deferred")
    {
        outProfile = Profile::ClusteredDeferred;
        return true;
    }
    return false;
}

bool TryParseFeature(std::string_view text, Feature& outFeature) noexcept
{
    if (text == "hair")
    {
        outFeature = Feature::Hair;
        return true;
    }
    if (text == "skin")
    {
        outFeature = Feature::Skin;
        return true;
    }
    return false;
}

/// Read a file beside the app into owned bytes; an absent or oversized file is reported.
bool ReadLocalFile(std::string_view name, usize cap, ludus::content::Bytes& outBytes)
{
    const ludus::content::Status status = ludus::content::ReadFile(".", name, cap, outBytes);
    if (status != ludus::content::Status::Ok)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "Could not read required input {} (content status {})", name,
                        static_cast<uint32>(status));
        return false;
    }
    return true;
}

/// Load the cooked scene.luscn staged beside the app into a decoded SceneDefinition.
bool LoadCookedScene(resources::SceneDefinition& outScene)
{
    ludus::content::Bytes bytes;
    if (ReadLocalFile("scene.luscn", MAX_SCENE_BYTES, bytes) == false)
    {
        return false;
    }
    const std::span<const uint8> data = bytes.Data();
    if (resources::SceneDefinition::Decode(data.data(), data.size(), outScene) != resources::Status::Ok)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "Could not decode the cooked scene.luscn");
        return false;
    }
    return true;
}

/// Report whether a material index is the requested feature's typed material (zero metallic).
bool IsFeatureMaterial(const resources::SceneMaterial& material, Feature feature) noexcept
{
    return material.Scattering.Model == FeatureModel(feature);
}

/// Count the objects whose assigned material is the requested feature's typed material.
usize CountFeatureObjects(const resources::SceneDefinition& scene, Feature feature) noexcept
{
    usize count = 0;
    for (usize i = 0; i < scene.Objects.GetSize(); ++i)
    {
        if (IsFeatureMaterial(scene.Materials[scene.Objects[i].Material], feature) == true)
        {
            ++count;
        }
    }
    return count;
}

/// Translate a cooked SceneMaterial into the render graph's typed material. The hair opacity texture
/// and skin scattering survive verbatim: the alpha mask and cutoff, and the HairCard/Skin Scattering.
void MakeShaderMaterial(const resources::SceneDefinition& scene,
                        const resources::SceneMaterial& source,
                        renderer::SceneShaderMaterial& outMaterial) noexcept
{
    outMaterial.BaseColor = source.Reflectance;
    outMaterial.Metallic = source.Metallic;
    outMaterial.Roughness = source.Roughness;
    outMaterial.Emission = source.Emission;
    outMaterial.IsEmissionTwoSided = source.IsEmissionTwoSided;
    outMaterial.Scattering = source.Scattering;
    outMaterial.Program = source.Program;
    outMaterial.IsAlphaMasked = source.IsAlphaMasked;
    outMaterial.AlphaCutoff = source.AlphaCutoff;
    if (source.Texture != ~uint32{0})
    {
        const resources::SceneTexture& texture = scene.Textures[source.Texture];
        outMaterial.ColorTexture = {
            .Bytes = texture.Pixels.GetData(),
            .Size = texture.Pixels.GetSize(),
            .RowPitch = usize{texture.Width} * RGBA8_PIXEL_BYTES,
            .Width = texture.Width,
            .Height = texture.Height,
            .Format = rhi::RasterFormat::Rgba8Srgb,
        };
    }
}

/// Convert a cooked analytic light into the renderer's DirectLight.
renderer::DirectLight MakeDirectLight(const resources::SceneLight& source, uint64 sourceId) noexcept
{
    renderer::DirectLight light;
    light.SourceId = sourceId;
    light.Kind = source.Kind == resources::SceneLightKind::Directional ? renderer::LightKind::Directional
                 : source.Kind == resources::SceneLightKind::Spot       ? renderer::LightKind::Spot
                                                                        : renderer::LightKind::Point;
    // The cooked IntensityRgb carries hue and magnitude together; split it into the renderer's hue
    // and scalar intensity, preserving the brighter channel as the intensity.
    const float32 peak =
        math::Max(source.IntensityRgb.X, math::Max(source.IntensityRgb.Y, source.IntensityRgb.Z));
    light.Intensity = peak;
    light.Color = peak > 0 ? math::Vector3{source.IntensityRgb.X / peak,
                                           source.IntensityRgb.Y / peak,
                                           source.IntensityRgb.Z / peak}
                           : math::Vector3{1, 1, 1};
    light.Position = source.PositionWorld;
    light.Direction = source.DirectionWorld;
    light.Range = source.Range;
    light.InnerCos = source.InnerCos;
    light.OuterCos = source.OuterCos;
    return light;
}

/// Build the matched perspective camera from the cooked scene's authored view.
bool MakeCamera(const resources::SceneDefinition& scene,
                uint32 width,
                uint32 height,
                renderer::ViewDescription& outCamera) noexcept
{
    const float32 aspect = static_cast<float32>(width) / static_cast<float32>(height);
    math::Matrix4 viewToClip;
    math::Matrix4 worldToView;
    if (math::TryPerspectiveReverseZ(scene.VerticalFov, aspect, CAPTURE_NEAR_METRES, CAPTURE_FAR_METRES, viewToClip) !=
            math::MathStatus::Success ||
        math::TryLookAt(scene.Eye, scene.Target, scene.Up, worldToView) != math::MathStatus::Success)
    {
        return false;
    }
    outCamera.WorldToClip = viewToClip * worldToView;
    outCamera.Depth = renderer::DepthConvention::ReverseZ;
    outCamera.CameraPosition = scene.Eye;
    return true;
}

bool IsPending(Status status) noexcept
{
    return status == Status::Pending || status == Status::NotReady;
}

template <typename Operation>
bool WaitFor(rhi::DeviceHandle device, const char* name, Operation operation)
{
    const uint64 started = time::NowTicks();
    Status status = Status::Pending;
    while (time::NowTicks() - started < OPERATION_TIMEOUT_NANOSECONDS)
    {
        const Status lifetime = rhi::PollLifetime(device);
        if (lifetime != Status::Ready && IsPending(lifetime) == false)
        {
            LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "{} device polling failed ({})", name, static_cast<uint32>(lifetime));
            return false;
        }
        status = operation();
        if (status == Status::Ready)
        {
            return true;
        }
        if (IsPending(status) == false)
        {
            break;
        }
    }
    LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "{} failed or timed out (raster status {})", name, static_cast<uint32>(status));
    return false;
}

/// The typed resources the cooked scene supplies to the render graph, kept alive during the render.
struct SceneBinding final
{
    Array<renderer::RenderGraphMesh> Meshes;
    Array<renderer::RenderGraphItem> Items;
    Array<renderer::SceneShaderMaterial> Materials;
    Array<uint32> MaterialBindings; ///< One global material index per object, borrowed by its mesh.
    Array<renderer::DirectLight> Lights;
    renderer::SceneShaderInputs Inputs;
};

/// Translate the whole cooked scene into render-graph inputs: every mesh, its per-object material slot,
/// the lights and the constant environment. Geometry and materials are borrowed from the scene, which
/// must outlive the render.
bool BuildSceneBinding(const resources::SceneDefinition& scene, SceneBinding& outBinding)
{
    if (outBinding.Materials.TryResize(scene.Materials.GetSize()) == false ||
        outBinding.Meshes.TryResize(scene.Objects.GetSize()) == false ||
        outBinding.Items.TryResize(scene.Objects.GetSize()) == false ||
        outBinding.MaterialBindings.TryResize(scene.Objects.GetSize()) == false ||
        outBinding.Lights.TryResize(scene.Lights.GetSize()) == false)
    {
        return false;
    }
    for (usize i = 0; i < scene.Materials.GetSize(); ++i)
    {
        MakeShaderMaterial(scene, scene.Materials[i], outBinding.Materials[i]);
    }
    // One draw item per object. Each borrows its source mesh's geometry and binds the object's single
    // material slot to the global material table.
    for (usize i = 0; i < scene.Objects.GetSize(); ++i)
    {
        const resources::SceneObject& object = scene.Objects[i];
        const resources::SceneMesh& sourceMesh = scene.Meshes[object.Mesh];
        outBinding.MaterialBindings[i] = object.Material;
        renderer::RenderGraphMesh& mesh = outBinding.Meshes[i];
        mesh.Geometry.Positions = sourceMesh.Positions.GetData();
        mesh.Geometry.VertexCount = sourceMesh.Positions.GetSize();
        mesh.Geometry.Indices = sourceMesh.Indices.GetData();
        mesh.Geometry.IndexCount = sourceMesh.Indices.GetSize();
        mesh.Geometry.Normals = sourceMesh.Normals.IsEmpty() == true ? nullptr : sourceMesh.Normals.GetData();
        mesh.Geometry.Texcoords = sourceMesh.Texcoords.IsEmpty() == true ? nullptr : sourceMesh.Texcoords.GetData();
        mesh.MaterialBindings = {&outBinding.MaterialBindings[i], 1};
        renderer::RenderGraphItem& item = outBinding.Items[i];
        item.SourceId = i + 1;
        item.MeshIndex = static_cast<uint32>(i);
        item.MeshToWorld = object.Transform;
        item.Coverage = scene.Materials[object.Material].IsAlphaMasked == true
                            ? renderer::RenderGraphMaterial::Masked
                            : renderer::RenderGraphMaterial::Opaque;
    }
    for (usize i = 0; i < scene.Lights.GetSize(); ++i)
    {
        outBinding.Lights[i] = MakeDirectLight(scene.Lights[i], i + 1);
    }
    outBinding.Inputs.Materials = {outBinding.Materials.GetData(), outBinding.Materials.GetSize()};
    outBinding.Inputs.Lights = {outBinding.Lights.GetData(), outBinding.Lights.GetSize()};
    outBinding.Inputs.Environment = scene.Environment.Rgb;
    return true;
}

/// Append a base-ten unsigned integer to the provenance builder.
bool AppendUnsigned(foundation::String& inoutText, uint64 value)
{
    char digits[20];
    usize count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + static_cast<char>(value % 10));
        value /= 10;
    } while (value > 0);
    char ordered[20];
    for (usize i = 0; i < count; ++i)
    {
        ordered[i] = digits[count - 1 - i];
    }
    return inoutText.TryAppend({ordered, count}) == foundation::StringStatus::Ok;
}

/// Append a "key=value\n" line with an unsigned value.
bool AppendCount(foundation::String& inoutText, std::string_view key, uint64 value)
{
    return inoutText.TryAppend(key) == foundation::StringStatus::Ok && AppendUnsigned(inoutText, value) == true &&
           inoutText.TryAppend("\n") == foundation::StringStatus::Ok;
}

/// Write the capture provenance the acceptance evidence requires: profile, feature, SDK identity,
/// target and scene counts, as plain key=value lines (no JSON parser or <format> is linked).
void WriteProvenance(Profile profile,
                     Feature feature,
                     const resources::SceneDefinition& scene,
                     usize featureObjectCount,
                     const rhi::DeviceInfo& information)
{
    foundation::String text;
    const std::string_view fixed[] = {
        "issue=1400\nprofile=",           ProfileName(profile),
        "\nfeature=",                     FeatureName(feature),
        "\npackage=",                     ProfilePackageFile(profile),
        "\nsdk_version=",                 LUDUS_SDK_VERSION,
        "\nsdk_source_revision=",         LUDUS_SDK_SOURCE_REVISION,
        "\ntarget=",                      LUDUS_SDK_TARGET,
        "\nadapter=",                     information.Adapter.Name.GetData(),
        "\nencoding=rgba8-unorm-top-left\n",
    };
    if (text.TryAppendMany({fixed, sizeof(fixed) / sizeof(fixed[0])}) != foundation::StringStatus::Ok)
    {
        return;
    }
    const bool isComplete = AppendCount(text, "backend=", static_cast<uint32>(information.Startup.SelectedBackend)) &&
                            AppendCount(text, "width=", CAPTURE_WIDTH) &&
                            AppendCount(text, "height=", CAPTURE_HEIGHT) &&
                            AppendCount(text, "meshes=", scene.Meshes.GetSize()) &&
                            AppendCount(text, "materials=", scene.Materials.GetSize()) &&
                            AppendCount(text, "objects=", scene.Objects.GetSize()) &&
                            AppendCount(text, "lights=", scene.Lights.GetSize()) &&
                            AppendCount(text, "textures=", scene.Textures.GetSize()) &&
                            AppendCount(text, "feature_objects=", featureObjectCount);
    if (isComplete == false)
    {
        return;
    }
    const std::string_view view = text.GetView();
    static_cast<void>(ludus::content::SaveFile(
        ".", "capture.txt", {reinterpret_cast<const uint8*>(view.data()), view.size()}, nullptr));
}

/// Report whether a selected profile can shade the requested feature on the cooked scene, with an
/// actionable diagnostic. The negative-check entry point: it runs before any render, so an
/// incompatible selection or a missing feature input never ships.
int CheckProfile(Feature feature, Profile profile)
{
    resources::SceneDefinition scene;
    if (LoadCookedScene(scene) == false)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN,
                        "Missing required dependency: the cooked scene.luscn is absent or undecodable. Stage the "
                        "published bust scene beside the app before packaging.");
        return EXIT_MISSING_DEPENDENCY;
    }
    if (OwnedFeature(profile) != feature)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN,
                        "Incompatible renderer profile: the '{}' profile cannot shade {} on the character-bust "
                        "World. It shades {} with ordinary lobes, losing the {}. Select the '{}' profile for {}.",
                        ProfileName(profile), FeatureName(feature), FeatureName(feature),
                        feature == Feature::Hair ? "authored strand highlight" : "subsurface diffusion",
                        CompatibleProfileName(feature), FeatureName(feature));
        return EXIT_INCOMPATIBLE_PROFILE;
    }
    const usize featureObjects = CountFeatureObjects(scene, feature);
    if (featureObjects == 0)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN,
                        "Missing required dependency: the cooked scene carries no {} material. Re-cook the bust "
                        "level with its {} slot assigned before packaging.",
                        FeatureName(feature), FeatureName(feature));
        return EXIT_MISSING_DEPENDENCY;
    }
    LUDUS_LOG_INFO(LOG_HAIR_SKIN, "Profile '{}' shades {} on the bust: {} of {} objects carry the {} material.",
                   ProfileName(profile), FeatureName(feature), featureObjects, scene.Objects.GetSize(),
                   FeatureName(feature));
    return EXIT_OK;
}

int Render(Profile profile, rhi::DeviceHandle device, rhi::SurfaceHandle surface, const rhi::DeviceInfo& information)
{
    const Feature feature = OwnedFeature(profile);
    resources::SceneDefinition scene;
    if (LoadCookedScene(scene) == false)
    {
        return EXIT_MISSING_DEPENDENCY;
    }
    const usize featureObjects = CountFeatureObjects(scene, feature);
    if (featureObjects == 0)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "The cooked scene carries no {} material; re-cook the bust level.",
                        FeatureName(feature));
        return EXIT_MISSING_DEPENDENCY;
    }

    ludus::content::Bytes packageBytes;
    if (ReadLocalFile(ProfilePackageFile(profile), MAX_SCENE_BYTES, packageBytes) == false)
    {
        return EXIT_MISSING_DEPENDENCY;
    }
    renderer::RenderGraphPackage package;
    const std::span<const uint8> packageData = packageBytes.Data();
    if (package.Decode({packageData.data(), packageData.size()}) != renderer::RenderGraphPackageStatus::Ready)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "Could not decode the packaged graph {}", ProfilePackageFile(profile));
        return EXIT_MISSING_DEPENDENCY;
    }

    renderer::RenderGraph graph;
    const renderer::RenderGraphPlan plan = package.GetPlan();
    if (plan.Passes.GetSize() == 0)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "The packaged graph {} has no passes", ProfilePackageFile(profile));
        return EXIT_FAILURE_CODE;
    }
    const Status initialized = graph.Initialize(device, plan,
                                                 package.GetPrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                     ludus::shaders::renderer_composite::Fragment()));
    if ((initialized != Status::Ready && initialized != Status::Pending) ||
        WaitFor(device, "graph preparation", [&graph]() { return graph.Poll(); }) == false)
    {
        return EXIT_FAILURE_CODE;
    }

    SceneBinding binding;
    if (BuildSceneBinding(scene, binding) == false)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "Out of memory building the scene binding");
        return EXIT_FAILURE_CODE;
    }
    renderer::ViewDescription camera;
    if (MakeCamera(scene, CAPTURE_WIDTH, CAPTURE_HEIGHT, camera) == false)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "The cooked scene camera is not a valid perspective view");
        return EXIT_FAILURE_CODE;
    }

    renderer::RenderGraphReport report;
    const renderer::RenderGraphSceneRevisions revisions{1, 1, 1, 1, 1};
    const Status updated = graph.Update({binding.Meshes.GetData(), binding.Meshes.GetSize()},
                                        {binding.Items.GetData(), binding.Items.GetSize()},
                                        {&camera, 1},
                                        1,
                                        renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                        report,
                                        &binding.Inputs,
                                        0,
                                        revisions);
    if ((updated != Status::Ready && updated != Status::Pending) ||
        WaitFor(device, "scene upload", [&graph]() { return graph.Poll(); }) == false ||
        rhi::SetFrameTarget(device, surface, { .Width = CAPTURE_WIDTH, .Height = CAPTURE_HEIGHT }) !=
            rhi::DeviceStatus::Ready)
    {
        return EXIT_FAILURE_CODE;
    }

    rhi::SubmissionToken completion;
    const Status rendered = graph.Render(surface, CAPTURE_WIDTH, CAPTURE_HEIGHT, completion);
    if (rendered != Status::Ready && rendered != Status::Pending)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "Raster submission failed ({})", static_cast<uint32>(rendered));
        return EXIT_FAILURE_CODE;
    }

    // Resolve the graph's final colour export (its last pass's colour resource) and read it back.
    const uint32 output = plan.Passes[plan.Passes.GetSize() - 1].Color;
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect{};
    if (WaitFor(device, "export", [&]() { return graph.GetExport(output, texture, aspect); }) == false ||
        aspect != rhi::TextureAspect::Color)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "The graph export is not a colour texture");
        return EXIT_FAILURE_CODE;
    }

    Array<uint8> pixels;
    const usize rowPitch = usize{CAPTURE_WIDTH} * RGBA8_PIXEL_BYTES;
    if (pixels.TryResize(rowPitch * CAPTURE_HEIGHT) == false)
    {
        return EXIT_FAILURE_CODE;
    }
    rhi::ReadbackTicket ticket;
    const rhi::TextureRegion region{ .Mip = 0, .X = 0, .Y = 0, .Width = CAPTURE_WIDTH, .Height = CAPTURE_HEIGHT };
    const Status requested = rhi::RequestTextureReadback(device, texture, region, rowPitch, ticket);
    if (requested != Status::Ready && requested != Status::Pending)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "Readback request rejected ({})", static_cast<uint32>(requested));
        return EXIT_FAILURE_CODE;
    }
    const bool hasPixels =
        WaitFor(device, "readback", [&]() { return rhi::GetStatus(device, ticket); }) == true &&
        rhi::CopyReadback(device, ticket, pixels.GetData(), pixels.GetSize()) == Status::Ready;
    const Status released = rhi::Release(device, ticket);
    if (hasPixels == false || released != Status::Ready)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "Could not read back the rendered frame");
        return EXIT_FAILURE_CODE;
    }
    if (ludus::content::SaveFile(".", "capture.rgba", {pixels.GetData(), pixels.GetSize()}, nullptr) !=
        ludus::content::Status::Ok)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "Could not write capture.rgba");
        return EXIT_FAILURE_CODE;
    }
    WriteProvenance(profile, feature, scene, featureObjects, information);
    LUDUS_LOG_INFO(LOG_HAIR_SKIN, "Rendered {} through the {} profile at {}x{} ({} draws)", FeatureName(feature),
                   ProfileName(profile), CAPTURE_WIDTH, CAPTURE_HEIGHT, binding.Items.GetSize());
    return EXIT_OK;
}

int RunWithDevice(Profile profile)
{
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    rhi::StartupInfo failure;
    if (rhi::CreateDevice({}, { .Width = PROBE_WIDTH, .Height = PROBE_HEIGHT }, description, device, surface,
                          &failure) != rhi::DeviceStatus::Ready)
    {
        LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "A GPU device is required to render the capture; startup error {}",
                        static_cast<uint32>(failure.Error));
        rhi::Shutdown();
        return EXIT_MISSING_DEPENDENCY;
    }
    rhi::DeviceInfo information;
    if (rhi::GetDeviceInfo(device, information) != rhi::DeviceStatus::Ready)
    {
        rhi::Shutdown();
        return EXIT_FAILURE_CODE;
    }
    const int result = Render(profile, device, surface, information);
    rhi::Shutdown();
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 4 && std::string_view(argv[1]) == "--check-profile")
    {
        Feature feature;
        Profile profile;
        if (TryParseFeature(argv[2], feature) == false || TryParseProfile(argv[3], profile) == false)
        {
            LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "Usage: --check-profile <hair|skin> <forward-lit|clustered-deferred>");
            return EXIT_FAILURE_CODE;
        }
        return CheckProfile(feature, profile);
    }
    if (argc == 3 && std::string_view(argv[1]) == "--profile")
    {
        Profile profile;
        if (TryParseProfile(argv[2], profile) == false)
        {
            LUDUS_LOG_ERROR(LOG_HAIR_SKIN, "Usage: --profile <forward-lit|clustered-deferred>");
            return EXIT_FAILURE_CODE;
        }
        return RunWithDevice(profile);
    }
    LUDUS_LOG_ERROR(LOG_HAIR_SKIN,
                    "Usage: hair_skin_capture --profile <forward-lit|clustered-deferred> | --check-profile "
                    "<hair|skin> <forward-lit|clustered-deferred>");
    return EXIT_FAILURE_CODE;
}
