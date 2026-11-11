#include <ludus/platform/native_keymap.h>

#include "internal/cocoa_keymap.hpp"

#if LUDUS_TARGET_OS == LUDUS_OS_LINUX
#    include "internal/evdev_keymap.hpp"
#endif

#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
#    include "internal/win32_keymap.hpp"
#endif

namespace ludus::platform
{
input::Key MapCocoaKeyCode(foundation::uint32 keyCode) noexcept
{
    return cocoa::MapKeyCode(keyCode);
}

#if LUDUS_TARGET_OS == LUDUS_OS_LINUX
input::Key MapEvdevKeyCode(foundation::uint32 code) noexcept
{
    return wayland::MapEvdevCode(code);
}
#endif
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
input::Key MapWin32KeyCode(foundation::uint32 scanCode, foundation::uint32 virtualKey) noexcept
{
    return win32::MapScanCode(scanCode, virtualKey);
}
#endif
} // namespace ludus::platform
