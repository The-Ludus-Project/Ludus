// Play in the viewport, P6b: a hosted game's frames as a PreviewViewport frame source.
// The GPU case plays the editor against the RHI test producer, an ordinary RHI
// application that renders a known colour into its hosted window
// (modules/graphics/rhi/tests/hosted_producer.cpp).
#include "internal/play_viewport_source.h"
#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/world_workspace.h"

#if defined(LUDUS_WORLD_DEMO_PLAYER)
#    include "internal/level.h"
#endif

#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/hosted_window.h>

#if defined(LUDUS_HOSTED_PRODUCER)
#    include "hosted_test_protocol.h"
#endif

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QImage>
#include <QJsonObject>
#include <QKeyEvent>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQuickWindow>
#include <QTemporaryDir>
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

/// A device identity that is never dereferenced: only an import would touch it.
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
#if defined(Q_OS_MACOS)
/// The API the viewport shares with Qt on this target, and one this build cannot import onto.
constexpr rhi::Backend NATIVE_API = rhi::Backend::Metal;
constexpr rhi::Backend FOREIGN_API = rhi::Backend::Vulkan;
#else
constexpr rhi::Backend NATIVE_API = rhi::Backend::Vulkan;
constexpr rhi::Backend FOREIGN_API = rhi::Backend::Metal;
#endif
/// Whether Qt presents on a desktop display through the shared GPU device on this target.
[[maybe_unused]] bool IsSharedDisplay()
{
    const QString platform = QGuiApplication::platformName();
#if defined(Q_OS_MACOS)
    return platform == QStringLiteral("cocoa");
#else
    return (platform == QStringLiteral("xcb") || platform.startsWith(QStringLiteral("wayland")) == true) &&
           qEnvironmentVariable("LUDUS_EDITOR_VIEWPORT") != QStringLiteral("offscreen");
#endif
}
} // namespace

TEST_CASE("Play in the viewport says why it cannot use a device", "[editor][play][viewport]")
{
    PlayViewportSource source;
    // Browser APIs have no hosted surface.
    CHECK_FALSE(source.Start(SOCKET_PARENT, MakeUntouchedIdentity(rhi::Backend::WebGPU), 64, 48, 1));
    CHECK(source.GetState() == PlaySourceState::Unavailable);
    CHECK(source.GetUnavailableReason() ==
          QStringLiteral("Play in the viewport needs a Metal or Vulkan viewport device."));
    CHECK(source.GetChannelAddress().isEmpty());
    source.Stop();
    // This build imports game images onto its own platform's API only.
    CHECK_FALSE(source.Start(SOCKET_PARENT, MakeUntouchedIdentity(FOREIGN_API), 64, 48, 1));
    CHECK(source.GetState() == PlaySourceState::Unavailable);
    CHECK(source.GetUnavailableReason() ==
          QStringLiteral("Play in the viewport cannot import game images on this GPU device."));
    CHECK(source.GetChannelAddress().isEmpty());
    // Game frames are not RHI images, so readback presentation is never offered.
    ludus::graphics::renderer::ViewportFrame frame;
    ludus::graphics::renderer::ViewportImage image;
    CHECK_FALSE(source.TakeFrame(frame, image));
    source.Stop();
    CHECK(source.GetState() == PlaySourceState::Stopped);
}

#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
TEST_CASE("A play source opens a private window channel and closes it on Stop", "[editor][play][viewport]")
{
    PlayViewportSource source;
    int32 resets = 0;
    QObject::connect(&source, &ViewportFrameSource::ResettingFrames, &source, [&resets]() { ++resets; });
    REQUIRE(source.Start(SOCKET_PARENT, MakeUntouchedIdentity(NATIVE_API), 64, 48, 2));
    CHECK(source.GetState() == PlaySourceState::Listening);
    CHECK(source.GetChannelAddress().startsWith(QStringLiteral("1:")));
    CHECK(source.GetEndpoint().GetState() == ludus::platform::HostedConnectionState::Listening);
    CHECK_FALSE(source.HasPendingFrame());
    // A second Start while a session is open is refused.
    CHECK_FALSE(source.Start(SOCKET_PARENT, MakeUntouchedIdentity(NATIVE_API), 64, 48, 2));
    source.Stop();
    CHECK(resets == 1);
    CHECK(source.GetState() == PlaySourceState::Stopped);
    CHECK(source.GetChannelAddress().isEmpty());
}
#endif

TEST_CASE("A hosted game's frames appear in the viewport and return to the game", "[editor][play][viewport][gpu]")
{
#if defined(LUDUS_HOSTED_PRODUCER) && (defined(Q_OS_MACOS) || defined(Q_OS_LINUX))
    if (IsSharedDisplay() == false)
    {
        SKIP("Play in the viewport presents through the shared GPU device (run with QT_QPA_PLATFORM=cocoa on macOS, "
             "xcb or wayland on Linux)");
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
    if (viewport.CanImportHostedImages() == false)
    {
        SKIP("The viewport's device cannot import hosted images (no VK_KHR_external_memory_fd)");
    }

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
    SKIP("Needs macOS or Linux, the native GPU preview and the RHI hosted test producer");
#endif
}

#if defined(LUDUS_WORLD_DEMO_PLAYER) && (defined(Q_OS_MACOS) || defined(Q_OS_LINUX))
namespace
{
#    if defined(Q_OS_MACOS)
/// macOS kVK_ANSI_D, Qt's native virtual key: the World-demo player moves right while it is held.
constexpr uint32 NATIVE_KEY_D = 0x02;
#    else
/// Linux evdev KEY_D as Qt's native scan code, an XKB key code (evdev plus eight).
constexpr uint32 EVDEV_KEY_D = 32;
constexpr uint32 XKB_KEYCODE_OFFSET = 8;
constexpr uint32 NATIVE_KEY_D = EVDEV_KEY_D + XKB_KEYCODE_OFFSET;
#    endif
/// How long the journey holds D, so the player moves a visible distance.
constexpr int32 HOLD_MILLISECONDS = 600;

/// The orange player through the Renderer's display transform (docs/examples/world-demo.md).
bool IsPlayerPixel(const QColor& color)
{
    return color.red() >= 175 && color.red() <= 200 && color.green() >= 155 && color.green() <= 185 &&
           color.blue() >= 85 && color.blue() <= 118;
}
/// Mean x of the player's pixels in the viewport's current image, or a negative value when absent.
float64 GetPlayerCentroid()
{
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() != QStringLiteral("LudusGpuViewport"))
        {
            continue;
        }
        const QImage image = static_cast<QQuickWindow*>(window)->grabWindow();
        float64 sum = 0;
        int32 count = 0;
        for (int32 y = 0; y < image.height(); ++y)
        {
            for (int32 x = 0; x < image.width(); ++x)
            {
                if (IsPlayerPixel(image.pixelColor(x, y)) == true)
                {
                    sum += x;
                    ++count;
                }
            }
        }
        return count == 0 ? -1 : sum / count;
    }
    return -1;
}
/// Send a key as the platform reports it: macOS by virtual key, X11 and Wayland by scan code.
void SendNativeKey(QWidget& inoutTarget, QEvent::Type type, int key, uint32 nativeKey, Qt::KeyboardModifiers modifiers)
{
#    if defined(Q_OS_MACOS)
    QKeyEvent event(type, key, modifiers, 0, nativeKey, 0);
#    else
    QKeyEvent event(type, key, modifiers, nativeKey, 0, 0);
#    endif
    QCoreApplication::sendEvent(&inoutTarget, &event);
}
} // namespace
#endif

TEST_CASE("Play runs the World-demo game in the World viewport, with keys, resize, Stop and a crash",
          "[editor][play][viewport][gpu][.play-native]")
{
#if defined(LUDUS_WORLD_DEMO_PLAYER) && (defined(Q_OS_MACOS) || defined(Q_OS_LINUX))
    // Run it on the native display: cocoa on macOS, xcb or wayland on Linux.
    REQUIRE(IsSharedDisplay());
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path =
        QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(QStringLiteral("room.world.json"));
    const std::string_view sample = ludus::world_demo::ExampleLevel();
    {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write(sample.data(), static_cast<int64>(sample.size())) == static_cast<int64>(sample.size()));
    }
    WorldWorkspace workspace;
    workspace.resize(1200, 850);
    workspace.show();
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(path));
    RenderPreview& preview = workspace.GetRenderPreview();
    REQUIRE(AwaitCondition([&preview]() { return preview.Running() == true && preview.Frames() >= 2; }));
    // PreviewViewport has no Q_OBJECT; the bound viewport is found by its object name.
    PreviewViewport* viewport =
        static_cast<PreviewViewport*>(workspace.findChild<QWidget*>(QStringLiteral("worldViewport")));
    REQUIRE(viewport != nullptr);
    const auto startGame = [&](QProcess& inoutGame) {
        QString address;
        const PlayWindow window = workspace.PreparePlay(PlayMode::InViewport, address);
        INFO(workspace.GetPlayUnavailableText().toStdString());
        REQUIRE(window == PlayWindow::InViewport);
        REQUIRE(address.isEmpty() == false);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QString::fromLatin1(ludus::platform::HOSTED_WINDOW_ENVIRONMENT_VARIABLE), address);
        inoutGame.setProcessEnvironment(environment);
        inoutGame.start(QStringLiteral(LUDUS_WORLD_DEMO_PLAYER), {QStringLiteral("--level"), path});
        REQUIRE(inoutGame.waitForStarted() == true);
        // The viewport switches to the game at its first frame.
        REQUIRE(AwaitCondition([&]() { return &viewport->GetFrameSource() != &preview; }));
        const uint64 shown = viewport->PresentedFrames();
        REQUIRE(AwaitCondition([&]() { return viewport->PresentedFrames() >= shown + PRESENTED_FRAMES; }));
    };

    QProcess game;
    startGame(game);
    CHECK(workspace.IsPlayingInViewport());
    // The game draws its own room: the player is on screen in the game's own frame.
    const float64 before = GetPlayerCentroid();
    REQUIRE(before >= 0);

    // The viewport is the game's window: focus, then a held physical D moves the player right.
    QFocusEvent focusIn(QEvent::FocusIn, Qt::MouseFocusReason);
    QCoreApplication::sendEvent(viewport, &focusIn);
    SendNativeKey(*viewport, QEvent::KeyPress, Qt::Key_D, NATIVE_KEY_D, Qt::NoModifier);
    QTest::qWait(HOLD_MILLISECONDS);
    SendNativeKey(*viewport, QEvent::KeyRelease, Qt::Key_D, NATIVE_KEY_D, Qt::NoModifier);
    const uint64 moved = viewport->PresentedFrames();
    REQUIRE(AwaitCondition([&]() { return viewport->PresentedFrames() >= moved + PRESENTED_FRAMES; }));
    const float64 after = GetPlayerCentroid();
    CHECK(after > before);

    // Editor shortcuts do not take the game's keys while it has the keyboard.
    QKeyEvent shortcut(QEvent::ShortcutOverride, Qt::Key_S, Qt::ControlModifier);
    QCoreApplication::sendEvent(viewport, &shortcut);
    CHECK(shortcut.isAccepted());
    // Shift+F6 stays the editor's Stop shortcut.
    QKeyEvent stop(QEvent::ShortcutOverride, Qt::Key_F6, Qt::ShiftModifier);
    stop.ignore();
    QCoreApplication::sendEvent(viewport, &stop);
    CHECK_FALSE(stop.isAccepted());

    // Resizing the World viewport resizes the game's window; its frames keep coming.
    workspace.resize(1000, 700);
    const uint64 resized = viewport->PresentedFrames();
    CHECK(AwaitCondition([&]() { return viewport->PresentedFrames() >= resized + PRESENTED_FRAMES; }));

    // Stop closes the game's window like its close button; the game exits normally.
    workspace.RequestPlayClose();
    REQUIRE(game.waitForFinished(TIMEOUT_MILLISECONDS) == true);
    CHECK(game.exitStatus() == QProcess::NormalExit);
    CHECK(game.exitCode() == 0);
    REQUIRE(AwaitCondition([&]() { return workspace.IsPlayingInViewport() == false; }));
    CHECK(&viewport->GetFrameSource() == &preview);
    const uint64 edited = viewport->PresentedFrames();
    CHECK(AwaitCondition([&]() { return viewport->PresentedFrames() >= edited + PRESENTED_FRAMES; }));

    // A crash ends Play too: the viewport edits again and the World is untouched.
    const QJsonObject draft = workspace.Document().Draft();
    QProcess crashing;
    startGame(crashing);
    crashing.kill();
    REQUIRE(crashing.waitForFinished(TIMEOUT_MILLISECONDS) == true);
    REQUIRE(AwaitCondition([&]() { return workspace.IsPlayingInViewport() == false; }));
    CHECK(&viewport->GetFrameSource() == &preview);
    CHECK(workspace.Document().Draft() == draft);
    const uint64 recovered = viewport->PresentedFrames();
    CHECK(AwaitCondition([&]() { return viewport->PresentedFrames() >= recovered + PRESENTED_FRAMES; }));
    workspace.StopPreview();
#else
    SKIP("Needs macOS or Linux, the native GPU preview and the World-demo player");
#endif
}
