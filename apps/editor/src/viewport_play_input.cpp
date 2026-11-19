// The viewport as a playing game's window (Play in the editor viewport, P6c). The
// game receives exactly what its own Cocoa or Wayland window would give it: focus
// with a held-key baseline, physical keys, visibility and opted-in relative mouse motion. See
// docs/architecture/editor-gui-systems.md, "Input, focus and capture".
#include "internal/preview_viewport.h"

#include <ludus/input/keyboard_event.h>
#include <ludus/platform/hosted_window.h>
#include <ludus/platform/native_keymap.h>

#include <QCursor>
#include <QEvent>
#include <QHideEvent>
#include <QShowEvent>
#include <QToolTip>

namespace ludus::editor
{
namespace
{
using foundation::uint32;
/// Qt reports X11/Wayland key codes as XKB codes: evdev plus eight.
[[maybe_unused]] constexpr uint32 XKB_KEYCODE_OFFSET = 8;

[[nodiscard]] bool IsChord(const QKeyEvent& event, int key) noexcept
{
    return event.key() == key && (event.modifiers() & Qt::ShiftModifier) != 0;
}
/// Shift+F6 stops Play through the editor's Stop shortcut.
[[nodiscard]] bool IsStopChord(const QKeyEvent& event) noexcept
{
    return IsChord(event, Qt::Key_F6);
}
/// Shift+F1 gives keyboard focus and the pointer back to the editor.
[[nodiscard]] bool IsReleaseChord(const QKeyEvent& event) noexcept
{
    return IsChord(event, Qt::Key_F1);
}
[[nodiscard]] input::Key MapNativeKey(const QKeyEvent& event) noexcept
{
#if defined(Q_OS_MACOS)
    return platform::MapCocoaKeyCode(event.nativeVirtualKey());
#elif defined(Q_OS_WIN)
    // Thanks to The Qt Company, qwindowskeymapper.cpp (Qt 6.8), for the
    // native key event's extended scan-code and virtual-key representation.
    // https://github.com/qt/qtbase/blob/v6.8.3/src/plugins/platforms/windows/qwindowskeymapper.cpp
    return platform::MapWin32KeyCode(event.nativeScanCode(), event.nativeVirtualKey());
#elif defined(Q_OS_LINUX)
    const uint32 code = event.nativeScanCode();
    return code < XKB_KEYCODE_OFFSET ? input::Key::Unknown : platform::MapEvdevKeyCode(code - XKB_KEYCODE_OFFSET);
#else
    static_cast<void>(event);
    return input::Key::Unknown;
#endif
}
} // namespace

void PreviewViewport::SetPlayInput(platform::HostedWindowEndpoint* endpointOrNull)
{
    if (endpointOrNull == mPlayEndpointOrNull)
    {
        return;
    }
    if (mPlayEndpointOrNull != nullptr)
    {
        sendPlayFocus(false);
    }
    EndFirstPerson();
    stopNavigation();
    mPlayEndpointOrNull = endpointOrNull;
    mIsPlayInputSuspended = false;
    updateObjectInteractionAvailability();
    for (bool& isHeld : mPlayHeldKeys)
    {
        isHeld = false;
    }
    if (mPlayEndpointOrNull != nullptr)
    {
        static_cast<void>(mPlayEndpointOrNull->SendVisibility(
            isVisible() == true ? platform::WindowVisibility::Visible : platform::WindowVisibility::Occluded));
        if (hasFocus() == true)
        {
            sendPlayFocus(true);
        }
    }
}

void PreviewViewport::SuspendPlayInput()
{
    mIsPlayInputSuspended = true;
    sendPlayFocus(false);
}

void PreviewViewport::sendPlayFocus(bool isFocused)
{
    if (mPlayEndpointOrNull == nullptr || mHasPlayFocus == isFocused ||
        (isFocused == true && mIsPlayInputSuspended == true))
    {
        return;
    }
    mHasPlayFocus = isFocused;
    if (isFocused == false)
    {
        endPlayPointerCapture();
        // The game drops held keys exactly as on its own window's focus loss.
        static_cast<void>(mPlayEndpointOrNull->SendFocusLost());
        for (bool& isHeld : mPlayHeldKeys)
        {
            isHeld = false;
        }
        return;
    }
    input::FocusBaseline baseline;
    baseline.Focused = true;
    static_cast<void>(mPlayEndpointOrNull->SendFocusEntered(baseline));
}

void PreviewViewport::beginPlayPointerCapture()
{
#if defined(Q_OS_MACOS)
    if (mHasPlayFocus == false || isVisible() == false || mHasPlayPointerCapture == true ||
        mPlayEndpointOrNull == nullptr || mPlayEndpointOrNull->ShouldForwardRelativePointer() == false)
    {
        return;
    }
    if (mRelativeMouse.TryBegin() == false)
    {
        QToolTip::showText(
            QCursor::pos(),
            QStringLiteral(
                "The game could not capture the mouse. Activate the editor window and click the Play viewport again."),
            this);
        return;
    }
    mHasPlayPointerCapture = true;
    grabMouse(Qt::BlankCursor);
#endif
}

void PreviewViewport::endPlayPointerCapture()
{
#if defined(Q_OS_MACOS)
    if (mHasPlayPointerCapture == true)
    {
        mHasPlayPointerCapture = false;
        mRelativeMouse.Stop();
        if (QWidget::mouseGrabber() == this)
        {
            releaseMouse();
        }
    }
#endif
}

bool PreviewViewport::handlePlayKey(QKeyEvent& inoutEvent, bool isPress)
{
    if (mPlayEndpointOrNull == nullptr)
    {
        return false;
    }
    if (IsStopChord(inoutEvent) == true)
    {
        // Not consumed: the editor's Stop action owns it.
        inoutEvent.ignore();
        return true;
    }
    if (IsReleaseChord(inoutEvent) == true)
    {
        if (isPress == true)
        {
            sendPlayFocus(false);
            clearFocus();
        }
        inoutEvent.accept();
        return true;
    }
    const input::Key key = MapNativeKey(inoutEvent);
    if (mHasPlayFocus == true && input::IsValidKey(key) == true)
    {
        const bool isRepeat = inoutEvent.isAutoRepeat();
        bool& isHeld = mPlayHeldKeys[input::KeyIndex(key)];
        // A release the game never saw pressed (pressed before focus entered) is not sent.
        if (isPress == true || isHeld == true)
        {
            static_cast<void>(
                mPlayEndpointOrNull->SendKey(key,
                                             isPress == true ? input::KeyTransition::Down : input::KeyTransition::Up,
                                             isRepeat));
            isHeld = isPress;
        }
    }
    // Every other key belongs to the game, mapped or not.
    inoutEvent.accept();
    return true;
}

bool PreviewViewport::event(QEvent* event)
{
    if (event->type() == QEvent::WindowDeactivate)
    {
        sendPlayFocus(false);
        EndFirstPerson();
    }
    if (event->type() == QEvent::WindowActivate && hasFocus() == true && isVisible() == true)
    {
        sendPlayFocus(true);
    }
    if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate)
    {
        if (mHasPlayPointerCapture == true)
        {
            sendPlayFocus(false);
        }
        stopNavigation();
    }
    if (event->type() == QEvent::ShortcutOverride && Preview_->IsExploringFirstPerson() == true)
    {
        event->accept();
        return true;
    }
    if (mPlayEndpointOrNull != nullptr && event->type() == QEvent::ShortcutOverride)
    {
        // Menus and editor shortcuts do not take the game's keys; only Stop stays a shortcut.
        QKeyEvent* key = static_cast<QKeyEvent*>(event);
        if (IsStopChord(*key) == false)
        {
            key->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

void PreviewViewport::focusInEvent(QFocusEvent* event)
{
    sendPlayFocus(true);
    QWidget::focusInEvent(event);
}

void PreviewViewport::showEvent(QShowEvent* event)
{
    setRenderPresentationVisible(true);
    if (mPlayEndpointOrNull != nullptr)
    {
        static_cast<void>(mPlayEndpointOrNull->SendVisibility(platform::WindowVisibility::Visible));
        if (hasFocus() == true)
        {
            sendPlayFocus(true);
        }
    }
    QWidget::showEvent(event);
}
} // namespace ludus::editor
