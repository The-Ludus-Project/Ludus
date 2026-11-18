// Opt-in benchmark of a supplied World with a clustered raster graph.
// Thanks to Joshua Barczak, "Comments on Future Directions for Compute in Graphics", The Burning Basis Vector
// (2017), "Visibility Buffers and Shading", https://www.joshbarczak.com/blog/?p=1317, for motivating a matched
// clustered-forward control before testing an opaque visibility resolve.
#include "internal/offscreen_frame_reader.h"
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/viewport_device_description.h"
#include "renderer_composite.h"
#include "viewport_fixture.h"

#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/base/types.h>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/measurement.h>

#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace shaders = ludus::shaders;
namespace
{
// About the 1.7 Mpx editor viewport of the reported Staircase2 capture.
constexpr uint32 BENCHMARK_WIDTH = 1600;
constexpr uint32 BENCHMARK_HEIGHT = 1080;
constexpr uint32 WARMUP_FRAMES = 3;
constexpr uint32 MEASURED_FRAMES = 20;
/// Most frames spent drawing mesh-light shadow faces before the measured frames; they are timed separately.
constexpr uint32 MAX_SHADOW_FILL_FRAMES = 4096;
constexpr int32 PREPARATION_TIMEOUT_MILLISECONDS = 120000;
/// Alternating material comparison blocks: each block measures the
/// control and then the World as authored, so GPU clock drift affects both alike.
constexpr uint32 MATERIAL_CONTROL_BLOCKS = 8;
constexpr float64 NANOSECONDS_PER_MILLISECOND = 1000000;

/// Count the layers offered to the depth test for the canonical, disconnected hair ribbons.
template <typename CaptureRibbon>
QJsonObject
MeasureHairOverdraw(const RenderGraphExtraction& extraction, qsizetype hairMaterial, CaptureRibbon captureRibbon)
{
    const renderer::RenderGraphItem* hairItemOrNull = nullptr;
    for (const renderer::RenderGraphItem& item : extraction.Items)
    {
        REQUIRE(item.MaterialOverrides.GetSize() == 1);
        if (item.MaterialOverrides[0] == static_cast<uint32>(hairMaterial))
        {
            REQUIRE(hairItemOrNull == nullptr);
            hairItemOrNull = &item;
        }
    }
    REQUIRE(hairItemOrNull != nullptr);
    renderer::RenderGraphItem item = *hairItemOrNull;
    renderer::RenderGraphMesh mesh = extraction.Meshes.at(item.MeshIndex);
    REQUIRE(mesh.MaterialSlots.IsEmpty() == true);
    const uint32 materialSlot = 0;
    item.MeshIndex = 0;
    item.MaterialOverrides = {&materialSlot, 1};
    mesh.MaterialBindings = {&materialSlot, 1};

    // The glTF fixture builds contiguous strips with independent vertex indices. Verify each partition
    // before using that layout so an importer or fixture change can't silently mix ribbons.
    constexpr usize RIBBONS = 44;
    constexpr usize INDICES_PER_RIBBON = 216;
    constexpr usize PIXEL_COUNT = static_cast<usize>(BENCHMARK_WIDTH) * BENCHMARK_HEIGHT;
    REQUIRE(mesh.Geometry.IndexCount == RIBBONS * INDICES_PER_RIBBON);
    const uint32* indices = mesh.Geometry.Indices;
    REQUIRE(indices != nullptr);
    QVector<usize> ribbonOfVertex(static_cast<qsizetype>(mesh.Geometry.VertexCount), RIBBONS);
    for (usize index = 0; index < mesh.Geometry.IndexCount; ++index)
    {
        REQUIRE(indices[index] < mesh.Geometry.VertexCount);
        usize& ribbon = ribbonOfVertex[indices[index]];
        const usize expectedRibbon = index / INDICES_PER_RIBBON;
        REQUIRE((ribbon == RIBBONS || ribbon == expectedRibbon));
        ribbon = expectedRibbon;
    }
    mesh.Geometry.IndexCount = INDICES_PER_RIBBON;

    // An emissive white ribbon marks every accepted sample without depending on the light or the lobe.
    // Its texture, UVs and cutoff still go through the real forward coverage shader.
    renderer::SceneShaderMaterial mask = extraction.Materials[hairMaterial];
    REQUIRE(mask.IsAlphaMasked == true);
    mask.BaseColor = {};
    mask.Emission = {1, 1, 1};
    mask.IsEmissionTwoSided = true;
    mask.ShouldRegisterRasterMeshLight = false;
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&mask, 1};
    const auto measure = [&]() {
        QVector<uint8> layers(static_cast<qsizetype>(PIXEL_COUNT), 0);
        for (usize ribbon = 0; ribbon < RIBBONS; ++ribbon)
        {
            mesh.Geometry.Indices = indices + ribbon * INDICES_PER_RIBBON;
            const QImage image = captureRibbon(mesh, item, inputs).convertToFormat(QImage::Format_RGBA8888);
            REQUIRE(image.width() == static_cast<int32>(BENCHMARK_WIDTH));
            REQUIRE(image.height() == static_cast<int32>(BENCHMARK_HEIGHT));
            constexpr usize CHANNELS = 4;
            for (uint32 y = 0; y < BENCHMARK_HEIGHT; ++y)
            {
                const uint8* rowOrNull = image.constScanLine(static_cast<int32>(y));
                REQUIRE(rowOrNull != nullptr);
                for (uint32 x = 0; x < BENCHMARK_WIDTH; ++x)
                {
                    if (rowOrNull[x * CHANNELS] != 0)
                    {
                        ++layers[static_cast<qsizetype>(y * BENCHMARK_WIDTH + x)];
                    }
                }
            }
        }
        return layers;
    };
    const QVector<uint8> authored = measure();
    mask.Scattering = {};
    const QVector<uint8> control = measure();
    const bool hasMatchingCoverage = authored == control;
    REQUIRE(hasMatchingCoverage == true);

    qsizetype samples = 0;
    qsizetype footprint = 0;
    qsizetype overlapping = 0;
    for (const uint8 count : authored)
    {
        samples += count;
        footprint += count > 0 ? 1 : 0;
        overlapping += count > 1 ? 1 : 0;
    }
    REQUIRE(footprint > 0);
    REQUIRE(overlapping > 0);
    return {
        {QStringLiteral("ribbon_samples"), samples},
        {QStringLiteral("footprint_pixels"), footprint},
        {QStringLiteral("overlapping_pixels"), overlapping},
        {QStringLiteral("samples_per_covered_pixel"), static_cast<float64>(samples) / static_cast<float64>(footprint)}};
}
} // namespace

TEST_CASE("Supplied World times lit raster passes", "[.raster-mesh-light-benchmark]")
{
    const QString root = qEnvironmentVariable("LUDUS_RASTER_BENCHMARK_PROJECT");
    // A World under the project's assets folder; the reported Staircase2 capture by default.
    QString worldName = qEnvironmentVariable("LUDUS_RASTER_BENCHMARK_WORLD");
    if (worldName.isEmpty() == true)
    {
        worldName = QStringLiteral("staircase2.world.json");
    }
    const QString output = qEnvironmentVariable("LUDUS_RASTER_BENCHMARK_OUTPUT");
    // Optional PNG of the last measured frame's first exported colour target.
    const QString imagePath = qEnvironmentVariable("LUDUS_RASTER_BENCHMARK_IMAGE");
    // Optional comparison against each selected material's ordinary metallic/roughness surface.
    namespace format = ludus::content::scene_format;
    const bool shouldCompareSkin = qEnvironmentVariableIsSet("LUDUS_RASTER_BENCHMARK_SKIN_CONTROL");
    const bool shouldCompareHair = qEnvironmentVariableIsSet("LUDUS_RASTER_BENCHMARK_HAIR_CONTROL");
    REQUIRE((shouldCompareSkin == false || shouldCompareHair == false));
    // Optional JSON file holding an editor navigation state (RenderPreview::RestoreNavigation) instead of the
    // default framing of the whole World, for example an orbit pivot, yaw, pitch and zoom inside the scene.
    const QString navigationPath = qEnvironmentVariable("LUDUS_RASTER_BENCHMARK_NAVIGATION");
    QString runtime = qEnvironmentVariable("LUDUS_RASTER_BENCHMARK_RUNTIME");
    if (runtime.isEmpty() == true)
    {
        runtime = GetDefaultRenderGraphRuntime();
    }
    if (root.isEmpty() == true || output.isEmpty() == true || runtime.isEmpty() == true)
    {
        SKIP("Provide the project and report destination through LUDUS_RASTER_BENCHMARK_PROJECT/OUTPUT; "
             "LUDUS_RASTER_BENCHMARK_RUNTIME overrides the build's default graph and LUDUS_RASTER_BENCHMARK_WORLD "
             "selects another World under the project's assets folder.");
    }
    const RenderScene scene = tests::LoadSuppliedWorld(root, worldName);
    QString error;
    RenderPreview viewport;
    REQUIRE(viewport.SetScene(scene) == true);
    if (navigationPath.isEmpty() == false)
    {
        const QJsonDocument navigation = QJsonDocument::fromJson(tests::ReadTestFile(navigationPath));
        REQUIRE(navigation.isObject() == true);
        viewport.RestoreNavigation(navigation.object());
    }
    renderer::ViewDescription view;
    REQUIRE(viewport.DescribeConventionalView(BENCHMARK_WIDTH, BENCHMARK_HEIGHT, view, error) == true);
    if (shouldCompareHair == true)
    {
        view.WorldGrid.IsVisible = false;
    }
    RenderGraphExtraction extraction;
    REQUIRE(MakeRenderGraphExtraction(scene, view, 1, extraction, error) == true);

    rhi::Shutdown();
    // The editor viewport's budgets: its per-frame draw limit decides how fast mesh-light shadow faces complete.
    rhi::DeviceDescription description = MakeViewportDeviceDescription();
    description.EnableGpuTiming = true;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    REQUIRE(
        rhi::CreateDevice({}, { .Width = BENCHMARK_WIDTH, .Height = BENCHMARK_HEIGHT }, description, device, surface) ==
        rhi::DeviceStatus::Ready);
    rhi::MeasurementAdapter adapter;
    REQUIRE(rhi::GetMeasurementAdapter(device, adapter) == rhi::RasterStatus::Ready);
    QJsonObject result;
    {
        const QByteArray bytes = tests::ReadTestFile(runtime);
        renderer::RenderGraphPackage package;
        REQUIRE(package.Decode({reinterpret_cast<const uint8*>(bytes.constData()), static_cast<usize>(bytes.size())}) ==
                renderer::RenderGraphPackageStatus::Ready);
        const renderer::RenderGraphPrograms programs =
            package.GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
        INFO("Supply a cooked lit raster runtime package.");
        REQUIRE((programs.HasClusteredForward == true || shouldCompareHair == true));
        if (shouldCompareHair == true)
        {
            REQUIRE(programs.HasClusteredDeferred == false);
        }
        renderer::RenderGraph graph;
        rhi::RasterStatus status =
            graph.Initialize(device, package.GetPlan(), programs, nullptr, BENCHMARK_WIDTH, BENCHMARK_HEIGHT);
        QElapsedTimer timer;
        timer.start();
        const auto waitReady = [&graph, &timer](rhi::RasterStatus initial) {
            rhi::RasterStatus current = initial;
            while (current == rhi::RasterStatus::Pending && timer.elapsed() < PREPARATION_TIMEOUT_MILLISECONDS)
            {
                QThread::msleep(1);
                current = graph.Poll();
            }
            REQUIRE(current == rhi::RasterStatus::Ready);
        };
        waitReady(status);
        renderer::RenderGraphReport report;
        timer.restart();
        status = graph.Update({extraction.Meshes.constData(), static_cast<usize>(extraction.Meshes.size())},
                              {extraction.Items.constData(), static_cast<usize>(extraction.Items.size())},
                              {&extraction.View, 1},
                              extraction.Revision,
                              renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                              report,
                              &extraction.ShaderInputs);
        waitReady(status);
        const float64 preparationMilliseconds =
            static_cast<float64>(timer.nsecsElapsed()) / NANOSECONDS_PER_MILLISECOND;
        const auto renderFrame = [&]() {
            REQUIRE(rhi::ArmGpuTiming(device) == rhi::RasterStatus::Ready);
            rhi::SubmissionToken completion;
            REQUIRE(graph.Render(surface, BENCHMARK_WIDTH, BENCHMARK_HEIGHT, completion) == rhi::RasterStatus::Ready);
            timer.restart();
            rhi::RasterStatus frameStatus = rhi::GetStatus(device, completion);
            while (frameStatus == rhi::RasterStatus::Pending && timer.elapsed() < PREPARATION_TIMEOUT_MILLISECONDS)
            {
                QThread::msleep(1);
                frameStatus = rhi::GetStatus(device, completion);
            }
            REQUIRE(frameStatus == rhi::RasterStatus::Ready);
            rhi::GpuTiming timing;
            REQUIRE(rhi::GetGpuTiming(device, timing) == rhi::RasterStatus::Ready);
            REQUIRE(timing.State == rhi::GpuTimingState::Available);
            return static_cast<float64>(timing.DurationNanoseconds) / NANOSECONDS_PER_MILLISECOND;
        };
        // Frames that draw cached mesh-light shadow faces are reported apart from the measured steady state.
        QJsonArray fillFrames;
        while (graph.GetReport().PendingMeshLightShadowFaces != 0 &&
               static_cast<uint32>(fillFrames.size()) < MAX_SHADOW_FILL_FRAMES)
        {
            fillFrames.append(renderFrame());
        }
        REQUIRE(graph.GetReport().PendingMeshLightShadowFaces == 0);
        QJsonArray frames;
        QJsonArray cpuRenderMilliseconds;
        QJsonArray passFrames;
        QJsonArray passNames;
        for (uint32 frame = 0; frame < WARMUP_FRAMES + MEASURED_FRAMES; ++frame)
        {
            REQUIRE(rhi::ArmGpuTiming(device) == rhi::RasterStatus::Ready);
            REQUIRE(rhi::ArmGpuPassTimings(device) == rhi::RasterStatus::Ready);
            rhi::SubmissionToken completion;
            timer.restart();
            status = graph.Render(surface, BENCHMARK_WIDTH, BENCHMARK_HEIGHT, completion);
            const float64 cpuMilliseconds = static_cast<float64>(timer.nsecsElapsed()) / NANOSECONDS_PER_MILLISECOND;
            REQUIRE(status == rhi::RasterStatus::Ready);
            timer.restart();
            status = rhi::GetStatus(device, completion);
            while (status == rhi::RasterStatus::Pending && timer.elapsed() < PREPARATION_TIMEOUT_MILLISECONDS)
            {
                QThread::msleep(1);
                status = rhi::GetStatus(device, completion);
            }
            REQUIRE(status == rhi::RasterStatus::Ready);
            rhi::GpuTiming timing;
            rhi::GpuPassTimings passes;
            REQUIRE(rhi::GetGpuTiming(device, timing) == rhi::RasterStatus::Ready);
            REQUIRE(rhi::GetGpuPassTimings(device, passes) == rhi::RasterStatus::Ready);
            REQUIRE(timing.State == rhi::GpuTimingState::Available);
            REQUIRE(passes.State == rhi::GpuTimingState::Available);
            if (frame < WARMUP_FRAMES)
            {
                continue;
            }
            frames.append(static_cast<float64>(timing.DurationNanoseconds) / NANOSECONDS_PER_MILLISECOND);
            cpuRenderMilliseconds.append(cpuMilliseconds);
            if (passNames.isEmpty() == true)
            {
                for (usize pass = 0; pass < passes.Count; ++pass)
                {
                    passNames.append(QString::fromUtf8(passes.Passes[pass].Scope.Name.GetData()));
                }
            }
            QJsonArray passMilliseconds;
            for (usize pass = 0; pass < passes.Count; ++pass)
            {
                passMilliseconds.append(passes.Passes[pass].Timing.State == rhi::GpuTimingState::Available
                                            ? static_cast<float64>(passes.Passes[pass].Timing.DurationNanoseconds) /
                                                  NANOSECONDS_PER_MILLISECOND
                                            : -1.0);
            }
            passFrames.append(passMilliseconds);
        }
        const auto readImage = [&]() {
            const renderer::RenderGraphPlan plan = package.GetPlan();
            uint32 exported = static_cast<uint32>(plan.Resources.GetSize());
            for (usize resource = 0; resource < plan.Resources.GetSize(); ++resource)
            {
                if (plan.Resources[resource].IsExported != 0 &&
                    plan.Resources[resource].Kind == ludus::content::render_graph_format::Aspect::Color)
                {
                    exported = static_cast<uint32>(resource);
                    break;
                }
            }
            REQUIRE(exported < plan.Resources.GetSize());
            rhi::TextureHandle texture;
            rhi::TextureAspect aspect = rhi::TextureAspect::Color;
            REQUIRE(graph.GetExport(exported, texture, aspect) == rhi::RasterStatus::Ready);
            OffscreenFrameReader reader;
            REQUIRE(reader.Begin(device, texture, BENCHMARK_WIDTH, BENCHMARK_HEIGHT) == true);
            timer.restart();
            OffscreenReadStatus read = reader.Poll();
            while (read == OffscreenReadStatus::Pending && timer.elapsed() < PREPARATION_TIMEOUT_MILLISECONDS)
            {
                QThread::msleep(1);
                read = reader.Poll();
            }
            REQUIRE(read == OffscreenReadStatus::Complete);
            return reader.TakeImage();
        };
        if (imagePath.isEmpty() == false)
        {
            REQUIRE(readImage().save(imagePath, "PNG") == true);
        }
        const renderer::RenderGraphAllocationReport allocations = graph.GetAllocationReport();
        // The control changes only the scattering model. Geometry, texture, mask, camera and lights are shared.
        QJsonObject materialControl;
        if (shouldCompareSkin == true || shouldCompareHair == true)
        {
            QVector<renderer::SceneShaderMaterial> controlMaterials = extraction.Materials;
            const format::MaterialModel model =
                shouldCompareHair == true ? format::MaterialModel::HairCard : format::MaterialModel::Skin;
            qsizetype materialCount = 0;
            qsizetype hairMaterial = -1;
            for (qsizetype materialIndex = 0; materialIndex < controlMaterials.size(); ++materialIndex)
            {
                renderer::SceneShaderMaterial& material = controlMaterials[materialIndex];
                if (material.Scattering.Model == model)
                {
                    material.Scattering = {};
                    ++materialCount;
                    hairMaterial = materialIndex;
                }
            }
            renderer::SceneShaderInputs controlInputs = extraction.ShaderInputs;
            controlInputs.Materials = {controlMaterials.constData(), static_cast<usize>(controlMaterials.size())};
            uint64 revision = extraction.Revision;
            const auto prepare = [&](const renderer::SceneShaderInputs& inputs) {
                ++revision;
                waitReady(graph.Update({extraction.Meshes.constData(), static_cast<usize>(extraction.Meshes.size())},
                                       {extraction.Items.constData(), static_cast<usize>(extraction.Items.size())},
                                       {&extraction.View, 1},
                                       revision,
                                       renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                       report,
                                       &inputs));
            };
            const auto measureBlock = [&]() {
                QJsonArray block;
                for (uint32 frame = 0; frame < WARMUP_FRAMES + MEASURED_FRAMES; ++frame)
                {
                    const float64 milliseconds = renderFrame();
                    if (frame >= WARMUP_FRAMES)
                    {
                        block.append(milliseconds);
                    }
                }
                return block;
            };
            QJsonArray controlBlocks;
            QJsonArray authoredBlocks;
            for (uint32 block = 0; block < MATERIAL_CONTROL_BLOCKS; ++block)
            {
                prepare(controlInputs);
                controlBlocks.append(measureBlock());
                prepare(extraction.ShaderInputs);
                authoredBlocks.append(measureBlock());
            }
            if (imagePath.isEmpty() == false)
            {
                prepare(controlInputs);
                static_cast<void>(measureBlock());
                QString controlPath = imagePath;
                controlPath.replace(QStringLiteral(".png"), QStringLiteral("-control.png"));
                REQUIRE(readImage().save(controlPath, "PNG") == true);
            }
            materialControl = {
                {shouldCompareHair == true ? QStringLiteral("hair_materials") : QStringLiteral("skin_materials"),
                 materialCount},
                {QStringLiteral("control_gpu_frame_ms"), controlBlocks},
                {QStringLiteral("authored_gpu_frame_ms"), authoredBlocks}};
            if (shouldCompareHair == true)
            {
                REQUIRE(materialCount == 1);
                const auto captureRibbon = [&](const renderer::RenderGraphMesh& mesh,
                                               const renderer::RenderGraphItem& item,
                                               const renderer::SceneShaderInputs& inputs) {
                    waitReady(graph.Update({&mesh, 1},
                                           {&item, 1},
                                           {&extraction.View, 1},
                                           ++revision,
                                           renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                           report,
                                           &inputs));
                    static_cast<void>(renderFrame());
                    return readImage();
                };
                const QJsonObject overdraw = MeasureHairOverdraw(extraction, hairMaterial, captureRibbon);
                materialControl.insert(QStringLiteral("overdraw"), overdraw);
                prepare(extraction.ShaderInputs);
            }
        }
        result = {
            {QStringLiteral("adapter"), QString::fromUtf8(adapter.Name)},
            {QStringLiteral("world"), worldName},
            {QStringLiteral("runtime"), runtime},
            {QStringLiteral("profile"),
             programs.HasClusteredDeferred == true  ? QStringLiteral("ClusteredDeferred")
             : programs.HasClusteredForward == true ? QStringLiteral("ClusteredForward")
                                                    : QStringLiteral("ForwardLit")},
            {QStringLiteral("width"), static_cast<int32>(BENCHMARK_WIDTH)},
            {QStringLiteral("height"), static_cast<int32>(BENCHMARK_HEIGHT)},
            {QStringLiteral("deferred_attachment_bytes"), static_cast<qint64>(allocations.DeferredAttachmentBytes)},
            {QStringLiteral("deferred_skin_allocations"), static_cast<qint64>(allocations.DeferredSkinAllocations)},
            {QStringLiteral("deferred_skin_attachment_bytes"),
             static_cast<qint64>(allocations.DeferredSkinAttachmentBytes)},
            {QStringLiteral("mesh_light_polygons"), static_cast<qint64>(graph.GetReport().Resources.MeshLightPolygons)},
            {QStringLiteral("mesh_light_bytes"), static_cast<qint64>(graph.GetReport().Resources.MeshLightBytes)},
            {QStringLiteral("mesh_light_shadow_casters"),
             static_cast<qint64>(graph.GetReport().Resources.MeshLightShadowCasters)},
            {QStringLiteral("unshadowed_mesh_lights"),
             static_cast<qint64>(graph.GetReport().Resources.UnshadowedMeshLights)},
            {QStringLiteral("mesh_light_shadow_faces"),
             static_cast<qint64>(graph.GetReport().Resources.MeshLightShadowFaces)},
            {QStringLiteral("preparation_ms"), preparationMilliseconds},
            {QStringLiteral("shadow_fill_gpu_frame_ms"), fillFrames},
            {QStringLiteral("cpu_render_ms"), cpuRenderMilliseconds},
            {QStringLiteral("gpu_frame_ms"), frames},
            {QStringLiteral("pass_names"), passNames},
            {QStringLiteral("pass_ms"), passFrames},
            {QStringLiteral("skin_control"), shouldCompareSkin == true ? materialControl : QJsonObject{}},
            {QStringLiteral("hair_control"), shouldCompareHair == true ? materialControl : QJsonObject{}}};
    }
    static_cast<void>(rhi::DestroyDevice(device));
    rhi::Shutdown();
    QFile file(output);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) == true);
    const QByteArray serialized = QJsonDocument(result).toJson(QJsonDocument::Indented);
    REQUIRE(file.write(serialized) == serialized.size());
}
