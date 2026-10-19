#pragma once

// Public access to the native physical key-code maps that the Cocoa and Wayland
// backends already use. Key translation has one owner: an adapter that receives
// native key codes from another toolkit (the editor's Qt viewport) converts them
// here instead of keeping a second table. See
// docs/architecture/editor-gui-systems.md, "The hosted window".

#include <ludus/foundation/base/core.h>

#include <ludus/input/key.h>

namespace ludus::platform
{
/// @brief Map a macOS virtual key code to the physical Ludus key the Cocoa backend publishes.
///
/// This is the table behind the Cocoa window's keyboard records and focus
/// baselines, so a code converted here names the same key a native Cocoa window
/// would report. Codes are hardware positions (`NSEvent.keyCode`, the `kVK_*`
/// constants of HIToolbox/Events.h), not characters, and are layout-independent.
/// Qt reports this code as `QKeyEvent::nativeVirtualKey()` on macOS.
///
/// Available on every target: the table is plain data with no AppKit dependency.
/// Thread-safe and allocation-free.
/// @param keyCode Native virtual key code.
/// @return The mapped key; Key::Unknown for a code the Cocoa backend does not map
/// (Fn, media, ISO/JIS-only and keypad-equals positions) or that is out of range.
[[nodiscard]] input::Key MapCocoaKeyCode(foundation::uint32 keyCode) noexcept;

#if LUDUS_TARGET_OS == LUDUS_OS_LINUX
/// @brief Map a Linux evdev key code to the physical Ludus key the Wayland backend publishes.
///
/// This is the table behind `wl_keyboard.key` and `wl_keyboard.enter`
/// normalization. The code uses `<linux/input-event-codes.h>` numbering. An XKB
/// keycode is the evdev code plus 8: Qt's `QKeyEvent::nativeScanCode()` on xcb and
/// Wayland is an XKB keycode, so subtract 8 before calling. No keysym or layout
/// lookup is performed.
///
/// Declared on Linux only, because the table is built from the Linux input-event
/// header. Thread-safe and allocation-free.
/// @param code Native evdev key code.
/// @return The mapped key; Key::Unknown for an unmapped, vendor or out-of-range code.
[[nodiscard]] input::Key MapEvdevKeyCode(foundation::uint32 code) noexcept;
#endif
} // namespace ludus::platform
