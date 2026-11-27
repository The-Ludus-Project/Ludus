// A standalone installed-SDK application that renders ONE Editor-saved World with a project-owned BRDF provider
// (#1310): the `project/oren-nayar` closure program assigned to one board through the World's `project_brdf`
// binding, beside a board of the built-in Diffuse control, under the World's two directional lights. It renders
// both boards through the installed compute ray-tracing template, the same unchanged general graph the Editor
// captures, and writes the graph's linear mean as a `.lurgcap` file for verify.py to compare with the analytic
// direct response and the Editor's paired raw CPU and GPU reference captures.
//
// It links no Qt: it loads the cooked scene.luscn through Ludus::Resources, renders through Ludus::GraphicsRenderer
// and reads and writes files through Ludus::Content.
//
// Usage:
//   project_brdf_capture --lights <mask> <output.lurgcap>
//       Render with the cooked lights whose table indices are set in the decimal bit mask switched on.
//   project_brdf_capture --check-materials
//       Report, without rendering, whether the cooked scene carries the project material and the built-in control.
//       A missing project material prints the repair and returns a distinct exit code.
#include "renderer_composite.h"

#include <ludus/content/content.h>
#include <ludus/content/scene_format/material.h>
#include <ludus/content/scene_format/material_program.h>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/strings/status.hpp>
#include <ludus/foundation/strings/string.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/light_selection.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/render_graph_capture.hpp>
#include <ludus/graphics/renderer/scene_shader.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/runtime/resources/scene.h>

#include <cstring>
#include <string_view>

namespace
{
using namespace ludus::foundation;
namespace foundation = ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace format = ludus::content::scene_format;
namespace resources = ludus::resources;
namespace graph_format = ludus::content::render_graph_format;
using Status = rhi::RasterStatus;
LUDUS_DEFINE_LOG_CATEGORY(LOG_PROJECT_BRDF, "ProjectBrdfWorld");

/// Distinct process exit codes, matched by verify.py.
constexpr int EXIT_OK = 0;
constexpr int EXIT_FAILURE_CODE = 1;
constexpr int EXIT_MISSING_DEPENDENCY = 4;
constexpr int EXIT_MISSING_PROJECT_MATERIAL = 5;
/// The project material the World binds; its provider is tests/scenes/materials/oren-nayar-provider.luscene.json.
constexpr std::string_view PROJECT_MATERIAL_NAME = "project/oren-nayar";
/// Longest wait for preparing or completing the capture, in nanoseconds.
constexpr uint64 OPERATION_TIMEOUT_NANOSECONDS = 120 * time::NANOSECONDS_PER_SECOND;
/// Capture extent and samples per pixel, matched with the Editor export.
constexpr uint32 CAPTURE_WIDTH = 64;
constexpr uint32 CAPTURE_HEIGHT = 32;
constexpr uint32 CAPTURE_SAMPLES = 128;
/// The Editor's World graph capture submits this fixed scene revision, which also seeds the graph's sampling.
constexpr uint64 CAPTURE_REVISION = 1;
/// Largest cooked scene bundle the app admits, matching the assembly-profile bound.
constexpr usize MAX_SCENE_BYTES = 576 * 1024 * 1024;
/// Largest packaged graph the app reads.
constexpr usize MAX_PACKAGE_BYTES = 64 * 1024 * 1024;
/// The view file the Editor export writes: "LUVIEW01", uint32 depth convention, uint32 orthographic flag, sixteen
/// float32 of WorldToClip column by column, and the float32 camera position, all little-endian.
constexpr std::string_view VIEW_MAGIC = "LUVIEW01";
constexpr usize VIEW_MATRIX_FLOATS = 16;
constexpr usize VIEW_POSITION_FLOATS = 3;
constexpr usize VIEW_BYTES = VIEW_MAGIC.size() + 2 * sizeof(uint32) + (VIEW_MATRIX_FLOATS + VIEW_POSITION_FLOATS) * 4;
/// Bytes of one RGBA8 texel of a cooked texture.
constexpr usize RGBA8_PIXEL_BYTES = 4;
/// Decimal radix of the light mask argument.
constexpr uint32 DECIMAL_BASE = 10;
/// Largest light mask the app parses: one bit per cooked light it can switch.
constexpr uint32 MAX_LIGHT_MASK_BITS = 31;

/// Read a file staged beside the app into owned bytes; an absent or oversized file is reported by name.
bool ReadLocalFile(std::string_view name, usize capacity, ludus::content::Bytes& outBytes)
{
    const ludus::content::Status status = ludus::content::ReadFile(".", name, capacity, outBytes);
    if (status != ludus::content::Status::Ok)
    {
        LUDUS_LOG_ERROR(LOG_PROJECT_BRDF,
                        "Missing required dependency: could not read {} beside the app (content status {}). Stage "
                        "the Editor export beside the app before launching it.",
                        name,
                        static_cast<uint32>(status));
        return false;
    }
    return true;
}

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
        LUDUS_LOG_ERROR(LOG_PROJECT_BRDF, "Could not decode the cooked scene.luscn; export the World again.");
        return false;
    }
    return true;
}

bool ReadFloat(std::span<const uint8> bytes, usize& inoutOffset, float32& outValue) noexcept
{
    uint32 word = 0;
    if (TryReadLittleEndian(BorrowedView<const uint8>{bytes.data() + inoutOffset, bytes.size() - inoutOffset}, word) ==
        false)
    {
        return false;
    }
    std::memcpy(&outValue, &word, sizeof(outValue));
    inoutOffset += sizeof(word);
    return true;
}

/// Load the Editor's capture view, so both hosts trace the same camera rays.
bool LoadView(renderer::ViewDescription& outView)
{
    ludus::content::Bytes bytes;
    if (ReadLocalFile("view.bin", VIEW_BYTES, bytes) == false)
    {
        return false;
    }
    const std::span<const uint8> data = bytes.Data();
    if (data.size() != VIEW_BYTES ||
        std::string_view(reinterpret_cast<const char*>(data.data()), VIEW_MAGIC.size()) != VIEW_MAGIC)
    {
        LUDUS_LOG_ERROR(LOG_PROJECT_BRDF, "view.bin is not a LUVIEW01 capture view; export the World again.");
        return false;
    }
    usize offset = VIEW_MAGIC.size();
    uint32 depth = 0;
    uint32 orthographic = 0;
    if (TryReadLittleEndian(BorrowedView<const uint8>{data.data() + offset, sizeof(uint32)}, depth) == false ||
        TryReadLittleEndian(BorrowedView<const uint8>{data.data() + offset + sizeof(uint32), sizeof(uint32)},
                            orthographic) == false)
    {
        return false;
    }
    offset += 2 * sizeof(uint32);
    outView.Depth = static_cast<renderer::DepthConvention>(depth);
    outView.OrthographicCamera = orthographic != 0;
    for (math::Vector4& column : outView.WorldToClip.Columns)
    {
        if (ReadFloat(data, offset, column.X) == false || ReadFloat(data, offset, column.Y) == false ||
            ReadFloat(data, offset, column.Z) == false || ReadFloat(data, offset, column.W) == false)
        {
            return false;
        }
    }
    return ReadFloat(data, offset, outView.CameraPosition.X) == true &&
           ReadFloat(data, offset, outView.CameraPosition.Y) == true &&
           ReadFloat(data, offset, outView.CameraPosition.Z) == true;
}

/// Report whether a cooked material is the project's Oren-Nayar program: a closure program whose lobes are all
/// Oren-Nayar.
bool IsProjectMaterial(const resources::SceneMaterial& material) noexcept
{
    if (material.Name.View() != PROJECT_MATERIAL_NAME || material.Scattering.Model != format::MaterialModel::Program ||
        material.Program.LobeCount == 0)
    {
        return false;
    }
    for (uint32 lobe = 0; lobe < material.Program.LobeCount; ++lobe)
    {
        if (material.Program.Lobes[lobe].Operation != format::MaterialLobeOperation::OrenNayar)
        {
            return false;
        }
    }
    return true;
}

/// Count the objects that draw the project material and those that draw the built-in Diffuse control.
void CountBoards(const resources::SceneDefinition& scene, usize& outProject, usize& outControl) noexcept
{
    outProject = 0;
    outControl = 0;
    for (const resources::SceneObject& object : scene.Objects)
    {
        const resources::SceneMaterial& material = scene.Materials[object.Material];
        outProject += IsProjectMaterial(material) == true ? 1U : 0U;
        outControl += material.Scattering.Model == format::MaterialModel::Diffuse ? 1U : 0U;
    }
}

/// The cooked material's renderer description, with its closure program verbatim.
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
        outMaterial.ColorTexture =
        {
            .Bytes = texture.Pixels.GetData(),
            .Size = texture.Pixels.GetSize(),
            .RowPitch = usize{texture.Width} * RGBA8_PIXEL_BYTES,
            .Width = texture.Width,
            .Height = texture.Height,
            .Format = rhi::RasterFormat::Rgba8Srgb,
        };
    }
}

/// Convert a cooked directional light into the renderer's light: the cooked RGB intensity is hue times magnitude.
renderer::DirectLight MakeDirectLight(const resources::SceneLight& source) noexcept
{
    renderer::DirectLight light;
    light.SourceId = source.SourceId;
    light.Kind = source.Kind == resources::SceneLightKind::Directional ? renderer::LightKind::Directional
                 : source.Kind == resources::SceneLightKind::Spot      ? renderer::LightKind::Spot
                                                                       : renderer::LightKind::Point;
    const float32 peak = math::Max(source.IntensityRgb.X, math::Max(source.IntensityRgb.Y, source.IntensityRgb.Z));
    light.Intensity = peak;
    light.Color =
        peak > 0
            ? math::Vector3{source.IntensityRgb.X / peak, source.IntensityRgb.Y / peak, source.IntensityRgb.Z / peak}
            : math::Vector3{1, 1, 1};
    light.Position = source.PositionWorld;
    light.Direction = source.DirectionWorld;
    light.Range = source.Range;
    light.InnerCos = source.InnerCos;
    light.OuterCos = source.OuterCos;
    return light;
}

/// The cooked scene's graph inputs, kept alive while the capture borrows them.
struct SceneBinding final
{
    Array<renderer::RenderGraphMesh> Meshes;
    Array<renderer::RenderGraphItem> Items;
    Array<renderer::SceneShaderMaterial> Materials;
    Array<uint32> MaterialBindings;
    Array<renderer::DirectLight> Lights;
    renderer::SceneShaderInputs Inputs;
};

/// Translate the cooked scene into graph inputs, with only the lights whose table index is set in `lightMask`.
bool BuildSceneBinding(const resources::SceneDefinition& scene, uint32 lightMask, SceneBinding& outBinding)
{
    if (outBinding.Materials.TryResize(scene.Materials.GetSize()) == false ||
        outBinding.Meshes.TryResize(scene.Objects.GetSize()) == false ||
        outBinding.Items.TryResize(scene.Objects.GetSize()) == false ||
        outBinding.MaterialBindings.TryResize(scene.Objects.GetSize()) == false)
    {
        return false;
    }
    for (usize i = 0; i < scene.Materials.GetSize(); ++i)
    {
        MakeShaderMaterial(scene, scene.Materials[i], outBinding.Materials[i]);
    }
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
        item.Coverage = scene.Materials[object.Material].IsAlphaMasked == true ? renderer::RenderGraphMaterial::Masked
                                                                               : renderer::RenderGraphMaterial::Opaque;
    }
    for (usize i = 0; i < scene.Lights.GetSize() && i < MAX_LIGHT_MASK_BITS; ++i)
    {
        if ((lightMask & (1U << i)) != 0 && outBinding.Lights.TryAdd(MakeDirectLight(scene.Lights[i])) == false)
        {
            return false;
        }
    }
    outBinding.Inputs.Materials = {outBinding.Materials.GetData(), outBinding.Materials.GetSize()};
    outBinding.Inputs.Lights = {outBinding.Lights.GetData(), outBinding.Lights.GetSize()};
    outBinding.Inputs.Environment = scene.Environment.Rgb;
    return true;
}

/// Parse a decimal light mask with at most MAX_LIGHT_MASK_BITS bits. Failure preserves outMask.
bool TryParseLightMask(std::string_view text, uint32& outMask) noexcept
{
    if (text.empty() == true)
    {
        return false;
    }
    uint32 mask = 0;
    for (const char digit : text)
    {
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        mask = mask * DECIMAL_BASE + static_cast<uint32>(digit - '0');
        if (mask >= (1U << MAX_LIGHT_MASK_BITS))
        {
            return false;
        }
    }
    outMask = mask;
    return true;
}

/// Check the staged scene for the project material and the control; name a missing provider and its repair.
int CheckMaterials()
{
    resources::SceneDefinition scene;
    if (LoadCookedScene(scene) == false)
    {
        return EXIT_MISSING_DEPENDENCY;
    }
    usize project = 0;
    usize control = 0;
    CountBoards(scene, project, control);
    if (project == 0)
    {
        LUDUS_LOG_ERROR(LOG_PROJECT_BRDF,
                        "The cooked scene draws no '{}' closure program: its project provider was not cooked into the "
                        "World. In the Editor, import oren-nayar-provider.luscene.json, assign '{}' with Assign "
                        "Project BRDF, save the World and export it again.",
                        PROJECT_MATERIAL_NAME,
                        PROJECT_MATERIAL_NAME);
        return EXIT_MISSING_PROJECT_MATERIAL;
    }
    LUDUS_LOG_INFO(LOG_PROJECT_BRDF,
                   "The cooked scene draws '{}' on {} object(s) and the built-in Diffuse control on {}; {} light(s).",
                   PROJECT_MATERIAL_NAME,
                   project,
                   control,
                   scene.Lights.GetSize());
    return control == 0 ? EXIT_MISSING_PROJECT_MATERIAL : EXIT_OK;
}

/// Service the capture until it leaves `phase` or the timeout elapses.
void ServiceWhile(renderer::RenderGraphCapture& inoutCapture, renderer::RenderGraphCapturePhase phase)
{
    const uint64 started = time::NowTicks();
    while (inoutCapture.GetPhase() == phase && time::NowTicks() - started < OPERATION_TIMEOUT_NANOSECONDS)
    {
        static_cast<void>(inoutCapture.Service());
    }
}

int Render(uint32 lightMask, std::string_view output, rhi::DeviceHandle device, rhi::SurfaceHandle surface)
{
    resources::SceneDefinition scene;
    renderer::ViewDescription view;
    if (LoadCookedScene(scene) == false || LoadView(view) == false)
    {
        return EXIT_MISSING_DEPENDENCY;
    }
    usize project = 0;
    usize control = 0;
    CountBoards(scene, project, control);
    if (project == 0)
    {
        return CheckMaterials();
    }
    ludus::content::Bytes packageBytes;
    if (ReadLocalFile("compute_ray_tracing.lurgrt", MAX_PACKAGE_BYTES, packageBytes) == false)
    {
        return EXIT_MISSING_DEPENDENCY;
    }
    renderer::RenderGraphPackage package;
    const std::span<const uint8> packageData = packageBytes.Data();
    if (package.Decode({packageData.data(), packageData.size()}) != renderer::RenderGraphPackageStatus::Ready)
    {
        LUDUS_LOG_ERROR(LOG_PROJECT_BRDF, "Could not decode the packaged compute_ray_tracing.lurgrt; rebuild the app.");
        return EXIT_MISSING_DEPENDENCY;
    }
    const renderer::RenderGraphPlan plan = package.GetPlan();
    const renderer::RenderGraphPrograms programs = package.GetPrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                       ludus::shaders::renderer_composite::Fragment());
    // Capture the first exported colour, as the Editor's World graph capture does.
    renderer::RenderGraphCaptureSettings settings;
    settings.TargetSamples = CAPTURE_SAMPLES;
    for (uint32 resource = 0; resource < plan.Resources.GetSize(); ++resource)
    {
        if (plan.Resources[resource].IsExported == 1 && plan.Resources[resource].Kind == graph_format::Aspect::Color)
        {
            settings.Output = resource;
            break;
        }
    }
    SceneBinding binding;
    if (BuildSceneBinding(scene, lightMask, binding) == false)
    {
        LUDUS_LOG_ERROR(LOG_PROJECT_BRDF, "Out of memory building the scene binding");
        return EXIT_FAILURE_CODE;
    }
    renderer::RenderGraphCapture capture;
    const Status started = capture.Start(device, surface, plan, programs, CAPTURE_WIDTH, CAPTURE_HEIGHT, settings);
    if (started != Status::Ready && started != Status::Pending)
    {
        LUDUS_LOG_ERROR(LOG_PROJECT_BRDF,
                        "The graph capture could not start (status {}, problem {})",
                        static_cast<uint32>(started),
                        static_cast<uint32>(capture.GetProblem()));
        return EXIT_FAILURE_CODE;
    }
    ServiceWhile(capture, renderer::RenderGraphCapturePhase::Initializing);
    if (capture.GetPhase() != renderer::RenderGraphCapturePhase::AwaitingScene)
    {
        LUDUS_LOG_ERROR(LOG_PROJECT_BRDF,
                        "The graph did not become ready (problem {})",
                        static_cast<uint32>(capture.GetProblem()));
        return EXIT_FAILURE_CODE;
    }
    // As in the Editor, the one view feeds every camera of the plan.
    Array<renderer::ViewDescription> cameras;
    if (cameras.TryResize(plan.Description.CameraCount == 0 ? 1 : plan.Description.CameraCount) == false)
    {
        return EXIT_FAILURE_CODE;
    }
    for (renderer::ViewDescription& camera : cameras)
    {
        camera = view;
    }
    const Status submitted = capture.SubmitScene({binding.Meshes.GetData(), binding.Meshes.GetSize()},
                                                 {binding.Items.GetData(), binding.Items.GetSize()},
                                                 {cameras.GetData(), cameras.GetSize()},
                                                 CAPTURE_REVISION,
                                                 &binding.Inputs);
    if (submitted != Status::Ready && submitted != Status::Pending)
    {
        LUDUS_LOG_ERROR(LOG_PROJECT_BRDF,
                        "The graph rejected the scene (status {}, problem {})",
                        static_cast<uint32>(submitted),
                        static_cast<uint32>(capture.GetProblem()));
        return EXIT_FAILURE_CODE;
    }
    const uint64 renderStarted = time::NowTicks();
    while (capture.GetPhase() != renderer::RenderGraphCapturePhase::Complete &&
           capture.GetPhase() != renderer::RenderGraphCapturePhase::Failed &&
           time::NowTicks() - renderStarted < OPERATION_TIMEOUT_NANOSECONDS)
    {
        static_cast<void>(capture.Service());
    }
    if (capture.GetPhase() != renderer::RenderGraphCapturePhase::Complete)
    {
        LUDUS_LOG_ERROR(LOG_PROJECT_BRDF,
                        "The graph capture did not complete (phase {}, problem {})",
                        static_cast<uint32>(capture.GetPhase()),
                        static_cast<uint32>(capture.GetProblem()));
        return EXIT_FAILURE_CODE;
    }
    Array<uint8> encoded;
    if (capture.Encode(encoded) != Status::Ready ||
        ludus::content::SaveFile(".", output, {encoded.GetData(), encoded.GetSize()}, nullptr) !=
            ludus::content::Status::Ok)
    {
        LUDUS_LOG_ERROR(LOG_PROJECT_BRDF, "Could not write {}", output);
        return EXIT_FAILURE_CODE;
    }
    LUDUS_LOG_INFO(LOG_PROJECT_BRDF,
                   "Rendered '{}' on {} object(s) and the Diffuse control on {} through compute_ray_tracing at "
                   "{}x{}, {} samples, {} of {} lights; SDK {} ({}, {})",
                   PROJECT_MATERIAL_NAME,
                   project,
                   control,
                   CAPTURE_WIDTH,
                   CAPTURE_HEIGHT,
                   CAPTURE_SAMPLES,
                   binding.Lights.GetSize(),
                   scene.Lights.GetSize(),
                   LUDUS_SDK_VERSION,
                   LUDUS_SDK_SOURCE_REVISION,
                   LUDUS_SDK_TARGET);
    return EXIT_OK;
}

int RunWithDevice(uint32 lightMask, std::string_view output)
{
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Preferred.RayQueries = true;
    rhi::StartupInfo failure;
    if (rhi::CreateDevice({},
                          {
                              .Width = CAPTURE_WIDTH,
                              .Height = CAPTURE_HEIGHT,
                          },
                          description,
                          device,
                          surface,
                          &failure) != rhi::DeviceStatus::Ready)
    {
        LUDUS_LOG_ERROR(LOG_PROJECT_BRDF,
                        "A GPU device is required to render the capture; startup error {}",
                        static_cast<uint32>(failure.Error));
        rhi::Shutdown();
        return EXIT_MISSING_DEPENDENCY;
    }
    const int result = Render(lightMask, output, device, surface);
    static_cast<void>(rhi::DestroyDevice(device));
    rhi::Shutdown();
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--check-materials")
    {
        return CheckMaterials();
    }
    uint32 lightMask = 0;
    if (argc == 4 && std::string_view(argv[1]) == "--lights" && TryParseLightMask(argv[2], lightMask) == true)
    {
        return RunWithDevice(lightMask, argv[3]);
    }
    LUDUS_LOG_ERROR(LOG_PROJECT_BRDF,
                    "Usage: project_brdf_capture --lights <decimal mask> <output.lurgcap> | --check-materials");
    return EXIT_FAILURE_CODE;
}
