#pragma once

// Explicit Linux evdev-position -> ludus::input::Key mapping (K01, K10).
//
// Wayland's wl_keyboard.key event carries a native key *code*. On the Linux
// evdev backend that code is the evdev position (the same numbering as
// <linux/input-event-codes.h>), NOT an XKB keysym and NOT the XKB keycode (which
// is evdev + 8). We therefore map the raw evdev code directly and never apply
// the +8 offset here; that offset is only relevant to a future XKB *keysym*
// lookup, which this physical-only milestone does not perform.
//
// Every lookup is bounds-checked and defaults to Key::Unknown, so an
// unsupported / vendor / media key — or a fabricated large code — normalizes to
// Unknown with no state change. This is a private implementation header.

#include <ludus/foundation/base/types.h>
#include <ludus/input/key.h>
#include <ludus/input/keyboard_event.h>

#include <linux/input-event-codes.h>

namespace ludus::platform::wayland
{
using ludus::foundation::uint32;
using ludus::input::Key;

// Highest evdev code we map; anything at/above is Unknown. KEY_MAX is 0x2ff but
// our supported set is far smaller, so cap the table modestly and bounds-check.
inline constexpr uint32 EVDEV_TABLE_SIZE = 256;

// Build the dense table at compile time. Unlisted codes stay Key::Unknown.
struct EvdevKeyTable final
{
    // Zero-initialized: Key::Unknown is value 0, so every unlisted code already
    // reads back as Unknown without an explicit fill loop.
    Key Entries[EVDEV_TABLE_SIZE] = {};

    constexpr EvdevKeyTable() noexcept
    {
        // Letters.
        set(KEY_A, Key::KeyA);
        set(KEY_B, Key::KeyB);
        set(KEY_C, Key::KeyC);
        set(KEY_D, Key::KeyD);
        set(KEY_E, Key::KeyE);
        set(KEY_F, Key::KeyF);
        set(KEY_G, Key::KeyG);
        set(KEY_H, Key::KeyH);
        set(KEY_I, Key::KeyI);
        set(KEY_J, Key::KeyJ);
        set(KEY_K, Key::KeyK);
        set(KEY_L, Key::KeyL);
        set(KEY_M, Key::KeyM);
        set(KEY_N, Key::KeyN);
        set(KEY_O, Key::KeyO);
        set(KEY_P, Key::KeyP);
        set(KEY_Q, Key::KeyQ);
        set(KEY_R, Key::KeyR);
        set(KEY_S, Key::KeyS);
        set(KEY_T, Key::KeyT);
        set(KEY_U, Key::KeyU);
        set(KEY_V, Key::KeyV);
        set(KEY_W, Key::KeyW);
        set(KEY_X, Key::KeyX);
        set(KEY_Y, Key::KeyY);
        set(KEY_Z, Key::KeyZ);

        // Top-row digits.
        set(KEY_0, Key::Digit0);
        set(KEY_1, Key::Digit1);
        set(KEY_2, Key::Digit2);
        set(KEY_3, Key::Digit3);
        set(KEY_4, Key::Digit4);
        set(KEY_5, Key::Digit5);
        set(KEY_6, Key::Digit6);
        set(KEY_7, Key::Digit7);
        set(KEY_8, Key::Digit8);
        set(KEY_9, Key::Digit9);

        // Punctuation (US positions).
        set(KEY_MINUS, Key::Minus);
        set(KEY_EQUAL, Key::Equal);
        set(KEY_LEFTBRACE, Key::BracketLeft);
        set(KEY_RIGHTBRACE, Key::BracketRight);
        set(KEY_BACKSLASH, Key::Backslash);
        set(KEY_SEMICOLON, Key::Semicolon);
        set(KEY_APOSTROPHE, Key::Quote);
        set(KEY_GRAVE, Key::Backquote);
        set(KEY_COMMA, Key::Comma);
        set(KEY_DOT, Key::Period);
        set(KEY_SLASH, Key::Slash);

        // Whitespace / control.
        set(KEY_SPACE, Key::Space);
        set(KEY_ENTER, Key::Enter);
        set(KEY_TAB, Key::Tab);
        set(KEY_BACKSPACE, Key::Backspace);
        set(KEY_ESC, Key::Escape);

        // Editing / navigation.
        set(KEY_INSERT, Key::Insert);
        set(KEY_DELETE, Key::Delete);
        set(KEY_HOME, Key::Home);
        set(KEY_END, Key::End);
        set(KEY_PAGEUP, Key::PageUp);
        set(KEY_PAGEDOWN, Key::PageDown);
        set(KEY_LEFT, Key::ArrowLeft);
        set(KEY_RIGHT, Key::ArrowRight);
        set(KEY_UP, Key::ArrowUp);
        set(KEY_DOWN, Key::ArrowDown);

        set(KEY_CAPSLOCK, Key::CapsLock);
        set(KEY_SYSRQ, Key::PrintScreen);
        set(KEY_SCROLLLOCK, Key::ScrollLock);
        set(KEY_PAUSE, Key::Pause);

        // Function row.
        set(KEY_F1, Key::F1);
        set(KEY_F2, Key::F2);
        set(KEY_F3, Key::F3);
        set(KEY_F4, Key::F4);
        set(KEY_F5, Key::F5);
        set(KEY_F6, Key::F6);
        set(KEY_F7, Key::F7);
        set(KEY_F8, Key::F8);
        set(KEY_F9, Key::F9);
        set(KEY_F10, Key::F10);
        set(KEY_F11, Key::F11);
        set(KEY_F12, Key::F12);
        set(KEY_F13, Key::F13);
        set(KEY_F14, Key::F14);
        set(KEY_F15, Key::F15);
        set(KEY_F16, Key::F16);
        set(KEY_F17, Key::F17);
        set(KEY_F18, Key::F18);
        set(KEY_F19, Key::F19);
        set(KEY_F20, Key::F20);
        set(KEY_F21, Key::F21);
        set(KEY_F22, Key::F22);
        set(KEY_F23, Key::F23);
        set(KEY_F24, Key::F24);

        // Keypad.
        set(KEY_NUMLOCK, Key::NumLock);
        set(KEY_KPSLASH, Key::KeypadDivide);
        set(KEY_KPASTERISK, Key::KeypadMultiply);
        set(KEY_KPMINUS, Key::KeypadSubtract);
        set(KEY_KPPLUS, Key::KeypadAdd);
        set(KEY_KPENTER, Key::KeypadEnter);
        set(KEY_KPDOT, Key::KeypadDecimal);
        set(KEY_KP0, Key::Keypad0);
        set(KEY_KP1, Key::Keypad1);
        set(KEY_KP2, Key::Keypad2);
        set(KEY_KP3, Key::Keypad3);
        set(KEY_KP4, Key::Keypad4);
        set(KEY_KP5, Key::Keypad5);
        set(KEY_KP6, Key::Keypad6);
        set(KEY_KP7, Key::Keypad7);
        set(KEY_KP8, Key::Keypad8);
        set(KEY_KP9, Key::Keypad9);

        // Distinct left/right modifiers.
        set(KEY_LEFTSHIFT, Key::ShiftLeft);
        set(KEY_RIGHTSHIFT, Key::ShiftRight);
        set(KEY_LEFTCTRL, Key::ControlLeft);
        set(KEY_RIGHTCTRL, Key::ControlRight);
        set(KEY_LEFTALT, Key::AltLeft);
        set(KEY_RIGHTALT, Key::AltRight);
        set(KEY_LEFTMETA, Key::SuperLeft);
        set(KEY_RIGHTMETA, Key::SuperRight);
        set(KEY_COMPOSE, Key::Menu);
    }

    constexpr void set(uint32 code, Key key) noexcept
    {
        if (code < EVDEV_TABLE_SIZE)
        {
            Entries[code] = key;
        }
    }
};

static_assert(static_cast<uint32>(Key::Unknown) == 0, "Zero-initialized evdev table assumes Key::Unknown == 0.");

inline constexpr EvdevKeyTable EVDEV_KEY_TABLE{};

// Map a native evdev code to a Ludus key. Out-of-range or unmapped -> Unknown.
[[nodiscard]] inline constexpr Key MapEvdevCode(uint32 code) noexcept
{
    if (code >= EVDEV_TABLE_SIZE)
    {
        return Key::Unknown;
    }
    return EVDEV_KEY_TABLE.Entries[code];
}

// --- Pure normalization helpers (compositor-independent) -----------------
// These encapsulate the exact enter-array normalization and key-event mapping
// the wl_keyboard listeners perform, so the routing/normalization semantics can
// be unit-tested without a live compositor (the OS->compositor transport is the
// only part these cannot cover). The listeners call these; see window_wayland.

// Build a focused held-key baseline from a wl_keyboard.enter key array. `codes`
// points at `count` native evdev codes (the borrowed wl_array contents, already
// copied by the caller). Unsupported/out-of-range codes are skipped; valid keys
// are marked held. No press edges are implied (K06).
[[nodiscard]] inline ludus::input::FocusBaseline BuildFocusBaseline(const uint32* codes,
                                                                    ludus::foundation::usize count) noexcept
{
    ludus::input::FocusBaseline baseline;
    baseline.Focused = true;
    if (codes == nullptr)
    {
        return baseline;
    }
    for (ludus::foundation::usize i = 0; i < count; ++i)
    {
        const Key key = MapEvdevCode(codes[i]);
        if (ludus::input::IsValidKey(key))
        {
            baseline.Keys[ludus::input::KeyIndex(key)] = true;
        }
    }
    return baseline;
}

// Translate a wl_keyboard.key event (native evdev `code`, `pressed` state, and
// `timeMs` diagnostic timestamp) into a normalized record. Returns false for an
// unsupported/unmapped code so the caller drops it with no state change (K01).
// `pressed` true => Down, false => Up. Native time is carried as diagnostic
// metadata only (K03).
[[nodiscard]] inline bool
BuildKeyRecord(uint32 code, bool pressed, uint32 timeMs, ludus::input::KeyboardRecord& outRecord) noexcept
{
    const Key physical = MapEvdevCode(code);
    if (ludus::input::IsValidKey(physical) == false)
    {
        return false;
    }
    outRecord.Source = ludus::input::RecordSource::Native;
    outRecord.Transition = pressed ? ludus::input::KeyTransition::Down : ludus::input::KeyTransition::Up;
    outRecord.PhysicalKey = physical;
    outRecord.Repeat = false; // compositor repeats are not gameplay presses (K04)
    outRecord.HasNativeTime = true;
    outRecord.NativeTimeMs = timeMs;
    return true;
}
} // namespace ludus::platform::wayland
