#include "internal/viewport_relative_mouse.h"

#include "internal/preview_viewport.h"

#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/log.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

namespace ludus::editor
{
namespace
{
LUDUS_DEFINE_LOG_CATEGORY(LOG_VIEWPORT_INPUT, "ViewportInput");
// Cursor association is process-wide; a second viewport must not undo another capture.
ViewportRelativeMouse* gCaptureOwnerOrNull = nullptr;
} // namespace

ViewportRelativeMouse::ViewportRelativeMouse(PreviewViewport& inoutViewport) noexcept : mViewport(inoutViewport) {}
ViewportRelativeMouse::~ViewportRelativeMouse()
{
    Stop();
}
bool ViewportRelativeMouse::TryBegin() noexcept
{
    if (mIsActive == true)
    {
        return true;
    }
    if (gCaptureOwnerOrNull != nullptr || QGuiApplication::platformName() != QStringLiteral("cocoa") ||
        QGuiApplication::applicationState() != Qt::ApplicationActive)
    {
        return false;
    }
    // Thanks to Apple, "CGAssociateMouseAndMouseCursorPosition" (Core Graphics):
    // https://developer.apple.com/documentation/coregraphics/cgassociatemouseandmousecursorposition(_:)
    // Disconnected events retain physical deltas without moving or repeatedly warping the cursor.
    if (CGAssociateMouseAndMouseCursorPosition(false) != kCGErrorSuccess)
    {
        return false;
    }
    mCaptureBeganSeconds = NSProcessInfo.processInfo.systemUptime;
    mIsActive = true;
    gCaptureOwnerOrNull = this;
    QCoreApplication::instance()->installNativeEventFilter(this);
    return true;
}
void ViewportRelativeMouse::Stop() noexcept
{
    if (mIsActive == false)
    {
        return;
    }
    mIsActive = false;
    gCaptureOwnerOrNull = nullptr;
    QCoreApplication::instance()->removeNativeEventFilter(this);
    if (CGAssociateMouseAndMouseCursorPosition(true) != kCGErrorSuccess)
    {
        LUDUS_LOG_TEXT(LOG_VIEWPORT_INPUT, Warning, "Could not restore the viewport mouse cursor association");
    }
}
bool ViewportRelativeMouse::nativeEventFilter(const QByteArray& eventType, void* message, qintptr*)
{
    if (mIsActive == false || eventType != QByteArrayLiteral("mac_generic_NSEvent") || message == nullptr)
    {
        return false;
    }
    NSEvent* event = static_cast<NSEvent*>(message);
    const NSEventType type = event.type;
    if (type != NSEventTypeMouseMoved && type != NSEventTypeLeftMouseDragged && type != NSEventTypeRightMouseDragged &&
        type != NSEventTypeOtherMouseDragged)
    {
        return false;
    }
    if (event.timestamp < mCaptureBeganSeconds)
    {
        return true;
    }
    // Thanks to The Qt Company, "QAbstractNativeEventFilter Class":
    // https://doc.qt.io/qt-6/qabstractnativeeventfilter.html#nativeEventFilter
    // Consume native motion once, before Qt translates the constant absolute position.
    // Apple, "NSEvent.deltaX / deltaY" (AppKit) supplies fractional mouse/trackpad deltas:
    // https://developer.apple.com/documentation/appkit/nsevent/deltax
    // https://developer.apple.com/documentation/appkit/nsevent/deltay
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    if ((event.modifierFlags & NSEventModifierFlagOption) != 0)
    {
        modifiers |= Qt::AltModifier;
    }
    if ((event.modifierFlags & NSEventModifierFlagShift) != 0)
    {
        modifiers |= Qt::ShiftModifier;
    }
    const Qt::MouseButtons buttons = QGuiApplication::mouseButtons() | Qt::RightButton;
    static_cast<void>(mViewport.navigateMouse(static_cast<foundation::float32>(event.deltaX),
                                              static_cast<foundation::float32>(event.deltaY),
                                              buttons,
                                              modifiers));
    return true;
}
} // namespace ludus::editor
