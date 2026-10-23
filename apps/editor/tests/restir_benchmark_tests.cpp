// Explicit supplied-project benchmark. Reads assets and shaders; writes only the requested report.
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
constexpr uint32 BENCHMARK_WIDTH = 640;
constexpr uint32 BENCHMARK_HEIGHT = 480;
constexpr uint32 WARMUP_FRAMES = 3;
constexpr uint32 MEASURED_FRAMES = 60;
constexpr int32 PREPARATION_TIMEOUT_MILLISECONDS = 120000;
constexpr uint32 MEBIBYTE = 1024 * 1024;
constexpr float64 NANOSECONDS_PER_MILLISECOND = 1000000;

QByteArray ReadBenchmarkFile(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

struct BenchmarkDevice final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    BenchmarkDevice()
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Required.RayQueries = true;
        description.EnableGpuTiming = true;
        description.Storage.ResourcesPerKind = 4096;
        description.Storage.BindingDescriptors = 32768;
        description.Storage.HostBytes = usize{256} * MEBIBYTE;
        description.Storage.ResourceBytes = uint64{2048} * MEBIBYTE;
        REQUIRE(rhi::CreateDevice({},
                                  { .Width = BENCHMARK_WIDTH, .Height = BENCHMARK_HEIGHT },
                                  description,
                                  Device,
                                  Surface) == rhi::DeviceStatus::Ready);
    }
    ~BenchmarkDevice()
    {
        static_cast<void>(rhi::DestroyDevice(Device));
        rhi::Shutdown();
    }
};

// The graph and package are scoped inside each round so no producer or borrowed program
// outlives its device/package. Preparation and warmup are excluded from all samples.
QJsonObject
MeasureRestirGraph(BenchmarkDevice& inoutDevice, const RenderGraphExtraction& extraction, const QString& runtime)
{
    const QByteArray bytes = ReadBenchmarkFile(runtime);
    renderer::RenderGraphPackage package;
    REQUIRE(package.Decode({reinterpret_cast<const uint8*>(bytes.constData()), static_cast<usize>(bytes.size())}) ==
            renderer::RenderGraphPackageStatus::Ready);
    renderer::RenderGraph graph;
    rhi::RasterStatus status = graph.Initialize(
        inoutDevice.Device,
        package.GetPlan(),
        package.GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()),
        nullptr,
        BENCHMARK_WIDTH,
        BENCHMARK_HEIGHT);
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
    status = graph.Update({extraction.Meshes.constData(), static_cast<usize>(extraction.Meshes.size())},
                          {extraction.Items.constData(), static_cast<usize>(extraction.Items.size())},
                          {&extraction.View, 1},
                          extraction.Revision,
                          renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                          report,
                          &extraction.ShaderInputs);
    waitReady(status);
    QJsonArray frames;
    QJsonArray firstCompute;
    for (uint32 frame = 0; frame < WARMUP_FRAMES + MEASURED_FRAMES; ++frame)
    {
        REQUIRE(rhi::ArmGpuTiming(inoutDevice.Device) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::ArmGpuPassTimings(inoutDevice.Device) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(graph.Render(inoutDevice.Surface, BENCHMARK_WIDTH, BENCHMARK_HEIGHT, completion) ==
                rhi::RasterStatus::Ready);
        timer.restart();
        status = rhi::GetStatus(inoutDevice.Device, completion);
        while (status == rhi::RasterStatus::Pending && timer.elapsed() < PREPARATION_TIMEOUT_MILLISECONDS)
        {
            QThread::msleep(1);
            status = rhi::GetStatus(inoutDevice.Device, completion);
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
        rhi::GpuTiming timing;
        rhi::GpuPassTimings passes;
        REQUIRE(rhi::GetGpuTiming(inoutDevice.Device, timing) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::GetGpuPassTimings(inoutDevice.Device, passes) == rhi::RasterStatus::Ready);
        REQUIRE(timing.State == rhi::GpuTimingState::Available);
        REQUIRE(passes.State == rhi::GpuTimingState::Available);
        REQUIRE(passes.Count >= 3);
        REQUIRE(passes.Passes[0].Kind == rhi::GpuPassKind::Compute);
        REQUIRE(passes.Passes[0].Timing.State == rhi::GpuTimingState::Available);
        if (frame >= WARMUP_FRAMES)
        {
            frames.append(timing.DurationNanoseconds / NANOSECONDS_PER_MILLISECOND);
            firstCompute.append(passes.Passes[0].Timing.DurationNanoseconds / NANOSECONDS_PER_MILLISECOND);
        }
    }
    return {{QStringLiteral("runtime"), runtime},
            {QStringLiteral("gpu_frame_ms"), frames},
            {QStringLiteral("first_compute_ms"), firstCompute}};
}
} // namespace

TEST_CASE("Supplied World compares baseline and optimized ReSTIR on the same GPU", "[.restir-benchmark]")
{
    const QString root = qEnvironmentVariable("LUDUS_RESTIR_BENCHMARK_PROJECT");
    const QString baseline = qEnvironmentVariable("LUDUS_RESTIR_BASELINE_RUNTIME");
    const QString candidate = qEnvironmentVariable("LUDUS_RESTIR_CANDIDATE_RUNTIME");
    const QString output = qEnvironmentVariable("LUDUS_RESTIR_BENCHMARK_OUTPUT");
    if (root.isEmpty() == true || baseline.isEmpty() == true || candidate.isEmpty() == true || output.isEmpty() == true)
    {
        SKIP("Provide project, baseline/candidate runtimes and report destination through LUDUS_RESTIR_BENCHMARK_* "
             "variables.");
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
    BenchmarkDevice device;
    rhi::MeasurementAdapter adapter;
    REQUIRE(rhi::GetMeasurementAdapter(device.Device, adapter) == rhi::RasterStatus::Ready);
    QJsonArray rounds;
    // ABBA reduces monotonic warmup/thermal ordering bias; both paths retain eight candidates.
    for (const QString& runtime : {baseline, candidate, candidate, baseline})
    {
        rounds.append(MeasureRestirGraph(device, extraction, runtime));
    }
    const QJsonObject result{{QStringLiteral("adapter"), QString::fromUtf8(adapter.Name)},
                             {QStringLiteral("width"), static_cast<int32>(BENCHMARK_WIDTH)},
                             {QStringLiteral("height"), static_cast<int32>(BENCHMARK_HEIGHT)},
                             {QStringLiteral("rounds"), rounds}};
    QFile file(output);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) == true);
    const QByteArray report = QJsonDocument(result).toJson(QJsonDocument::Indented);
    REQUIRE(file.write(report) == report.size());
}
