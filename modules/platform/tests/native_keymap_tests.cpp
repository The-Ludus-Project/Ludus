#include <ludus/platform/native_keymap.h>

#include "internal/cocoa_keymap.hpp"

#if LUDUS_TARGET_OS == LUDUS_OS_LINUX
#    include "internal/evdev_keymap.hpp"

#    include <linux/input-event-codes.h>
#endif

#include <catch2/catch_test_macros.hpp>

using ludus::foundation::uint32;
using ludus::input::Key;

namespace
{
// Codes past each table's end must stay Unknown, so the parity sweep runs beyond it.
constexpr uint32 OUT_OF_RANGE_SWEEP = 64;
} // namespace

TEST_CASE("The public Cocoa key-code map is the Cocoa backend table", "[platform][input]")
{
    for (uint32 code = 0; code < ludus::platform::cocoa::KEYCODE_COUNT + OUT_OF_RANGE_SWEEP; ++code)
    {
        CHECK(ludus::platform::MapCocoaKeyCode(code) == ludus::platform::cocoa::MapKeyCode(code));
    }
    CHECK(ludus::platform::MapCocoaKeyCode(0x0D) == Key::KeyW);
    CHECK(ludus::platform::MapCocoaKeyCode(0x38) == Key::ShiftLeft);
    CHECK(ludus::platform::MapCocoaKeyCode(0x35) == Key::Escape);
    CHECK(ludus::platform::MapCocoaKeyCode(0x3F) == Key::Unknown);
    CHECK(ludus::platform::MapCocoaKeyCode(0xFFFFFFFF) == Key::Unknown);
}

#if LUDUS_TARGET_OS == LUDUS_OS_LINUX
TEST_CASE("The public evdev key-code map is the Wayland backend table", "[platform][input]")
{
    for (uint32 code = 0; code < ludus::platform::wayland::EVDEV_TABLE_SIZE + OUT_OF_RANGE_SWEEP; ++code)
    {
        CHECK(ludus::platform::MapEvdevKeyCode(code) == ludus::platform::wayland::MapEvdevCode(code));
    }
    CHECK(ludus::platform::MapEvdevKeyCode(KEY_W) == Key::KeyW);
    CHECK(ludus::platform::MapEvdevKeyCode(KEY_RIGHTSHIFT) == Key::ShiftRight);
    CHECK(ludus::platform::MapEvdevKeyCode(0xFFFFFFFF) == Key::Unknown);
}
#endif
