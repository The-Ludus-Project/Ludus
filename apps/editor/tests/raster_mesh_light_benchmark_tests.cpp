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

#include <ludus/foundation/base/types.h>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/measurement.h>

#include <QElapsedTimer>
#include <QFile>
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
constexpr float64 NANOSECONDS_PER_MILLISECOND = 1000000;
} // namespace

TEST_CASE("Supplied World times clustered raster passes", "[.raster-mesh-light-benchmark]")
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
        INFO("Supply a cooked ClusteredForward or ClusteredDeferred runtime package.");
        REQUIRE(programs.HasClusteredForward == true);
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
        if (imagePath.isEmpty() == false)
        {
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
            REQUIRE(reader.TakeImage().save(imagePath, "PNG") == true);
        }
        const renderer::RenderGraphAllocationReport allocations = graph.GetAllocationReport();
        result = {
            {QStringLiteral("adapter"), QString::fromUtf8(adapter.Name)},
            {QStringLiteral("world"), worldName},
            {QStringLiteral("runtime"), runtime},
            {QStringLiteral("profile"),
             programs.HasClusteredDeferred == true ? QStringLiteral("ClusteredDeferred")
                                                   : QStringLiteral("ClusteredForward")},
            {QStringLiteral("width"), static_cast<int32>(BENCHMARK_WIDTH)},
            {QStringLiteral("height"), static_cast<int32>(BENCHMARK_HEIGHT)},
            {QStringLiteral("deferred_attachment_bytes"), static_cast<qint64>(allocations.DeferredAttachmentBytes)},
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
            {QStringLiteral("pass_ms"), passFrames}};
    }
    static_cast<void>(rhi::DestroyDevice(device));
    rhi::Shutdown();
    QFile file(output);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) == true);
    const QByteArray serialized = QJsonDocument(result).toJson(QJsonDocument::Indented);
    REQUIRE(file.write(serialized) == serialized.size());
}
