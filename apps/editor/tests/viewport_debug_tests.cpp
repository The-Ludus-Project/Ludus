#include "design_tokens.h"
#include "internal/offscreen_frame_reader.h"
#include "internal/preview_shading.h"
#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/software_viewport.h"
#include "internal/viewport_hud_text.h"
#include "internal/viewport_pass_bars.h"
#include "internal/viewport_size.h"
#include "internal/viewport_statistics.h"

#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/lighting_layout.h>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/viewport.hpp>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/graphics/rhi/rhi.h>

#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QObject>
#include <QPainter>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTest>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
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
/// Side of the square wireframe test frames, in pixels.
constexpr int32 WIREFRAME_FRAME_PIXELS = 96;
/// Smallest channel of a light pixel: the white triangle or a wireframe edge, not the dark grid or background.
constexpr int32 LIGHT_CHANNEL_MINIMUM = 100;
/// Largest channel spread of a light pixel, which excludes the saturated red, green and blue axes.
constexpr int32 LIGHT_CHANNEL_SPREAD = 40;
/// Largest difference of red/blue and green/blue ratios from the shared wireframe colour's ratios.
constexpr float64 WIREFRAME_RATIO_TOLERANCE = .03;
/// Largest per-channel difference from the shared wireframe colour on the software edge, which faces the camera
/// within a few degrees.
constexpr int32 SOFTWARE_WIREFRAME_TOLERANCE = 4;

/// One camera-facing triangle in an unlit World, framed automatically by the preview.
RenderScene MakeTriangleScene()
{
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("wireframe-triangle");
    scene.AssetsIdentity = scene.Identity;
    PreviewMesh mesh;
    mesh.Positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    mesh.Uvs = {{0, 0}, {1, 0}, {.5F, 1}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.append(mesh);
    PreviewObject object;
    object.Id = QStringLiteral("triangle");
    object.Mesh = 0;
    scene.Objects.append(object);
    return scene;
}
/// Whether pixel is light and nearly grey: the white triangle or a wireframe edge, never the editor's dark world
/// grid, background or saturated axes.
bool IsLightPixel(QRgb pixel) noexcept
{
    const int32 smallest = qMin(qRed(pixel), qMin(qGreen(pixel), qBlue(pixel)));
    const int32 largest = qMax(qRed(pixel), qMax(qGreen(pixel), qBlue(pixel)));
    return smallest >= LIGHT_CHANNEL_MINIMUM && largest - smallest <= LIGHT_CHANNEL_SPREAD;
}
usize CountLightPixels(const QImage& image)
{
    usize count = 0;
    for (int32 y = 0; y < image.height(); ++y)
    {
        for (int32 x = 0; x < image.width(); ++x)
        {
            count += IsLightPixel(image.pixel(x, y)) == true ? 1U : 0U;
        }
    }
    return count;
}
/// The brightest pixel of image by channel sum, which on a wireframe is an edge facing the camera.
QRgb FindBrightestPixel(const QImage& image)
{
    QRgb brightest = image.pixel(0, 0);
    for (int32 y = 0; y < image.height(); ++y)
    {
        for (int32 x = 0; x < image.width(); ++x)
        {
            const QRgb pixel = image.pixel(x, y);
            if (IsLightPixel(pixel) == true &&
                qRed(pixel) + qGreen(pixel) + qBlue(pixel) > qRed(brightest) + qGreen(brightest) + qBlue(brightest))
            {
                brightest = pixel;
            }
        }
    }
    return brightest;
}
/// Check that pixel is the shared wireframe colour at some facing shade: equal channel ratios, no brighter.
void CheckWireframeHue(QRgb pixel)
{
    CAPTURE(qRed(pixel), qGreen(pixel), qBlue(pixel));
    REQUIRE(qBlue(pixel) > 0);
    const float64 blue = static_cast<float64>(renderer::MATERIAL_DEBUG_WIREFRAME_BLUE);
    CHECK(qAbs(static_cast<float64>(qRed(pixel)) / qBlue(pixel) - renderer::MATERIAL_DEBUG_WIREFRAME_RED / blue) <=
          WIREFRAME_RATIO_TOLERANCE);
    CHECK(qAbs(static_cast<float64>(qGreen(pixel)) / qBlue(pixel) - renderer::MATERIAL_DEBUG_WIREFRAME_GREEN / blue) <=
          WIREFRAME_RATIO_TOLERANCE);
    CHECK(qBlue(pixel) <= static_cast<int32>(renderer::MATERIAL_DEBUG_WIREFRAME_BLUE) + 1);
}
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
/// Timeout for preparing and reading back one preview frame, in milliseconds.
constexpr int32 PREVIEW_TIMEOUT_MILLISECONDS = 30000;
/// Draw the running preview in shading and read the displayed frame back; null on timeout or failure.
QImage CaptureShading(RenderPreview& inoutPreview, PreviewShading shading)
{
    inoutPreview.SetShading(shading);
    const bool hasFrame = QTest::qWaitFor(
        [&inoutPreview]() {
            return (inoutPreview.Frames() >= 2 && inoutPreview.HasPendingFrame() == true) ||
                   inoutPreview.Running() == false;
        },
        PREVIEW_TIMEOUT_MILLISECONDS);
    renderer::ViewportFrame frame;
    renderer::ViewportImage image;
    if (hasFrame == false || inoutPreview.TakeFrame(frame, image) == false)
    {
        return {};
    }
    OffscreenFrameReader reader;
    QImage result;
    if (reader.Begin(inoutPreview.GetDevice(), image.Texture, image.Width, image.Height) == true)
    {
        OffscreenReadStatus status = OffscreenReadStatus::Pending;
        const bool isRead = QTest::qWaitFor(
            [&inoutPreview, &reader, &status]() {
                static_cast<void>(ludus::graphics::rhi::PollLifetime(inoutPreview.GetDevice()));
                status = reader.Poll();
                return status != OffscreenReadStatus::Pending;
            },
            PREVIEW_TIMEOUT_MILLISECONDS);
        if (isRead == true && status == OffscreenReadStatus::Complete)
        {
            result = reader.TakeImage();
        }
        reader.Cancel();
    }
    inoutPreview.ReleaseFrame(frame);
    return result;
}
#endif
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
    CHECK(GetMaterialDebugView(PreviewShading::Wireframe) == renderer::MaterialDebugView::Wireframe);
    CHECK(GetPreviewShadingLabel(PreviewShading::Wireframe).contains(QStringLiteral("edges")) == true);
    CHECK(GetPreviewShadingName(PreviewShading::LightCount) == QStringLiteral("Light count"));
    CHECK(GetPreviewShadingName(PreviewShading::Wireframe) == QStringLiteral("Wireframe"));
    for (usize i = 0; i < PREVIEW_SHADING_COUNT; ++i)
    {
        const PreviewShading shading = static_cast<PreviewShading>(i);
        INFO(GetPreviewShadingKey(shading).toStdString());
        CHECK(IsPreviewLineShading(shading) == (shading == PreviewShading::Wireframe));
        // The graph viewport leaves Solid and Lit to the graph's own output.
        CHECK(IsPreviewGraphDebugView(shading) == (shading != PreviewShading::Solid && shading != PreviewShading::Lit));
    }
    PreviewShading unchanged = PreviewShading::Metallic;
    CHECK(TryParsePreviewShading(QStringLiteral("overdraw"), unchanged) == false);
    CHECK(TryParsePreviewShading(QString(), unchanged) == false);
    CHECK(unchanged == PreviewShading::Metallic);
}

TEST_CASE("Debug ramps match viridis and brighten steadily from end to end", "[editor][viewport][shading]")
{
    constexpr float32 VIRIDIS_TOLERANCE = .02F;
    const math::Vector3 start = GetPreviewRampColour(PreviewLegendRamp::Viridis, 0);
    const math::Vector3 middle = GetPreviewRampColour(PreviewLegendRamp::Viridis, .5F);
    const math::Vector3 end = GetPreviewRampColour(PreviewLegendRamp::Viridis, 1);
    // Anchors of matplotlib's viridis at 0, 0.5 and 1.
    CHECK(start.X == Catch::Approx(.267F).margin(VIRIDIS_TOLERANCE));
    CHECK(start.Y == Catch::Approx(.005F).margin(VIRIDIS_TOLERANCE));
    CHECK(start.Z == Catch::Approx(.329F).margin(VIRIDIS_TOLERANCE));
    CHECK(middle.X == Catch::Approx(.128F).margin(VIRIDIS_TOLERANCE));
    CHECK(middle.Y == Catch::Approx(.567F).margin(VIRIDIS_TOLERANCE));
    CHECK(middle.Z == Catch::Approx(.551F).margin(VIRIDIS_TOLERANCE));
    CHECK(end.X == Catch::Approx(.993F).margin(VIRIDIS_TOLERANCE));
    CHECK(end.Y == Catch::Approx(.906F).margin(VIRIDIS_TOLERANCE));
    CHECK(end.Z == Catch::Approx(.144F).margin(VIRIDIS_TOLERANCE));
    // Positions outside [0,1] clamp to the ends.
    CHECK(GetPreviewRampColour(PreviewLegendRamp::Viridis, -3).X == Catch::Approx(start.X));
    CHECK(GetPreviewRampColour(PreviewLegendRamp::Viridis, 3).Y == Catch::Approx(end.Y));
    // Luma (Rec. 709 weights on the display values) rises at every step, which is what a heat map hides.
    constexpr usize STEP_COUNT = 32;
    constexpr float32 RED_LUMA = .2126F;
    constexpr float32 GREEN_LUMA = .7152F;
    constexpr float32 BLUE_LUMA = .0722F;
    for (const PreviewLegendRamp ramp : {PreviewLegendRamp::Viridis, PreviewLegendRamp::Grey})
    {
        float32 previousLuma = -1;
        for (usize step = 0; step <= STEP_COUNT; ++step)
        {
            const math::Vector3 colour =
                GetPreviewRampColour(ramp, static_cast<float32>(step) / static_cast<float32>(STEP_COUNT));
            const float32 luma = RED_LUMA * colour.X + GREEN_LUMA * colour.Y + BLUE_LUMA * colour.Z;
            CHECK(luma > previousLuma);
            previousLuma = luma;
        }
    }
}

TEST_CASE("Scalar debug views have a legend and the other modes do not", "[editor][viewport][shading]")
{
    PreviewLegend legend;
    legend.Title = QStringLiteral("unchanged");
    for (const PreviewShading shading : {PreviewShading::Solid,
                                         PreviewShading::Albedo,
                                         PreviewShading::Lit,
                                         PreviewShading::Normal,
                                         PreviewShading::Uv})
    {
        CHECK(TryGetPreviewLegend(shading, legend) == false);
        CHECK(legend.Title == QStringLiteral("unchanged"));
    }
    for (const PreviewShading shading :
         {PreviewShading::Depth, PreviewShading::Roughness, PreviewShading::Metallic, PreviewShading::LightCount})
    {
        INFO(GetPreviewShadingKey(shading).toStdString());
        REQUIRE(TryGetPreviewLegend(shading, legend) == true);
        CHECK(legend.Title.isEmpty() == false);
        CHECK(legend.LowLabel.isEmpty() == false);
        CHECK(legend.HighLabel.isEmpty() == false);
    }
    REQUIRE(TryGetPreviewLegend(PreviewShading::LightCount, legend) == true);
    CHECK(legend.HighLabel == QStringLiteral("16 or more"));
    CHECK(legend.Ramp == PreviewLegendRamp::Viridis);
    REQUIRE(TryGetPreviewLegend(PreviewShading::Metallic, legend) == true);
    CHECK(legend.Ramp == PreviewLegendRamp::Grey);
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
    CHECK(average.GetPercentile95() == 0);
    CHECK(average.GetMaximum() == 0);
    CHECK(average.GetLatest() == 0);
    for (usize i = 0; i < VIEWPORT_STATISTICS_AVERAGE_FRAMES; ++i)
    {
        average.Add(STEADY_MILLISECONDS);
    }
    CHECK(average.GetCount() == VIEWPORT_STATISTICS_AVERAGE_FRAMES);
    CHECK(average.GetAverage() == Catch::Approx(STEADY_MILLISECONDS));
    // One more sample evicts the oldest; the window never grows past N.
    average.Add(OUTLIER_MILLISECONDS);
    CHECK(average.GetLatest() == OUTLIER_MILLISECONDS);
    CHECK(average.GetPercentile95() == STEADY_MILLISECONDS);
    CHECK(average.GetMaximum() == OUTLIER_MILLISECONDS);
    average.Add(OUTLIER_MILLISECONDS);
    CHECK(average.GetPercentile95() == OUTLIER_MILLISECONDS);
    CHECK(average.GetCount() == VIEWPORT_STATISTICS_AVERAGE_FRAMES);
    CHECK(average.GetAverage() ==
          Catch::Approx(STEADY_MILLISECONDS + 2 * (OUTLIER_MILLISECONDS - STEADY_MILLISECONDS) /
                                                  static_cast<float64>(VIEWPORT_STATISTICS_AVERAGE_FRAMES)));
    average.Reset();
    CHECK(average.GetCount() == 0);
    CHECK(average.GetPercentile95() == 0);
    CHECK(average.GetMaximum() == 0);
    average.Add(OUTLIER_MILLISECONDS);
    for (usize index = 0; index < VIEWPORT_STATISTICS_AVERAGE_FRAMES; ++index)
    {
        average.Add(STEADY_MILLISECONDS);
    }
    average.Add(-1);
    average.Add(std::numeric_limits<float64>::quiet_NaN());
    average.Add(std::numeric_limits<float64>::infinity());
    CHECK(average.GetPercentile95() == STEADY_MILLISECONDS);
    CHECK(average.GetMaximum() == STEADY_MILLISECONDS);

    ViewportStatistics statistics;
    statistics.Width = 1280;
    statistics.Height = 720;
    statistics.Draws = 42;
    CHECK(FormatViewportStatistics(statistics) ==
          QStringLiteral("GPU: …\nEditor CPU per frame: …\nImage: 1280×720 · 42 draws"));
    statistics.CpuMilliseconds.Add(1.5);
    statistics.CpuMilliseconds.Add(2.5);
    statistics.GpuState = ViewportGpuTimeState::Available;
    statistics.GpuMilliseconds.Add(0.25);
    CHECK(FormatViewportStatistics(statistics) ==
          QStringLiteral("GPU: 0.25 ms (p95 0.25, max 0.25 ms)\nEditor CPU per frame: 2.00 ms (p95 2.50, max 2.50 "
                         "ms)\nImage: 1280×720 · "
                         "42 draws"));
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
    CHECK(FormatViewportStatistics(statistics) ==
          QStringLiteral("GPU: n/a\nEditor CPU per frame: 4.00 ms (p95 4.00, max 4.00 ms)\nImage: 64×32 · 1 draw"));
}

TEST_CASE("Pass statistics sum a scope's passes and average completed batches",
          "[editor][viewport][statistics][passes]")
{
    constexpr uint64 SCENE = 1;
    constexpr uint64 COMPOSITE = 2;
    ViewportPassStatistics passes;
    // Two physical passes of one scope sum; invalid samples are ignored.
    passes.AddSample(SCENE, QStringLiteral("Viewport/Scene"), 1.0);
    passes.AddSample(SCENE, QStringLiteral("Viewport/Scene"), 2.0);
    passes.AddSample(COMPOSITE, QStringLiteral("Viewport/Composite"), 0.5);
    passes.AddSample(COMPOSITE, QStringLiteral("Viewport/Composite"), -1.0);
    passes.AddSample(0, QStringLiteral("No identity"), 1.0);
    passes.AddSample(3, QStringLiteral("Not finite"), std::numeric_limits<float64>::infinity());
    passes.CommitBatch();
    REQUIRE(passes.GetSeries().GetSize() == 2);
    CHECK(passes.GetSeries()[0].Milliseconds.GetAverage() == Catch::Approx(3.0));
    CHECK(passes.GetSeries()[1].Milliseconds.GetAverage() == Catch::Approx(0.5));
    passes.AddSample(SCENE, QStringLiteral("Viewport/Scene"), 5.0);
    passes.AddSample(COMPOSITE, QStringLiteral("Viewport/Composite"), 0.5);
    passes.CommitBatch();
    CHECK(passes.GetSeries()[0].Milliseconds.GetAverage() == Catch::Approx(4.0));
    // The same identity under a new name is a different scope.
    passes.AddSample(SCENE, QStringLiteral("Viewport/Renamed"), 1.0);
    passes.CommitBatch();
    CHECK(passes.GetSeries().GetSize() == 3);
}

TEST_CASE("Pass statistics drop scopes that stop rendering and cap the tracked scopes",
          "[editor][viewport][statistics][passes]")
{
    ViewportPassStatistics passes;
    passes.AddSample(1, QStringLiteral("Gone"), 1.0);
    passes.AddSample(2, QStringLiteral("Stays"), 1.0);
    passes.CommitBatch();
    for (usize batch = 1; batch < VIEWPORT_PASS_STALE_BATCHES; ++batch)
    {
        passes.AddSample(2, QStringLiteral("Stays"), 1.0);
        passes.CommitBatch();
    }
    CHECK(passes.GetSeries().GetSize() == 2);
    passes.AddSample(2, QStringLiteral("Stays"), 1.0);
    passes.CommitBatch();
    REQUIRE(passes.GetSeries().GetSize() == 1);
    CHECK(passes.GetSeries()[0].Name == QStringLiteral("Stays"));

    passes.Reset();
    for (usize index = 1; index <= MAX_VIEWPORT_PASS_SERIES + 5; ++index)
    {
        passes.AddSample(index, QStringLiteral("Pass %1").arg(index), 1.0);
    }
    passes.CommitBatch();
    CHECK(passes.GetSeries().GetSize() == MAX_VIEWPORT_PASS_SERIES);
}

TEST_CASE("Pass lines list the slowest scopes with bars scaled to the slowest",
          "[editor][viewport][statistics][passes]")
{
    ViewportStatistics statistics;
    statistics.Width = 64;
    statistics.Height = 32;
    statistics.Draws = 1;
    CHECK(FormatViewportPassLines(statistics).isEmpty() == true);
    CHECK(MakeViewportPassBars(statistics).RowCount == 0);

    statistics.PassState = ViewportPassTimeState::Available;
    statistics.Passes.AddSample(1, QStringLiteral("Viewport/Overlay"), 0.5);
    statistics.Passes.AddSample(2, QStringLiteral("Viewport/Scene"), 2.0);
    statistics.Passes.AddSample(3, QStringLiteral("Viewport/Composite"), 1.0);
    statistics.Passes.CommitBatch();
    CHECK(FormatViewportPassLines(statistics) == QStringLiteral("Passes: 3.50 ms\n"
                                                                "Viewport/Scene: 2.00 ms\n"
                                                                "Viewport/Composite: 1.00 ms\n"
                                                                "Viewport/Overlay: 0.50 ms"));
    const ViewportPassBars bars = MakeViewportPassBars(statistics);
    REQUIRE(bars.RowCount == 3);
    CHECK(bars.TrailingLines == 0);
    CHECK(bars.SlowestRow == 0);
    CHECK(bars.Fractions[0] == Catch::Approx(1.0F));
    CHECK(bars.Fractions[1] == Catch::Approx(0.5F));
    CHECK(bars.Fractions[2] == Catch::Approx(0.25F));
    // The slowest pass takes 2.0 of 3.5 ms, more than half, so its bar is a warning.
    CHECK(bars.IsSlowestWarning == true);
    CHECK(FormatViewportStatistics(statistics)
              .startsWith(QStringLiteral("GPU: …\nEditor CPU per frame: …\nImage: 64×32 · 1 draw\nPasses: ")));
}

TEST_CASE("Pass lines keep the slowest rows, sum the rest and shorten long names",
          "[editor][viewport][statistics][passes]")
{
    ViewportStatistics statistics;
    statistics.PassState = ViewportPassTimeState::Available;
    statistics.Passes.AddSample(1, QStringLiteral("Renderer/Very/Long/Pass/Name/Beyond/Columns"), 10.0);
    for (usize index = 0; index < MAX_VIEWPORT_PASS_ROWS + 2; ++index)
    {
        statistics.Passes.AddSample(index + 2, QStringLiteral("Pass %1").arg(index), 1.0);
    }
    statistics.Passes.CommitBatch();
    const QStringList lines = FormatViewportPassLines(statistics).split(QLatin1Char('\n'));
    REQUIRE(lines.size() == 1 + MAX_VIEWPORT_PASS_ROWS + 1);
    CHECK(lines[0] == QStringLiteral("Passes: 18.00 ms"));
    CHECK(lines[1].startsWith(QStringLiteral("Renderer/V…ond/Columns")) == true);
    CHECK(lines[1].endsWith(QStringLiteral("10.00 ms")) == true);
    CHECK(lines.last().startsWith(QStringLiteral("+3 more")) == true);
    CHECK(lines.last().endsWith(QStringLiteral(" 3.00 ms")) == true);
    const ViewportPassBars bars = MakeViewportPassBars(statistics);
    CHECK(bars.RowCount == MAX_VIEWPORT_PASS_ROWS);
    CHECK(bars.TrailingLines == 1);
    CHECK(bars.Fractions[1] == Catch::Approx(0.1F));
}

TEST_CASE("The slowest pass is a warning only when it takes more than half of the passes' time",
          "[editor][viewport][statistics][passes]")
{
    ViewportStatistics statistics;
    statistics.PassState = ViewportPassTimeState::Available;
    statistics.Passes.AddSample(1, QStringLiteral("Only"), 4.0);
    statistics.Passes.CommitBatch();
    // A lone pass is always the slowest, which says nothing about where time goes.
    CHECK(MakeViewportPassBars(statistics).IsSlowestWarning == false);
    statistics.Passes.Reset();
    statistics.Passes.AddSample(1, QStringLiteral("Scene"), 2.0);
    statistics.Passes.AddSample(2, QStringLiteral("Composite"), 2.0);
    statistics.Passes.CommitBatch();
    // An even split is exactly half: not a warning.
    CHECK(MakeViewportPassBars(statistics).IsSlowestWarning == false);
    statistics.Passes.Reset();
    statistics.Passes.AddSample(1, QStringLiteral("Scene"), 3.0);
    statistics.Passes.AddSample(2, QStringLiteral("Composite"), 1.0);
    statistics.Passes.CommitBatch();
    CHECK(MakeViewportPassBars(statistics).IsSlowestWarning == true);
}

TEST_CASE("Pass bars paint the slowest row in the warning token and the rest in the accent token",
          "[editor][viewport][statistics][passes]")
{
    constexpr int WIDTH = 200;
    constexpr int ROW_HEIGHT = 14;
    constexpr int OPACITY = 120;
    constexpr int OPAQUE = 255;
    constexpr int TOLERANCE = 3;
    QImage image(WIDTH, ROW_HEIGHT * 3, QImage::Format_ARGB32_Premultiplied);
    image.fill(qRgb(0, 0, 0));
    ViewportPassBars bars;
    bars.RowCount = 2;
    bars.TrailingLines = 1;
    bars.Fractions[0] = 1.0F;
    bars.Fractions[1] = 0.5F;
    bars.SlowestRow = 0;
    bars.IsSlowestWarning = true;
    {
        QPainter painter(&image);
        // Lines 0 and 1 are barred; line 2 is the unbarred "+N more" row.
        PaintViewportPassBars(painter, QRectF(10, 0, 180, ROW_HEIGHT * 3), ROW_HEIGHT, 0, bars);
    }
    const QRgb warning = ludus::editor::design::GetThemeArgb(ludus::editor::design::ThemeColour::StatusWarning, true);
    const QRgb accent = ludus::editor::design::GetThemeArgb(ludus::editor::design::ThemeColour::AccentPrimary, true);
    const auto isNear = [](QRgb actual, QRgb token) {
        return std::abs(qRed(actual) - qRed(token) * OPACITY / OPAQUE) <= TOLERANCE &&
               std::abs(qGreen(actual) - qGreen(token) * OPACITY / OPAQUE) <= TOLERANCE &&
               std::abs(qBlue(actual) - qBlue(token) * OPACITY / OPAQUE) <= TOLERANCE;
    };
    CHECK(isNear(image.pixel(100, ROW_HEIGHT / 2), warning) == true);
    CHECK(isNear(image.pixel(185, ROW_HEIGHT / 2), warning) == true);
    CHECK(isNear(image.pixel(50, ROW_HEIGHT + ROW_HEIGHT / 2), accent) == true);
    // The second bar is half as long, so the right half of its row stays empty.
    CHECK(image.pixel(150, ROW_HEIGHT + ROW_HEIGHT / 2) == qRgb(0, 0, 0));
    CHECK(image.pixel(100, ROW_HEIGHT * 2 + ROW_HEIGHT / 2) == qRgb(0, 0, 0));
    // Padding outside the text area stays clear, and a negative first line draws nothing.
    CHECK(image.pixel(4, ROW_HEIGHT / 2) == qRgb(0, 0, 0));
    QImage untouched(WIDTH, ROW_HEIGHT, QImage::Format_ARGB32_Premultiplied);
    untouched.fill(qRgb(0, 0, 0));
    {
        QPainter painter(&untouched);
        PaintViewportPassBars(painter, QRectF(10, 0, 180, ROW_HEIGHT), ROW_HEIGHT, -1, bars);
    }
    CHECK(untouched.pixel(100, ROW_HEIGHT / 2) == qRgb(0, 0, 0));
    // Without a dominant pass, the slowest bar keeps the accent colour.
    bars.IsSlowestWarning = false;
    QImage even(WIDTH, ROW_HEIGHT, QImage::Format_ARGB32_Premultiplied);
    even.fill(qRgb(0, 0, 0));
    {
        QPainter painter(&even);
        PaintViewportPassBars(painter, QRectF(10, 0, 180, ROW_HEIGHT), ROW_HEIGHT, 0, bars);
    }
    CHECK(isNear(even.pixel(100, ROW_HEIGHT / 2), accent) == true);
}

TEST_CASE("Pass timing reads n/a when the backend has no per-pass queries", "[editor][viewport][statistics][passes]")
{
    ViewportStatistics statistics;
    statistics.Width = 64;
    statistics.Height = 32;
    statistics.Draws = 1;
    statistics.GpuState = ViewportGpuTimeState::Available;
    statistics.PassState = ViewportPassTimeState::Unavailable;
    CHECK(FormatViewportPassLines(statistics) == QStringLiteral("Passes: n/a"));
    CHECK(FormatViewportStatistics(statistics).endsWith(QStringLiteral("\nPasses: n/a")) == true);
    CHECK(MakeViewportPassBars(statistics).RowCount == 0);
    // Without timestamps at all, both lines say so.
    statistics.GpuState = ViewportGpuTimeState::Unavailable;
    CHECK(FormatViewportStatistics(statistics).startsWith(QStringLiteral("GPU: n/a\n")) == true);
    CHECK(FormatViewportStatistics(statistics).endsWith(QStringLiteral("\nPasses: n/a")) == true);
}

TEST_CASE("Graph statistics measure delivered updates and separate active CPU from elapsed readback",
          "[editor][viewport][statistics][graph-statistics]")
{
    GraphViewportStatistics statistics;
    ViewportStatistics device;
    CHECK(FormatGraphViewportStatistics(statistics, device, 0) ==
          QStringLiteral("Updates: idle, renders on change\nEditor CPU per frame: …\nGPU: …"));
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
          QStringLiteral("Updates: 20.0 fps\nEditor CPU per frame: 3.00 ms (p95 4.00, max 4.00 ms)\nGPU: 0.25 ms "
                         "(p95 0.25, max 0.25 ms)\nOutput readback: 25.00 ms"));
    // GPU output delivery must not replace the separately measured diagnostic readback with zero.
    statistics.AddInspection(6, 40);
    statistics.AddInspectionConversion(12, 45);
    statistics.CompleteFrame(100'000'000, 0);
    CHECK(statistics.GetInspectionCpuMilliseconds().GetAverage() == 6);
    CHECK(statistics.GetInspectionReadbackMilliseconds().GetAverage() == 40);
    CHECK(statistics.GetConversionWorkMilliseconds().GetAverage() == 12);
    CHECK(statistics.GetConversionElapsedMilliseconds().GetAverage() == 45);
    CHECK(statistics.GetCpuMilliseconds().GetAverage() == 2); // Worker work is excluded from GUI service CPU.
    CHECK(FormatGraphViewportStatistics(statistics, device, 100'000'000)
              .contains(QStringLiteral("Conversion worker: 12.00 ms\nConversion elapsed: 45.00 ms")) == true);
    CHECK(FormatGraphViewportStatistics(statistics, device, 100'000'000)
              .contains(QStringLiteral("Inspection CPU: 6.00 ms\nInspection readback: 40.00 ms")) == true);
    statistics.SetInspectionEnabled(false);
    // Paused inspection takes no rows rather than a "paused" line.
    CHECK(FormatGraphViewportStatistics(statistics, device, 100'000'000).contains(QStringLiteral("Inspection")) ==
          false);
    CHECK(FormatGraphViewportStatistics(statistics, device, 100'000'000).contains(QStringLiteral("Inspection CPU:")) ==
          false);
    CHECK(statistics.GetInspectionCpuMilliseconds().GetAverage() == 6);
    device.GpuState = ViewportGpuTimeState::Unavailable;
    CHECK(FormatGraphViewportStatistics(statistics, device, 50'000'000).contains(QStringLiteral("GPU: n/a")));
    // No new output is produced while an unchanged graph is idle. Its former rate must expire.
    CHECK(statistics.GetFramesPerSecond(100'000'000 + GraphViewportStatistics::IDLE_NANOSECONDS) == 0);
    statistics.CompleteFrame(2'000'000'000, 1);
    CHECK(statistics.GetFramesPerSecond(2'000'000'000) == 0);
    statistics.CompleteFrame(2'100'000'000, 1);
    CHECK(statistics.GetFramesPerSecond(2'100'000'000) == Catch::Approx(10));
    CHECK(statistics.GetFramesPerSecond(1) == 0);
    statistics.Reset();
    CHECK(statistics.GetCpuMilliseconds().GetCount() == 0);
    CHECK(statistics.GetReadbackMilliseconds().GetCount() == 0);
    CHECK(statistics.GetInspectionCpuMilliseconds().GetCount() == 0);
    CHECK(statistics.GetInspectionReadbackMilliseconds().GetCount() == 0);
    CHECK(statistics.GetConversionWorkMilliseconds().GetCount() == 0);
    CHECK(statistics.GetConversionElapsedMilliseconds().GetCount() == 0);
    CHECK(statistics.GetFramesPerSecond(2'100'000'000) == 0);
}

TEST_CASE("Slow frame monitor reports consecutive slow frames of one generation after its cold first frame",
          "[editor][viewport][statistics][slow-frames]")
{
    constexpr float64 SLOW = ViewportSlowFrameMonitor::DEFAULT_SLOW_FRAME_MILLISECONDS;
    constexpr float64 FAST = 16;
    ViewportSlowFrameMonitor monitor;
    CHECK(monitor.GetStreakMilliseconds() == 0);
    // The first frame of a generation pays for its preparation and never counts, however slow.
    CHECK(monitor.AddFrame(1, SLOW * 30) == false);
    CHECK(monitor.AddFrame(1, SLOW) == false);
    CHECK(monitor.AddFrame(1, SLOW * 2) == false);
    CHECK(monitor.AddFrame(1, SLOW * 3) == true);
    CHECK(monitor.GetStreakMilliseconds() == Catch::Approx(SLOW * 2));
    // A renderer that stays slow is reported again only after another full streak.
    CHECK(monitor.AddFrame(1, SLOW) == false);
    CHECK(monitor.AddFrame(1, SLOW) == false);
    CHECK(monitor.AddFrame(1, SLOW) == true);
    CHECK(monitor.GetStreakMilliseconds() == Catch::Approx(SLOW));
    // One fast frame means a stall, not a slow renderer: the streak starts over.
    CHECK(monitor.AddFrame(1, SLOW) == false);
    CHECK(monitor.AddFrame(1, SLOW) == false);
    CHECK(monitor.AddFrame(1, FAST) == false);
    CHECK(monitor.AddFrame(1, SLOW) == false);
    CHECK(monitor.AddFrame(1, SLOW) == false);
    CHECK(monitor.AddFrame(1, std::numeric_limits<float64>::quiet_NaN()) == false);
    CHECK(monitor.AddFrame(1, SLOW) == false);
    CHECK(monitor.AddFrame(1, SLOW) == false);
    // A new generation (a rebuild or Force raster) starts over and skips its own first frame.
    CHECK(monitor.AddFrame(2, SLOW) == false);
    CHECK(monitor.AddFrame(2, SLOW) == false);
    CHECK(monitor.AddFrame(2, SLOW) == false);
    CHECK(monitor.AddFrame(2, SLOW) == true);
    CHECK(monitor.AddFrame(0, SLOW) == false);
    // Reset forgets the generation, so its next frame is treated as a first frame again.
    monitor.Reset();
    CHECK(monitor.GetStreakMilliseconds() == 0);
    CHECK(monitor.AddFrame(2, SLOW) == false);
    // Zero counts every frame; invalid thresholds are ignored.
    monitor.SetThresholdMilliseconds(-1);
    monitor.SetThresholdMilliseconds(std::numeric_limits<float64>::infinity());
    CHECK(monitor.GetThresholdMilliseconds() == SLOW);
    monitor.SetThresholdMilliseconds(0);
    CHECK(monitor.AddFrame(2, 0) == false);
    CHECK(monitor.AddFrame(2, 0) == false);
    CHECK(monitor.AddFrame(2, FAST) == true);
}

TEST_CASE("Viewport size presets have unique keys and start with Fill", "[editor][viewport][viewport-size]")
{
    QSet<QString> keys;
    for (usize i = 0; i < VIEWPORT_SIZE_PRESET_COUNT; ++i)
    {
        const QString key = GetViewportSizeKey(i);
        INFO(key.toStdString());
        CHECK(GetViewportSizeLabel(i).isEmpty() == false);
        CHECK(GetViewportSizeDescription(i).isEmpty() == false);
        usize parsed = VIEWPORT_SIZE_PRESET_COUNT;
        REQUIRE(TryParseViewportSizeKey(key, parsed) == true);
        CHECK(parsed == i);
        const ViewportSize size = GetViewportSizePreset(i);
        CHECK((size.Kind == ViewportSizeKind::Fill) == (i == 0));
        if (size.Kind != ViewportSizeKind::Fill)
        {
            CHECK(size.Width > 0);
            CHECK(size.Height > 0);
        }
        keys.insert(key);
    }
    CHECK(static_cast<usize>(keys.size()) == VIEWPORT_SIZE_PRESET_COUNT);
    CHECK(IsViewportSizeGroupStart(0) == false);
    usize unchanged = 3;
    CHECK(TryParseViewportSizeKey(QStringLiteral("7:5"), unchanged) == false);
    CHECK(TryParseViewportSizeKey(QString(), unchanged) == false);
    CHECK(unchanged == 3);
    // Out-of-range indices fall back to Fill rather than reading past the table.
    CHECK(GetViewportSizePreset(VIEWPORT_SIZE_PRESET_COUNT).Kind == ViewportSizeKind::Fill);
}

TEST_CASE("Viewport size presets letterbox the frame and choose the drawable", "[editor][viewport][viewport-size]")
{
    const QRect area(0, 20, 800, 400);
    const ViewportSize fill;
    const ViewportSize wide{ .Kind = ViewportSizeKind::Aspect, .Width = 16, .Height = 9 };
    const ViewportSize portrait{ .Kind = ViewportSizeKind::Aspect, .Width = 9, .Height = 16 };
    const ViewportSize fixed{ .Kind = ViewportSizeKind::Fixed, .Width = 1920, .Height = 1080 };
    CHECK(ComputeViewportFrameRectangle(fill, area) == area);
    // 16:9 is limited by the height: 400 x 16 / 9 = 711.1 wide, centred horizontally.
    CHECK(ComputeViewportFrameRectangle(wide, area) == QRect(44, 20, 711, 400));
    CHECK(ComputeViewportFrameRectangle(portrait, area) == QRect(287, 20, 225, 400));
    // A fixed resolution keeps its shape at any area, and the drawable stays exact.
    CHECK(ComputeViewportFrameRectangle(fixed, area) == ComputeViewportFrameRectangle(wide, area));
    const QRect tall(10, 0, 300, 900);
    CHECK(ComputeViewportFrameRectangle(wide, tall) == QRect(10, 365, 300, 169));
    CHECK(ComputeViewportFrameRectangle(wide, QRect()) == QRect());
    // A sliver keeps at least one pixel on each side.
    CHECK(ComputeViewportFrameRectangle(wide, QRect(0, 0, 1000, 1)).size() == QSize(2, 1));
    CHECK(ComputeViewportFrameRectangle(portrait, QRect(0, 0, 1, 1000)).size() == QSize(1, 2));

    CHECK(ComputeViewportDrawableSize(fill, QSize(711, 400), 2) == QSize(1422, 800));
    CHECK(ComputeViewportDrawableSize(wide, QSize(711, 400), 1.5) == QSize(1067, 600));
    CHECK(ComputeViewportDrawableSize(fixed, QSize(711, 400), 2) == QSize(1920, 1080));
    CHECK(ComputeViewportDrawableSize(fixed, QSize(), 1) == QSize(1920, 1080));
    CHECK(ComputeViewportDrawableSize(fill, QSize(0, 0), 2) == QSize(0, 0));
    CHECK(ComputeViewportDrawableSize(fill, QSize(), 2) == QSize(0, 0));
}

TEST_CASE("Viewport status overlays wrap sentences and shorten only words wider than the panel",
          "[editor][viewport][render-graph-failures]")
{
    // A failed-pixel explanation must stay readable in full, however narrow the viewport.
    const QString sentence = QStringLiteral(
        "2,660,962 pixels of ray pass 0 failed (the failure colour). A shader set queries.HasFailed without a reason.");
    constexpr qreal NARROW_WIDTH = 320;
    const QString wrapped = FitOverlayText(sentence, NARROW_WIDTH);
    CHECK(wrapped.contains(QLatin1Char('\n')) == true);
    CHECK(wrapped.contains(QStringLiteral("\u2026")) == false);
    CHECK(wrapped.split(QLatin1Char('\n')).join(QLatin1Char(' ')) == sentence);
    // A path is one word: it keeps both ends, so the file name and line stay visible.
    const QString path = QStringLiteral("/Users/someone/Documents/Projects/Sample/graphs/Main/main_shaders/"
                                        "shader_Shader_raytracingpipeline_closestHit.slang:113");
    const QString shortened = FitOverlayText(path, NARROW_WIDTH);
    CHECK(shortened.contains(QStringLiteral("\u2026")) == true);
    CHECK(shortened.endsWith(QStringLiteral(":113")) == true);
    CHECK(shortened.contains(QLatin1Char('\n')) == false);
    // Explicit line breaks survive, and a width the overlay cannot use keeps the text.
    CHECK(FitOverlayText(QStringLiteral("one\ntwo"), NARROW_WIDTH) == QStringLiteral("one\ntwo"));
    CHECK(FitOverlayText(sentence, 0) == sentence);
}

TEST_CASE("Wireframe draws only triangle edges in the shared colour on the GPU", "[editor][viewport][shading][gpu]")
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    ludus::graphics::rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            ludus::graphics::rhi::Shutdown();
        }
    } guard;
    RenderPreview preview;
    bool isAdmitted = false;
    QObject::connect(&preview, &RenderPreview::DeviceRequested, &preview, [&preview, &isAdmitted]() {
        isAdmitted = preview.AdmitOffscreenDevice();
    });
    preview.Resize(WIREFRAME_FRAME_PIXELS, WIREFRAME_FRAME_PIXELS);
    // Keep the sampled edge above the world grid: an axis blended into an edge can be the brightest pixel.
    RenderScene scene = MakeTriangleScene();
    scene.Objects[0].Transform.Translation.Y = 2;
    REQUIRE(preview.SetScene(scene) == true);
    const bool hasStarted = preview.Start();
    if (isAdmitted == false)
    {
        SKIP("No headless GPU device on this host");
    }
    REQUIRE(hasStarted == true);
    ludus::graphics::rhi::RasterCapabilities capabilities;
    REQUIRE(ludus::graphics::rhi::GetRasterCapabilities(preview.GetDevice(), capabilities) ==
            ludus::graphics::rhi::RasterStatus::Ready);
    const QImage albedo = CaptureShading(preview, PreviewShading::Albedo);
    INFO(preview.Status().toStdString());
    REQUIRE(albedo.isNull() == false);
    if (capabilities.HasLineFill == false)
    {
        preview.SetShading(PreviewShading::Wireframe);
        CHECK(QTest::qWaitFor([&preview]() { return preview.Status().contains(QStringLiteral("line rasterization")); },
                              PREVIEW_TIMEOUT_MILLISECONDS) == true);
        preview.Stop();
        SKIP("The GPU backend has no line fill; the viewport reports it instead of drawing");
    }
    const QImage wireframe = CaptureShading(preview, PreviewShading::Wireframe);
    INFO(preview.Status().toStdString());
    REQUIRE(wireframe.isNull() == false);
    CHECK(preview.Status().contains(GetPreviewShadingStatus(PreviewShading::Wireframe)) == true);
    const QString shotDirectory = qEnvironmentVariable("LUDUS_VIEWPORT_DEBUG_SHOTS");
    if (shotDirectory.isEmpty() == false)
    {
        CHECK(albedo.save(QStringLiteral("%1/wireframe-albedo.png").arg(shotDirectory)) == true);
        CHECK(wireframe.save(QStringLiteral("%1/wireframe.png").arg(shotDirectory)) == true);
    }
    // The white triangle covers its interior; its wireframe only the three one-pixel edges. The world grid and
    // axes are drawn in both views, so only light, nearly grey pixels count.
    const usize filled = CountLightPixels(albedo);
    const usize edges = CountLightPixels(wireframe);
    CHECK(edges > 0);
    CHECK(edges * 3 < filled);
    // The camera looks down on the triangle at an angle, so its edges show the shared colour partly dimmed.
    CheckWireframeHue(FindBrightestPixel(wireframe));
    preview.Stop();
#else
    SKIP("The GPU wireframe needs the native preview");
#endif
}

TEST_CASE("Software wireframe draws only triangle edges in the shared colour", "[editor][viewport][shading]")
{
    const RenderScene scene = MakeTriangleScene();
    renderer::ViewDescription view;
    // A camera at z = 2 looks down -Z; the triangle spans the middle half of the frame.
    view.CameraPosition = {0, 0, 2};
    view.CameraForward = {0, 0, -1};
    constexpr float32 NEAR_PLANE = .1F;
    view.WorldToClip = math::Matrix4::Identity();
    // Reverse-Z perspective: clip Z = near, W = distance in front of the camera.
    view.WorldToClip.Columns[2] = {0, 0, 0, -1};
    view.WorldToClip.Columns[3] = {0, 0, NEAR_PLANE, 2};
    const uint32 side = static_cast<uint32>(WIREFRAME_FRAME_PIXELS);
    const QImage albedo = RenderSoftwareView(scene, view, side, side, PreviewShading::Albedo);
    const QImage wireframe = RenderSoftwareView(scene, view, side, side, PreviewShading::Wireframe);
    REQUIRE(albedo.isNull() == false);
    REQUIRE(wireframe.isNull() == false);
    const usize filled = CountLightPixels(albedo);
    const usize edges = CountLightPixels(wireframe);
    CHECK(edges > 0);
    CHECK(edges * 3 < filled);
    // The triangle nearly faces the camera, so its edges keep almost the whole shared colour.
    const QRgb brightest = FindBrightestPixel(wireframe);
    CheckWireframeHue(brightest);
    CHECK(qBlue(brightest) >=
          static_cast<int32>(renderer::MATERIAL_DEBUG_WIREFRAME_BLUE) - SOFTWARE_WIREFRAME_TOLERANCE);
}

TEST_CASE("Viewport HUD text lays label: value lines out as a two-column grid", "[editor][viewport][statistics][hud]")
{
    const QFont font = GetViewportHudFont();
    const QFontMetricsF metrics(font);
    CHECK(MeasureViewportHudGrid(QString(), font).isEmpty() == true);
    const QString text = QStringLiteral("GPU: 1.00 ms\nEditor CPU per frame: 2.00 ms\nA line without a value");
    const QSizeF grid = MeasureViewportHudGrid(text, font);
    // Every row is one line spacing tall, so pass bars line up with rows by index.
    CHECK(grid.height() == Catch::Approx(metrics.lineSpacing() * 3));
    const qreal columns = metrics.horizontalAdvance(QStringLiteral("Editor CPU per frame")) +
                          GetViewportHudColumnGap() + metrics.horizontalAdvance(QStringLiteral("2.00 ms"));
    const qreal spanning = metrics.horizontalAdvance(QStringLiteral("A line without a value"));
    CHECK(grid.width() == Catch::Approx(std::max(columns, spanning)));

    // Labels are drawn on the left in the label colour; values are right-aligned in the value colour.
    constexpr int WIDTH = 400;
    const QRgb label = qRgb(255, 0, 0);
    const QRgb value = qRgb(0, 0, 255);
    const int height = static_cast<int>(std::ceil(metrics.lineSpacing()));
    QImage image(WIDTH, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(qRgb(0, 0, 0));
    {
        QPainter painter(&image);
        PaintViewportHudGrid(painter, QRectF(0, 0, WIDTH, height), QStringLiteral("GPU: 8.88 ms"), font, label, value);
    }
    const qreal labelWidth = metrics.horizontalAdvance(QStringLiteral("GPU"));
    const qreal valueWidth = metrics.horizontalAdvance(QStringLiteral("8.88 ms"));
    bool hasLabel = false;
    bool hasValue = false;
    bool hasMiddle = false;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < WIDTH; ++x)
        {
            const QRgb pixel = image.pixel(x, y);
            const bool isLabelInk = qRed(pixel) > 0 && qBlue(pixel) == 0;
            const bool isValueInk = qBlue(pixel) > 0 && qRed(pixel) == 0;
            hasLabel = hasLabel == true || (isLabelInk == true && x < labelWidth + 1);
            hasValue = hasValue == true || (isValueInk == true && x >= WIDTH - valueWidth - 1);
            hasMiddle =
                hasMiddle == true || (x > labelWidth + 1 && x < WIDTH - valueWidth - 1 && pixel != qRgb(0, 0, 0));
        }
    }
    CHECK(hasLabel == true);
    CHECK(hasValue == true);
    CHECK(hasMiddle == false);
}
