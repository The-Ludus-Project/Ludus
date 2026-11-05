#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/input/key.h>

#include <windows.h>

namespace ludus::platform::win32
{
// Thanks to Microsoft, "Keyboard Input Overview", Scan Codes and Keystroke Message Flags.
// https://learn.microsoft.com/windows/win32/inputdev/about-keyboard-input
// Keep the physical position independent of the active layout. Use the virtual key
// only to distinguish special keys whose scan codes are ambiguous in messages.
constexpr UINT EXTENDED_SCAN_PREFIX = 0xE000;
constexpr UINT SCAN_CODE_MASK = 0xFF;
constexpr UINT KEYBOARD_STATE_DOWN = 0x80;
constexpr UINT VIRTUAL_KEY_COUNT = 256;

using foundation::StaticArray;
using foundation::usize;
using input::Key;

struct ScanCodeEntry final
{
    UINT Code;
    Key PhysicalKey;
};
constexpr ScanCodeEntry SCAN_CODES[] = {
    {0x0001, Key::Escape},
    {0x0002, Key::Digit1},
    {0x0003, Key::Digit2},
    {0x0004, Key::Digit3},
    {0x0005, Key::Digit4},
    {0x0006, Key::Digit5},
    {0x0007, Key::Digit6},
    {0x0008, Key::Digit7},
    {0x0009, Key::Digit8},
    {0x000A, Key::Digit9},
    {0x000B, Key::Digit0},
    {0x000C, Key::Minus},
    {0x000D, Key::Equal},
    {0x000E, Key::Backspace},
    {0x000F, Key::Tab},
    {0x0010, Key::KeyQ},
    {0x0011, Key::KeyW},
    {0x0012, Key::KeyE},
    {0x0013, Key::KeyR},
    {0x0014, Key::KeyT},
    {0x0015, Key::KeyY},
    {0x0016, Key::KeyU},
    {0x0017, Key::KeyI},
    {0x0018, Key::KeyO},
    {0x0019, Key::KeyP},
    {0x001A, Key::BracketLeft},
    {0x001B, Key::BracketRight},
    {0x001C, Key::Enter},
    {0x001D, Key::ControlLeft},
    {0x001E, Key::KeyA},
    {0x001F, Key::KeyS},
    {0x0020, Key::KeyD},
    {0x0021, Key::KeyF},
    {0x0022, Key::KeyG},
    {0x0023, Key::KeyH},
    {0x0024, Key::KeyJ},
    {0x0025, Key::KeyK},
    {0x0026, Key::KeyL},
    {0x0027, Key::Semicolon},
    {0x0028, Key::Quote},
    {0x0029, Key::Backquote},
    {0x002A, Key::ShiftLeft},
    {0x002B, Key::Backslash},
    {0x002C, Key::KeyZ},
    {0x002D, Key::KeyX},
    {0x002E, Key::KeyC},
    {0x002F, Key::KeyV},
    {0x0030, Key::KeyB},
    {0x0031, Key::KeyN},
    {0x0032, Key::KeyM},
    {0x0033, Key::Comma},
    {0x0034, Key::Period},
    {0x0035, Key::Slash},
    {0x0036, Key::ShiftRight},
    {0x0037, Key::KeypadMultiply},
    {0x0038, Key::AltLeft},
    {0x0039, Key::Space},
    {0x003A, Key::CapsLock},
    {0x003B, Key::F1},
    {0x003C, Key::F2},
    {0x003D, Key::F3},
    {0x003E, Key::F4},
    {0x003F, Key::F5},
    {0x0040, Key::F6},
    {0x0041, Key::F7},
    {0x0042, Key::F8},
    {0x0043, Key::F9},
    {0x0044, Key::F10},
    {0x0046, Key::ScrollLock},
    {0x0047, Key::Keypad7},
    {0x0048, Key::Keypad8},
    {0x0049, Key::Keypad9},
    {0x004A, Key::KeypadSubtract},
    {0x004B, Key::Keypad4},
    {0x004C, Key::Keypad5},
    {0x004D, Key::Keypad6},
    {0x004E, Key::KeypadAdd},
    {0x004F, Key::Keypad1},
    {0x0050, Key::Keypad2},
    {0x0051, Key::Keypad3},
    {0x0052, Key::Keypad0},
    {0x0053, Key::KeypadDecimal},
    {0x0057, Key::F11},
    {0x0058, Key::F12},
    {0x0064, Key::F13},
    {0x0065, Key::F14},
    {0x0066, Key::F15},
    {0x0067, Key::F16},
    {0x0068, Key::F17},
    {0x0069, Key::F18},
    {0x006A, Key::F19},
    {0x006B, Key::F20},
    {0x006C, Key::F21},
    {0x006D, Key::F22},
    {0x006E, Key::F23},
    {0x0076, Key::F24},
    {0xE01C, Key::KeypadEnter},
    {0xE01D, Key::ControlRight},
    {0xE035, Key::KeypadDivide},
    {0xE037, Key::PrintScreen},
    {0xE038, Key::AltRight},
    {0xE047, Key::Home},
    {0xE048, Key::ArrowUp},
    {0xE049, Key::PageUp},
    {0xE04B, Key::ArrowLeft},
    {0xE04D, Key::ArrowRight},
    {0xE04F, Key::End},
    {0xE050, Key::ArrowDown},
    {0xE051, Key::PageDown},
    {0xE052, Key::Insert},
    {0xE053, Key::Delete},
    {0xE05B, Key::SuperLeft},
    {0xE05C, Key::SuperRight},
    {0xE05D, Key::Menu},
};
constexpr usize SCAN_CODE_COUNT = SCAN_CODE_MASK + 1;
constexpr usize EXTENDED_SCAN_OFFSET = SCAN_CODE_COUNT;

constexpr StaticArray<Key, 2 * SCAN_CODE_COUNT> MakeScanTable() noexcept
{
    StaticArray<Key, 2 * SCAN_CODE_COUNT> keys{};
    for (const ScanCodeEntry& entry : SCAN_CODES)
    {
        const usize offset = (entry.Code & EXTENDED_SCAN_PREFIX) != 0 ? EXTENDED_SCAN_OFFSET : 0;
        keys[offset + (entry.Code & SCAN_CODE_MASK)] = entry.PhysicalKey;
    }
    return keys;
}

constexpr StaticArray<Key, 2 * SCAN_CODE_COUNT> SCAN_TABLE = MakeScanTable();

[[nodiscard]] LUDUS_INLINE bool HasAmbiguousScanCode(UINT virtualKey) noexcept
{
    // A virtual-key snapshot cannot distinguish keypad Enter or NumLock-off
    // navigation keys from their extended-key counterparts.
    switch (virtualKey)
    {
        case VK_RETURN:
        case VK_CLEAR:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_SHIFT:
        case VK_CONTROL:
        case VK_MENU:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] LUDUS_INLINE Key MapScanCode(UINT scanCode, WPARAM virtualKey) noexcept
{
    if (virtualKey == VK_PAUSE || virtualKey == VK_CANCEL)
    {
        return Key::Pause;
    }
    if (virtualKey == VK_NUMLOCK)
    {
        return Key::NumLock;
    }
    if (virtualKey == VK_SNAPSHOT)
    {
        return Key::PrintScreen;
    }
    const UINT prefix = scanCode & ~SCAN_CODE_MASK;
    if (prefix != 0 && prefix != EXTENDED_SCAN_PREFIX)
    {
        return Key::Unknown;
    }
    const usize offset = prefix == EXTENDED_SCAN_PREFIX ? EXTENDED_SCAN_OFFSET : 0;
    return SCAN_TABLE[offset + (scanCode & SCAN_CODE_MASK)];
}
} // namespace ludus::platform::win32
