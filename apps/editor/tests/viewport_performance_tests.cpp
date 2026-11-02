// The production editor viewport, graph service and Qt presentation, rather than
// a renderer-only/offscreen substitute. Timings are report-only; coverage is required.
#include "internal/preview_viewport.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
#include "viewport_fixture.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/graphics/rhi/measurement.h>
#include <ludus/graphics/rhi/rhi.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QObject>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdio>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace
{
constexpr int32 WIDTH = 640;
constexpr int32 HEIGHT = 480;
constexpr int32 WARMUP_FRAMES = 5;
constexpr int32 MEASURED_FRAMES = 30;
constexpr int32 DEADLINE_MILLISECONDS = 60'000;
constexpr int32 IDLE_MILLISECONDS = 250;
constexpr float32 ORBIT_DELTA_PIXEL = 2;
constexpr float64 NANOSECONDS_PER_MILLISECOND = 1'000'000;
} // namespace

TEST_CASE("Editor viewport reports navigation CPU GPU and presentation latency", "[.editor-viewport-benchmark]")
{
    const bool hasMeshLights = GENERATE(false, true);
    REQUIRE(PreviewViewport::GetPresentation() == ViewportPresentation::SharedDevice);
    rhi::Shutdown();
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    RenderPreview source;
    source.SetForceRaster(true);
    PreviewViewport viewport(&source);
    viewport.UseProjectGraph(workspace);
    workspace.SetPreviewSource(&source);
    REQUIRE(workspace.PreviewRuntime() == true);
    RenderScene scene;
    tests::MakeLitScene(QFileInfo(project.path()).canonicalFilePath(), scene);
    if (hasMeshLights == true)
    {
        REQUIRE(scene.Surfaces.isEmpty() == false);
        constexpr float32 EMITTED_RADIANCE = 2;
        for (PreviewSurface& surface : scene.Surfaces)
        {
            surface.Emission = {EMITTED_RADIANCE, EMITTED_RADIANCE, EMITTED_RADIANCE};
        }
    }
    viewport.resize(WIDTH, HEIGHT);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport) == true);
    REQUIRE(source.SetScene(scene) == true);
    REQUIRE(source.Start() == true);
    REQUIRE(QTest::qWaitFor([&viewport]() { return viewport.PresentedFrames() > 0; }, DEADLINE_MILLISECONDS) == true);
    REQUIRE(source.SetGpuProfilingEnabled(true) == true);
    RenderGraphPreview& graph = *workspace.GetPreviewOrNull();
    uint64 gpuSamples = 0;
    uint64 gpuNanoseconds = 0;
    const QMetaObject::Connection sampleConnection = QObject::connect(
        &source,
        &RenderPreview::GpuProfileSample,
        &source,
        [&gpuSamples, &gpuNanoseconds](uint64, uint64, uint64, uint64 durationNanoseconds, bool isValid) {
            if (isValid == true)
            {
                ++gpuSamples;
                gpuNanoseconds = durationNanoseconds;
            }
        });
    QTextStream output(stdout);
    for (int32 sample = -WARMUP_FRAMES; sample < MEASURED_FRAMES; ++sample)
    {
        const uint64 previousFrame = viewport.PresentedFrames();
        const uint64 previousRevision = graph.GetPresentedRevision();
        const uint64 previousGpuSamples = gpuSamples;
        QElapsedTimer latency;
        latency.start();
        source.Orbit(ORBIT_DELTA_PIXEL, 0);
        while ((viewport.PresentedFrames() <= previousFrame || graph.GetPresentedRevision() <= previousRevision ||
                gpuSamples <= previousGpuSamples) &&
               latency.elapsed() < DEADLINE_MILLISECONDS)
        {
            QTest::qWait(1);
        }
        INFO(source.Status().toStdString());
        INFO(graph.GetStatus().toStdString());
        REQUIRE(viewport.PresentedFrames() > previousFrame);
        REQUIRE(graph.GetPresentedRevision() > previousRevision);
        REQUIRE(gpuSamples > previousGpuSamples);
        REQUIRE(graph.HasGpuFrame() == true);
        REQUIRE(graph.GetImage().isNull() == true);
        REQUIRE(graph.GetStatistics().GetReadbackMilliseconds().GetLatest() == 0);
        LUDUS_PROFILE_FRAME();
        if (sample >= 0)
        {
            output << "EDVIEW," << (hasMeshLights == true ? "mesh-lights" : "analytic-lights") << ',' << sample << ','
                   << graph.GetStatistics().GetCpuMilliseconds().GetLatest() * NANOSECONDS_PER_MILLISECOND << ','
                   << gpuNanoseconds << ',' << latency.nsecsElapsed() << '\n';
        }
    }
    rhi::MeasurementAdapter adapter;
    REQUIRE(rhi::GetMeasurementAdapter(source.GetDevice(), adapter) == rhi::RasterStatus::Ready);
    // Both generated workloads use the same default adapter selection. The report
    // accepts exactly one adapter observation for this process.
    if (hasMeshLights == true)
    {
        output << "R7ADAPTER," << QByteArray(adapter.Name).toHex() << ',' << adapter.VendorId << ',' << adapter.DeviceId
               << ',' << adapter.DriverVersion << ',' << (adapter.SoftwareKnown == true ? 1 : 0) << ','
               << (adapter.Software == true ? 1 : 0) << '\n';
    }
    // An unchanged on-demand graph must stop producing GPU frames while retaining its image.
    QTest::qWait(IDLE_MILLISECONDS);
    const uint64 settledRevision = graph.GetPresentedRevision();
    const uint64 settledSamples = gpuSamples;
    QTest::qWait(IDLE_MILLISECONDS);
    CHECK(graph.GetPresentedRevision() == settledRevision);
    CHECK(gpuSamples == settledSamples);
    CHECK(viewport.HasRetainedGraphFrame() == true);
    QObject::disconnect(sampleConnection);
    source.Stop();
    workspace.SetPreviewSource(nullptr);
    rhi::Shutdown();
}
