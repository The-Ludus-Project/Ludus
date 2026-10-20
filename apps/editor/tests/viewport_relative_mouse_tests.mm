#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/viewport_relative_mouse.h"

#include <ludus/foundation/base/types.h>

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QByteArray>
#include <QFocusEvent>
#include <QJsonObject>
#include <QMouseEvent>
#include <QTest>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;

bool DispatchRelativeMotion(QPoint position, QPoint delta, float64 timestamp)
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
    qintptr result = 0;
    const bool isConsumed =
        QAbstractEventDispatcher::instance()->filterNativeEvent(QByteArrayLiteral("mac_generic_NSEvent"),
                                                                event,
                                                                &result);
    CFRelease(native);
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
