#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/foundation/time/timers.hpp>
#include <ludus/platform/base/window.h>
#include <ludus/platform/native_window.h>
#include <ludus/platform/wayland/window.h>

#include <cstdlib>

#include <catch2/catch_test_macros.hpp>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

#include "../src/internal/wayland_scale.hpp"

namespace timing = ludus::foundation::time;

namespace
{
void onSyncDone(void* data,
                [[maybe_unused]] wl_callback* callback,
                [[maybe_unused]] ludus::foundation::uint32 serial) noexcept
{
    *static_cast<bool*>(data) = true;
}
} // namespace

TEST_CASE("Wayland event pumping progresses without incoming events", "[platform][wayland]")
{
    if (std::getenv("LUDUS_TEST_WAYLAND") == nullptr)
    {
        SKIP("Set LUDUS_TEST_WAYLAND=1 with a running compositor to test event pumping.");
    }

    ludus::platform::WindowManager manager;
    REQUIRE(manager.Initialize({}));
    auto window = ludus::platform::wayland::CreateWindow({ .Name = "Ludus event pump test" });
    REQUIRE(window.Get() != nullptr);

    // The surface has no attached buffer, so there are no frame callbacks to
    // drive progress. A blocking dispatch hangs here and hits the CTest timeout.
    for (ludus::foundation::uint32 iteration = 0; iteration < 10000; ++iteration)
    {
        REQUIRE(window->HandleEvent({}));
    }

    // Prove that pumping also flushes requests and reads fresh socket events,
    // rather than merely dispatching callbacks already queued by libwayland.
    bool syncDone = false;
    wl_callback* callback = wl_display_sync(window->GetNativeWindowInfo().Display);
    REQUIRE(callback != nullptr);
    const wl_callback_listener listener{ .done = onSyncDone };
    REQUIRE(wl_callback_add_listener(callback, &listener, &syncDone) == 0);
    timing::Deadline deadline;
    REQUIRE(deadline.Arm(timing::Now(), timing::Seconds(2)) == timing::TimeStatus::Ok);
    while (syncDone == false && deadline.IsExpired(timing::Now()) == false)
    {
        REQUIRE(window->HandleEvent({}));
    }
    wl_callback_destroy(callback);
    CHECK(syncDone);

    window->SetClosed(true);
    CHECK_FALSE(window->HandleEvent({}));
}

// Compositor-independent: the mapping from an xdg_toplevel.configure state array
// to WindowVisibility (xdg-shell.xml, xdg_toplevel.state "suspended", since 6).
namespace
{
void AppendState(wl_array& inoutStates, ludus::foundation::uint32 state)
{
    ludus::foundation::uint32* slot =
        static_cast<ludus::foundation::uint32*>(wl_array_add(&inoutStates, sizeof(ludus::foundation::uint32)));
    REQUIRE(slot != nullptr);
    *slot = state;
}
} // namespace

TEST_CASE("Toplevel configure states map the suspended state to Occluded", "[platform][wayland][visibility]")
{
    using ludus::platform::WindowVisibility;
    using ludus::platform::wayland::ComputeToplevelVisibility;

    wl_array states;
    wl_array_init(&states);

    // No states: a plain visible toplevel.
    CHECK(ComputeToplevelVisibility(states) == WindowVisibility::Visible);

    // Ordinary window states never hide the window.
    AppendState(states, XDG_TOPLEVEL_STATE_ACTIVATED);
    AppendState(states, XDG_TOPLEVEL_STATE_MAXIMIZED);
    AppendState(states, XDG_TOPLEVEL_STATE_FULLSCREEN);
    CHECK(ComputeToplevelVisibility(states) == WindowVisibility::Visible);

#if defined(XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION)
    STATIC_CHECK(XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION == 6);
    // Suspended anywhere in the array hides the window; the protocol does not say minimized.
    AppendState(states, XDG_TOPLEVEL_STATE_SUSPENDED);
    CHECK(ComputeToplevelVisibility(states) == WindowVisibility::Occluded);

    wl_array_release(&states);
    wl_array_init(&states);
    AppendState(states, XDG_TOPLEVEL_STATE_SUSPENDED);
    AppendState(states, XDG_TOPLEVEL_STATE_ACTIVATED);
    CHECK(ComputeToplevelVisibility(states) == WindowVisibility::Occluded);

    // A later configure without the state is the window becoming visible again.
    wl_array_release(&states);
    wl_array_init(&states);
    AppendState(states, XDG_TOPLEVEL_STATE_ACTIVATED);
    CHECK(ComputeToplevelVisibility(states) == WindowVisibility::Visible);
#else
    WARN("wayland-protocols older than 1.32: no suspended state, Wayland windows are always Visible");
#endif

    wl_array_release(&states);
}

TEST_CASE("Wayland configure applies size and visibility together and retains client-chosen axes",
          "[platform][wayland]")
{
    using ludus::platform::WindowVisibility;
    ludus::platform::NativeWindowInfo snapshot{ .Width = 641, .Height = 481 };
    WindowVisibility visibility = WindowVisibility::Visible;
    ludus::platform::wayland::SurfaceScale scale(snapshot, visibility);
    scale.PendingWidth = 0;
    scale.PendingHeight = 501;
    scale.PendingVisibility = WindowVisibility::Occluded;
    CHECK(scale.LogicalHeight == 481);
    CHECK(visibility == WindowVisibility::Visible);
    scale.Acknowledge();
    CHECK(scale.LogicalWidth == 641);
    CHECK(scale.LogicalHeight == 501);
    CHECK(visibility == WindowVisibility::Occluded);

    scale.PendingWidth = 711;
    scale.PendingHeight = 0;
    scale.PendingVisibility = WindowVisibility::Visible;
    scale.Acknowledge();
    CHECK(scale.LogicalWidth == 711);
    CHECK(scale.LogicalHeight == 501);
    CHECK(visibility == WindowVisibility::Visible);
}
