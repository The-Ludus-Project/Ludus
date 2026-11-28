#include "internal/builtin_world.h"
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/captured_image_view.h"
#    include "internal/celestial_time_lapse_window.h"
#    include "internal/world_power_budget.h"
#    include <ludus/content/render_graph_format/capture.h>
#endif
#include "internal/light_switches.h"
#include "internal/reference_capture_window.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/world_absorbing_medium.h"
#include "internal/world_document.h"

#include "entropy_fixture.h"
#include "internal/preview_lighting.h"
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_preview.h"
#include "internal/renderer_workspace.h"
#include "internal/viewport_reference_capture.h"
#include "internal/viewport_tone_curve.h"
#include "internal/workspace_style.h"
#include "viewport_fixture.h"
#include <ludus/content/scene_cook/scene_assets.h>

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/ray.h>
#include <ludus/graphics/rhi/rhi.h>

#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/static_lighting.hpp>
#include <ludus/graphics/renderer/tone_mapping.hpp>

#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSize>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QString>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QtEndian>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstring>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace renderer = ludus::graphics::renderer;

namespace
{
/// Small enough that a debug CPU-oracle capture of the lit fixture completes in seconds.
constexpr uint32 TEST_LONG_SIDE = 24;
constexpr uint32 TEST_SAMPLES = 2;
constexpr uint32 TEST_DEPTH = 3;
/// Generous wall-time bound for a CPU-oracle capture of the small fixture on a loaded machine.
constexpr int64 CPU_CAPTURE_TIMEOUT_MILLISECONDS = 120000;
constexpr int64 MILLISECONDS_PER_SECOND = 1000;
/// GPU traversals compile the monolithic reference kernel cold, so they share the renderer-owned cold
/// preparation budget (modules/graphics/renderer/CMakeLists.txt).
constexpr int64 GPU_CAPTURE_TIMEOUT_MILLISECONDS =
    LUDUS_EDITOR_REFERENCE_TEST_COLD_TIMEOUT_SECONDS * MILLISECONDS_PER_SECOND;
constexpr uint64 TEST_SERVICE_BUDGET_NANOSECONDS = 50'000'000;
constexpr usize LUHDR_HEADER_BYTES = 256;
/// Byte offset of the LUHDR01 flags word, and its centre-ray bit (scripts/reference-hdr).
constexpr usize LUHDR_FLAGS_OFFSET = 44;
constexpr usize LUHDR_TARGET_SAMPLES_OFFSET = 32;
constexpr usize LUHDR_SAMPLE_COUNT_OFFSET = 12;
constexpr uint32 LUHDR_PARTIAL_FLAG = 32;
constexpr uint32 LUHDR_CENTER_RAYS_FLAG = 2;
constexpr usize LUHDR_PIXEL_BYTES = 16;

/// One headless device per test; RHI is a process-wide session.
struct DeviceFixture final
{
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    bool IsAvailable = false;
    DeviceFixture()
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Preferred.RayQueries = true;
        rhi::StartupInfo failure;
        IsAvailable = rhi::CreateDevice({}, { .Width = 1, .Height = 1 }, description, Device, Surface, &failure) ==
                      rhi::DeviceStatus::Ready;
        if (IsAvailable == false)
        {
            REQUIRE(failure.Error == rhi::StartupError::AdapterUnavailable);
        }
    }
    ~DeviceFixture()
    {
        if (IsAvailable == true)
        {
            static_cast<void>(rhi::DestroyDevice(Device));
        }
        rhi::Shutdown();
    }
    DeviceFixture(const DeviceFixture&) = delete;
    DeviceFixture& operator=(const DeviceFixture&) = delete;
};

#if defined(LUDUS_EDITOR_REFERENCE_NATIVE_QUERIES)
bool HasNativeRays(rhi::DeviceHandle device)
{
    rhi::RayCapabilities capabilities;
    return rhi::GetRayCapabilities(device, capabilities) == rhi::RasterStatus::Ready && capabilities.MaxRays >= 2;
}
#endif

/// Columns of the folded patch: each sample cube then emits from more planar patches (six faces plus one per
/// column) than the raster light capacity holds across the fixture's cubes, as a curved pbrt bulb does.
constexpr uint32 FOLDED_PATCH_CELLS = 24;
/// Uniform emitted radiance of every surface in MakeManyFacetedEmitterScene.
constexpr float32 FACETED_EMISSION = 2;

void MakeManyFacetedEmitterScene(const QString& root, RenderScene& outScene)
{
    tests::MakeLitScene(root, outScene, { .ExtraPatchCells = FOLDED_PATCH_CELLS, .IsPatchFolded = true });
    for (PreviewSurface& surface : outScene.Surfaces)
    {
        surface.Emission = {FACETED_EMISSION, FACETED_EMISSION, FACETED_EMISSION};
    }
}

void DescribeTestView(const RenderScene& scene, const QSize& extent, renderer::ViewDescription& outView)
{
    PreviewViewOptions options;
    options.Width = static_cast<uint32>(extent.width());
    options.Height = static_cast<uint32>(extent.height());
    QString error;
    const bool isDescribed = scene.View(options, outView, error);
    INFO(error.toStdString());
    REQUIRE(isDescribed == true);
}

/// The wall bound for preparing and tracing a capture of the small fixture with this traversal. Waits take
/// it from the traversal they chose, so a GPU capture never runs under the CPU oracle's bound. Cold GPU
/// captures belong in [reference-compute] cases, which CTest runs with their own deadline.
int64 GetCaptureTimeoutMilliseconds(renderer::ReferenceTraversal traversal)
{
    return traversal == renderer::ReferenceTraversal::CpuOracle ? CPU_CAPTURE_TIMEOUT_MILLISECONDS
                                                                : GPU_CAPTURE_TIMEOUT_MILLISECONDS;
}

/// Choose a traversal as the user does, so a later device change keeps it rather than the default.
void SelectTraversal(ReferenceCaptureWindow& inoutWindow, renderer::ReferenceTraversal traversal)
{
    QComboBox* traversalOrNull = inoutWindow.findChild<QComboBox*>(QStringLiteral("referenceTraversal"));
    REQUIRE(traversalOrNull != nullptr);
    const int32 index = static_cast<int32>(traversal);
    REQUIRE(index < traversalOrNull->count());
    traversalOrNull->setCurrentIndex(index);
    Q_EMIT traversalOrNull->activated(index);
}

/// Wait for the window's capture to finish within the budget of the traversal the window runs, which is GPU
/// compute by default on a capable device.
bool WaitForCapture(const ReferenceCaptureWindow& window, QSignalSpy& inoutFinished)
{
    const QComboBox* traversalOrNull = window.findChild<QComboBox*>(QStringLiteral("referenceTraversal"));
    REQUIRE(traversalOrNull != nullptr);
    const renderer::ReferenceTraversal traversal =
        static_cast<renderer::ReferenceTraversal>(traversalOrNull->currentIndex());
    return inoutFinished.wait(static_cast<int32>(GetCaptureTimeoutMilliseconds(traversal)));
}

ViewportReferencePhase RunToEnd(ViewportReferenceCapture& inoutCapture, int64 timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    ViewportReferencePhase phase = inoutCapture.GetPhase();
    while (phase == ViewportReferencePhase::Preparing || phase == ViewportReferencePhase::Tracing)
    {
        const int64 elapsedMilliseconds = timer.elapsed();
        if (elapsedMilliseconds >= timeoutMilliseconds)
        {
            WARN("Reference capture was still "
                 << (phase == ViewportReferencePhase::Preparing ? "preparing" : "tracing") << " after "
                 << elapsedMilliseconds << " ms (budget " << timeoutMilliseconds << " ms)");
            break;
        }
        QThread::msleep(1);
        phase = inoutCapture.Service(TEST_SERVICE_BUDGET_NANOSECONDS);
    }
    return phase;
}

bool HasLitPixel(const QImage& image)
{
    for (int32 y = 0; y < image.height(); ++y)
    {
        for (int32 x = 0; x < image.width(); ++x)
        {
            const QRgb pixel = image.pixel(x, y);
            if (qRed(pixel) != 0 || qGreen(pixel) != 0 || qBlue(pixel) != 0)
            {
                return true;
            }
        }
    }
    return false;
}

void CheckCompleteCapture(const ViewportReferenceCapture& capture, const QSize& extent)
{
    renderer::ReferenceCaptureInfo info;
    REQUIRE(capture.GetInfo(info) == true);
    CHECK(info.Complete == true);
    CHECK(info.InvalidContributions == 0);
    CHECK(info.MinimumSamples == TEST_SAMPLES);
    CHECK(info.MaximumSamples == TEST_SAMPLES);
    CHECK(info.Width == static_cast<uint32>(extent.width()));
    CHECK(info.Height == static_cast<uint32>(extent.height()));
    CHECK(info.MaxDepth == TEST_DEPTH);
    QString error;
    QImage image;
    REQUIRE(capture.CopyDisplayImage(image, error) == true);
    CHECK(image.size() == extent);
    CHECK(HasLitPixel(image) == true);
}
} // namespace

TEST_CASE("Reference extent keeps the viewport aspect on the requested long side", "[editor][reference-capture]")
{
    CHECK(ComputeReferenceExtent(QSize(1600, 900), 512) == QSize(512, 288));
    CHECK(ComputeReferenceExtent(QSize(900, 1600), 512) == QSize(288, 512));
    CHECK(ComputeReferenceExtent(QSize(640, 640), 100) == QSize(100, 100));
    // An extreme aspect still keeps at least one pixel on the short side.
    CHECK(ComputeReferenceExtent(QSize(10000, 1), 16) == QSize(16, 1));
    CHECK(ComputeReferenceExtent(QSize(0, 480), 512).isEmpty() == true);
    CHECK(ComputeReferenceExtent(QSize(640, 480), 0).isEmpty() == true);
}

TEST_CASE("Reference display encoding matches the viewport's exposure-one PBR Neutral sRGB output",
          "[editor][reference-capture]")
{
    CHECK(EncodeReferenceDisplayChannel(0) == 0);
    // PBR Neutral leaves a grey of 0.3 at 0.26 once its 0.04 offset is removed, which sRGB encodes as 139 of 255.
    CHECK(EncodeReferenceDisplayChannel(0.3F) == 139);
    // Grey 1 compresses to 0.8691 (0.96 peak, 0.44 divisor), which sRGB encodes as 240 of 255.
    CHECK(EncodeReferenceDisplayChannel(1) == 240);
    CHECK(EncodeReferenceDisplayChannel(1.0e30F) == 255);
    CHECK(EncodeReferenceDisplayChannel(-1) == 0);
    CHECK(EncodeReferenceDisplayChannel(__builtin_nanf("")) == 0);
}

TEST_CASE("Reference display transform follows the viewport output's ToneMap exposure and curve",
          "[editor][reference-capture][exposure]")
{
    renderer::ViewDescription view;
    // Without a ToneMap the view's exposure fields do not apply: the viewport's exposure-one transform.
    view.Exposure = renderer::ExposureMode::Physical;
    view.ExposureValue100 = 12;
    ViewportDisplayTransform display = ComputeReferenceDisplayTransform(view, 0, renderer::ToneCurve::Reinhard);
    CHECK(display.Exposure == 1);
    CHECK(display.Curve == VIEWPORT_TONE_CURVE);
    // Graph exposure keeps the authored multiplier and curve, and compensation doubles it per stop.
    constexpr float32 AUTHORED_EXPOSURE = 2;
    view.Exposure = renderer::ExposureMode::Graph;
    view.RasterExposureStops = 1;
    display = ComputeReferenceDisplayTransform(view, AUTHORED_EXPOSURE, renderer::ToneCurve::Reinhard);
    CHECK(display.Exposure == Catch::Approx(4));
    CHECK(display.Curve == renderer::ToneCurve::Reinhard);
    // A physical camera replaces the authored multiplier, as RenderGraphDeferredPass does.
    view.Exposure = renderer::ExposureMode::Physical;
    view.RasterExposureStops = -1;
    display = ComputeReferenceDisplayTransform(view, AUTHORED_EXPOSURE, renderer::ToneCurve::PbrNeutral);
    CHECK(display.Exposure == Catch::Approx(renderer::ComputeExposureMultiplier(12) / 2));
    CHECK(display.Curve == renderer::ToneCurve::PbrNeutral);
    // Exposure fields RenderGraph rejects fall back to the default transform instead of an unusable exposure.
    view.ExposureValue100 = renderer::MAX_EXPOSURE_VALUE_100 + 1;
    CHECK(ComputeReferenceDisplayTransform(view, AUTHORED_EXPOSURE, renderer::ToneCurve::PbrNeutral).Exposure == 1);
    view.ExposureValue100 = 12;
    view.RasterExposureStops = __builtin_nanf("");
    CHECK(ComputeReferenceDisplayTransform(view, AUTHORED_EXPOSURE, renderer::ToneCurve::PbrNeutral).Exposure == 1);
}

TEST_CASE("Reference display encoding applies the supplied exposure and curve", "[editor][reference-capture][exposure]")
{
    // Daylight radiance in physical units saturates at exposure one but encodes like 0.3 once exposed.
    constexpr float32 DAYLIGHT_RADIANCE = 3000;
    constexpr float32 EXPOSED_GREY = 0.3F;
    const ViewportDisplayTransform exposed{ .Exposure = EXPOSED_GREY / DAYLIGHT_RADIANCE };
    // PBR Neutral approaches white asymptotically; saturated means within half an eight-bit level of it.
    constexpr float64 HALF_EIGHT_BIT_LEVEL = 0.5 / 255;
    CHECK(EncodeReferenceDisplayColor({DAYLIGHT_RADIANCE, DAYLIGHT_RADIANCE, DAYLIGHT_RADIANCE}).X ==
          Catch::Approx(1).margin(HALF_EIGHT_BIT_LEVEL));
    CHECK(EncodeReferenceDisplayColor({DAYLIGHT_RADIANCE, DAYLIGHT_RADIANCE, DAYLIGHT_RADIANCE}, exposed).X ==
          Catch::Approx(EncodeReferenceDisplayValue(EXPOSED_GREY)));
    // Reinhard maps 0.3 to 0.3 / 1.3 before sRGB, unlike PBR Neutral.
    const ViewportDisplayTransform reinhard{ .Curve = renderer::ToneCurve::Reinhard };
    CHECK(EncodeReferenceDisplayColor({EXPOSED_GREY, EXPOSED_GREY, EXPOSED_GREY}, reinhard).X ==
          Catch::Approx(math::EncodeSrgb(EXPOSED_GREY / (1 + EXPOSED_GREY))));
    // An unusable exposure encodes black rather than an arbitrary colour.
    CHECK(EncodeReferenceDisplayColor({1, 1, 1}, { .Exposure = 0 }).X == 0);
    CHECK(EncodeReferenceDisplayColor({1, 1, 1}, { .Exposure = __builtin_nanf("") }).X == 0);
}

TEST_CASE("Reference lights exclude the raster viewport's emissive area proxies", "[editor][reference-capture]")
{
    renderer::LightSelection selection;
    const renderer::LightKind kinds[] = {renderer::LightKind::Point,
                                         renderer::LightKind::AreaOneSided,
                                         renderer::LightKind::Directional,
                                         renderer::LightKind::AreaTwoSided,
                                         renderer::LightKind::Spot};
    for (const renderer::LightKind kind : kinds)
    {
        renderer::DirectLight& light = selection.Lights[selection.Count];
        light.Kind = kind;
        light.SourceId = selection.Count + 1;
        ++selection.Count;
    }
    ReferenceLightArray lights{};
    REQUIRE(CollectReferenceLights(selection, lights) == 3);
    CHECK(lights[0].Kind == renderer::LightKind::Point);
    CHECK(lights[0].SourceId == 1);
    CHECK(lights[1].Kind == renderer::LightKind::Directional);
    CHECK(lights[1].SourceId == 3);
    CHECK(lights[2].Kind == renderer::LightKind::Spot);
    CHECK(lights[2].SourceId == 5);
}

TEST_CASE("Reference lighting inputs skip the raster area proxies of many-faceted emitters",
          "[editor][reference-capture]")
{
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    MakeManyFacetedEmitterScene(root.path(), scene);
    const QByteArray key = preview_lighting::MakeKey(scene);
    // Raster shading needs one proxy light per planar patch and cannot hold them all.
    preview_lighting::Inputs raster;
    CHECK(preview_lighting::BuildInputs(scene, key, raster) == rhi::RasterStatus::CapacityExceeded);
    // Reference transport samples the emissive triangles itself, so the proxy capacity does not apply.
    preview_lighting::Inputs reference;
    REQUIRE(preview_lighting::BuildInputs(scene, key, reference, preview_lighting::InputsTarget::Reference) ==
            rhi::RasterStatus::Ready);
    CHECK(reference.Lights.Count == static_cast<uint32>(tests::LAMP_COUNT));
    CHECK(reference.AreaCount == 0);
    CHECK(reference.ShadowCount == 0);
    REQUIRE(reference.Surfaces.GetSize() == static_cast<usize>(scene.Objects.size()));
    const math::Vector3 emission{FACETED_EMISSION, FACETED_EMISSION, FACETED_EMISSION};
    for (const renderer::StaticLightingSurfaceDescription& surface : reference.Surfaces)
    {
        CHECK(surface.Emission == emission);
    }
}

TEST_CASE("Reference capture rejects unlit scenes and out-of-range settings before using a device",
          "[editor][reference-capture]")
{
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene unlit;
    tests::MakeLitScene(root.path(), unlit, { .HasLamps = false });
    const QSize extent(TEST_LONG_SIDE, TEST_LONG_SIDE);
    renderer::ViewDescription view;
    DescribeTestView(unlit, extent, view);
    ViewportReferenceCapture capture;
    QString error;
    ViewportReferenceSettings settings;
    settings.LongSide = TEST_LONG_SIDE;
    CHECK(capture.Start({}, {}, unlit, view, extent, settings, error) == false);
    CHECK(error.contains(QStringLiteral("lit scene")) == true);
    CHECK(capture.GetPhase() == ViewportReferencePhase::Idle);

    RenderScene lit;
    QTemporaryDir litRoot;
    REQUIRE(litRoot.isValid() == true);
    tests::MakeLitScene(litRoot.path(), lit);
    settings.Samples = 0;
    error.clear();
    CHECK(capture.Start({}, {}, lit, view, extent, settings, error) == false);
    CHECK(error.contains(QStringLiteral("admitted ranges")) == true);
    settings.Samples = 1;
    settings.MaxDepth = MAXIMUM_REFERENCE_DEPTH + 1;
    error.clear();
    CHECK(capture.Start({}, {}, lit, view, extent, settings, error) == false);
    settings.MaxDepth = 1;
    settings.LongSide = MAXIMUM_REFERENCE_LONG_SIDE + 1;
    error.clear();
    CHECK(capture.Start({}, {}, lit, view, extent, settings, error) == false);
    settings.LongSide = TEST_LONG_SIDE;
    settings.Display.Exposure = 0;
    error.clear();
    CHECK(capture.Start({}, {}, lit, view, extent, settings, error) == false);
    CHECK(error.contains(QStringLiteral("admitted ranges")) == true);
    CHECK(capture.GetPhase() == ViewportReferencePhase::Idle);
}

TEST_CASE("CPU oracle reference capture of the viewport scene completes and saves LUHDR and PNG",
          "[editor][reference-capture][gpu]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    // The collapsed triangle would make RT1 admission return Unsupported if it reached the capture.
    tests::MakeLitScene(root.path(), scene, { .HasCollapsedTriangle = true });
    const QSize extent = ComputeReferenceExtent(QSize(640, 480), TEST_LONG_SIDE);
    renderer::ViewDescription view;
    DescribeTestView(scene, extent, view);
    ViewportReferenceSettings settings;
    settings.LongSide = TEST_LONG_SIDE;
    settings.Samples = TEST_SAMPLES;
    settings.MaxDepth = TEST_DEPTH;
    settings.Traversal = renderer::ReferenceTraversal::CpuOracle;
    ViewportReferenceCapture capture;
    QString error;
    REQUIRE(capture.Start(device.Device, device.Surface, scene, view, extent, settings, error) == true);
    CHECK(capture.GetPhase() == ViewportReferencePhase::Preparing);
    const ViewportReferencePhase phase = RunToEnd(capture, GetCaptureTimeoutMilliseconds(settings.Traversal));
    INFO(capture.GetError().toStdString());
    REQUIRE(phase == ViewportReferencePhase::Complete);
    // Only the authored point lamps reach RT1; the fixture's cubes do not emit.
    CHECK(capture.GetLightCount() == static_cast<usize>(tests::LAMP_COUNT));
    CheckCompleteCapture(capture, extent);

    QByteArray hdr;
    REQUIRE(capture.EncodeHdr(hdr, error) == true);
    REQUIRE(static_cast<usize>(hdr.size()) ==
            LUHDR_HEADER_BYTES + static_cast<usize>(extent.width()) * extent.height() * LUHDR_PIXEL_BYTES);
    CHECK(hdr.startsWith(QByteArray("LUHDR01\0", 8)) == true);

    const QString path = QDir(root.path()).filePath(QStringLiteral("capture.luhdr"));
    REQUIRE(capture.Save(path, error) == true);
    QFile saved(path);
    REQUIRE(saved.open(QIODevice::ReadOnly) == true);
    CHECK(saved.readAll() == hdr);
    const QImage png(ViewportReferenceCapture::GetDisplayImagePath(path));
    CHECK(png.size() == extent);
    CHECK(ViewportReferenceCapture::GetDisplayImagePath(path).endsWith(QStringLiteral("capture.png")) == true);

    capture.Release();
    CHECK(capture.GetPhase() == ViewportReferencePhase::Idle);
    CHECK(capture.EncodeHdr(hdr, error) == false);
}

TEST_CASE("A reference capture traces pixel centres when asked and records it in the LUHDR flags",
          "[editor][reference-capture][gpu]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    tests::MakeLitScene(root.path(), scene);
    const QSize extent = ComputeReferenceExtent(QSize(640, 480), TEST_LONG_SIDE);
    renderer::ViewDescription view;
    DescribeTestView(scene, extent, view);
    ViewportReferenceSettings settings;
    settings.LongSide = TEST_LONG_SIDE;
    settings.Samples = TEST_SAMPLES;
    settings.MaxDepth = TEST_DEPTH;
    settings.Traversal = renderer::ReferenceTraversal::CpuOracle;
    for (const bool shouldCenterRays : {false, true})
    {
        INFO("Centre rays: " << shouldCenterRays);
        settings.ShouldCenterRays = shouldCenterRays;
        ViewportReferenceCapture capture;
        QString error;
        REQUIRE(capture.Start(device.Device, device.Surface, scene, view, extent, settings, error) == true);
        CHECK(capture.AreRaysCentered() == shouldCenterRays);
        const ViewportReferencePhase phase = RunToEnd(capture, GetCaptureTimeoutMilliseconds(settings.Traversal));
        INFO(capture.GetError().toStdString());
        REQUIRE(phase == ViewportReferencePhase::Complete);
        QByteArray hdr;
        REQUIRE(capture.EncodeHdr(hdr, error) == true);
        REQUIRE(static_cast<usize>(hdr.size()) >= LUHDR_HEADER_BYTES);
        const uint32 flags = qFromLittleEndian<uint32>(hdr.constData() + LUHDR_FLAGS_OFFSET);
        CHECK(((flags & LUHDR_CENTER_RAYS_FLAG) != 0) == shouldCenterRays);
    }
}

TEST_CASE("Reference capture lights the scene with the World Atmosphere's sky as the raster does",
          "[editor][reference-capture][gpu][atmosphere]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    tests::MakeLitScene(root.path(), scene);
    const QSize extent = ComputeReferenceExtent(QSize(640, 480), TEST_LONG_SIDE);
    renderer::ViewDescription view;
    DescribeTestView(scene, extent, view);
    ViewportReferenceSettings settings;
    settings.LongSide = TEST_LONG_SIDE;
    settings.Samples = TEST_SAMPLES;
    settings.MaxDepth = TEST_DEPTH;
    settings.Traversal = renderer::ReferenceTraversal::CpuOracle;
    const auto captureRadiance = [&](const RenderScene& source, bool& outHasSky) {
        ViewportReferenceCapture capture;
        QString error;
        REQUIRE(capture.Start(device.Device, device.Surface, source, view, extent, settings, error) == true);
        const ViewportReferencePhase phase = RunToEnd(capture, GetCaptureTimeoutMilliseconds(settings.Traversal));
        INFO(capture.GetError().toStdString());
        REQUIRE(phase == ViewportReferencePhase::Complete);
        Array<renderer::ReferencePixel> pixels;
        REQUIRE(capture.CopyPixels(pixels, error) == true);
        float64 total = 0;
        for (const renderer::ReferencePixel& pixel : pixels)
        {
            total += static_cast<float64>(pixel.Radiance.X + pixel.Radiance.Y + pixel.Radiance.Z);
        }
        outHasSky = capture.HasAtmosphereSky();
        return total;
    };
    bool hasSky = true;
    const float64 withoutAtmosphere = captureRadiance(scene, hasSky);
    CHECK(hasSky == false);

    // A clear sky with its ground ten metres below the camera and a Sun linked to no scene light.
    constexpr float32 ANCHOR_DEPTH_METRES = 10;
    constexpr float32 SOLAR_ILLUMINANCE = 4;
    RenderScene sky = scene;
    sky.Atmosphere.IsEnabled = true;
    sky.Atmosphere.ShouldLightScene = true;
    sky.Atmosphere.SurfaceAnchorWorld = view.CameraPosition - math::Vector3{0, ANCHOR_DEPTH_METRES, 0};
    sky.Atmosphere.DirectionToSunWorld = math::Vector3{.3F, .8F, .2F} / math::Length(math::Vector3{.3F, .8F, .2F});
    sky.Atmosphere.SolarIlluminance = {SOLAR_ILLUMINANCE, SOLAR_ILLUMINANCE, SOLAR_ILLUMINANCE};
    const float64 withAtmosphere = captureRadiance(sky, hasSky);
    CHECK(hasSky == true);
    CAPTURE(withoutAtmosphere, withAtmosphere);
    CHECK(withAtmosphere > withoutAtmosphere);

    // Below the Atmosphere's ground the raster adds no sky, and neither does the reference.
    RenderScene buried = sky;
    buried.Atmosphere.SurfaceAnchorWorld = view.CameraPosition + math::Vector3{0, ANCHOR_DEPTH_METRES, 0};
    const float64 belowGround = captureRadiance(buried, hasSky);
    CHECK(hasSky == false);
    CHECK(belowGround == withoutAtmosphere);
}

TEST_CASE("GPU compute reference capture completes with more emissive patches than raster lights",
          "[editor][reference-capture][gpu][reference-compute][emissive-patches]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    MakeManyFacetedEmitterScene(root.path(), scene);
    const QSize extent = ComputeReferenceExtent(QSize(640, 480), TEST_LONG_SIDE);
    renderer::ViewDescription view;
    DescribeTestView(scene, extent, view);
    ViewportReferenceSettings settings;
    settings.LongSide = TEST_LONG_SIDE;
    settings.Samples = TEST_SAMPLES;
    settings.MaxDepth = TEST_DEPTH;
    settings.Traversal = renderer::ReferenceTraversal::GpuCompute;
    ViewportReferenceCapture capture;
    QString error;
    REQUIRE(capture.Start(device.Device, device.Surface, scene, view, extent, settings, error) == true);
    const ViewportReferencePhase phase = RunToEnd(capture, GetCaptureTimeoutMilliseconds(settings.Traversal));
    INFO(capture.GetError().toStdString());
    REQUIRE(phase == ViewportReferencePhase::Complete);
    CHECK(capture.GetLightCount() == static_cast<usize>(tests::LAMP_COUNT));
    CheckCompleteCapture(capture, extent);
}

TEST_CASE("Native query reference capture completes where the device supports ray queries",
          "[editor][reference-capture][gpu]")
{
#if defined(LUDUS_EDITOR_REFERENCE_NATIVE_QUERIES)
    DeviceFixture device;
    if (device.IsAvailable == false || HasNativeRays(device.Device) == false)
    {
        SKIP("Native ray queries are unavailable on this host");
    }
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    tests::MakeLitScene(root.path(), scene, { .HasCollapsedTriangle = true });
    const QSize extent = ComputeReferenceExtent(QSize(640, 480), TEST_LONG_SIDE);
    renderer::ViewDescription view;
    DescribeTestView(scene, extent, view);
    ViewportReferenceSettings settings;
    settings.LongSide = TEST_LONG_SIDE;
    settings.Samples = TEST_SAMPLES;
    settings.MaxDepth = TEST_DEPTH;
    settings.Traversal = renderer::ReferenceTraversal::NativeQueries;
    ViewportReferenceCapture capture;
    QString error;
    REQUIRE(capture.Start(device.Device, device.Surface, scene, view, extent, settings, error) == true);
    const ViewportReferencePhase phase = RunToEnd(capture, GetCaptureTimeoutMilliseconds(settings.Traversal));
    INFO(capture.GetError().toStdString());
    REQUIRE(phase == ViewportReferencePhase::Complete);
    CheckCompleteCapture(capture, extent);
    renderer::ReferenceCaptureInfo info;
    REQUIRE(capture.GetInfo(info) == true);
    CHECK(info.Traversal == renderer::ReferenceTraversal::NativeQueries);
    CHECK(info.ClosestQueries != 0);
#else
    SKIP("This build has no native ray query shader");
#endif
}

TEST_CASE("GPU compute reference capture matches the CPU oracle on the viewport scene",
          "[editor][reference-capture][gpu][reference-compute][oracle-parity]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    tests::SequentialEntropy entropy;
    // Source IDs determine normalized light ordering. Freeze them so matched random
    // addresses select the same physical lights on every run of this parity test.
    tests::MakeLitScene(root.path(),
                        scene,
                        { .HasCollapsedTriangle = true, .Entropy = tests::MakeSequentialEntropySource(entropy) });
    const QSize extent = ComputeReferenceExtent(QSize(640, 480), TEST_LONG_SIDE);
    renderer::ViewDescription view;
    DescribeTestView(scene, extent, view);
    ViewportReferenceSettings settings;
    settings.LongSide = TEST_LONG_SIDE;
    settings.Samples = TEST_SAMPLES;
    settings.MaxDepth = TEST_DEPTH;
    Array<renderer::ReferencePixel> pixels[2];
    const renderer::ReferenceTraversal traversals[]{renderer::ReferenceTraversal::CpuOracle,
                                                    renderer::ReferenceTraversal::GpuCompute};
    for (usize index = 0; index < 2; ++index)
    {
        settings.Traversal = traversals[index];
        ViewportReferenceCapture capture;
        QString error;
        REQUIRE(capture.Start(device.Device, device.Surface, scene, view, extent, settings, error) == true);
        const ViewportReferencePhase phase = RunToEnd(capture, GetCaptureTimeoutMilliseconds(settings.Traversal));
        INFO(capture.GetError().toStdString());
        REQUIRE(phase == ViewportReferencePhase::Complete);
        CheckCompleteCapture(capture, extent);
        renderer::ReferenceCaptureInfo info;
        REQUIRE(capture.GetInfo(info) == true);
        CHECK(info.Traversal == traversals[index]);
        REQUIRE(capture.CopyPixels(pixels[index], error) == true);
    }
    // Both traversals read the same addressed samples; the device's float32 arithmetic
    // keeps every raw mean within a small absolute RGB distance of the oracle.
    constexpr float32 MAXIMUM_DISTANCE = 2e-3F;
    REQUIRE(pixels[0].GetSize() == pixels[1].GetSize());
    float32 largest = 0;
    usize largestIndex = 0;
    usize divergentPixels = 0;
    for (usize i = 0; i < pixels[0].GetSize(); ++i)
    {
        CHECK(pixels[1][i].Samples == pixels[0][i].Samples);
        const float32 distance = math::Distance(pixels[0][i].Radiance, pixels[1][i].Radiance);
        if (distance > MAXIMUM_DISTANCE)
        {
            ++divergentPixels;
        }
        if (distance > largest)
        {
            largest = distance;
            largestIndex = i;
        }
    }
    // Locate and decompose the worst pixel, so a failure on one device says whether a few
    // edge pixels or the whole image diverge, and in which transport term.
    const usize width = static_cast<usize>(extent.width());
    const renderer::ReferencePixel& oracle = pixels[0][largestIndex];
    const renderer::ReferencePixel& compute = pixels[1][largestIndex];
    INFO("divergent pixels " << divergentPixels << " of " << pixels[0].GetSize() << "; worst at x "
                             << largestIndex % width << " y " << largestIndex / width);
    INFO("oracle emission " << oracle.Emission.X << " " << oracle.Emission.Y << " " << oracle.Emission.Z << " direct "
                            << oracle.Direct.X << " " << oracle.Direct.Y << " " << oracle.Direct.Z << " indirect "
                            << oracle.Indirect.X << " " << oracle.Indirect.Y << " " << oracle.Indirect.Z);
    INFO("compute emission " << compute.Emission.X << " " << compute.Emission.Y << " " << compute.Emission.Z
                             << " direct " << compute.Direct.X << " " << compute.Direct.Y << " " << compute.Direct.Z
                             << " indirect " << compute.Indirect.X << " " << compute.Indirect.Y << " "
                             << compute.Indirect.Z);
    CAPTURE(largest);
    CHECK(largest <= MAXIMUM_DISTANCE);
}

namespace
{
// The installed-SDK reference-scene sample's absorbing-medium fixture (examples/reference-scene/tests/
// check_medium.py): a unit box of absorber centred at the origin, seen from (0, 0, -3) along +Z with a 0.5 rad
// vertical field of view. The World here authors the same box and coefficients with World commands; a lit plane
// behind it replaces the sample's emitter, because a default World authors no emissive surface.
constexpr float32 MEDIUM_CAMERA_DISTANCE = 3;
constexpr float32 MEDIUM_VERTICAL_FOV_RADIANS = 0.5F;
constexpr float32 MEDIUM_NEAR = 0.05F;
constexpr float32 MEDIUM_FAR = 100;
/// Sample absorption per metre, and the length of the box along the camera axis.
const math::Vector3 SAMPLE_ABSORPTION{0.5F, 1, 2};
constexpr float32 SAMPLE_BOX_LENGTH = 1;
/// An odd image side gives a centre pixel whose centre ray runs along the box axis.
constexpr uint32 MEDIUM_LONG_SIDE = 25;
/// The sample's Beer-Lambert tolerances: relative slack at the centre, and float32 rounding elsewhere.
constexpr float32 SAMPLE_CENTER_TOLERANCE = 2e-3F;
constexpr float32 SAMPLE_UNATTENUATED_TOLERANCE = 1e-6F;
/// Relative CPU/GPU distance of raw means on the medium scene; the GPU repeats the oracle's float32 order.
constexpr float32 MEDIUM_PARITY_TOLERANCE = 1e-4F;

/// A default World holding a plane facing the camera 2.5 m behind the box, a directional light whose shadow rays
/// miss the box, and the Absorbing Medium preset with the sample's coefficients. outMediumKey names the medium.
WorldDocument MakeMediumWorld(QString& outMediumKey)
{
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument world;
    REQUIRE(world.LoadSchema(schema, error) == true);
    REQUIRE(world.New(error) == true);
    tests::SequentialEntropy entropy;
    const auto create = [&world, &entropy](const QString& preset, const QJsonObject& transform) {
        QString key;
        QString createError;
        INFO(createError.toStdString());
        REQUIRE(CreateWorldObject(world, tests::MakeSequentialEntropySource(entropy), preset, key, createError) ==
                true);
        QJsonObject components = world.Entity(key).value(QStringLiteral("components")).toObject();
        QJsonObject values = components.value(QStringLiteral("transform")).toObject();
        for (QJsonObject::const_iterator value = transform.constBegin(); value != transform.constEnd(); ++value)
        {
            values.insert(value.key(), value.value());
        }
        components.insert(QStringLiteral("transform"), values);
        REQUIRE(world.Edit(key, components, world.Entity(key).value(QStringLiteral("name")).toString(), createError) ==
                true);
        return key;
    };
    // The plane mesh lies in XZ facing +Y; turned -90 degrees about X it lies in XY facing -Z.
    static_cast<void>(create(QStringLiteral("Plane"),
                             {{QStringLiteral("z"), 2.5},
                              {QStringLiteral("rx"), -90},
                              {QStringLiteral("sx"), 6},
                              {QStringLiteral("sz"), 6}}));
    // Light travels along (-0.866, 0, 0.5): the plane's front sees it at 60 degrees and its shadow rays leave the
    // visible plane toward +X, away from the box.
    static_cast<void>(create(QStringLiteral("Directional Light"), {{QStringLiteral("ry"), 120}}));
    outMediumKey = create(QStringLiteral("Absorbing Medium"), {});
    QJsonObject components = world.Entity(outMediumKey).value(QStringLiteral("components")).toObject();
    QJsonObject medium = components.value(GetAbsorbingMediumComponentId()).toObject();
    REQUIRE(medium.value(QStringLiteral("absorptionR")).toDouble() == static_cast<float64>(SAMPLE_ABSORPTION.X));
    REQUIRE(medium.value(QStringLiteral("absorptionG")).toDouble() == static_cast<float64>(SAMPLE_ABSORPTION.Y));
    REQUIRE(medium.value(QStringLiteral("absorptionB")).toDouble() == static_cast<float64>(SAMPLE_ABSORPTION.Z));
    return world;
}

void SetMediumAbsorption(WorldDocument& inoutWorld, const QString& key, math::Vector3 absorption)
{
    QJsonObject components = inoutWorld.Entity(key).value(QStringLiteral("components")).toObject();
    QJsonObject medium = components.value(GetAbsorbingMediumComponentId()).toObject();
    medium.insert(QStringLiteral("absorptionR"), static_cast<float64>(absorption.X));
    medium.insert(QStringLiteral("absorptionG"), static_cast<float64>(absorption.Y));
    medium.insert(QStringLiteral("absorptionB"), static_cast<float64>(absorption.Z));
    components.insert(GetAbsorbingMediumComponentId(), medium);
    QString error;
    REQUIRE(inoutWorld.Edit(key, components, inoutWorld.Entity(key).value(QStringLiteral("name")).toString(), error) ==
            true);
}

RenderScene LoadMediumScene(const WorldDocument& world)
{
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    QString error;
    const bool isLoaded = RenderScene::Load(root.path(), world, scene, error);
    INFO(error.toStdString());
    REQUIRE(isLoaded == true);
    REQUIRE(scene.Lit == true);
    REQUIRE(scene.Medium.IsPresent() == true);
    return scene;
}

/// The sample's pinhole camera at the reference extent.
renderer::ViewDescription DescribeMediumView(const RenderScene& scene, const QSize& extent)
{
    renderer::ViewDescription view;
    DescribeTestView(scene, extent, view);
    math::Matrix4 worldToView;
    math::Matrix4 viewToClip;
    REQUIRE(math::TryLookAt({0, 0, -MEDIUM_CAMERA_DISTANCE}, {0, 0, 0}, {0, 1, 0}, worldToView) ==
            math::MathStatus::Success);
    REQUIRE(math::TryPerspectiveReverseZ(MEDIUM_VERTICAL_FOV_RADIANS, 1, MEDIUM_NEAR, MEDIUM_FAR, viewToClip) ==
            math::MathStatus::Success);
    view.WorldToClip = viewToClip * worldToView;
    view.CameraPosition = {0, 0, -MEDIUM_CAMERA_DISTANCE};
    view.OrthographicCamera = false;
    view.Depth = renderer::DepthConvention::ReverseZ;
    view.InfiniteFar = false;
    return view;
}

ViewportReferenceSettings MakeMediumSettings(renderer::ReferenceTraversal traversal)
{
    ViewportReferenceSettings settings;
    settings.LongSide = MEDIUM_LONG_SIDE;
    settings.Samples = TEST_SAMPLES;
    // Direct light only: every camera path then crosses the box once and its light paths miss it.
    settings.MaxDepth = 1;
    settings.ShouldCenterRays = true;
    settings.Traversal = traversal;
    return settings;
}

bool StartMediumCapture(ViewportReferenceCapture& inoutCapture,
                        const DeviceFixture& device,
                        const RenderScene& scene,
                        renderer::ReferenceTraversal traversal,
                        QString& outError)
{
    const QSize extent(static_cast<int32>(MEDIUM_LONG_SIDE), static_cast<int32>(MEDIUM_LONG_SIDE));
    return inoutCapture.Start(device.Device,
                              device.Surface,
                              scene,
                              DescribeMediumView(scene, extent),
                              extent,
                              MakeMediumSettings(traversal),
                              outError);
}

Array<renderer::ReferencePixel> FinishMediumCapture(ViewportReferenceCapture& inoutCapture,
                                                    renderer::ReferenceTraversal traversal)
{
    const ViewportReferencePhase phase = RunToEnd(inoutCapture, GetCaptureTimeoutMilliseconds(traversal));
    INFO(inoutCapture.GetError().toStdString());
    REQUIRE(phase == ViewportReferencePhase::Complete);
    renderer::ReferenceCaptureInfo info;
    REQUIRE(inoutCapture.GetInfo(info) == true);
    CHECK(info.Traversal == traversal);
    CHECK(info.InvalidContributions == 0);
    Array<renderer::ReferencePixel> pixels;
    QString error;
    REQUIRE(inoutCapture.CopyPixels(pixels, error) == true);
    REQUIRE(pixels.GetSize() == static_cast<usize>(MEDIUM_LONG_SIDE) * MEDIUM_LONG_SIDE);
    return pixels;
}

float32 GetChannel(math::Vector3 value, usize channel)
{
    return channel == 0 ? value.X : (channel == 1 ? value.Y : value.Z);
}

/// The sample's acceptance: the centre pixel keeps exp(-sigma_a * length) of the vacuum image and the corners,
/// whose rays miss the box, keep everything.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the medium capture, then its vacuum reference
void CheckBeerLambert(const Array<renderer::ReferencePixel>& medium,
                      const Array<renderer::ReferencePixel>& vacuum,
                      math::Vector3 absorption)
{
    const usize side = MEDIUM_LONG_SIDE;
    const usize centre = (side / 2) * side + side / 2;
    for (usize channel = 0; channel < 3; ++channel)
    {
        const float32 unattenuated = GetChannel(vacuum[centre].Radiance, channel);
        REQUIRE(unattenuated > 0);
        const float32 expected =
            unattenuated *
            static_cast<float32>(math::Exp(-static_cast<float64>(GetChannel(absorption, channel) * SAMPLE_BOX_LENGTH)));
        const float32 measured = GetChannel(medium[centre].Radiance, channel);
        CAPTURE(channel, measured, expected);
        CHECK(math::Abs(measured - expected) <= SAMPLE_CENTER_TOLERANCE * expected);
    }
    for (const usize corner : {usize{0}, side - 1, side * (side - 1), side * side - 1})
    {
        for (usize channel = 0; channel < 3; ++channel)
        {
            const float32 unattenuated = GetChannel(vacuum[corner].Radiance, channel);
            CAPTURE(corner, channel);
            REQUIRE(unattenuated > 0);
            CHECK(math::Abs(GetChannel(medium[corner].Radiance, channel) - unattenuated) <=
                  SAMPLE_UNATTENUATED_TOLERANCE * unattenuated);
        }
    }
}
} // namespace

TEST_CASE("A World's absorbing medium attenuates the RT1 capture like the installed-SDK sample's fixture",
          "[editor][reference-capture][gpu][absorbing-medium]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    QString key;
    WorldDocument world = MakeMediumWorld(key);
    RenderScene scene = LoadMediumScene(world);
    // The medium is not geometry; the plane is the only object raster and RT1 see.
    REQUIRE(scene.Objects.size() == 1);
    const renderer::ReferenceTraversal traversal = renderer::ReferenceTraversal::CpuOracle;
    ViewportReferenceCapture inFlight;
    QString error;
    REQUIRE(StartMediumCapture(inFlight, device, scene, traversal, error) == true);

    // The author makes the box a vacuum while the capture prepares. The workspace publishes the new extraction in
    // place of the old one; the started capture keeps the scene it froze.
    SetMediumAbsorption(world, key, {0, 0, 0});
    scene = LoadMediumScene(world);
    CHECK(scene.Medium.AbsorptionPerWorld == math::Vector3{0, 0, 0});
    const Array<renderer::ReferencePixel> ink = FinishMediumCapture(inFlight, traversal);

    // The accepted edit appears in the next capture.
    ViewportReferenceCapture next;
    REQUIRE(StartMediumCapture(next, device, scene, traversal, error) == true);
    const Array<renderer::ReferencePixel> vacuum = FinishMediumCapture(next, traversal);
    CheckBeerLambert(ink, vacuum, SAMPLE_ABSORPTION);

    // The saved LUHDR records the medium as the sample's captures do: flag 512, absorption and 12 triangles.
    QByteArray hdr;
    REQUIRE(inFlight.EncodeHdr(hdr, error) == true);
    constexpr uint32 LUHDR_MEDIUM_FLAG = 512;
    constexpr usize LUHDR_MEDIUM_ABSORPTION_OFFSET = 184;
    constexpr usize LUHDR_MEDIUM_TRIANGLES_OFFSET = 196;
    REQUIRE(static_cast<usize>(hdr.size()) > LUHDR_HEADER_BYTES);
    CHECK((qFromLittleEndian<uint32>(hdr.constData() + LUHDR_FLAGS_OFFSET) & LUHDR_MEDIUM_FLAG) != 0);
    const uint32 redBits = qFromLittleEndian<uint32>(hdr.constData() + LUHDR_MEDIUM_ABSORPTION_OFFSET);
    float32 red = 0;
    std::memcpy(&red, &redBits, sizeof(red));
    CHECK(red == SAMPLE_ABSORPTION.X);
    CHECK(qFromLittleEndian<uint32>(hdr.constData() + LUHDR_MEDIUM_TRIANGLES_OFFSET) ==
          static_cast<uint32>(ABSORBING_MEDIUM_BOX_TRIANGLES));
}

TEST_CASE("GPU compute captures a World's absorbing medium like the CPU oracle",
          "[editor][reference-capture][gpu][reference-compute][oracle-parity][absorbing-medium]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    QString key;
    WorldDocument world = MakeMediumWorld(key);
    const RenderScene scene = LoadMediumScene(world);
    Array<renderer::ReferencePixel> pixels[2];
    const renderer::ReferenceTraversal traversals[]{renderer::ReferenceTraversal::CpuOracle,
                                                    renderer::ReferenceTraversal::GpuCompute};
    for (usize index = 0; index < 2; ++index)
    {
        ViewportReferenceCapture capture;
        QString error;
        const bool isStarted = StartMediumCapture(capture, device, scene, traversals[index], error);
        INFO(error.toStdString());
        if (isStarted == false && traversals[index] == renderer::ReferenceTraversal::GpuCompute)
        {
            SKIP("GPU compute is unavailable on this device: " << error.toStdString());
        }
        REQUIRE(isStarted == true);
        pixels[index] = FinishMediumCapture(capture, traversals[index]);
    }
    float32 largest = 0;
    for (usize i = 0; i < pixels[0].GetSize(); ++i)
    {
        CHECK(pixels[1][i].Samples == pixels[0][i].Samples);
        for (usize channel = 0; channel < 3; ++channel)
        {
            const float32 oracle = GetChannel(pixels[0][i].Radiance, channel);
            const float32 compute = GetChannel(pixels[1][i].Radiance, channel);
            largest = math::Max(largest, math::Abs(compute - oracle) / math::Max(oracle, 1.0F));
        }
    }
    CAPTURE(largest);
    CHECK(largest <= MEDIUM_PARITY_TOLERANCE);
}

TEST_CASE("Reference capture of a World with an absorbing medium refuses native ray queries with the repair",
          "[editor][reference-capture][absorbing-medium]")
{
    QString key;
    const WorldDocument world = MakeMediumWorld(key);
    const RenderScene scene = LoadMediumScene(world);
    const QSize extent(static_cast<int32>(MEDIUM_LONG_SIDE), static_cast<int32>(MEDIUM_LONG_SIDE));
    ViewportReferenceCapture capture;
    QString error;
    // Refused before the device is used, so no adapter is needed.
    CHECK(capture.Start({},
                        {},
                        scene,
                        DescribeMediumView(scene, extent),
                        extent,
                        MakeMediumSettings(renderer::ReferenceTraversal::NativeQueries),
                        error) == false);
    CHECK(error.contains(QStringLiteral("CPU oracle or GPU compute")) == true);
    CHECK(error.contains(QStringLiteral("Absorbing Medium")) == true);
    CHECK(capture.GetPhase() == ViewportReferencePhase::Idle);

    // The capture window offers only the traversals that transport the medium and leaves native ray queries.
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    QComboBox traversal;
    PopulateReferenceTraversalChoices(traversal, renderer::ReferenceTraversal::NativeQueries);
    UpdateReferenceTraversalAvailability(traversal, &preview, false);
    CHECK(traversal.currentIndex() != static_cast<int32>(renderer::ReferenceTraversal::NativeQueries));
    const QStandardItemModel* modelOrNull = qobject_cast<const QStandardItemModel*>(traversal.model());
    REQUIRE(modelOrNull != nullptr);
    const QStandardItem* nativeOrNull =
        modelOrNull->item(static_cast<int32>(renderer::ReferenceTraversal::NativeQueries));
    REQUIRE(nativeOrNull != nullptr);
    CHECK(nativeOrNull->isEnabled() == false);
    CHECK(nativeOrNull->toolTip().contains(QStringLiteral("Absorbing Medium")) == true);
}

TEST_CASE("Reference capture window explains a missing viewport device without starting", "[editor][reference-capture]")
{
    ReferenceCaptureWindow window(nullptr);
    CHECK(window.StartCapture() == false);
    CHECK(window.GetCapture().GetPhase() == ViewportReferencePhase::Idle);
    CHECK(window.GetStatusText().contains(QStringLiteral("no GPU device")) == true);
    QPushButton* save = window.findChild<QPushButton*>(QStringLiteral("referenceSave"));
    REQUIRE(save != nullptr);
    CHECK(save->isEnabled() == false);
    QPushButton* stop = window.findChild<QPushButton*>(QStringLiteral("referenceStop"));
    REQUIRE(stop != nullptr);
    CHECK(stop->isEnabled() == false);
}

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
namespace
{
RenderScene MakePowerFixture()
{
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("power-fixture");
    PreviewSurface emitter;
    emitter.Scattering.Model = ludus::content::scene_format::MaterialModel::Diffuse;
    emitter.Emission = {1, 1, 1};
    scene.Surfaces.append(emitter);
    PreviewMesh mesh;
    mesh.Positions = {{-0.25F, -0.25F, 0}, {0.25F, -0.25F, 0}, {-0.25F, 0.25F, 0}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.append(mesh);
    PreviewObject object;
    object.Id = QStringLiteral("emitter");
    object.Surface = 0;
    scene.Objects.append(object);
    return scene;
}

void AddPowerFixtureEnclosure(RenderScene& inoutScene, math::Vector4 reflectance)
{
    PreviewSurface wall;
    wall.Scattering.Model = ludus::content::scene_format::MaterialModel::Diffuse;
    wall.Tint = reflectance;
    PreviewMesh cube;
    cube.Positions =
        {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    cube.Indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                    1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
    PreviewObject object;
    object.Id = QStringLiteral("enclosure");
    object.Mesh = 1;
    object.Surface = 1;
    inoutScene.Meshes.append(cube);
    inoutScene.Surfaces.append(wall);
    inoutScene.Objects.append(object);
}
} // namespace

TEST_CASE("World power probes integrate source flux and reject unsupported input", "[editor][world-power]")
{
    RenderScene scene = MakePowerFixture();
    WorldPowerProbes probes;
    const float64 expected = math::kPi * 0.125;

    SECTION("two-sided source flux scales with placed emitter area")
    {
        scene.Objects[0].Transform.Columns[0].X = 2;
        scene.Surfaces[0].IsEmissionTwoSided = true;
        REQUIRE(BuildWorldPowerProbes(scene, 1, probes).isEmpty() == true);
        CHECK(probes.Emitted.X == Catch::Approx(4 * expected));
        CHECK(probes.Rays.GetSize() == probes.EscapeRayCount);
        CHECK(probes.Weights.GetSize() == probes.Rays.GetSize());
        CHECK(probes.Rays.GetSize() % probes.RowWidth == 0);
    }
    SECTION("authored flux is measured per placement")
    {
        scene.Surfaces[0].HasEmissionPower = true;
        scene.Surfaces[0].IsEmissionTwoSided = true;
        scene.Surfaces[0].EmissionPower = 25;
        REQUIRE(BuildWorldPowerProbes(scene, 1, probes).isEmpty() == true);
        CHECK(probes.Emitted.X == Catch::Approx(25));
    }
    SECTION("the probe integrals are independent")
    {
        AddPowerFixtureEnclosure(scene, {0, 0, 0, 1});
        REQUIRE(BuildWorldPowerProbes(scene, 1, probes).isEmpty() == true);
        Array<renderer::ReferencePixel> pixels;
        REQUIRE(pixels.TryResize(probes.Rays.GetSize()) == true);
        WorldPowerFlux flux;
        CHECK(IntegrateWorldPower(probes, {pixels.GetData(), pixels.GetSize()}, flux) == false);
        for (usize index = 0; index < pixels.GetSize(); ++index)
        {
            pixels[index].Radiance = index < probes.EscapeRayCount ? math::Vector3{} : math::Vector3{1, 1, 1};
            pixels[index].Samples = 1;
        }
        REQUIRE(IntegrateWorldPower(probes, {pixels.GetData(), pixels.GetSize()}, flux) == true);
        CHECK(flux.Emitted.X == Catch::Approx(expected));
        CHECK(flux.Escaped == math::Vector3d{});
        CHECK(flux.Absorbed.X > flux.Emitted.X);
    }
    SECTION("directional and other analytic lights require repair")
    {
        scene.AuthoredLights.Count = 1;
        CHECK(BuildWorldPowerProbes(scene, 1, probes).contains(QStringLiteral("Disable analytic lights")) == true);
    }
    SECTION("non-diffuse transport requires repair")
    {
        scene.Surfaces[0].Scattering.Model = ludus::content::scene_format::MaterialModel::Dielectric;
        CHECK(BuildWorldPowerProbes(scene, 1, probes).contains(QStringLiteral("emitter")) == true);
    }
}

namespace
{
void PublishPowerFixtureDefinition(RenderScene& inoutScene)
{
    using ludus::resources::SceneDefinition;
    using ludus::resources::SceneMesh;
    using ludus::resources::SceneObject;
    SharedPtr<SceneDefinition> definition;
    REQUIRE(TryMakeShared<SceneDefinition>(GetSystemAllocationDomain(), definition) == OwnershipStatus::Ok);
    REQUIRE(definition->Meshes.TryResize(static_cast<usize>(inoutScene.Meshes.size())) == true);
    REQUIRE(definition->Objects.TryResize(static_cast<usize>(inoutScene.Objects.size())) == true);
    REQUIRE(definition->Materials.TryResize(static_cast<usize>(inoutScene.Surfaces.size())) == true);
    for (qsizetype index = 0; index < inoutScene.Meshes.size(); ++index)
    {
        const PreviewMesh& source = inoutScene.Meshes[index];
        SceneMesh& target = definition->Meshes[static_cast<usize>(index)];
        for (const math::Vector3 point : source.Positions)
        {
            REQUIRE(target.Positions.TryAdd(point) == true);
            REQUIRE(target.Normals.TryAdd({0, 0, 1}) == true);
            REQUIRE(target.Texcoords.TryAdd({0, 0}) == true);
        }
        for (const uint32 vertex : source.Indices)
        {
            REQUIRE(target.Indices.TryAdd(vertex) == true);
        }
        const PreviewSurface& surface = inoutScene.Surfaces[index];
        definition->Materials[static_cast<usize>(index)].Emission = surface.Emission;
        definition->Materials[static_cast<usize>(index)].IsEmissionTwoSided = surface.IsEmissionTwoSided;
        definition->Materials[static_cast<usize>(index)].Scattering = surface.Scattering;
        const PreviewObject& object = inoutScene.Objects[index];
        SceneObject& placed = definition->Objects[static_cast<usize>(index)];
        placed.SourceId = static_cast<uint64>(index) + 1;
        placed.Mesh = static_cast<uint32>(object.Mesh);
        placed.Material = static_cast<uint32>(object.Surface);
        placed.Transform = object.Transform;
    }
    inoutScene.Lit = true;
    inoutScene.LogicalId = QStringLiteral("level/power-fixture");
    inoutScene.AssetsIdentity = inoutScene.Identity;
    inoutScene.Definition = definition;
}
} // namespace

TEST_CASE("The World power tab measures both RT1 tracers and clears stale results", "[editor][world-power][gpu]")
{
    rhi::Shutdown();
    RenderScene scene = MakePowerFixture();
    bool isEnclosed = false;
    SECTION("an open emitter lets its power escape") {}
    SECTION("an enclosed emitter sends its power to black walls")
    {
        isEnclosed = true;
        scene.Objects[0].Transform.Columns[0].X = 3;
        scene.Objects[0].Transform.Columns[1].Y = 3;
        AddPowerFixtureEnclosure(scene, {0, 0, 0, 1});
    }
    PublishPowerFixtureDefinition(scene);
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    preview.Resize(TEST_LONG_SIDE, TEST_LONG_SIDE);
    REQUIRE(preview.Start() == true);
    if (preview.AdmitOffscreenDevice() == false)
    {
        SKIP("no offscreen viewport GPU adapter");
    }
    ReferenceCaptureWindow window(&preview);
    QTabWidget* tabsOrNull = window.findChild<QTabWidget*>(QStringLiteral("referenceImages"));
    REQUIRE(tabsOrNull != nullptr);
    QWidget* tabOrNull = tabsOrNull->findChild<QWidget*>(QStringLiteral("worldPowerBudgetTab"));
    REQUIRE(tabOrNull != nullptr);
    tabsOrNull->setCurrentWidget(tabOrNull);
    QPushButton* measureOrNull = tabOrNull->findChild<QPushButton*>(QStringLiteral("measureWorldPower"));
    QLabel* statusOrNull = tabOrNull->findChild<QLabel*>(QStringLiteral("worldPowerStatus"));
    QLabel* cpuEscapeOrNull = tabOrNull->findChild<QLabel*>(QStringLiteral("cpuWorldPowerEscaped"));
    QLabel* gpuEscapeOrNull = tabOrNull->findChild<QLabel*>(QStringLiteral("gpuWorldPowerEscaped"));
    QLabel* cpuAbsorbedOrNull = tabOrNull->findChild<QLabel*>(QStringLiteral("cpuWorldPowerAbsorbed"));
    QLabel* gpuAbsorbedOrNull = tabOrNull->findChild<QLabel*>(QStringLiteral("gpuWorldPowerAbsorbed"));
    QLabel* cpuStatusOrNull = tabOrNull->findChild<QLabel*>(QStringLiteral("cpuWorldPowerStatus"));
    QLabel* gpuStatusOrNull = tabOrNull->findChild<QLabel*>(QStringLiteral("gpuWorldPowerStatus"));
    REQUIRE(cpuAbsorbedOrNull != nullptr);
    REQUIRE(gpuAbsorbedOrNull != nullptr);
    REQUIRE(cpuStatusOrNull != nullptr);
    REQUIRE(gpuStatusOrNull != nullptr);
    REQUIRE(measureOrNull != nullptr);
    REQUIRE(statusOrNull != nullptr);
    REQUIRE(cpuEscapeOrNull != nullptr);
    REQUIRE(gpuEscapeOrNull != nullptr);
    REQUIRE(measureOrNull->isEnabled() == true);
    if (isEnclosed == true)
    {
        QSpinBox* densityOrNull = tabOrNull->findChild<QSpinBox*>(QStringLiteral("worldPowerProbeDensity"));
        REQUIRE(densityOrNull != nullptr);
        densityOrNull->setValue(static_cast<int32>(MAX_WORLD_POWER_PROBE_DENSITY));
    }
    measureOrNull->click();
    REQUIRE(
        QTest::qWaitFor([measureOrNull]() { return measureOrNull->isEnabled(); }, GPU_CAPTURE_TIMEOUT_MILLISECONDS));
    INFO(cpuStatusOrNull->text().toStdString());
    INFO(gpuStatusOrNull->text().toStdString());
    REQUIRE(cpuStatusOrNull->text().startsWith(QStringLiteral("Complete")) == true);
    REQUIRE(gpuStatusOrNull->text().startsWith(QStringLiteral("Complete")) == true);
    const float64 cpuEscape = cpuEscapeOrNull->text().section(QStringLiteral(" "), 0, 0).toDouble();
    const float64 gpuEscape = gpuEscapeOrNull->text().section(QStringLiteral(" "), 0, 0).toDouble();
    const float64 cpuAbsorbed = cpuAbsorbedOrNull->text().section(QStringLiteral(" "), 0, 0).toDouble();
    const float64 gpuAbsorbed = gpuAbsorbedOrNull->text().section(QStringLiteral(" "), 0, 0).toDouble();
    const float64 emitted = math::kPi * 0.125 * (isEnclosed == true ? 9 : 1);
    if (isEnclosed == true)
    {
        CHECK(cpuAbsorbed == Catch::Approx(emitted).epsilon(0.35));
        CHECK(gpuAbsorbed == Catch::Approx(cpuAbsorbed).epsilon(0.001));
        CHECK(cpuEscape == 0);
        CHECK(gpuEscape == 0);
    }
    else
    {
        CHECK(cpuEscape == Catch::Approx(emitted).epsilon(0.08));
        CHECK(gpuEscape == Catch::Approx(cpuEscape).epsilon(0.001));
        CHECK(cpuAbsorbed == 0);
        CHECK(gpuAbsorbed == 0);
    }

    measureOrNull->click();
    scene.Identity = QByteArrayLiteral("world-power-edited");
    scene.AuthoredLights.Count = 1;
    REQUIRE(preview.SetScene(scene) == true);
    CHECK(measureOrNull->isEnabled() == true);
    CHECK(cpuEscapeOrNull->text().isEmpty() == true);
    CHECK(gpuEscapeOrNull->text().isEmpty() == true);
    measureOrNull->click();
    CHECK(statusOrNull->text().contains(QStringLiteral("Disable analytic lights")) == true);
    preview.Stop();
}

TEST_CASE("Reference capture shows a saved power budget and its signed heatmap", "[editor][reference-power]")
{
    namespace format = ludus::content::render_graph_format;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const auto writeCapture = [&directory](const QString& name, uint32 width, const QVector<float32>& pixels) {
        const QString path = directory.filePath(name);
        REQUIRE(pixels.size() % width == 0);
        const qsizetype bytesToWrite = format::CAPTURE_HEADER_BYTES + pixels.size() * format::CAPTURE_PIXEL_BYTES;
        QByteArray bytes(bytesToWrite, '\0');
        std::memcpy(bytes.data(), format::CAPTURE_MAGIC, format::CAPTURE_MAGIC_BYTES);
        const auto writeWord = [&bytes](usize offset, uint32 value) { qToLittleEndian(value, bytes.data() + offset); };
        writeWord(format::CAPTURE_VERSION_OFFSET, format::CAPTURE_VERSION);
        writeWord(format::CAPTURE_HEADER_SIZE_OFFSET, format::CAPTURE_HEADER_BYTES);
        writeWord(format::CAPTURE_PIXEL_SIZE_OFFSET, format::CAPTURE_PIXEL_BYTES);
        writeWord(format::CAPTURE_WIDTH_OFFSET, width);
        writeWord(format::CAPTURE_HEIGHT_OFFSET, static_cast<uint32>(pixels.size() / width));
        writeWord(format::CAPTURE_TARGET_SAMPLES_OFFSET, 1);
        writeWord(format::CAPTURE_SUBMITTED_SAMPLES_OFFSET, 1);
        // Orthographic projection with a clip-depth offset so the plane at Z=-1 is visible.
        constexpr StaticArray<float32, format::CAPTURE_MATRIX_VALUES> PROJECTION{
            1,
            0,
            0,
            0,
            0,
            1,
            0,
            0,
            0,
            0,
            1,
            0,
            0,
            0,
            1.5F,
            1,
        };
        for (usize i = 0; i < PROJECTION.GetSize(); ++i)
        {
            writeWord(format::CAPTURE_WORLD_TO_CLIP_OFFSET + i * sizeof(float32), std::bit_cast<uint32>(PROJECTION[i]));
        }
        for (qsizetype i = 0; i < pixels.size(); ++i)
        {
            const usize offset = format::CAPTURE_HEADER_BYTES + static_cast<usize>(i) * format::CAPTURE_PIXEL_BYTES;
            writeWord(offset, std::bit_cast<uint32>(pixels[i]));
            writeWord(offset + LUHDR_SAMPLE_COUNT_OFFSET, 1);
        }
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly) == true);
        REQUIRE(file.write(bytes) == bytes.size());
        return path;
    };

    ReferenceCaptureWindow window(nullptr);
    QTabWidget* tabsOrNull = window.findChild<QTabWidget*>(QStringLiteral("referenceImages"));
    REQUIRE(tabsOrNull != nullptr);
    QWidget* tabOrNull = tabsOrNull->findChild<QWidget*>(QStringLiteral("referencePowerTab"));
    REQUIRE(tabOrNull != nullptr);
    tabsOrNull->setCurrentWidget(tabOrNull);
    QLineEdit* measuredOrNull = tabOrNull->findChild<QLineEdit*>(QStringLiteral("powerMeasured"));
    QLineEdit* openOrNull = tabOrNull->findChild<QLineEdit*>(QStringLiteral("powerOpen"));
    QLineEdit* expectedOrNull = tabOrNull->findChild<QLineEdit*>(QStringLiteral("powerExpected"));
    QPushButton* measureOrNull = tabOrNull->findChild<QPushButton*>(QStringLiteral("measureReferencePower"));
    CapturedImageView* imageOrNull = tabOrNull->findChild<CapturedImageView*>(QStringLiteral("referencePowerImage"));
    QLabel* statusOrNull = tabOrNull->findChild<QLabel*>(QStringLiteral("referencePowerStatus"));
    REQUIRE(measuredOrNull != nullptr);
    REQUIRE(openOrNull != nullptr);
    REQUIRE(expectedOrNull != nullptr);
    REQUIRE(measureOrNull != nullptr);
    REQUIRE(imageOrNull != nullptr);
    REQUIRE(statusOrNull != nullptr);

    measuredOrNull->setText(writeCapture(QStringLiteral("measured.lurgcap"), 2, {1, 0}));
    openOrNull->setText(writeCapture(QStringLiteral("open.lurgcap"), 2, {1, 1}));
    expectedOrNull->setText(writeCapture(QStringLiteral("expected.lurgcap"), 2, {0, 1}));
    measureOrNull->click();
    constexpr int32 WAIT_MILLISECONDS = 5000;
    REQUIRE(QTest::qWaitFor([imageOrNull]() { return imageOrNull->GetImage().isNull() == false; }, WAIT_MILLISECONDS));
    CHECK(statusOrNull->text().contains(QStringLiteral("Within tolerance")) == true);
    CHECK(statusOrNull->text().contains(QStringLiteral("Excess 50.000%")) == true);
    const QImage& image = imageOrNull->GetImage();
    CHECK(qRed(image.pixel(0, 0)) > qBlue(image.pixel(0, 0)));
    CHECK(qBlue(image.pixel(1, 0)) > qRed(image.pixel(1, 0)));

    expectedOrNull->setText(directory.filePath(QStringLiteral("missing.lurgcap")));
    CHECK(imageOrNull->GetImage().isNull() == true);
    measureOrNull->click();
    REQUIRE(QTest::qWaitFor(
        [statusOrNull]() { return statusOrNull->text().contains(QStringLiteral("Choose a readable raw capture")); },
        WAIT_MILLISECONDS));
    CHECK(imageOrNull->GetImage().isNull() == true);

    QComboBox* modelOrNull = tabOrNull->findChild<QComboBox*>(QStringLiteral("referencePowerModel"));
    REQUIRE(modelOrNull != nullptr);
    QDoubleSpinBox* emittedOrNull = tabOrNull->findChild<QDoubleSpinBox*>(QStringLiteral("powerEmittedWatts"));
    REQUIRE(emittedOrNull != nullptr);
    modelOrNull->setCurrentText(QStringLiteral("Gaussian knife edge"));
    emittedOrNull->setValue(10);
    expectedOrNull->clear();
    measureOrNull->click();
    const auto waitForBudget = [measureOrNull]() { return measureOrNull->isEnabled(); };
    REQUIRE(QTest::qWaitFor(waitForBudget, WAIT_MILLISECONDS));
    // Both pixels are in the outer band. Even an exactly closing sum needs more receiver coverage.
    CHECK(statusOrNull->text().contains(QStringLiteral("Widen the receiver")) == true);
    CHECK(imageOrNull->GetImage().isNull() == true);

    constexpr uint32 WIDE_SIDE = 8;
    constexpr qsizetype CENTRE_PIXEL = 4 * WIDE_SIDE + 4;
    QVector<float32> beam(WIDE_SIDE * WIDE_SIDE, 0);
    beam[CENTRE_PIXEL] = 1;
    openOrNull->setText(writeCapture(QStringLiteral("wide-open.lurgcap"), WIDE_SIDE, beam));
    beam[CENTRE_PIXEL] = 0.5F;
    measuredOrNull->setText(writeCapture(QStringLiteral("wide-measured.lurgcap"), WIDE_SIDE, beam));
    measureOrNull->click();
    REQUIRE(QTest::qWaitFor(waitForBudget, WAIT_MILLISECONDS));
    CHECK(statusOrNull->text().contains(QStringLiteral("Emitted 10 W")) == true);
    CHECK(statusOrNull->text().contains(QStringLiteral("Absorbed by mask 5 W")) == true);
    CHECK(statusOrNull->text().contains(QStringLiteral("Escaped in frame 5 W")) == true);
    CHECK(statusOrNull->text().contains(QStringLiteral("closes within tolerance")) == true);

    beam[CENTRE_PIXEL] = 0.6F;
    measuredOrNull->setText(writeCapture(QStringLiteral("wide-bright.lurgcap"), WIDE_SIDE, beam));
    measureOrNull->click();
    REQUIRE(QTest::qWaitFor(waitForBudget, WAIT_MILLISECONDS));
    CHECK(statusOrNull->text().contains(QStringLiteral("measured escape exceeds")) == true);

    emittedOrNull->setValue(0);
    modelOrNull->setCurrentText(QStringLiteral("Gaussian single slit"));
    QDoubleSpinBox* radiusOrNull = tabOrNull->findChild<QDoubleSpinBox*>(QStringLiteral("powerMaskRadius"));
    QDoubleSpinBox* heightOrNull = tabOrNull->findChild<QDoubleSpinBox*>(QStringLiteral("powerSlitHeight"));
    REQUIRE(radiusOrNull != nullptr);
    REQUIRE(heightOrNull != nullptr);
    radiusOrNull->setValue(0.5);
    heightOrNull->setValue(1);
    QDoubleSpinBox* slitWidthOrNull = tabOrNull->findChild<QDoubleSpinBox*>(QStringLiteral("powerSlitWidth"));
    REQUIRE(slitWidthOrNull != nullptr);
    slitWidthOrNull->setValue(1);
    measureOrNull->click();
    REQUIRE(QTest::qWaitFor(waitForBudget, WAIT_MILLISECONDS));
    // Independent Gaussian integral for a 1 m square at a radius of 0.5 m.
    CHECK(statusOrNull->text().contains(QStringLiteral("Absorbed by mask 8.893%")) == true);

    modelOrNull->setCurrentText(QStringLiteral("Gaussian double slit"));
    measureOrNull->click();
    REQUIRE(QTest::qWaitFor(waitForBudget, WAIT_MILLISECONDS));
    CHECK(statusOrNull->text().contains(QStringLiteral("openings overlap")) == true);

    QDoubleSpinBox* separationOrNull = tabOrNull->findChild<QDoubleSpinBox*>(QStringLiteral("powerSlitSeparation"));
    REQUIRE(separationOrNull != nullptr);
    separationOrNull->setValue(1);
    slitWidthOrNull->setValue(0.5);
    measureOrNull->click();
    REQUIRE(QTest::qWaitFor(waitForBudget, WAIT_MILLISECONDS));
    CHECK(statusOrNull->text().contains(QStringLiteral("Absorbed by mask 69.970%")) == true);
}
#endif

TEST_CASE("Reference capture releases viewport GPU admission on stop and completion", "[editor][reference-capture]")
{
    rhi::Shutdown();
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    tests::MakeLitScene(root.path(), scene);
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    preview.Resize(TEST_LONG_SIDE, TEST_LONG_SIDE);
    REQUIRE(preview.Start() == true);
    if (preview.AdmitOffscreenDevice() == false)
    {
        SKIP("no offscreen viewport GPU adapter");
    }

    ReferenceCaptureWindow window(&preview);
    QSpinBox* longSide = window.findChild<QSpinBox*>(QStringLiteral("referenceLongSide"));
    QSpinBox* samples = window.findChild<QSpinBox*>(QStringLiteral("referenceSamples"));
    REQUIRE(longSide != nullptr);
    REQUIRE(samples != nullptr);
    longSide->setValue(TEST_LONG_SIDE);
    samples->setValue(TEST_SAMPLES);
    // Every traversal holds the same admission. The window would choose GPU compute and compile its kernel
    // cold inside this shared test shard; GPU captures run in the [reference-compute] cases instead.
    constexpr renderer::ReferenceTraversal TRAVERSAL = renderer::ReferenceTraversal::CpuOracle;
    SelectTraversal(window, TRAVERSAL);
    REQUIRE(window.StartCapture() == true);
    CHECK(preview.HasActiveGpuCapture() == true);
    window.StopCapture();
    CHECK(preview.HasActiveGpuCapture() == false);

    QSignalSpy finished(&window, &ReferenceCaptureWindow::CaptureFinished);
    REQUIRE(window.StartCapture() == true);
    CHECK(preview.HasActiveGpuCapture() == true);
    REQUIRE(WaitForCapture(window, finished) == true);
    INFO(window.GetStatusText().toStdString());
    CHECK(window.GetCapture().GetPhase() == ViewportReferencePhase::Complete);
    renderer::ReferenceCaptureInfo information;
    REQUIRE(window.GetCapture().GetInfo(information) == true);
    CHECK(information.Traversal == TRAVERSAL);
    CHECK(preview.HasActiveGpuCapture() == false);
}

TEST_CASE("Reference capture window offers the render graph source only with a project graph",
          "[editor][reference-capture][render-graph-capture]")
{
    ReferenceCaptureWindow window(nullptr);
    QComboBox* source = window.findChild<QComboBox*>(QStringLiteral("referenceSource"));
    REQUIRE(source != nullptr);
    CHECK(source->currentIndex() == CAPTURE_SOURCE_REFERENCE_INDEX);
    const QStandardItemModel* model = qobject_cast<const QStandardItemModel*>(source->model());
    REQUIRE(model != nullptr);
    // Without a viewport project graph there is nothing to capture but the RT1 reference.
    CHECK(model->item(CAPTURE_SOURCE_GRAPH_INDEX)->isEnabled() == false);
    CHECK(window.GetGraphText().startsWith(QStringLiteral("None.")) == true);
    QSpinBox* depth = window.findChild<QSpinBox*>(QStringLiteral("referenceMaxDepth"));
    REQUIRE(depth != nullptr);
    CHECK(depth->isEnabled() == true);
    QCheckBox* centerRays = window.findChild<QCheckBox*>(QStringLiteral("referenceCenterRays"));
    REQUIRE(centerRays != nullptr);
    CHECK(centerRays->isEnabled() == true);
    CHECK(centerRays->isChecked() == false);
    // Depth, traversal and centre rays describe RT1; the render graph source runs the graph as authored.
    source->setCurrentIndex(CAPTURE_SOURCE_GRAPH_INDEX);
    CHECK(depth->isEnabled() == false);
    CHECK(centerRays->isEnabled() == false);
    CHECK(window.findChild<QComboBox*>(QStringLiteral("referenceTraversal"))->isEnabled() == false);
    CHECK(window.findChild<QCheckBox*>(QStringLiteral("referenceCompare"))->isEnabled() == false);
}

TEST_CASE("RT1 sample presets edit only the next reference target", "[editor][reference-capture]")
{
    ReferenceCaptureWindow window(nullptr);
    QComboBox* presetOrNull = window.findChild<QComboBox*>(QStringLiteral("referenceSamplePreset"));
    QComboBox* sourceOrNull = window.findChild<QComboBox*>(QStringLiteral("referenceSource"));
    QSpinBox* samplesOrNull = window.findChild<QSpinBox*>(QStringLiteral("referenceSamples"));
    QSpinBox* graphSamplesOrNull = window.findChild<QSpinBox*>(QStringLiteral("referenceGraphSamples"));
    QSpinBox* longSideOrNull = window.findChild<QSpinBox*>(QStringLiteral("referenceLongSide"));
    QSpinBox* depthOrNull = window.findChild<QSpinBox*>(QStringLiteral("referenceMaxDepth"));
    QComboBox* traversalOrNull = window.findChild<QComboBox*>(QStringLiteral("referenceTraversal"));
    QCheckBox* centerRaysOrNull = window.findChild<QCheckBox*>(QStringLiteral("referenceCenterRays"));
    REQUIRE(presetOrNull != nullptr);
    REQUIRE(sourceOrNull != nullptr);
    REQUIRE(samplesOrNull != nullptr);
    REQUIRE(graphSamplesOrNull != nullptr);
    REQUIRE(longSideOrNull != nullptr);
    REQUIRE(depthOrNull != nullptr);
    REQUIRE(traversalOrNull != nullptr);
    REQUIRE(centerRaysOrNull != nullptr);
    longSideOrNull->setValue(TEST_LONG_SIDE);
    depthOrNull->setValue(TEST_DEPTH);
    SelectTraversal(window, renderer::ReferenceTraversal::CpuOracle);
    centerRaysOrNull->setChecked(true);

    CHECK(presetOrNull->currentText() == QStringLiteral("Quick inspection"));
    CHECK(samplesOrNull->value() == presetOrNull->currentData().toInt());
    const int32 quickSamples = samplesOrNull->value();
    presetOrNull->setCurrentText(QStringLiteral("Detailed comparison"));
    CHECK(presetOrNull->currentText() == QStringLiteral("Detailed comparison"));
    CHECK(samplesOrNull->value() == presetOrNull->currentData().toInt());
    CHECK(samplesOrNull->value() > quickSamples);
    const int32 detailedSamples = samplesOrNull->value();
    presetOrNull->setCurrentText(QStringLiteral("Custom"));
    CHECK(samplesOrNull->value() == detailedSamples);
    CHECK(longSideOrNull->value() == TEST_LONG_SIDE);
    CHECK(depthOrNull->value() == TEST_DEPTH);
    CHECK(traversalOrNull->currentIndex() == static_cast<int32>(renderer::ReferenceTraversal::CpuOracle));
    CHECK(centerRaysOrNull->isChecked() == true);

    presetOrNull->setCurrentText(QStringLiteral("Quick inspection"));
    samplesOrNull->setValue(TEST_SAMPLES);
    CHECK(presetOrNull->currentText() == QStringLiteral("Custom"));
    CHECK(samplesOrNull->value() == TEST_SAMPLES);
    sourceOrNull->setCurrentIndex(CAPTURE_SOURCE_GRAPH_INDEX);
    CHECK(presetOrNull->isHidden() == true);
    CHECK(samplesOrNull->isHidden() == true);
    CHECK(graphSamplesOrNull->isHidden() == false);
    CHECK(graphSamplesOrNull->value() == ViewportReferenceSettings{}.Samples);
    graphSamplesOrNull->setValue(1);
    sourceOrNull->setCurrentIndex(CAPTURE_SOURCE_REFERENCE_INDEX);
    CHECK(presetOrNull->isHidden() == false);
    CHECK(presetOrNull->currentText() == QStringLiteral("Custom"));
    CHECK(samplesOrNull->value() == TEST_SAMPLES);
    CHECK(graphSamplesOrNull->value() == 1);
}

TEST_CASE("RT1 preset and manual captures preserve their accepted HDR settings",
          "[editor][reference-capture][reference-compute][sample-presets]")
{
    rhi::Shutdown();
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    tests::MakeLitScene(root.path(), scene);
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    preview.Resize(TEST_LONG_SIDE, TEST_LONG_SIDE);
    REQUIRE(preview.Start() == true);
    if (preview.AdmitOffscreenDevice() == false)
    {
        SKIP("no offscreen viewport GPU adapter");
    }
    ReferenceCaptureWindow window(&preview);
    QComboBox* presetOrNull = window.findChild<QComboBox*>(QStringLiteral("referenceSamplePreset"));
    QSpinBox* samplesOrNull = window.findChild<QSpinBox*>(QStringLiteral("referenceSamples"));
    QSpinBox* longSideOrNull = window.findChild<QSpinBox*>(QStringLiteral("referenceLongSide"));
    QSpinBox* depthOrNull = window.findChild<QSpinBox*>(QStringLiteral("referenceMaxDepth"));
    REQUIRE(presetOrNull != nullptr);
    REQUIRE(samplesOrNull != nullptr);
    REQUIRE(longSideOrNull != nullptr);
    REQUIRE(depthOrNull != nullptr);
    longSideOrNull->setValue(TEST_LONG_SIDE);
    depthOrNull->setValue(TEST_DEPTH);
    const uint32 quickSamples = static_cast<uint32>(samplesOrNull->value());

    QSignalSpy finished(&window, &ReferenceCaptureWindow::CaptureFinished);
    REQUIRE(window.StartCapture() == true);
    CHECK(presetOrNull->isEnabled() == false);
    REQUIRE(WaitForCapture(window, finished) == true);
    renderer::ReferenceCaptureInfo information;
    REQUIRE(window.GetCapture().GetInfo(information) == true);
    CHECK(information.Complete == true);
    CHECK(information.TargetSamples == quickSamples);
    CHECK(information.MinimumSamples == quickSamples);
    CHECK(information.MaximumSamples == quickSamples);
    CHECK(information.MaxDepth == TEST_DEPTH);
    CHECK(information.Width == TEST_LONG_SIDE);
    CHECK(information.Height == TEST_LONG_SIDE);

    // Change the next capture before saving the completed one.
    presetOrNull->setCurrentText(QStringLiteral("Detailed comparison"));
    CHECK(samplesOrNull->value() > static_cast<int32>(quickSamples));
    QString error;
    const QString presetPath = root.filePath(QStringLiteral("preset.luhdr"));
    REQUIRE(window.SaveCapture(presetPath, error) == true);
    const QByteArray presetHdr = tests::ReadTestFile(presetPath);
    REQUIRE(presetHdr.size() > static_cast<qsizetype>(LUHDR_HEADER_BYTES));
    CHECK(qFromLittleEndian<uint32>(presetHdr.constData() + LUHDR_TARGET_SAMPLES_OFFSET) == quickSamples);

    samplesOrNull->setValue(static_cast<int32>(quickSamples));
    REQUIRE(presetOrNull->currentText() == QStringLiteral("Custom"));
    REQUIRE(window.StartCapture() == true);
    REQUIRE(WaitForCapture(window, finished) == true);
    const QString manualPath = root.filePath(QStringLiteral("manual.luhdr"));
    REQUIRE(window.SaveCapture(manualPath, error) == true);
    CHECK(tests::ReadTestFile(manualPath) == presetHdr);

    const uint32 partialTarget = static_cast<uint32>(samplesOrNull->maximum());
    samplesOrNull->setValue(static_cast<int32>(partialTarget));
    REQUIRE(window.StartCapture() == true);
    // GPU progress can advance before the image readback is available to Save.
    Array<renderer::ReferencePixel> partialPixels;
    REQUIRE(QTest::qWaitFor(
                [&window, &partialPixels, &error]() {
                    return window.GetCapture().CopyPixels(partialPixels, error) == true &&
                           partialPixels.IsEmpty() == false && partialPixels[0].Samples > 0;
                },
                static_cast<int32>(GPU_CAPTURE_TIMEOUT_MILLISECONDS)) == true);
    REQUIRE(window.GetCapture().GetPhase() == ViewportReferencePhase::Tracing);
    const QString partialPath = root.filePath(QStringLiteral("partial.luhdr"));
    REQUIRE(window.SaveCapture(partialPath, error) == true);
    const QByteArray partialHdr = tests::ReadTestFile(partialPath);
    const qsizetype pixelCount = static_cast<qsizetype>(TEST_LONG_SIDE) * TEST_LONG_SIDE;
    REQUIRE(partialPixels.GetSize() == static_cast<usize>(pixelCount));
    REQUIRE(partialHdr.size() == static_cast<qsizetype>(LUHDR_HEADER_BYTES + pixelCount * LUHDR_PIXEL_BYTES));
    CHECK(qFromLittleEndian<uint32>(partialHdr.constData() + LUHDR_TARGET_SAMPLES_OFFSET) == partialTarget);
    CHECK((qFromLittleEndian<uint32>(partialHdr.constData() + LUHDR_FLAGS_OFFSET) & LUHDR_PARTIAL_FLAG) != 0);
    for (qsizetype index = 0; index < pixelCount; ++index)
    {
        const uint32 count = qFromLittleEndian<uint32>(partialHdr.constData() + LUHDR_HEADER_BYTES +
                                                       index * LUHDR_PIXEL_BYTES + LUHDR_SAMPLE_COUNT_OFFSET);
        REQUIRE(count == partialPixels[static_cast<usize>(index)].Samples);
        REQUIRE(count < partialTarget);
    }
    REQUIRE(QImage(ViewportReferenceCapture::GetDisplayImagePath(partialPath)).isNull() == false);

    window.StopCapture();
    presetOrNull->setCurrentText(QStringLiteral("Quick inspection"));
    CHECK(samplesOrNull->value() == static_cast<int32>(quickSamples));
    CHECK(window.GetCapture().GetPhase() == ViewportReferencePhase::Idle);
    CHECK(window.SaveCapture(root.filePath(QStringLiteral("stopped.luhdr")), error) == false);
    CHECK(tests::ReadTestFile(partialPath) == partialHdr);
}

TEST_CASE("Capture progress hides when idle, moves without a fraction before samples and fills once they are known",
          "[editor][reference-capture]")
{
    constexpr int32 TARGET = 64;
    constexpr int32 HALF = 32;
    const CaptureProgressView idle = DescribeCaptureProgress(false, 0, TARGET);
    CHECK(idle.IsVisible == false);
    // Building the scene or rendering the raster side has no sample count to show yet.
    const CaptureProgressView preparing = DescribeCaptureProgress(true, 0, TARGET);
    CHECK(preparing.IsVisible == true);
    CHECK(preparing.IsBusy == true);
    const CaptureProgressView unknownTarget = DescribeCaptureProgress(true, HALF, 0);
    CHECK(unknownTarget.IsBusy == true);
    const CaptureProgressView tracing = DescribeCaptureProgress(true, HALF, TARGET);
    CHECK(tracing.IsVisible == true);
    CHECK(tracing.IsBusy == false);
    CHECK(tracing.Maximum == TARGET);
    CHECK(tracing.Value == HALF);
    // A finished or stopped capture keeps what it reached; a count past the target never overflows the bar.
    const CaptureProgressView stopped = DescribeCaptureProgress(false, HALF, TARGET);
    CHECK(stopped.IsVisible == true);
    CHECK(stopped.IsBusy == false);
    CHECK(stopped.Value == HALF);
    CHECK(DescribeCaptureProgress(false, TARGET + HALF, TARGET).Value == TARGET);
}

TEST_CASE("Reference capture window keeps its settings in a column beside the images at default and floating sizes",
          "[editor][reference-capture][layout]")
{
    // The floating size a capture window takes beside the editor on a MacBook Air.
    constexpr int32 FLOATING_WIDTH = 680;
    constexpr int32 FLOATING_HEIGHT = 780;
    constexpr int32 SHORT_HEIGHT = 400;
    constexpr int32 LONG_GRAPH_SEGMENT_LENGTH = 100;
    ReferenceCaptureWindow window(nullptr);
    ApplyWorkspaceStyle(&window);
    QScrollArea* settings = window.findChild<QScrollArea*>(QStringLiteral("referenceSettingsScroll"));
    QWidget* images = window.findChild<QTabWidget*>(QStringLiteral("referenceImages"));
    QLabel* graph = window.findChild<QLabel*>(QStringLiteral("referenceGraph"));
    QComboBox* graphTarget = window.findChild<QComboBox*>(QStringLiteral("referenceGraphTarget"));
    QLabel* status = window.findChild<QLabel*>(QStringLiteral("referenceStatus"));
    QPushButton* capture = window.findChild<QPushButton*>(QStringLiteral("referenceCapture"));
    QProgressBar* progress = window.findChild<QProgressBar*>(QStringLiteral("referenceProgress"));
    REQUIRE(settings != nullptr);
    REQUIRE(images != nullptr);
    REQUIRE(graph != nullptr);
    REQUIRE(graphTarget != nullptr);
    REQUIRE(status != nullptr);
    REQUIRE(capture != nullptr);
    REQUIRE(progress != nullptr);
    CHECK(capture->property("visualRole").toString() == QStringLiteral("primary"));
    // Nothing has run, so an empty bar would only take room.
    CHECK(progress->isHidden() == true);
    // Without a project graph the line stays short and the tooltip says why.
    CHECK(window.GetGraphText() == QStringLiteral("None. RT1 reference only"));
    CHECK(graph->toolTip().startsWith(QStringLiteral("This viewport has no project render graph")) == true);

    const QString graphName = QStringLiteral("Library%1.lurender.json#ComputeRayTracing/FinalColor")
                                  .arg(QString(LONG_GRAPH_SEGMENT_LENGTH, QLatin1Char('X')));
    graphTarget->addItem(graphName);
    graphTarget->setCurrentIndex(graphTarget->count() - 1);
    const QString diagnostic = QStringLiteral("Building %1 for the capture.").arg(graphName);
    status->setText(diagnostic);
    window.show();

    const QSize defaultSize = window.size();
    CHECK(defaultSize.width() >= window.minimumSizeHint().width());
    CHECK(defaultSize.height() >= window.minimumSizeHint().height());
    CHECK(window.minimumSizeHint().width() <= FLOATING_WIDTH);
    for (const QSize size : {defaultSize, QSize(FLOATING_WIDTH, FLOATING_HEIGHT), QSize(FLOATING_WIDTH, SHORT_HEIGHT)})
    {
        CAPTURE(size.width(), size.height());
        window.resize(size);
        QCoreApplication::sendPostedEvents();
        QApplication::processEvents();
        const QRect column = settings->geometry();
        const QRect preview = images->geometry();
        // Settings lead, the images take the rest, and neither overlaps the other or the window's edge.
        CHECK(column.right() < preview.left());
        CHECK(preview.width() > column.width());
        CHECK(preview.right() < window.width());
        CHECK(preview.bottom() < window.height());
        CHECK(column.top() == preview.top());
        // The summary is one line under the Graph choice, never wrapped to push the form down.
        CHECK(graph->height() < 2 * graph->fontMetrics().lineSpacing());
        // Every field and label sits whole inside the column; the form's labels share one leading edge.
        QWidget* content = settings->widget();
        REQUIRE(content != nullptr);
        CHECK(content->width() <= settings->viewport()->width());
        CHECK(settings->horizontalScrollBar()->maximum() == 0);
        if (size.height() == SHORT_HEIGHT)
        {
            CHECK(settings->verticalScrollBar()->maximum() > 0);
        }
        CHECK(graphTarget->currentText() == graphName);
        CHECK(graphTarget->toolTip().startsWith(graphName) == true);
        CHECK(status->text() == diagnostic);
        CHECK(status->height() >= status->heightForWidth(status->width()));
        QList<int32> labelEdges;
        QFormLayout* form = content->findChild<QFormLayout*>();
        REQUIRE(form != nullptr);
        for (int32 row = 0; row < form->rowCount(); ++row)
        {
            QLayoutItem* labelItemOrNull = form->itemAt(row, QFormLayout::LabelRole);
            if (labelItemOrNull != nullptr && labelItemOrNull->widget() != nullptr &&
                labelItemOrNull->widget()->isVisible() == true)
            {
                labelEdges.append(labelItemOrNull->widget()->geometry().left());
            }
        }
        REQUIRE(labelEdges.isEmpty() == false);
        CHECK(std::all_of(labelEdges.cbegin(), labelEdges.cend(), [&labelEdges](int32 edge) {
                  return edge == labelEdges.front();
              }) == true);
        for (QWidget* child : content->findChildren<QWidget*>())
        {
            if (child->isVisible() == false || child->parentWidget() != content)
            {
                continue;
            }
            CAPTURE(child->objectName().toStdString(), child->metaObject()->className());
            CHECK(child->geometry().left() >= 0);
            CHECK(child->geometry().right() < content->width());
        }
    }
}

TEST_CASE("Reference capture window names the compared image after the render graph, not raster",
          "[editor][reference-capture]")
{
    ReferenceCaptureWindow window(nullptr);
    QTabWidget* images = window.findChild<QTabWidget*>(QStringLiteral("referenceImages"));
    REQUIRE(images != nullptr);
    REQUIRE(images->count() == 3);
    // The viewport may draw any render graph, including an accumulating ray-traced one.
    CHECK(images->tabText(0) == QStringLiteral("Reference"));
    CHECK(images->tabText(1) == QStringLiteral("Render graph"));
    CHECK(images->tabText(2) == QStringLiteral("Difference"));
    CHECK(images->widget(1)->accessibleName() == QStringLiteral("Render graph image"));
}

namespace ludus::editor::tests
{
RenderScene LoadSuppliedWorld(const QString& root, const QString& worldFile)
{
    WorldDocument world;
    QString error;
    QByteArray schema;
    const QString schemaPath = QDir(root).filePath(QStringLiteral("authoring/world.schema.json"));
    if (QFileInfo::exists(schemaPath) == true)
    {
        schema = tests::ReadTestFile(schemaPath);
    }
    else
    {
        REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    }
    REQUIRE(world.LoadSchema(schema, error) == true);
    const QByteArray worldBytes = ReadTestFile(QDir(root).filePath(QStringLiteral("assets/") + worldFile));
    const bool hasLoadedWorld = world.Load(worldBytes, error);
    INFO(error.toStdString());
    REQUIRE(hasLoadedWorld == true);
    const QJsonObject level = world.SceneLevel();
    const QByteArray levelBytes = QJsonDocument(level).toJson(QJsonDocument::Compact);
    const QByteArray rootBytes = root.toUtf8();
    Array<uint8> bundle;
    ludus::content::scene_cook::AssetError assetError;
    const bool hasBundle = ludus::content::scene_cook::AssembleLevel(
        {rootBytes.constData(), static_cast<usize>(rootBytes.size())},
        {reinterpret_cast<const uint8*>(levelBytes.constData()), static_cast<usize>(levelBytes.size())},
        {},
        bundle,
        assetError);
    INFO(assetError.Message.View());
    REQUIRE(hasBundle == true);
    const QByteArray cooked = QByteArray::fromRawData(reinterpret_cast<const char*>(bundle.GetData()),
                                                      static_cast<qsizetype>(bundle.GetSize()));
    RenderScene scene;
    const bool hasScene = RenderScene::LoadCooked(cooked, level, scene, error, {});
    INFO(error.toStdString());
    REQUIRE(hasScene == true);
    if (RenderScene::GetSource(root, world) == RenderSceneSource::ProjectBindings)
    {
        RenderScene authored;
        const bool hasAuthoredScene = RenderScene::Load(root, world, authored, error, false);
        INFO(error.toStdString());
        REQUIRE(hasAuthoredScene == true);
        const bool hasMergedScene = RenderScene::MergeLoaded(authored, cooked, level, scene, scene, error, {});
        INFO(error.toStdString());
        REQUIRE(hasMergedScene == true);
    }
    // Present the World's switched-off lights, as the World viewport does.
    LightSwitchReport switchReport;
    REQUIRE(ApplyLightSwitches(world.GetLightSwitches(), scene, switchReport) == true);
    CHECK(switchReport.MissingSources.isEmpty() == true);
    return scene;
}
} // namespace ludus::editor::tests

TEST_CASE("Supplied World completes reference capture with all reference modes", "[.supplied-reference-capture]")
{
    const QString root = qEnvironmentVariable("LUDUS_REFERENCE_TEST_PROJECT");
    if (root.isEmpty() == true)
    {
        SKIP("Set LUDUS_REFERENCE_TEST_PROJECT to a project with assets/staircase2.world.json.");
    }
    const QString worldName = qEnvironmentVariable("LUDUS_REFERENCE_TEST_WORLD", QStringLiteral("staircase2"));
    const RenderScene scene = tests::LoadSuppliedWorld(root, worldName + QStringLiteral(".world.json"));
    // LUDUS_REFERENCE_TEST_EXTENT=<width>x<height> captures another size, for example a source film's 720x1280.
    QSize extent(512, 225);
    const QStringList requestedExtent = qEnvironmentVariable("LUDUS_REFERENCE_TEST_EXTENT").split(QLatin1Char('x'));
    if (requestedExtent.size() == 2)
    {
        constexpr int32 MAX_SUPPLIED_EXTENT = 4096;
        const int32 width = requestedExtent[0].toInt();
        const int32 height = requestedExtent[1].toInt();
        REQUIRE((width > 0 && height > 0 && width <= MAX_SUPPLIED_EXTENT && height <= MAX_SUPPLIED_EXTENT));
        extent = QSize(width, height);
    }
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    const QString navigationPath = qEnvironmentVariable("LUDUS_REFERENCE_TEST_NAVIGATION");
    if (navigationPath.isEmpty() == false)
    {
        preview.RestoreNavigation(QJsonDocument::fromJson(tests::ReadTestFile(navigationPath)).object());
    }
    renderer::ViewDescription view;
    QString error;
    REQUIRE(
        preview.DescribeView(static_cast<uint32>(extent.width()), static_cast<uint32>(extent.height()), view, error) ==
        true);
    // LUDUS_REFERENCE_TEST_AUTHORED_CAMERA=1 looks through the World's authored import camera instead.
    if (qEnvironmentVariableIntValue("LUDUS_REFERENCE_TEST_AUTHORED_CAMERA") == 1)
    {
        constexpr float32 AUTHORED_NEAR_PLANE = .01F;
        constexpr float32 AUTHORED_FAR_PLANE = 1000;
        const ludus::resources::SceneDefinition& definition = *scene.Definition;
        math::Matrix4 worldToView;
        math::Matrix4 viewToClip;
        REQUIRE(math::TryLookAt(definition.Eye, definition.Target, definition.Up, worldToView) ==
                math::MathStatus::Success);
        REQUIRE(
            math::TryPerspectiveReverseZ(definition.VerticalFov,
                                         static_cast<float32>(extent.width()) / static_cast<float32>(extent.height()),
                                         AUTHORED_NEAR_PLANE,
                                         AUTHORED_FAR_PLANE,
                                         viewToClip) == math::MathStatus::Success);
        view.WorldToClip = viewToClip * worldToView;
        view.CameraPosition = definition.Eye;
        view.Depth = renderer::DepthConvention::ReverseZ;
        view.InfiniteFar = false;
        view.OrthographicCamera = false;
    }
    const int32 requestedSamples = qEnvironmentVariableIntValue("LUDUS_REFERENCE_TEST_SAMPLES");
    // LUDUS_REFERENCE_TEST_MAX_DEPTH=<1..64> matches a source renderer's path depth, for example 64 for maxdepth 65.
    const int32 requestedDepth = qEnvironmentVariableIntValue("LUDUS_REFERENCE_TEST_MAX_DEPTH");
    REQUIRE((requestedDepth >= 0 && static_cast<uint32>(requestedDepth) <= MAXIMUM_REFERENCE_DEPTH));
    constexpr uint32 DEFAULT_SUPPLIED_DEPTH = 8;
    constexpr uint32 DEFAULT_SUPPLIED_SAMPLES = 64;
    constexpr int64 SUPPLIED_CAPTURE_TIMEOUT_MILLISECONDS = 600000;
    REQUIRE(preview.Start() == true);
    REQUIRE(preview.AdmitOffscreenDevice() == true);
#if defined(__APPLE__)
    rhi::DeviceInfo deviceInfo;
    REQUIRE(rhi::GetDeviceInfo(preview.GetDevice(), deviceInfo) == rhi::DeviceStatus::Ready);
    REQUIRE(deviceInfo.Startup.SelectedBackend == rhi::Backend::Metal);
#endif
    // LUDUS_REFERENCE_TEST_GRAPH keeps a supplied runtime graph resident while the window renders
    // RT1 and its Force raster comparison on the same device.
    const QString graphPath = qEnvironmentVariable("LUDUS_REFERENCE_TEST_GRAPH");
    if (graphPath.isEmpty() == false)
    {
        preview.Resize(extent.width(), extent.height());
        RendererWorkspace workspace({});
        RenderGraphPreview* graphOrNull = workspace.GetPreviewOrNull();
        REQUIRE(graphOrNull != nullptr);
        graphOrNull->SetForceRaster(true);
        REQUIRE(graphOrNull->Attach(preview.GetDevice(), preview.GetSurface()) == true);
        REQUIRE(graphOrNull->Load(graphPath, GetDefaultRenderGraphRuntime()) == RenderGraphPreviewLoad::Staged);
        graphOrNull->SetReferenceSize(extent);
        renderer::ViewDescription graphView;
        REQUIRE(preview.DescribeConventionalView(static_cast<uint32>(extent.width()),
                                                 static_cast<uint32>(extent.height()),
                                                 graphView,
                                                 error) == true);
        RenderGraphExtraction extraction;
        REQUIRE(MakeRenderGraphExtraction(scene, graphView, 1, extraction, error) == true);
        graphOrNull->SetExtraction(extraction);
        QElapsedTimer timer;
        timer.start();
        while (graphOrNull->GetPresentedRevision() != 1 && timer.elapsed() < SUPPLIED_CAPTURE_TIMEOUT_MILLISECONDS)
        {
            graphOrNull->Tick();
            QThread::msleep(1);
        }
        INFO(graphOrNull->GetDisplayStatus().toStdString());
        REQUIRE(graphOrNull->GetPresentedRevision() == 1);
        ReferenceCaptureWindow window(&preview);
        window.SetProjectGraph(&workspace);
        QSpinBox* longSideOrNull = window.findChild<QSpinBox*>(QStringLiteral("referenceLongSide"));
        QSpinBox* samplesOrNull = window.findChild<QSpinBox*>(QStringLiteral("referenceSamples"));
        QSpinBox* depthOrNull = window.findChild<QSpinBox*>(QStringLiteral("referenceMaxDepth"));
        QComboBox* traversalOrNull = window.findChild<QComboBox*>(QStringLiteral("referenceTraversal"));
        QCheckBox* compareOrNull = window.findChild<QCheckBox*>(QStringLiteral("referenceCompare"));
        REQUIRE(longSideOrNull != nullptr);
        REQUIRE(samplesOrNull != nullptr);
        REQUIRE(depthOrNull != nullptr);
        REQUIRE(traversalOrNull != nullptr);
        REQUIRE(compareOrNull != nullptr);
        longSideOrNull->setValue(math::Max(extent.width(), extent.height()));
        samplesOrNull->setValue(requestedSamples > 0 ? requestedSamples : DEFAULT_SUPPLIED_SAMPLES);
        depthOrNull->setValue(requestedDepth > 0 ? requestedDepth : DEFAULT_SUPPLIED_DEPTH);
        const int32 traversalIndex = qEnvironmentVariableIsSet("LUDUS_REFERENCE_TEST_TRAVERSAL") == true
                                         ? qEnvironmentVariableIntValue("LUDUS_REFERENCE_TEST_TRAVERSAL")
                                         : static_cast<int32>(renderer::ReferenceTraversal::CpuOracle);
        REQUIRE(traversalIndex >= 0);
        REQUIRE(traversalIndex < traversalOrNull->count());
        SelectTraversal(window, static_cast<renderer::ReferenceTraversal>(traversalIndex));
        REQUIRE(compareOrNull->isEnabled() == true);
        compareOrNull->setChecked(true);
        QSignalSpy finished(&window, &ReferenceCaptureWindow::CaptureFinished);
        REQUIRE(window.StartCapture() == true);
        const bool hasFinished = finished.wait(static_cast<int32>(SUPPLIED_CAPTURE_TIMEOUT_MILLISECONDS));
        INFO(window.GetStatusText().toStdString());
        REQUIRE(hasFinished == true);
        CHECK(window.GetCapture().GetPhase() == ViewportReferencePhase::Complete);
        renderer::ReferenceCaptureInfo information;
        REQUIRE(window.GetCapture().GetInfo(information) == true);
        CHECK(information.Traversal == static_cast<renderer::ReferenceTraversal>(traversalIndex));
        REQUIRE(window.HasComparison() == true);
        const ReferenceComparison& comparison = window.GetComparison();
        CHECK(comparison.IsReferenceComplete == true);
        CHECK(comparison.ComparedPixels == static_cast<uint64>(information.Width) * information.Height);
        CHECK(comparison.MinimumImageSamples == static_cast<uint32>(samplesOrNull->value()));
        const QString output = qEnvironmentVariable("LUDUS_REFERENCE_TEST_OUTPUT");
        if (output.isEmpty() == false)
        {
            const QString path = QDir(output).filePath(
                QStringLiteral("%1-comparison-%2.luhdr").arg(worldName).arg(traversalOrNull->currentIndex()));
            CHECK(window.SaveCapture(path, error) == true);
        }
        graphOrNull->Detach();
        return;
    }
    const QString requestedSeed = qEnvironmentVariable("LUDUS_REFERENCE_TEST_SEED");
    bool isValidSeed = false;
    const uint64 seed = requestedSeed.toULongLong(&isValidSeed);
    REQUIRE((requestedSeed.isEmpty() == true || isValidSeed == true));
    const QString selectedTraversal = qEnvironmentVariable("LUDUS_REFERENCE_TEST_TRAVERSAL");
    constexpr usize TRAVERSAL_COUNT = 3;
    bool isValidTraversal = false;
    const uint32 selectedTraversalValue = selectedTraversal.toUInt(&isValidTraversal);
    REQUIRE((selectedTraversal.isEmpty() == true ||
             (isValidTraversal == true && selectedTraversalValue < TRAVERSAL_COUNT)));
    const StaticArray<renderer::ReferenceTraversal, TRAVERSAL_COUNT> traversals{
        {renderer::ReferenceTraversal::CpuOracle,
         renderer::ReferenceTraversal::NativeQueries,
         renderer::ReferenceTraversal::GpuCompute}};
    for (const renderer::ReferenceTraversal traversal : traversals)
    {
        if (selectedTraversal.isEmpty() == false && selectedTraversalValue != static_cast<uint32>(traversal))
        {
            continue;
        }
        ViewportReferenceSettings settings;
        settings.Samples = requestedSamples > 0 ? static_cast<uint32>(requestedSamples) : DEFAULT_SUPPLIED_SAMPLES;
        settings.MaxDepth = requestedDepth > 0 ? static_cast<uint32>(requestedDepth) : DEFAULT_SUPPLIED_DEPTH;
        settings.Traversal = traversal;
        if (isValidSeed == true)
        {
            settings.Seed = seed;
        }
        ViewportReferenceCapture capture;
        QElapsedTimer captureTimer;
        captureTimer.start();
        REQUIRE(capture.Start(preview.GetDevice(), preview.GetSurface(), scene, view, extent, settings, error) == true);
        const ViewportReferencePhase phase = RunToEnd(capture, SUPPLIED_CAPTURE_TIMEOUT_MILLISECONDS);
        const int64 elapsedMilliseconds = captureTimer.elapsed();
        CAPTURE(elapsedMilliseconds);
        INFO(capture.GetError().toStdString());
        renderer::ReferenceCaptureInfo information;
        REQUIRE(capture.GetInfo(information) == true);
        INFO("traversal=" << static_cast<uint32>(traversal) << " committed=" << information.CommittedSamples
                          << " reason=" << static_cast<uint32>(information.Failure));
        CHECK(phase == ViewportReferencePhase::Complete);
        CHECK(information.Complete == true);
        CHECK(information.InvalidContributions == 0);
        CHECK(information.MinimumSamples == settings.Samples);
        CHECK(information.CommittedSamples ==
              static_cast<uint64>(extent.width()) * static_cast<uint64>(extent.height()) * settings.Samples);
        // LUDUS_REFERENCE_TEST_OUTPUT names a directory that receives each capture as LUHDR and PNG.
        const QString output = qEnvironmentVariable("LUDUS_REFERENCE_TEST_OUTPUT");
        if (output.isEmpty() == false)
        {
            const QString path =
                QDir(output).filePath(QStringLiteral("%1-%2.luhdr").arg(worldName).arg(static_cast<uint32>(traversal)));
            CHECK(capture.Save(path, error) == true);
        }
    }
    preview.Stop();
    rhi::Shutdown();
}

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
TEST_CASE("Celestial time-lapse evaluates absolute times, including both selected endpoints", "[editor][time-lapse]")
{
    constexpr float64 START = 9700.125;
    constexpr float64 END = 9700.25;
    constexpr int32 FRAME_COUNT = 121;
    float64 time = 0;
    REQUIRE(TryGetCelestialFrameTime(START, END, FRAME_COUNT - 1, FRAME_COUNT, time) == true);
    CHECK(time == END);
    REQUIRE(TryGetCelestialFrameTime(START, END, FRAME_COUNT / 2, FRAME_COUNT, time) == true);
    CHECK(time == (START + END) * 0.5);
    REQUIRE(TryGetCelestialFrameTime(START, END, 0, FRAME_COUNT, time) == true);
    CHECK(time == START);
    CHECK(TryGetCelestialFrameTime(END, START, 0, FRAME_COUNT, time) == false);
    CHECK(TryGetCelestialFrameTime(START, END, FRAME_COUNT, FRAME_COUNT, time) == false);
    CHECK(TryGetCelestialFrameTime(START, END, 0, 1, time) == false);
    CHECK(time == START);
}
#endif
