#include "internal/reference_capture_window.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/world_document.h"

#include "internal/viewport_reference_capture.h"
#include "viewport_fixture.h"
#include <ludus/content/scene_cook/scene_assets.h>

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/ray.h>
#include <ludus/graphics/rhi/rhi.h>

#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

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
/// Generous wall-time bound for preparation plus tracing on a loaded machine.
constexpr int64 CAPTURE_TIMEOUT_MILLISECONDS = 120000;
constexpr uint64 TEST_SERVICE_BUDGET_NANOSECONDS = 50'000'000;
constexpr usize LUHDR_HEADER_BYTES = 256;
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

ViewportReferencePhase RunToEnd(ViewportReferenceCapture& inoutCapture,
                                int64 timeoutMilliseconds = CAPTURE_TIMEOUT_MILLISECONDS)
{
    QElapsedTimer timer;
    timer.start();
    ViewportReferencePhase phase = inoutCapture.GetPhase();
    while ((phase == ViewportReferencePhase::Preparing || phase == ViewportReferencePhase::Tracing) &&
           timer.elapsed() < timeoutMilliseconds)
    {
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

TEST_CASE("Reference display encoding matches the viewport's exposure-one Reinhard sRGB output",
          "[editor][reference-capture]")
{
    CHECK(EncodeReferenceDisplayChannel(0) == 0);
    // Reinhard maps 1 to 0.5, which sRGB encodes as 0.7354, i.e. 188 of 255.
    CHECK(EncodeReferenceDisplayChannel(1) == 188);
    CHECK(EncodeReferenceDisplayChannel(1.0e30F) == 255);
    CHECK(EncodeReferenceDisplayChannel(-1) == 0);
    CHECK(EncodeReferenceDisplayChannel(__builtin_nanf("")) == 0);
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
    const ViewportReferencePhase phase = RunToEnd(capture);
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
    const ViewportReferencePhase phase = RunToEnd(capture);
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

TEST_CASE("Supplied World completes reference capture with CPU and native traversal", "[.supplied-reference-capture]")
{
    const QString root = qEnvironmentVariable("LUDUS_REFERENCE_TEST_PROJECT");
    if (root.isEmpty() == true)
    {
        SKIP("Set LUDUS_REFERENCE_TEST_PROJECT to a project with assets/staircase2.world.json.");
    }
    WorldDocument world;
    QString error;
    REQUIRE(world.LoadSchema(QByteArrayLiteral("{\"version\":1,\"id\":\"ludus.world.default.v1\",\"label\":\"World\","
                                               "\"components\":[],\"assets\":[]}"),
                             error) == true);
    REQUIRE(world.Load(tests::ReadTestFile(QDir(root).filePath(QStringLiteral("assets/staircase2.world.json"))),
                       error) == true);
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
    const QSize extent(512, 225);
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    const QString navigationPath = qEnvironmentVariable("LUDUS_REFERENCE_TEST_NAVIGATION");
    if (navigationPath.isEmpty() == false)
    {
        preview.RestoreNavigation(QJsonDocument::fromJson(tests::ReadTestFile(navigationPath)).object());
    }
    renderer::ViewDescription view;
    REQUIRE(
        preview.DescribeView(static_cast<uint32>(extent.width()), static_cast<uint32>(extent.height()), view, error) ==
        true);
    const int32 requestedSamples = qEnvironmentVariableIntValue("LUDUS_REFERENCE_TEST_SAMPLES");
    constexpr uint32 DEFAULT_SUPPLIED_SAMPLES = 64;
    constexpr int64 SUPPLIED_CAPTURE_TIMEOUT_MILLISECONDS = 600000;
    REQUIRE(preview.Start() == true);
    REQUIRE(preview.AdmitOffscreenDevice() == true);
    for (const renderer::ReferenceTraversal traversal :
         {renderer::ReferenceTraversal::CpuOracle, renderer::ReferenceTraversal::NativeQueries})
    {
        ViewportReferenceSettings settings;
        settings.Samples = requestedSamples > 0 ? static_cast<uint32>(requestedSamples) : DEFAULT_SUPPLIED_SAMPLES;
        settings.MaxDepth = 8;
        settings.Traversal = traversal;
        ViewportReferenceCapture capture;
        REQUIRE(capture.Start(preview.GetDevice(), preview.GetSurface(), scene, view, extent, settings, error) == true);
        const ViewportReferencePhase phase = RunToEnd(capture, SUPPLIED_CAPTURE_TIMEOUT_MILLISECONDS);
        INFO(capture.GetError().toStdString());
        renderer::ReferenceCaptureInfo information;
        REQUIRE(capture.GetInfo(information) == true);
        INFO("traversal=" << static_cast<uint32>(traversal) << " committed=" << information.CommittedSamples);
        CHECK(phase == ViewportReferencePhase::Complete);
        CHECK(information.Complete == true);
        CHECK(information.InvalidContributions == 0);
        CHECK(information.MinimumSamples == settings.Samples);
        CHECK(information.CommittedSamples ==
              static_cast<uint64>(extent.width()) * static_cast<uint64>(extent.height()) * settings.Samples);
    }
    preview.Stop();
    rhi::Shutdown();
}
