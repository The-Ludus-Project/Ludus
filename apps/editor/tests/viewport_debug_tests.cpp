#include "internal/preview_shading.h"
#include "internal/viewport_statistics.h"

#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include <QSet>
#include <QString>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;

namespace
{
/// Arbitrary frame time used to fill the rolling window in tests, in milliseconds.
constexpr float64 STEADY_MILLISECONDS = 4;
/// Arbitrary outlier added after the window fills, in milliseconds.
constexpr float64 OUTLIER_MILLISECONDS = 34;
} // namespace

TEST_CASE("Viewport shading modes have unique keys, self-explaining labels and renderer debug views",
          "[editor][viewport][shading]")
{
    QSet<QString> keys;
    QSet<QString> labels;
    for (usize i = 0; i < PREVIEW_SHADING_COUNT; ++i)
    {
        const PreviewShading shading = static_cast<PreviewShading>(i);
        const QString key = GetPreviewShadingKey(shading);
        const QString label = GetPreviewShadingLabel(shading);
        INFO(key.toStdString());
        CHECK(key.isEmpty() == false);
        CHECK(label.contains(QStringLiteral(" — ")) == true);
        CHECK(GetPreviewShadingStatus(shading).isEmpty() == false);
        keys.insert(key);
        labels.insert(label);
        PreviewShading parsed = PreviewShading::Solid;
        REQUIRE(TryParsePreviewShading(key, parsed) == true);
        CHECK(parsed == shading);
        const bool isDebug = i > static_cast<usize>(PreviewShading::Lit);
        CHECK(IsPreviewDebugShading(shading) == isDebug);
        CHECK((GetMaterialDebugView(shading) != renderer::MaterialDebugView::None) == isDebug);
    }
    CHECK(static_cast<usize>(keys.size()) == PREVIEW_SHADING_COUNT);
    CHECK(static_cast<usize>(labels.size()) == PREVIEW_SHADING_COUNT);
    CHECK(GetPreviewShadingLabel(PreviewShading::Solid) == QStringLiteral("Solid — neutral preview shading"));
    CHECK(GetPreviewShadingLabel(PreviewShading::Albedo).contains(QStringLiteral("colours")) == true);
    CHECK(GetMaterialDebugView(PreviewShading::Normal) == renderer::MaterialDebugView::NormalWorld);
    CHECK(GetMaterialDebugView(PreviewShading::Depth) == renderer::MaterialDebugView::LinearDepth);
    CHECK(GetMaterialDebugView(PreviewShading::Roughness) == renderer::MaterialDebugView::Roughness);
    CHECK(GetMaterialDebugView(PreviewShading::Metallic) == renderer::MaterialDebugView::Metallic);
    CHECK(GetMaterialDebugView(PreviewShading::Uv) == renderer::MaterialDebugView::TexcoordUv);
    CHECK(GetMaterialDebugView(PreviewShading::LightCount) == renderer::MaterialDebugView::LightCount);
    PreviewShading unchanged = PreviewShading::Metallic;
    CHECK(TryParsePreviewShading(QStringLiteral("overdraw"), unchanged) == false);
    CHECK(TryParsePreviewShading(QString(), unchanged) == false);
    CHECK(unchanged == PreviewShading::Metallic);
}

TEST_CASE("Debug depth range follows the camera lens and tightens to scene bounds", "[editor][viewport][shading]")
{
    renderer::ViewDescription view;
    view.CameraPosition = {0, 0, 10};
    view.CameraForward = {0, 0, -1};
    math::Vector2 range = ComputeDebugDepthRange(view, 0.1F, 100, nullptr);
    CHECK(range.X == Catch::Approx(0.1F));
    CHECK(range.Y == Catch::Approx(100));

    const math::Aabb3 bounds{{-1, -1, -1}, {1, 1, 1}};
    range = ComputeDebugDepthRange(view, 0.1F, 100, &bounds);
    // Closest box point (0, 0, 1) and farthest corner (±1, ±1, -1) from (0, 0, 10): radial distances.
    CHECK(range.X == Catch::Approx(9));
    CHECK(range.Y == Catch::Approx(math::Length(math::Vector3{1, 1, -11})));
    // A camera inside the bounds keeps the lens near plane.
    view.CameraPosition = {0, 0, 0};
    range = ComputeDebugDepthRange(view, 0.1F, 100, &bounds);
    CHECK(range.X == Catch::Approx(0.1F));
    view.CameraPosition = {0, 0, 10};

    view.OrthographicCamera = true;
    range = ComputeDebugDepthRange(view, 0.1F, 100, &bounds);
    CHECK(range.X == Catch::Approx(9));
    CHECK(range.Y == Catch::Approx(11));

    // An infinite far plane without bounds stays finite so the renderer accepts it.
    range = ComputeDebugDepthRange(view, 0.1F, std::numeric_limits<float32>::infinity(), nullptr);
    CHECK(math::IsFinite(range.Y) == true);
    CHECK(range.Y > range.X);
    // Bounds behind the camera never collapse the range.
    const math::Aabb3 behind{{-1, -1, 20}, {1, 1, 22}};
    range = ComputeDebugDepthRange(view, 0.1F, 100, &behind);
    CHECK(range.Y == Catch::Approx(100));
    range = ComputeDebugDepthRange(view, -5, 0, nullptr);
    CHECK(range.X == 0);
    CHECK(range.Y > range.X);
}

TEST_CASE("Viewport statistics average a fixed window and format the overlay", "[editor][viewport][statistics]")
{
    RollingAverage average;
    CHECK(average.GetCount() == 0);
    CHECK(average.GetAverage() == 0);
    for (usize i = 0; i < VIEWPORT_STATISTICS_AVERAGE_FRAMES; ++i)
    {
        average.Add(STEADY_MILLISECONDS);
    }
    CHECK(average.GetCount() == VIEWPORT_STATISTICS_AVERAGE_FRAMES);
    CHECK(average.GetAverage() == Catch::Approx(STEADY_MILLISECONDS));
    // One more sample evicts the oldest; the window never grows past N.
    average.Add(OUTLIER_MILLISECONDS);
    CHECK(average.GetCount() == VIEWPORT_STATISTICS_AVERAGE_FRAMES);
    CHECK(average.GetAverage() ==
          Catch::Approx(STEADY_MILLISECONDS + (OUTLIER_MILLISECONDS - STEADY_MILLISECONDS) /
                                                  static_cast<float64>(VIEWPORT_STATISTICS_AVERAGE_FRAMES)));
    average.Reset();
    CHECK(average.GetCount() == 0);

    ViewportStatistics statistics;
    statistics.Width = 1280;
    statistics.Height = 720;
    statistics.Draws = 42;
    CHECK(FormatViewportStatistics(statistics) == QStringLiteral("GPU: …\nCPU: …\n1280×720 · draws 42"));
    statistics.CpuMilliseconds.Add(1.5);
    statistics.CpuMilliseconds.Add(2.5);
    statistics.GpuState = ViewportGpuTimeState::Available;
    statistics.GpuMilliseconds.Add(0.25);
    CHECK(FormatViewportStatistics(statistics) == QStringLiteral("GPU: 0.25 ms\nCPU: 2.00 ms\n1280×720 · draws 42"));
}

TEST_CASE("Viewport statistics show GPU n/a when timestamps are unavailable", "[editor][viewport][statistics]")
{
    ViewportStatistics statistics;
    statistics.GpuState = ViewportGpuTimeState::Unavailable;
    // Stale samples from an earlier device never leak through once timing is unavailable.
    statistics.GpuMilliseconds.Add(STEADY_MILLISECONDS);
    statistics.CpuMilliseconds.Add(STEADY_MILLISECONDS);
    statistics.Width = 64;
    statistics.Height = 32;
    statistics.Draws = 1;
    CHECK(FormatViewportStatistics(statistics) == QStringLiteral("GPU: n/a\nCPU: 4.00 ms\n64×32 · draws 1"));
}

TEST_CASE("Graph statistics measure delivered updates and separate active CPU from elapsed readback",
          "[editor][viewport][statistics][graph-statistics]")
{
    GraphViewportStatistics statistics;
    ViewportStatistics device;
    CHECK(FormatGraphViewportStatistics(statistics, device, 0) ==
          QStringLiteral("FPS (updates): 0.0\nCPU: …\nGPU: …\nReadback: …"));
    statistics.AddCpuWork(1.5);
    statistics.AddCpuWork(2.5);
    statistics.AddCpuWork(-1);
    statistics.AddCpuWork(std::numeric_limits<float64>::infinity());
    statistics.CompleteFrame(0, 20);
    CHECK(statistics.GetCpuMilliseconds().GetAverage() == 4);
    statistics.AddCpuWork(2);
    statistics.CompleteFrame(50'000'000, 30);
    CHECK(statistics.GetFramesPerSecond(50'000'000) == Catch::Approx(20));
    CHECK(statistics.GetCpuMilliseconds().GetAverage() == 3);
    CHECK(statistics.GetReadbackMilliseconds().GetAverage() == 25);
    device.GpuMilliseconds.Add(0.25);
    device.GpuState = ViewportGpuTimeState::Available;
    CHECK(FormatGraphViewportStatistics(statistics, device, 50'000'000) ==
          QStringLiteral("FPS (updates): 20.0\nCPU: 3.00 ms\nGPU: 0.25 ms\nReadback: 25.00 ms"));
    device.GpuState = ViewportGpuTimeState::Unavailable;
    CHECK(FormatGraphViewportStatistics(statistics, device, 50'000'000).contains(QStringLiteral("GPU: n/a")));
    // No new output is produced while an unchanged graph is idle. Its former rate must expire.
    CHECK(statistics.GetFramesPerSecond(50'000'000 + GraphViewportStatistics::IDLE_NANOSECONDS) == 0);
    statistics.CompleteFrame(2'000'000'000, 1);
    CHECK(statistics.GetFramesPerSecond(2'000'000'000) == 0);
    statistics.CompleteFrame(2'100'000'000, 1);
    CHECK(statistics.GetFramesPerSecond(2'100'000'000) == Catch::Approx(10));
    CHECK(statistics.GetFramesPerSecond(1) == 0);
    statistics.Reset();
    CHECK(statistics.GetCpuMilliseconds().GetCount() == 0);
    CHECK(statistics.GetReadbackMilliseconds().GetCount() == 0);
    CHECK(statistics.GetFramesPerSecond(2'100'000'000) == 0);
}
