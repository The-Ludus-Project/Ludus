#include "internal/builtin_world.h"
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/celestial_time_lapse_window.h"
#endif
#include "internal/light_switches.h"
#include "internal/reference_capture_window.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/world_document.h"

#include "entropy_fixture.h"
#include "internal/preview_lighting.h"
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_preview.h"
#include "internal/renderer_workspace.h"
#include "internal/viewport_reference_capture.h"
#include "internal/viewport_tone_curve.h"
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
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
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
    ReferenceCaptureWindow window(nullptr);
    window.show();
    QWidget* settings = window.findChild<QWidget*>(QStringLiteral("referenceSettingsScroll"));
    QWidget* images = window.findChild<QTabWidget*>(QStringLiteral("referenceImages"));
    QLabel* graph = window.findChild<QLabel*>(QStringLiteral("referenceGraph"));
    QPushButton* capture = window.findChild<QPushButton*>(QStringLiteral("referenceCapture"));
    QProgressBar* progress = window.findChild<QProgressBar*>(QStringLiteral("referenceProgress"));
    REQUIRE(settings != nullptr);
    REQUIRE(images != nullptr);
    REQUIRE(graph != nullptr);
    REQUIRE(capture != nullptr);
    REQUIRE(progress != nullptr);
    CHECK(capture->property("visualRole").toString() == QStringLiteral("primary"));
    // Nothing has run, so an empty bar would only take room.
    CHECK(progress->isVisible() == false);
    // Without a project graph the line stays short and the tooltip says why.
    CHECK(window.GetGraphText() == QStringLiteral("None. RT1 reference only"));
    CHECK(graph->toolTip().startsWith(QStringLiteral("This viewport has no project render graph")) == true);

    const QSize defaultSize = window.size();
    CHECK(defaultSize.width() >= window.minimumSizeHint().width());
    CHECK(defaultSize.height() >= window.minimumSizeHint().height());
    CHECK(window.minimumSizeHint().width() <= FLOATING_WIDTH);
    for (const QSize size : {defaultSize, QSize(FLOATING_WIDTH, FLOATING_HEIGHT)})
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
        QWidget* content = settings->findChild<QWidget*>(QStringLiteral("referenceSettings"));
        REQUIRE(content != nullptr);
        CHECK(content->width() <= settings->width());
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
