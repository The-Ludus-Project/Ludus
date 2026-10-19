#include "internal/offscreen_frame_reader.h"
#include "internal/preview_shading.h"
#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/viewport_statistics.h"
#include "viewport_fixture.h"

#include <ludus/foundation/time/time.hpp>

#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/rhi/rhi.h>

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QtGlobal>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;

namespace
{
/// Width wider than one staging row alignment so bands carry row padding.
constexpr uint32 FRAME_WIDTH = 300;
/// Height spanning several 256 KiB transfer slices at FRAME_WIDTH.
constexpr uint32 FRAME_HEIGHT = 700;
constexpr usize BYTES_PER_TEXEL = 4;
/// Upper bound for one asynchronous copy on a software rasterizer.
constexpr uint64 TIMEOUT_SECONDS = 20;
/// Upper bound for lit preparation, the shadow atlas and two frames on a software rasterizer.
constexpr int32 LIT_TIMEOUT_MILLISECONDS = 120000;
/// Qt event-loop slice between preview ticks while waiting for frames.
constexpr int32 POLL_MILLISECONDS = 20;
/// Debug-view capture size: small enough for lavapipe, large enough to resolve every cube face.
constexpr int32 DEBUG_FRAME_WIDTH = 160;
constexpr int32 DEBUG_FRAME_HEIGHT = 120;
/// Depth and UV vary continuously, so they show many more colours than background plus faces.
constexpr qsizetype MINIMUM_VARYING_COLOURS = 8;
/// Flat cube faces: background plus at least the three faces a corner view always shows.
constexpr qsizetype MINIMUM_NORMAL_COLOURS = 4;
/// Directory for PNG captures of each debug view; captures are skipped when unset.
constexpr const char* DEBUG_SHOT_DIRECTORY_VARIABLE = "LUDUS_VIEWPORT_DEBUG_SHOTS";
uint8 GetPatternByte(uint32 x, uint32 y, uint32 channel)
{
    return static_cast<uint8>((x * 7U + y * 13U + channel * 61U) & 0xFFU);
}
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW) && defined(Q_OS_LINUX)
/// Wait for frames rendered in the current shading mode, then copy the newest one to the CPU.
QImage CaptureFrame(RenderPreview& inoutPreview)
{
    QElapsedTimer timer;
    timer.start();
    while ((inoutPreview.Frames() < 2 || inoutPreview.HasPendingFrame() == false) && inoutPreview.Running() == true &&
           timer.elapsed() < LIT_TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    ludus::graphics::renderer::ViewportFrame frame;
    ludus::graphics::renderer::ViewportImage image;
    if (inoutPreview.HasPendingFrame() == false || inoutPreview.TakeFrame(frame, image) == false)
    {
        return {};
    }
    OffscreenFrameReader reader;
    QImage result;
    if (reader.Begin(inoutPreview.GetDevice(), image.Texture, image.Width, image.Height) == true)
    {
        OffscreenReadStatus status = OffscreenReadStatus::Pending;
        while (status == OffscreenReadStatus::Pending && timer.elapsed() < LIT_TIMEOUT_MILLISECONDS)
        {
            static_cast<void>(rhi::PollLifetime(inoutPreview.GetDevice()));
            status = reader.Poll();
        }
        if (status == OffscreenReadStatus::Complete)
        {
            result = reader.TakeImage();
        }
        reader.Cancel();
    }
    inoutPreview.ReleaseFrame(frame);
    return result;
}
qsizetype CountColours(const QImage& image)
{
    QSet<QRgb> colours;
    for (int32 y = 0; y < image.height(); ++y)
    {
        for (int32 x = 0; x < image.width(); ++x)
        {
            colours.insert(image.pixel(x, y));
        }
    }
    return colours.size();
}
/// Render every debug view of scene on lavapipe; each must differ from the others and from Lit.
void CheckDebugViews(const RenderScene& scene, const QString& captureSuffix)
{
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    preview.SetStatisticsVisible(true);
    preview.Resize(DEBUG_FRAME_WIDTH, DEBUG_FRAME_HEIGHT);
    REQUIRE(preview.Start() == true);
    if (preview.AdmitOffscreenDevice() == false)
    {
        SKIP("No Vulkan device (install lavapipe for this smoke test)");
    }
    const QString shotDirectory = qEnvironmentVariable(DEBUG_SHOT_DIRECTORY_VARIABLE);
    if (shotDirectory.isEmpty() == false)
    {
        REQUIRE(QDir().mkpath(shotDirectory) == true);
    }
    QSet<QByteArray> images;
    for (usize i = 0; i < PREVIEW_SHADING_COUNT; ++i)
    {
        const PreviewShading shading = static_cast<PreviewShading>(i);
        const QString key = GetPreviewShadingKey(shading);
        INFO(key.toStdString());
        preview.SetShading(shading);
        const QImage image = CaptureFrame(preview);
        INFO(preview.Status().toStdString());
        REQUIRE(preview.Running() == true);
        REQUIRE(image.isNull() == false);
        CHECK(image.width() == DEBUG_FRAME_WIDTH);
        CHECK(image.height() == DEBUG_FRAME_HEIGHT);
        if (shotDirectory.isEmpty() == false)
        {
            CHECK(image.save(QStringLiteral("%1/%2%3.png").arg(shotDirectory, key, captureSuffix)) == true);
        }
        if (shading == PreviewShading::Depth || shading == PreviewShading::Uv)
        {
            CHECK(CountColours(image) >= MINIMUM_VARYING_COLOURS);
        }
        if (shading == PreviewShading::Normal)
        {
            CHECK(CountColours(image) >= MINIMUM_NORMAL_COLOURS);
        }
        if (IsPreviewDebugShading(shading) == true)
        {
            CHECK(preview.Status().contains(GetPreviewShadingStatus(shading)) == true);
        }
        // An unlit World has no lights: Lit falls back to Albedo, and Light count is honestly black
        // and may match Metallic 0, so only the remaining views must be unique there.
        if (scene.Lit == true || (shading != PreviewShading::LightCount && shading != PreviewShading::Lit))
        {
            const QByteArray bytes(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes());
            CHECK(images.contains(bytes) == false);
            images.insert(bytes);
        }
    }
    // Statistics were gathered while the views rendered; the GPU sample is read back with latency.
    const ViewportStatistics& statistics = preview.GetStatistics();
    CHECK(statistics.CpuMilliseconds.GetCount() > 0);
    CHECK(statistics.Draws > 0);
    CHECK(statistics.Width == static_cast<uint32>(DEBUG_FRAME_WIDTH));
    CHECK(statistics.Height == static_cast<uint32>(DEBUG_FRAME_HEIGHT));
    CHECK(FormatViewportStatistics(statistics).startsWith(QStringLiteral("GPU: ")) == true);
    preview.Stop();
}
#endif
} // namespace

TEST_CASE("Offscreen presentation is forced by the environment on Linux", "[editor][viewport][offscreen]")
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW) && defined(Q_OS_LINUX)
    const QByteArray previous = qgetenv("LUDUS_EDITOR_VIEWPORT");
    qputenv("LUDUS_EDITOR_VIEWPORT", QByteArrayLiteral("offscreen"));
    CHECK(PreviewViewport::GetPresentation() == ViewportPresentation::OffscreenReadback);
    CHECK(PreviewViewport::CanPresent() == true);
    if (previous.isEmpty() == true)
    {
        qunsetenv("LUDUS_EDITOR_VIEWPORT");
    }
    else
    {
        qputenv("LUDUS_EDITOR_VIEWPORT", previous);
    }
#else
    SKIP("Offscreen readback presentation is Linux-only");
#endif
}

TEST_CASE("Offscreen frame reader copies a texture in bands without waiting", "[editor][viewport][offscreen][gpu]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
    if (started != rhi::DeviceStatus::Ready)
    {
        SKIP("No Vulkan device (install lavapipe for this smoke test)");
    }
    QByteArray pixels(static_cast<qsizetype>(usize{FRAME_WIDTH} * FRAME_HEIGHT * BYTES_PER_TEXEL), '\0');
    for (uint32 y = 0; y < FRAME_HEIGHT; ++y)
    {
        for (uint32 x = 0; x < FRAME_WIDTH; ++x)
        {
            for (uint32 channel = 0; channel < BYTES_PER_TEXEL; ++channel)
            {
                const usize index = (usize{y} * FRAME_WIDTH + x) * BYTES_PER_TEXEL + channel;
                pixels[static_cast<qsizetype>(index)] = static_cast<char>(GetPatternByte(x, y, channel));
            }
        }
    }
    rhi::TextureHandle texture;
    const rhi::RasterStatus created = rhi::CreateTexture(
        device,
        { .Width = FRAME_WIDTH, .Height = FRAME_HEIGHT, .Format = rhi::RasterFormat::Rgba8Unorm },
        {
            .Bytes = {reinterpret_cast<const uint8*>(pixels.constData()), static_cast<usize>(pixels.size())},
            .RowPitch = usize{FRAME_WIDTH} * BYTES_PER_TEXEL,
        },
        texture);
    REQUIRE((created == rhi::RasterStatus::Ready || created == rhi::RasterStatus::Pending));
    const uint64 start = time::NowTicks();
    while (rhi::GetStatus(device, texture) == rhi::RasterStatus::Pending &&
           time::NowTicks() - start < TIMEOUT_SECONDS * time::NANOSECONDS_PER_SECOND)
    {
        static_cast<void>(rhi::PollLifetime(device));
    }
    REQUIRE(rhi::GetStatus(device, texture) == rhi::RasterStatus::Ready);

    OffscreenFrameReader reader;
    CHECK(reader.Poll() == OffscreenReadStatus::Idle);
    REQUIRE(reader.Begin(device, texture, FRAME_WIDTH, FRAME_HEIGHT) == true);
    CHECK(reader.Begin(device, texture, FRAME_WIDTH, FRAME_HEIGHT) == false);
    OffscreenReadStatus status = OffscreenReadStatus::Pending;
    uint32 polls = 0;
    while (status == OffscreenReadStatus::Pending &&
           time::NowTicks() - start < TIMEOUT_SECONDS * time::NANOSECONDS_PER_SECOND)
    {
        static_cast<void>(rhi::PollLifetime(device));
        status = reader.Poll();
        ++polls;
    }
    REQUIRE(status == OffscreenReadStatus::Complete);
    CHECK(polls > 1); // More than one band: the copy never completes inside Begin.
    const QImage image = reader.TakeImage();
    CHECK(reader.IsBusy() == false);
    REQUIRE(image.width() == static_cast<int32>(FRAME_WIDTH));
    REQUIRE(image.height() == static_cast<int32>(FRAME_HEIGHT));
    bool isEqual = true;
    for (uint32 y = 0; y < FRAME_HEIGHT && isEqual == true; ++y)
    {
        const uint8* row = image.constScanLine(static_cast<int32>(y));
        for (uint32 x = 0; x < FRAME_WIDTH && isEqual == true; ++x)
        {
            for (uint32 channel = 0; channel < BYTES_PER_TEXEL; ++channel)
            {
                isEqual = isEqual && row[usize{x} * BYTES_PER_TEXEL + channel] == GetPatternByte(x, y, channel);
            }
        }
    }
    CHECK(isEqual == true);

    REQUIRE(reader.Begin(device, texture, FRAME_WIDTH, FRAME_HEIGHT) == true);
    reader.Cancel();
    CHECK(reader.IsBusy() == false);
    {
        rhi::TextureHandle released = texture;
        CHECK(rhi::Destroy(device, released) == rhi::RasterStatus::Ready);
    }
    CHECK(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
}

TEST_CASE("Lit shading with many shadowed lights renders through the offscreen Vulkan device",
          "[editor][viewport][offscreen][gpu][lighting]")
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW) && defined(Q_OS_LINUX)
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    RenderScene scene;
    tests::MakeLitScene(QFileInfo(directory.path()).canonicalFilePath(), scene, { .HasCollapsedTriangle = true });
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    preview.SetShading(PreviewShading::Lit);
    preview.Resize(96, 64);
    REQUIRE(preview.Start() == true);
    // The Linux readback fallback presents through this device. It must carry the
    // same lighting budgets as the Qt-shared device or the shadow atlas is refused.
    if (preview.AdmitOffscreenDevice() == false)
    {
        SKIP("No Vulkan device (install lavapipe for this smoke test)");
    }
    QElapsedTimer timer;
    timer.start();
    while (preview.Frames() < 2 && preview.Running() == true && timer.elapsed() < LIT_TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    INFO(preview.Status().toStdString());
    REQUIRE(preview.Running() == true);
    REQUIRE(preview.Frames() >= 2);
    CHECK(preview.Status().contains(QStringLiteral("Lit: %1 lights").arg(tests::LAMP_COUNT)) == true);
    CHECK(preview.Status().contains(
              QStringLiteral("%1 shadow faces")
                  .arg(tests::LAMP_COUNT * static_cast<int32>(ludus::graphics::renderer::POINT_SHADOW_FACES))) == true);
    CHECK(preview.Status().contains(QStringLiteral("direct lighting")) == true);
    CHECK(preview.Status().contains(QStringLiteral("bake")) == false);
    preview.Stop();
#else
    SKIP("Offscreen Vulkan presentation is Linux-only");
#endif
}

TEST_CASE("Debug shading views render distinct images on the offscreen Vulkan device",
          "[editor][viewport][offscreen][gpu][shading]")
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW) && defined(Q_OS_LINUX)
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    SECTION("Authored lighting scene")
    {
        RenderScene scene;
        tests::MakeLitScene(QFileInfo(directory.path()).canonicalFilePath(), scene);
        CheckDebugViews(scene, QString());
    }
    SECTION("Unlit authored World")
    {
        RenderScene scene;
        tests::MakeLitScene(QFileInfo(directory.path()).canonicalFilePath(), scene, { .HasLamps = false });
        CheckDebugViews(scene, QStringLiteral("-unlit"));
    }
#else
    SKIP("Offscreen Vulkan presentation is Linux-only");
#endif
}

TEST_CASE("GPU frame timing reaches the statistics overlay without stalling", "[editor][viewport][offscreen][gpu]")
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW) && defined(Q_OS_LINUX)
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    RenderScene scene;
    tests::MakeLitScene(QFileInfo(directory.path()).canonicalFilePath(), scene, { .HasLamps = false });
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    preview.SetStatisticsVisible(true);
    preview.Resize(DEBUG_FRAME_WIDTH, DEBUG_FRAME_HEIGHT);
    REQUIRE(preview.Start() == true);
    if (preview.AdmitOffscreenDevice() == false)
    {
        SKIP("No Vulkan device (install lavapipe for this smoke test)");
    }
    QElapsedTimer timer;
    timer.start();
    while (preview.GetStatistics().GpuState == ViewportGpuTimeState::Waiting && preview.Running() == true &&
           timer.elapsed() < LIT_TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    const ViewportStatistics& statistics = preview.GetStatistics();
    // Either timestamps are measured or the device says they are unavailable; never stuck waiting.
    REQUIRE(statistics.GpuState != ViewportGpuTimeState::Waiting);
    const QString text = FormatViewportStatistics(statistics);
    INFO(text.toStdString());
    if (statistics.GpuState == ViewportGpuTimeState::Available)
    {
        CHECK(statistics.GpuMilliseconds.GetCount() > 0);
        CHECK(text.startsWith(QStringLiteral("GPU: ")) == true);
        CHECK(text.contains(QStringLiteral(" ms")) == true);
    }
    else
    {
        CHECK(text.startsWith(QStringLiteral("GPU: n/a")) == true);
    }
    // Hiding the overlay stops sampling and discards the window.
    preview.SetStatisticsVisible(false);
    CHECK(preview.GetStatistics().CpuMilliseconds.GetCount() == 0);
    preview.Stop();
#else
    SKIP("Offscreen Vulkan presentation is Linux-only");
#endif
}

TEST_CASE("Statistics overlay paints over the offscreen readback canvas",
          "[editor][viewport][offscreen][gpu][statistics]")
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW) && defined(Q_OS_LINUX)
    const QByteArray previous = qgetenv("LUDUS_EDITOR_VIEWPORT");
    qputenv("LUDUS_EDITOR_VIEWPORT", QByteArrayLiteral("offscreen"));
    rhi::Shutdown();
    struct Guard final
    {
        QByteArray Previous;
        ~Guard()
        {
            rhi::Shutdown();
            if (Previous.isEmpty() == true)
            {
                qunsetenv("LUDUS_EDITOR_VIEWPORT");
            }
            else
            {
                qputenv("LUDUS_EDITOR_VIEWPORT", Previous);
            }
        }
    } guard{previous};
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    RenderScene scene;
    tests::MakeLitScene(QFileInfo(directory.path()).canonicalFilePath(), scene, { .HasLamps = false });
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(DEBUG_FRAME_WIDTH * 2, DEBUG_FRAME_HEIGHT * 2);
    viewport.show();
    REQUIRE(preview.SetScene(scene) == true);
    REQUIRE(preview.Start() == true);
    QElapsedTimer timer;
    timer.start();
    while (viewport.PresentedFrames() < 2 && preview.Running() == true && timer.elapsed() < LIT_TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    if (preview.HasDevice() == false)
    {
        SKIP("No Vulkan device (install lavapipe for this smoke test)");
    }
    QWidget* canvas = viewport.findChild<QWidget*>(QStringLiteral("LudusOffscreenViewport"));
    REQUIRE(canvas != nullptr);
    const QImage hidden = canvas->grab().toImage();
    QToolButton* statistics = viewport.findChild<QToolButton*>(QStringLiteral("viewportStatistics"));
    REQUIRE(statistics != nullptr);
    statistics->click();
    REQUIRE(
        QTest::qWaitFor([&viewport]() { return viewport.GetStatisticsText().contains(QStringLiteral("…")) == false; },
                        LIT_TIMEOUT_MILLISECONDS) == true);
    INFO(viewport.GetStatisticsText().toStdString());
    const QImage shown = canvas->grab().toImage();
    const QString shotDirectory = qEnvironmentVariable(DEBUG_SHOT_DIRECTORY_VARIABLE);
    if (shotDirectory.isEmpty() == false)
    {
        REQUIRE(QDir().mkpath(shotDirectory) == true);
        CHECK(shown.save(shotDirectory + QStringLiteral("/stats-overlay-offscreen.png")) == true);
    }
    REQUIRE(hidden.size() == shown.size());
    // The panel sits in the top-left corner; the rest of the frame is unchanged by the overlay.
    const QRect corner(0, 0, shown.width() / 4, shown.height() / 4);
    CHECK(shown.copy(corner) != hidden.copy(corner));
    statistics->click();
    CHECK(viewport.GetStatisticsText().isEmpty() == true);
    preview.Stop();
#else
    SKIP("Offscreen Vulkan presentation is Linux-only");
#endif
}
