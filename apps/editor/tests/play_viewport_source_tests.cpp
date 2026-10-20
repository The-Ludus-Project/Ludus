// Play in the viewport, P6b: a hosted game's frames as a PreviewViewport frame source.
// The GPU case plays the editor against the RHI test producer, an ordinary RHI
// application that renders a known colour into its hosted window
// (modules/graphics/rhi/tests/hosted_producer.cpp).
#include "internal/play_viewport_source.h"
#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"

#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/hosted_window.h>

#if defined(LUDUS_HOSTED_PRODUCER)
#    include "hosted_test_protocol.h"
#endif

#include <QByteArray>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQuickWindow>
#include <QTest>

#include <cstdlib>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;

namespace
{
/// Short parent for the endpoint's socket directory; sockaddr_un paths are at most 103 bytes.
const QString SOCKET_PARENT = QStringLiteral("/tmp");
/// Upper bound for the game to connect, render and close.
constexpr int32 TIMEOUT_MILLISECONDS = 20000;
constexpr int32 POLL_MILLISECONDS = 10;
/// Frames the viewport presents in each phase of the GPU case.
constexpr uint64 PRESENTED_FRAMES = 4;
constexpr int32 VIEWPORT_WIDTH = 320;
constexpr int32 VIEWPORT_HEIGHT = 240;
/// One 8-bit code of Qt's swapchain rounding on top of the producer's own tolerance.
constexpr int32 DISPLAY_TOLERANCE = 2;

template <typename Condition>
bool AwaitCondition(Condition condition)
{
    QElapsedTimer wait;
    wait.start();
    while (condition() == false && wait.elapsed() < TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    return condition();
}

/// A Metal-shaped identity that is never dereferenced: only an import would touch it.
rhi::NativeDeviceIdentity MakeUntouchedIdentity(rhi::Backend api)
{
    static int32 device = 0;
    static int32 queue = 0;
    rhi::NativeDeviceIdentity identity;
    identity.Api = api;
    identity.Device = rhi::NativeLogicalDevice(&device);
    identity.CommandQueue = rhi::NativeCommandQueue(&queue);
    identity.Instance = rhi::NativeInstance(&device);
    identity.PhysicalDevice = rhi::NativePhysicalDevice(&device);
    return identity;
}
} // namespace

TEST_CASE("Play in the viewport says why it is unavailable with Vulkan", "[editor][play][viewport]")
{
    PlayViewportSource source;
    CHECK_FALSE(source.Start(SOCKET_PARENT, MakeUntouchedIdentity(rhi::Backend::Vulkan), 64, 48, 1));
    CHECK(source.GetState() == PlaySourceState::Unavailable);
    CHECK(source.GetUnavailableReason().contains(QStringLiteral("own window")));
    CHECK(source.GetChannelAddress().isEmpty());
    // Game frames are not RHI images, so readback presentation is never offered.
    ludus::graphics::renderer::ViewportFrame frame;
    ludus::graphics::renderer::ViewportImage image;
    CHECK_FALSE(source.TakeFrame(frame, image));
    source.Stop();
    CHECK(source.GetState() == PlaySourceState::Stopped);
}

#if defined(Q_OS_MACOS)
TEST_CASE("A play source opens a private window channel and closes it on Stop", "[editor][play][viewport]")
{
    PlayViewportSource source;
    int32 resets = 0;
    QObject::connect(&source, &ViewportFrameSource::ResettingFrames, &source, [&resets]() { ++resets; });
    REQUIRE(source.Start(SOCKET_PARENT, MakeUntouchedIdentity(rhi::Backend::Metal), 64, 48, 2));
    CHECK(source.GetState() == PlaySourceState::Listening);
    CHECK(source.GetChannelAddress().startsWith(QStringLiteral("1:")));
    CHECK(source.GetEndpoint().GetState() == ludus::platform::HostedConnectionState::Listening);
    CHECK_FALSE(source.HasPendingFrame());
    // A second Start while a session is open is refused.
    CHECK_FALSE(source.Start(SOCKET_PARENT, MakeUntouchedIdentity(rhi::Backend::Metal), 64, 48, 2));
    source.Stop();
    CHECK(resets == 1);
    CHECK(source.GetState() == PlaySourceState::Stopped);
    CHECK(source.GetChannelAddress().isEmpty());
}
#endif

TEST_CASE("A hosted game's frames appear in the viewport and return to the game", "[editor][play][viewport][gpu]")
{
#if defined(LUDUS_HOSTED_PRODUCER) && defined(Q_OS_MACOS)
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
    {
        SKIP("Play in the viewport presents through the Metal-shared device (run with QT_QPA_PLATFORM=cocoa)");
    }
    rhi::Shutdown();
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport) == true);
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("play-source");
    scene.AssetsIdentity = scene.Identity;
    PreviewMesh mesh;
    mesh.Positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    mesh.Uvs = {{0, 0}, {1, 0}, {.5F, 1}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.append(mesh);
    scene.Objects.append(PreviewObject{});
    REQUIRE(preview.SetScene(scene) == true);
    if (preview.Start() == false)
    {
        SKIP("No shared GPU device for the viewport: " << preview.Status().toStdString());
    }
    REQUIRE(AwaitCondition([&viewport]() { return viewport.PresentedFrames() >= 1; }));

    // The game's images live on the viewport's own device: the one Qt samples with.
    rhi::NativeDeviceLease lease;
    REQUIRE(preview.PinDevice(lease) == rhi::RasterStatus::Ready);
    rhi::NativeDeviceInfo device;
    REQUIRE(rhi::GetNativeDeviceInfo(lease, device) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ReleaseNativeDevice(lease) == rhi::RasterStatus::Ready);

    PlayViewportSource play;
    REQUIRE(play.Start(SOCKET_PARENT, device.Identity, 64, 48, 1));
    QProcess game;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromLatin1(ludus::platform::HOSTED_WINDOW_ENVIRONMENT_VARIABLE),
                       play.GetChannelAddress());
    game.setProcessEnvironment(environment);
    game.start(QStringLiteral(LUDUS_HOSTED_PRODUCER), {QStringLiteral("render")});
    REQUIRE(game.waitForStarted() == true);
    REQUIRE(AwaitCondition([&play]() { return play.GetState() == PlaySourceState::Playing; }));

    // Switch only once the game has a frame: the edit image stays until then.
    viewport.SetFrameSource(&play);
    CHECK(&viewport.GetFrameSource() == &play);
    const uint64 switched = viewport.PresentedFrames();
    REQUIRE(AwaitCondition([&]() { return viewport.PresentedFrames() >= switched + PRESENTED_FRAMES; }));
    // More frames than the game's three images: every shown image was returned to it.
    CHECK(play.GetTakenFrames() > ludus::platform::HOSTED_SURFACE_IMAGE_COUNT);
    QQuickWindow* windowOrNull = nullptr;
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport"))
        {
            windowOrNull = static_cast<QQuickWindow*>(window);
        }
    }
    REQUIRE(windowOrNull != nullptr);
    const QImage shown = windowOrNull->grabWindow();
    REQUIRE(shown.isNull() == false);
    const QColor center = shown.pixelColor(shown.width() / 2, shown.height() / 2);
    namespace hosted_test = ludus::graphics::rhi::hosted_test;
    CHECK(std::abs(center.red() - static_cast<int32>(hosted_test::HOSTED_TEST_BYTES[0])) <= DISPLAY_TOLERANCE);
    CHECK(std::abs(center.green() - static_cast<int32>(hosted_test::HOSTED_TEST_BYTES[1])) <= DISPLAY_TOLERANCE);
    CHECK(std::abs(center.blue() - static_cast<int32>(hosted_test::HOSTED_TEST_BYTES[2])) <= DISPLAY_TOLERANCE);

    // Resizing the viewport resizes the game's window; its frames keep coming.
    viewport.resize(VIEWPORT_WIDTH + 80, VIEWPORT_HEIGHT + 40);
    const uint64 resized = viewport.PresentedFrames();
    CHECK(AwaitCondition([&]() { return viewport.PresentedFrames() >= resized + PRESENTED_FRAMES; }));

    // Back to edit: the viewport presents the preview's frames again.
    viewport.SetFrameSource(nullptr);
    CHECK(&viewport.GetFrameSource() == &preview);
    const uint64 edited = viewport.PresentedFrames();
    CHECK(AwaitCondition([&]() { return viewport.PresentedFrames() >= edited + PRESENTED_FRAMES; }));

    // Stopping closes the game's window; the game exits on its own.
    play.Stop();
    REQUIRE(game.waitForFinished(TIMEOUT_MILLISECONDS) == true);
    CHECK(game.exitStatus() == QProcess::NormalExit);
    CHECK(game.exitCode() == 0);
    preview.Stop();
#else
    SKIP("Needs macOS, the native GPU preview and the RHI hosted test producer");
#endif
}
