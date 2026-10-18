#include "internal/preview_shading.h"
#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "viewport_fixture.h"

#include <ludus/graphics/rhi/rhi.h>

#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;

namespace
{
/// Frames the shared viewport must present after Lit preparation completes.
constexpr uint64 PRESENTED_FRAMES = 3;
/// Upper bound for Lit preparation, the shadow atlas and presentation on an integrated GPU.
constexpr int32 SHARED_TIMEOUT_MILLISECONDS = 60000;
/// Qt event-loop slice between preview ticks while waiting for frames.
constexpr int32 POLL_MILLISECONDS = 20;
/// Embedded viewport size in device-independent pixels.
constexpr int32 VIEWPORT_WIDTH = 640;
constexpr int32 VIEWPORT_HEIGHT = 420;
/// Time a viewport stays hidden, long enough for the compositor to unmap its surfaces.
constexpr int32 HIDDEN_MILLISECONDS = 200;
/// Hide/show round-trips per case; repeats catch state that only one cycle restores.
constexpr int32 VISIBILITY_CYCLES = 2;
/// Grabs compared with the reference while the engine keeps replacing leased images.
constexpr int32 TURNOVER_GRABS = 24;
/// Pause steps between turnover grabs, cycled so grabs land at varied frame phases.
constexpr int32 TURNOVER_PAUSE_STEPS = 4;
/// Pause added per step between turnover grabs, in milliseconds.
constexpr int32 TURNOVER_PAUSE_MILLISECONDS = 8;
/// Alpha of every pixel in a correctly read back opaque 8-bit grab.
constexpr int32 OPAQUE_ALPHA = 255;
/// Whether every pixel of the grab is opaque. Qt 6.4 wraps every non-BGRA8 swapchain
/// readback as RGBA8888, so a grab of an RGBA16F or RGB10A2 swapchain (Mesa lists
/// RGBA16F first for Intel on GNOME Wayland) has non-opaque bytes. Only an opaque
/// grab of this opaque window reports the pixels Qt actually presents.
bool IsOpaque(const QImage& image)
{
    for (int32 y = 0; y < image.height(); ++y)
    {
        for (int32 x = 0; x < image.width(); ++x)
        {
            if (qAlpha(image.pixel(x, y)) != OPAQUE_ALPHA)
            {
                return false;
            }
        }
    }
    return true;
}
/// Whether the grab holds a single color, such as only the clear color.
bool IsUniform(const QImage& image)
{
    const QRgb first = image.pixel(0, 0);
    for (int32 y = 0; y < image.height(); ++y)
    {
        for (int32 x = 0; x < image.width(); ++x)
        {
            if (image.pixel(x, y) != first)
            {
                return false;
            }
        }
    }
    return true;
}
QWindow* FindWindow(const QString& name)
{
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == name)
        {
            return window;
        }
    }
    return nullptr;
}
/// Waits until the viewport has presented target frames, the preview stops, or the timeout expires.
bool AwaitPresentedFrames(const PreviewViewport& viewport, const RenderPreview& preview, uint64 target)
{
    QElapsedTimer wait;
    wait.start();
    while (viewport.PresentedFrames() < target && preview.Running() == true &&
           wait.elapsed() < SHARED_TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    return viewport.PresentedFrames() >= target;
}
/// Returns why shared presentation cannot run on this display, or an empty string when it can.
QString GetSharedDisplaySkipReason()
{
    const QString platform = QGuiApplication::platformName();
    if (platform != QStringLiteral("xcb") && platform.startsWith(QStringLiteral("wayland")) == false)
    {
        return QStringLiteral("Shared presentation needs an X11 or Wayland display (run with QT_QPA_PLATFORM=xcb or "
                              "wayland)");
    }
    if (qEnvironmentVariable("LUDUS_EDITOR_VIEWPORT") == QStringLiteral("offscreen"))
    {
        return QStringLiteral("LUDUS_EDITOR_VIEWPORT=offscreen forces readback presentation");
    }
    return {};
}
} // namespace

TEST_CASE("Linux viewport shares an editor-created Vulkan device with Qt Quick", "[editor][viewport][shared][gpu]")
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW) && defined(Q_OS_LINUX)
    const QString skipReason = GetSharedDisplaySkipReason();
    if (skipReason.isEmpty() == false)
    {
        SKIP(skipReason.toStdString());
    }
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
    tests::MakeLitScene(QFileInfo(directory.path()).canonicalFilePath(), scene);
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport) == true);
    REQUIRE(preview.SetScene(scene) == true);
    preview.SetShading(PreviewShading::Lit);
    REQUIRE(PreviewViewport::GetPresentation() == ViewportPresentation::SharedDevice);
    REQUIRE(preview.Start() == true);
    QElapsedTimer timer;
    timer.start();
    while (viewport.PresentedFrames() < PRESENTED_FRAMES && timer.elapsed() < SHARED_TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    INFO(preview.Status().toStdString());
    // The shared path presents through Qt Quick, never through the readback canvas.
    CHECK(viewport.findChild<QWidget*>(QStringLiteral("LudusOffscreenViewport")) == nullptr);
    QWindow* window = FindWindow(QStringLiteral("LudusGpuViewport"));
    REQUIRE(window != nullptr);
    REQUIRE(preview.Running() == true);
    REQUIRE(viewport.PresentedFrames() >= PRESENTED_FRAMES);
    CHECK(preview.Status().contains(QStringLiteral("Lit: %1 lights").arg(tests::LAMP_COUNT)) == true);
    const auto awaitFrames = [&viewport, &preview](uint64 target) {
        return AwaitPresentedFrames(viewport, preview, target);
    };
    // Mode changes and resizes keep presenting through the same adopted device.
    preview.SetShading(PreviewShading::Solid);
    CHECK(awaitFrames(viewport.PresentedFrames() + PRESENTED_FRAMES) == true);
    // The renderer redraws the same Solid view every tick into alternating leased
    // images, and Vulkan's invariance rules make each replacement identical. Qt 6.4's
    // grabWindow syncs and renders without beforeFrameBegin/afterFrameEnd and often
    // promotes the pending frame itself, the only Qt Quick path outside that bracket;
    // its pixels must still equal the presented view. This checks presentation, not
    // lease timing: the scene graph samples only the active, still-leased image, and
    // RHI and Qt submit on one serialized queue, so even a premature release could
    // show only a newer frame, never torn pixels. Lit is not compared because its
    // viewport keeps presenting the authored lighting.
    QQuickWindow* quickWindow = static_cast<QQuickWindow*>(window);
    const QImage reference = quickWindow->grabWindow();
    REQUIRE(reference.isNull() == false);
    if (IsOpaque(reference) == false)
    {
        WARN("Pixels not compared: Qt misreads this swapchain format in grabWindow");
    }
    else
    {
        CHECK(IsUniform(reference) == false);
        const uint64 turnoverStart = viewport.PresentedFrames();
        int32 mismatches = 0;
        for (int32 i = 0; i < TURNOVER_GRABS; ++i)
        {
            QTest::qWait(i % TURNOVER_PAUSE_STEPS * TURNOVER_PAUSE_MILLISECONDS);
            if (quickWindow->grabWindow() != reference)
            {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
        CHECK(viewport.PresentedFrames() > turnoverStart);
    }
    viewport.resize(VIEWPORT_WIDTH / 2, VIEWPORT_HEIGHT / 2);
    CHECK(awaitFrames(viewport.PresentedFrames() + PRESENTED_FRAMES) == true);
    // Stop retires the RHI and Qt Quick before the editor destroys its device;
    // a restart creates and shares a fresh device.
    preview.Stop();
    QTest::qWait(POLL_MILLISECONDS);
    CHECK(FindWindow(QStringLiteral("LudusGpuViewport")) == nullptr);
    REQUIRE(preview.Start() == true);
    CHECK(awaitFrames(viewport.PresentedFrames() + PRESENTED_FRAMES) == true);
    CHECK(FindWindow(QStringLiteral("LudusGpuViewport")) != nullptr);
    CHECK(viewport.findChild<QWidget*>(QStringLiteral("LudusOffscreenViewport")) == nullptr);
    preview.Stop();
#else
    SKIP("Editor-created Vulkan device sharing is Linux-only");
#endif
}

TEST_CASE("Linux shared viewport keeps presenting after it is hidden and shown", "[editor][viewport][shared][gpu]")
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW) && defined(Q_OS_LINUX)
    const QString skipReason = GetSharedDisplaySkipReason();
    if (skipReason.isEmpty() == false)
    {
        SKIP(skipReason.toStdString());
    }
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
    tests::MakeLitScene(QFileInfo(directory.path()).canonicalFilePath(), scene);
    RenderPreview preview;
    // The host stands in for the editor window: hiding only the viewport is a tab
    // or page switch, hiding the host also unmaps the parent of the Qt Quick window.
    QWidget host;
    QVBoxLayout* layout = new QVBoxLayout(&host);
    PreviewViewport* viewport = new PreviewViewport(&preview, &host);
    layout->addWidget(viewport);
    host.resize(VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
    host.show();
    REQUIRE(QTest::qWaitForWindowExposed(&host) == true);
    REQUIRE(preview.SetScene(scene) == true);
    preview.SetShading(PreviewShading::Solid);
    REQUIRE(PreviewViewport::GetPresentation() == ViewportPresentation::SharedDevice);
    REQUIRE(preview.Start() == true);
    REQUIRE(AwaitPresentedFrames(*viewport, preview, PRESENTED_FRAMES) == true);
    QWindow* window = FindWindow(QStringLiteral("LudusGpuViewport"));
    REQUIRE(window != nullptr);
    const auto checkResumes = [&](QWidget& hidden) {
        for (int32 cycle = 0; cycle < VISIBILITY_CYCLES; ++cycle)
        {
            hidden.hide();
            QTest::qWait(HIDDEN_MILLISECONDS);
            hidden.show();
            REQUIRE(QTest::qWaitForWindowExposed(&host) == true);
            INFO("cycle " << cycle << ": " << preview.Status().toStdString());
            REQUIRE(preview.Running() == true);
            // Qt Quick renders only an exposed window; without exposure the view freezes.
            CHECK(AwaitPresentedFrames(*viewport, preview, viewport->PresentedFrames() + PRESENTED_FRAMES) == true);
            CHECK(window->isVisible() == true);
            CHECK(window->isExposed() == true);
        }
    };
    SECTION("viewport page switch")
    {
        checkResumes(*viewport);
    }
    SECTION("editor window hide")
    {
        checkResumes(host);
    }
    // Shared presentation stays on the adopted device, never the readback canvas.
    CHECK(FindWindow(QStringLiteral("LudusGpuViewport")) == window);
    CHECK(viewport->findChild<QWidget*>(QStringLiteral("LudusOffscreenViewport")) == nullptr);
    preview.Stop();
#else
    SKIP("Editor-created Vulkan device sharing is Linux-only");
#endif
}
