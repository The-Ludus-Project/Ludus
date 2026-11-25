#include "internal/play_viewport_source.h"
#include "internal/preview_viewport.h"
#include "internal/render_panel.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/viewport_relative_mouse.h"

#include <ludus/foundation/base/types.h>
#include <ludus/platform/hosted_window.h>

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QByteArray>
#include <QFocusEvent>
#include <QJsonObject>
#include <QMouseEvent>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTest>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#if defined(LUDUS_HOSTED_PRODUCER)
#    include "hosted_test_protocol.h"
#endif

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;

NSEvent* MakeRelativeMotion(QPoint position, QPoint delta, float64 timestamp)
{
    CGEventRef native = CGEventCreateMouseEvent(nullptr,
                                                kCGEventRightMouseDragged,
                                                CGPointMake(position.x(), position.y()),
                                                kCGMouseButtonRight);
    REQUIRE(native != nullptr);
    CGEventSetIntegerValueField(native, kCGMouseEventDeltaX, delta.x());
    CGEventSetIntegerValueField(native, kCGMouseEventDeltaY, delta.y());
    constexpr float64 NANOSECONDS_PER_SECOND = 1000000000;
    CGEventSetTimestamp(native, static_cast<CGEventTimestamp>(timestamp * NANOSECONDS_PER_SECOND));
    NSEvent* event = [NSEvent eventWithCGEvent:native];
    REQUIRE(event != nil);
    REQUIRE(event.deltaX == delta.x());
    REQUIRE(event.deltaY == delta.y());
    CFRelease(native);
    return event;
}

bool DispatchRelativeMotion(QPoint position, QPoint delta, float64 timestamp)
{
    NSEvent* event = MakeRelativeMotion(position, delta, timestamp);
    qintptr result = 0;
    const bool isConsumed =
        QAbstractEventDispatcher::instance()->filterNativeEvent(QByteArrayLiteral("mac_generic_NSEvent"),
                                                                event,
                                                                &result);
    return isConsumed;
}
} // namespace

TEST_CASE("Cocoa relative capture ignores absolute cursor jumps and releases every exit path",
          "[.render-native][editor][navigation][relative-mouse]")
{
    if (QApplication::platformName() != QStringLiteral("cocoa"))
    {
        SKIP("Requires the Cocoa native input filter");
    }
    RenderPreview preview;
    REQUIRE(preview.SetScene({}) == true);
    preview.Navigate(-1, {90, 20}, 3, false);
    PreviewViewport viewport(&preview, nullptr, false);
    viewport.resize(800, 600);
    viewport.show();
    viewport.raise();
    viewport.activateWindow();
    [NSApp activateIgnoringOtherApps:YES];
    REQUIRE(QTest::qWaitFor([]() { return QGuiApplication::applicationState() == Qt::ApplicationActive; }) == true);

    const QPoint start(200, 200);
    const QPoint absolutePosition(454, 157);
    const auto beginCapture = [&viewport, start]() {
        QTest::mousePress(&viewport, Qt::RightButton, Qt::NoModifier, start);
    };
    beginCapture();
    const QJsonObject before = preview.Navigation();
    CHECK(DispatchRelativeMotion(absolutePosition, {0, 40}, NSProcessInfo.processInfo.systemUptime) == true);
    const QJsonObject after = preview.Navigation();
    CHECK(after.value(QStringLiteral("yaw")).toDouble() == Catch::Approx(90).margin(0.001));
    CHECK(after.value(QStringLiteral("pitch")).toDouble() == Catch::Approx(30).margin(0.001));
    CHECK(after.value(QStringLiteral("eyeX")) == before.value(QStringLiteral("eyeX")));
    CHECK(after.value(QStringLiteral("eyeY")) == before.value(QStringLiteral("eyeY")));
    CHECK(after.value(QStringLiteral("eyeZ")) == before.value(QStringLiteral("eyeZ")));

    // Replay the absolute center/stale-position sequence seen in the physical trace.
    // Only NSEvent motion deltas contribute; absolute coordinates and old events do not.
    CHECK(DispatchRelativeMotion({399, 215}, {}, NSProcessInfo.processInfo.systemUptime) == true);
    CHECK(DispatchRelativeMotion(absolutePosition, {}, NSProcessInfo.processInfo.systemUptime) == true);
    CHECK(DispatchRelativeMotion(absolutePosition, {500, 500}, NSProcessInfo.processInfo.systemUptime - 1) == true);
    const QPointF absoluteGlobal = viewport.mapToGlobal(absolutePosition);
    QMouseEvent absolute(QEvent::MouseMove,
                         absolutePosition,
                         absoluteGlobal,
                         Qt::NoButton,
                         Qt::RightButton,
                         Qt::NoModifier);
    QApplication::sendEvent(&viewport, &absolute);
    CHECK(preview.Navigation() == after);

    // An inactive second owner cannot reconnect the process-wide cursor behind the first.
    ViewportRelativeMouse second(viewport);
    CHECK(second.TryBegin() == false);
    second.Stop();
    CHECK(DispatchRelativeMotion(absolutePosition, {}, NSProcessInfo.processInfo.systemUptime) == true);
    QTest::mouseRelease(&viewport, Qt::RightButton, Qt::NoModifier, start);
    CHECK(DispatchRelativeMotion(absolutePosition, {0, 40}, NSProcessInfo.processInfo.systemUptime) == false);
    CHECK(preview.Navigation() == after);

    for (const QEvent::Type exit :
         {QEvent::KeyPress, QEvent::FocusOut, QEvent::UngrabMouse, QEvent::WindowDeactivate, QEvent::Hide})
    {
        beginCapture();
        REQUIRE(DispatchRelativeMotion(absolutePosition, {}, NSProcessInfo.processInfo.systemUptime) == true);
        if (exit == QEvent::KeyPress)
        {
            QTest::keyClick(&viewport, Qt::Key_Escape);
        }
        else if (exit == QEvent::FocusOut)
        {
            QFocusEvent focus(QEvent::FocusOut);
            QApplication::sendEvent(&viewport, &focus);
        }
        else if (exit == QEvent::Hide)
        {
            viewport.hide();
        }
        else
        {
            QEvent event(exit);
            QApplication::sendEvent(&viewport, &event);
        }
        CHECK(DispatchRelativeMotion(absolutePosition, {0, 40}, NSProcessInfo.processInfo.systemUptime) == false);
        CHECK(preview.Navigation() == after);
    }
    {
        ViewportRelativeMouse temporary(viewport);
        REQUIRE(temporary.TryBegin() == true);
    }
    CHECK(DispatchRelativeMotion(absolutePosition, {0, 40}, NSProcessInfo.processInfo.systemUptime) == false);
}

#if defined(LUDUS_HOSTED_PRODUCER)
TEST_CASE("Cocoa Play sends relative look and movement to the hosted child and releases every exit",
          "[.play-native][editor][play][viewport][relative-pointer]")
{
    if (qEnvironmentVariableIsSet("LUDUS_TEST_COCOA") == false)
    {
        SKIP("Set LUDUS_TEST_COCOA=1 to run the native hosted input journey in a WindowServer session");
    }
    REQUIRE(QGuiApplication::platformName() == QStringLiteral("cocoa"));
    RenderPanel panel;
    PreviewViewport& viewport = panel.GetViewport();
    RenderPreview& preview = panel.Preview();
    panel.resize(840, 640);
    viewport.setGeometry(0, 0, 800, 600);
    viewport.show();
    panel.show();
    panel.raise();
    panel.activateWindow();
    [NSApp activateIgnoringOtherApps:YES];
    REQUIRE(QTest::qWaitFor([&panel]() { return panel.isActiveWindow(); }));
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("hosted-pointer-journey");
    scene.AssetsIdentity = scene.Identity;
    PreviewMesh mesh;
    mesh.Positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    mesh.Uvs = {{0, 0}, {1, 0}, {0.5F, 1}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.append(mesh);
    scene.Objects.append(PreviewObject{});
    REQUIRE(preview.SetScene(scene));
    const bool hasStartedPreview = preview.Start();
    INFO(preview.Status().toStdString());
    REQUIRE(hasStartedPreview);
    REQUIRE(QTest::qWaitFor([&viewport]() { return viewport.PresentedFrames() > 0; }));

    const PlayViewportStartResult started = panel.BeginPlay();
    INFO(started.Reason.toStdString());
    REQUIRE(started.Address.isEmpty() == false);
    QProcess game;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromLatin1(ludus::platform::HOSTED_WINDOW_ENVIRONMENT_VARIABLE), started.Address);
    game.setProcessEnvironment(environment);
    game.start(QStringLiteral(LUDUS_HOSTED_PRODUCER), {QStringLiteral("pointer")});
    REQUIRE(game.waitForStarted());
    REQUIRE(QTest::qWaitFor([&]() { return &viewport.GetFrameSource() != &preview; }));
    PlayViewportSource* play = panel.GetPlaySourceOrNull();
    REQUIRE(play != nullptr);
    REQUIRE(QTest::qWaitFor([play]() { return play->GetTakenFrames() > 0; }));

    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(panel.winId());
    NSWindow* nativeWindow = view.window;
    REQUIRE(nativeWindow != nil);
    const auto postKey = [nativeWindow](uint16 code, NSString* characters, NSEventModifierFlags flags) {
        NSEvent* event = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                          location:NSZeroPoint
                                     modifierFlags:flags
                                         timestamp:NSProcessInfo.processInfo.systemUptime
                                      windowNumber:nativeWindow.windowNumber
                                           context:nil
                                        characters:characters
                       charactersIgnoringModifiers:characters
                                         isARepeat:NO
                                           keyCode:code];
        REQUIRE(event != nil);
        [NSApp postEvent:event atStart:NO];
    };
    ludus::graphics::rhi::hosted_test::ProducerStatistics statistics;
    const auto readGameInput = [&game, &statistics]() {
        while (game.bytesAvailable() >= static_cast<qint64>(sizeof(statistics)))
        {
            REQUIRE(game.read(reinterpret_cast<char*>(&statistics), sizeof(statistics)) ==
                    static_cast<qint64>(sizeof(statistics)));
        }
    };
    const auto waitForGameInput = [&](uint32 focusLosses, float32 horizontal, bool isHeld) {
        const bool hasMatched = QTest::qWaitFor([&]() {
            readGameInput();
            return statistics.HeldMovementFocusLosses == focusLosses &&
                   statistics.HorizontalLookWhileMoving == horizontal && statistics.IsMovementHeld == isHeld;
        });
        CAPTURE(focusLosses,
                horizontal,
                isHeld,
                statistics.HeldMovementFocusLosses,
                statistics.HorizontalLookWhileMoving,
                statistics.IsMovementHeld);
        REQUIRE(hasMatched);
    };
    const QPoint clickPosition(220, 200);
    const auto postMotion = [&clickPosition](QPoint motion) {
        [NSApp postEvent:MakeRelativeMotion(clickPosition, motion, NSProcessInfo.processInfo.systemUptime) atStart:NO];
    };
    const auto capture = [&viewport, &clickPosition]() {
        QTest::mouseClick(&viewport, Qt::LeftButton, Qt::NoModifier, clickPosition);
        REQUIRE(QWidget::mouseGrabber() == &viewport);
    };

    // After keyboard focus alone, no look goes to the child. Click explicitly to capture.
    viewport.setFocus(Qt::TabFocusReason);
    REQUIRE(viewport.hasFocus());
    CHECK(QWidget::mouseGrabber() != &viewport);
    constexpr uint16 W_KEY_CODE = 0x0D;
    postKey(W_KEY_CODE, @"w", 0);
    postMotion({80, 90});
    waitForGameInput(0, 0, true);
    capture();
    postMotion({7, -5});
    waitForGameInput(0, 7, true);

    constexpr uint16 F1_KEY_CODE = 0x7A;
    const unichar f1 = NSF1FunctionKey;
    postKey(F1_KEY_CODE, [NSString stringWithCharacters:&f1 length:1], NSEventModifierFlagShift);
    REQUIRE(QTest::qWaitFor([&viewport]() { return QWidget::mouseGrabber() != &viewport; }));
    CHECK_FALSE(viewport.hasFocus());
    postMotion({40, 30});
    waitForGameInput(1, 7, false);

    capture();
    postKey(W_KEY_CODE, @"w", 0);
    postMotion({3, 8});
    waitForGameInput(1, 10, true);
    QWidget focusTarget;
    focusTarget.show();
    focusTarget.raise();
    focusTarget.activateWindow();
    REQUIRE(QTest::qWaitFor([&focusTarget]() { return focusTarget.isActiveWindow(); }));
    CHECK(QWidget::mouseGrabber() != &viewport);
    waitForGameInput(2, 10, false);

    panel.raise();
    panel.activateWindow();
    REQUIRE(QTest::qWaitFor([&panel]() { return panel.isActiveWindow(); }));
    CHECK(QWidget::mouseGrabber() != &viewport);
    capture();
    postKey(W_KEY_CODE, @"w", 0);
    postMotion({2, -2});
    waitForGameInput(2, 12, true);
    panel.RequestPlayClose();
    CHECK(QWidget::mouseGrabber() != &viewport);
    REQUIRE(game.waitForFinished());
    CHECK(game.exitStatus() == QProcess::NormalExit);
    CHECK(game.exitCode() == 0);
    REQUIRE(QTest::qWaitFor([&panel]() { return panel.IsPlaying() == false; }));
    CHECK(&viewport.GetFrameSource() == &preview);

    readGameInput();
    CHECK(game.bytesAvailable() == 0);
    CHECK_FALSE(statistics.IsMovementHeld);
    CHECK(statistics.ReadyFrames > 0);
    CHECK(statistics.HorizontalLookWhileMoving == 12);
    CHECK(statistics.VerticalLookWhileMoving == 1);
    CHECK(statistics.HeldMovementFocusLosses == 3);
}
#endif
