// Explicit supplied-project benchmark of the default raster graph's mesh-light resolve. Reads assets and the runtime
// package; writes only the requested report. Compare builds by running each binary in alternation and comparing
// per-pass ratios within one run: Apple GPU clocks vary several-fold between runs.
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/world_document.h"
#include "renderer_composite.h"

#include <ludus/content/scene_cook/scene_assets.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/measurement.h>

#include <QDir>
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
constexpr int32 PREPARATION_TIMEOUT_MILLISECONDS = 120000;
constexpr uint32 MEBIBYTE = 1024 * 1024;
constexpr float64 NANOSECONDS_PER_MILLISECOND = 1000000;

QByteArray ReadBenchmarkFile(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}
} // namespace

TEST_CASE("Supplied Staircase2 World times the default raster graph's mesh-light resolve",
          "[.raster-mesh-light-benchmark]")
{
    const QString root = qEnvironmentVariable("LUDUS_RASTER_BENCHMARK_PROJECT");
    const QString output = qEnvironmentVariable("LUDUS_RASTER_BENCHMARK_OUTPUT");
    QString runtime = qEnvironmentVariable("LUDUS_RASTER_BENCHMARK_RUNTIME");
    if (runtime.isEmpty() == true)
    {
        runtime = GetDefaultRenderGraphRuntime();
    }
    if (root.isEmpty() == true || output.isEmpty() == true || runtime.isEmpty() == true)
    {
        SKIP("Provide the project and report destination through LUDUS_RASTER_BENCHMARK_PROJECT/OUTPUT; "
             "LUDUS_RASTER_BENCHMARK_RUNTIME overrides the build's default graph.");
    }
    WorldDocument world;
    QString error;
    REQUIRE(world.LoadSchema(QByteArrayLiteral("{\"version\":1,\"id\":\"ludus.world.default.v1\",\"label\":\"World\","
                                               "\"components\":[],\"assets\":[]}"),
                             error) == true);
    REQUIRE(world.Load(ReadBenchmarkFile(QDir(root).filePath(QStringLiteral("assets/staircase2.world.json"))), error) ==
            true);
    const QJsonObject level = world.SceneLevel();
    const QByteArray levelBytes = QJsonDocument(level).toJson(QJsonDocument::Compact);
    const QByteArray rootBytes = root.toUtf8();
    Array<uint8> bundle;
    ludus::content::scene_cook::AssetError assetError;
    REQUIRE(ludus::content::scene_cook::AssembleLevel(
                {rootBytes.constData(), static_cast<usize>(rootBytes.size())},
                {reinterpret_cast<const uint8*>(levelBytes.constData()), static_cast<usize>(levelBytes.size())},
                {},
                bundle,
                assetError) == true);
    RenderScene scene;
    REQUIRE(RenderScene::LoadCooked(
                QByteArray(reinterpret_cast<const char*>(bundle.GetData()), static_cast<qsizetype>(bundle.GetSize())),
                level,
                scene,
                error,
                {}) == true);
    RenderPreview viewport;
    REQUIRE(viewport.SetScene(scene) == true);
    renderer::ViewDescription view;
    REQUIRE(viewport.DescribeConventionalView(BENCHMARK_WIDTH, BENCHMARK_HEIGHT, view, error) == true);
    RenderGraphExtraction extraction;
    REQUIRE(MakeRenderGraphExtraction(scene, view, 1, extraction, error) == true);

    rhi::Shutdown();
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.EnableGpuTiming = true;
    description.Storage.ResourcesPerKind = 4096;
    description.Storage.BindingDescriptors = 32768;
    description.Storage.HostBytes = usize{256} * MEBIBYTE;
    description.Storage.ResourceBytes = uint64{2048} * MEBIBYTE;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    REQUIRE(
        rhi::CreateDevice({}, { .Width = BENCHMARK_WIDTH, .Height = BENCHMARK_HEIGHT }, description, device, surface) ==
        rhi::DeviceStatus::Ready);
    rhi::MeasurementAdapter adapter;
    REQUIRE(rhi::GetMeasurementAdapter(device, adapter) == rhi::RasterStatus::Ready);
    QJsonObject result;
    {
        const QByteArray bytes = ReadBenchmarkFile(runtime);
        renderer::RenderGraphPackage package;
        REQUIRE(package.Decode({reinterpret_cast<const uint8*>(bytes.constData()), static_cast<usize>(bytes.size())}) ==
                renderer::RenderGraphPackageStatus::Ready);
        const renderer::RenderGraphPrograms programs =
            package.GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
        REQUIRE(programs.HasClusteredDeferred == true);
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
        QJsonArray frames;
        QJsonArray passFrames;
        for (uint32 frame = 0; frame < WARMUP_FRAMES + MEASURED_FRAMES; ++frame)
        {
            REQUIRE(rhi::ArmGpuTiming(device) == rhi::RasterStatus::Ready);
            REQUIRE(rhi::ArmGpuPassTimings(device) == rhi::RasterStatus::Ready);
            rhi::SubmissionToken completion;
            REQUIRE(graph.Render(surface, BENCHMARK_WIDTH, BENCHMARK_HEIGHT, completion) == rhi::RasterStatus::Ready);
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
        result = {{QStringLiteral("adapter"), QString::fromUtf8(adapter.Name)},
                  {QStringLiteral("runtime"), runtime},
                  {QStringLiteral("width"), static_cast<int32>(BENCHMARK_WIDTH)},
                  {QStringLiteral("height"), static_cast<int32>(BENCHMARK_HEIGHT)},
                  {QStringLiteral("mesh_light_bytes"), static_cast<qint64>(graph.GetReport().Resources.MeshLightBytes)},
                  {QStringLiteral("preparation_ms"), preparationMilliseconds},
                  {QStringLiteral("gpu_frame_ms"), frames},
                  {QStringLiteral("pass_ms"), passFrames}};
    }
    static_cast<void>(rhi::DestroyDevice(device));
    rhi::Shutdown();
    QFile file(output);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) == true);
    const QByteArray serialized = QJsonDocument(result).toJson(QJsonDocument::Indented);
    REQUIRE(file.write(serialized) == serialized.size());
}
