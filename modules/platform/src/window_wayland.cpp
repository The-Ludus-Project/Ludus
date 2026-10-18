#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
// wayland-scanner emits the protocol marshalling code as a .cpp that must be
// compiled directly into this translation unit; the include is intentional.
#include <xdg-shell-protocol.cpp> // NOLINT(bugprone-suspicious-include)

#include <cerrno>
#include <cstring> // TODO: remove all c libraries
#include <new>     // std::nothrow

#include <poll.h>
#include <unistd.h> // close() for keymap descriptors

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>

#include <ludus/input/key.h>
#include <ludus/input/keyboard_event.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/keyboard_sink.h>
#include <ludus/platform/log_categories.h>
#include <ludus/platform/native_window.h>
#include <ludus/platform/wayland/window.h>

#include "internal/evdev_keymap.hpp"

namespace ludus::foundation::core
{
template <>
struct DefaultDeleter<wl_display>
{
    LUDUS_INLINE void operator()(wl_display* ptr) const noexcept
    {
        if (ptr)
        {
            wl_display_disconnect(ptr);
        }
    }
};

template <>
struct DefaultDeleter<wl_registry>
{
    LUDUS_INLINE void operator()(wl_registry* ptr) const noexcept
    {
        if (ptr)
        {
            wl_registry_destroy(ptr);
        }
    }
};

template <>
struct DefaultDeleter<wl_compositor>
{
    LUDUS_INLINE void operator()([[maybe_unused]] wl_compositor* ptr) const noexcept {}
};

template <>
struct DefaultDeleter<xdg_wm_base>
{
    LUDUS_INLINE void operator()(xdg_wm_base* ptr) const noexcept
    {
        if (ptr)
        {
            xdg_wm_base_destroy(ptr);
        }
    }
};

void DefaultDeleter<wl_surface>::operator()(wl_surface* ptr) const noexcept
{
    if (ptr)
    {
        wl_surface_destroy(ptr);
    }
}

void DefaultDeleter<xdg_surface>::operator()(xdg_surface* ptr) const noexcept
{
    if (ptr)
    {
        xdg_surface_destroy(ptr);
    }
}

void DefaultDeleter<xdg_toplevel>::operator()(xdg_toplevel* ptr) const noexcept
{
    if (ptr)
    {
        xdg_toplevel_destroy(ptr);
    }
}
} // namespace ludus::foundation::core

namespace ludus::platform::wayland
{
static void
onGlobalRegistry(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) noexcept;
static void onGlobalRegistryRemove(void* data, wl_registry* registry, uint32_t name) noexcept;
static void onXdgWmBasePing(void* data, xdg_wm_base* xdgWmBase, uint32_t serial) noexcept;

// wl_seat / wl_keyboard lifecycle (K10).
static void onSeatCapabilities(void* data, wl_seat* seat, uint32_t capabilities) noexcept;
static void onSeatName(void* data, wl_seat* seat, const char* name) noexcept;
static void onKeyboardKeymap(void* data, wl_keyboard* keyboard, uint32_t format, int32_t fd, uint32_t size) noexcept;
static void
onKeyboardEnter(void* data, wl_keyboard* keyboard, uint32_t serial, wl_surface* surface, wl_array* keys) noexcept;
static void onKeyboardLeave(void* data, wl_keyboard* keyboard, uint32_t serial, wl_surface* surface) noexcept;
static void
onKeyboardKey(void* data, wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) noexcept;
static void onKeyboardModifiers(void* data,
                                wl_keyboard* keyboard,
                                uint32_t serial,
                                uint32_t modsDepressed,
                                uint32_t modsLatched,
                                uint32_t modsLocked,
                                uint32_t group) noexcept;
static void onKeyboardRepeatInfo(void* data, wl_keyboard* keyboard, int32_t rate, int32_t delay) noexcept;
static void onXdgSurfaceConfigure(void* data, xdg_surface* xdgSurface, uint32_t serial) noexcept;
static void
onXdgToplevelConfigure(void* data, xdg_toplevel* xdgToplevel, int32_t width, int32_t height, wl_array* states) noexcept;
static void onXdgToplevelClose(void* data, xdg_toplevel* xdgToplevel) noexcept;
static void onXdgToplevelConfigureBounds(void* data, xdg_toplevel* xdgToplevel, int32_t width, int32_t height) noexcept;
static void onXdgToplevelWmCapabilities(void* data, xdg_toplevel* xdgToplevel, wl_array* capabilities) noexcept;

static UniquePtr<wl_display> gDisplay = nullptr;
static UniquePtr<wl_registry> gRegistry = nullptr;
static UniquePtr<wl_compositor> gCompositor = nullptr;
static wl_registry_listener gRegistryListener = { .global = onGlobalRegistry, .global_remove = onGlobalRegistryRemove };
static UniquePtr<xdg_wm_base> gXdgWmBase = nullptr;
static xdg_wm_base_listener gXdgWmBaseListener = { .ping = onXdgWmBasePing };

// ---- Seat / keyboard state (K10) ---------------------------------------
// One seat is selected deterministically (lowest registry name advertising
// keyboard capability) and kept until it is removed. A replacement seat starts
// from reset state; held state is never merged across seats.
static wl_seat* gSeat = nullptr;
static uint32_t gSeatName = 0; // registry global name of the selected seat
static bool gSeatSelected = false;
static wl_keyboard* gKeyboard = nullptr;

// Surface-to-window routing. The display is shared across windows, so dispatch
// can deliver callbacks for any surface; we route strictly by the focused
// surface and never by whichever window called HandleEvent.
static WindowWayland* gFocusedWindow = nullptr; // window whose surface has keyboard focus

static wl_seat_listener gSeatListener =
{
    .capabilities = onSeatCapabilities,
    .name = onSeatName,
};
static wl_keyboard_listener gKeyboardListener =
{
    .keymap = onKeyboardKeymap,
    .enter = onKeyboardEnter,
    .leave = onKeyboardLeave,
    .key = onKeyboardKey,
    .modifiers = onKeyboardModifiers,
    .repeat_info = onKeyboardRepeatInfo,
};

static xdg_surface_listener gXdgSurfaceListener = { .configure = onXdgSurfaceConfigure };
static xdg_toplevel_listener gXdgToplevelListener =
{
    .configure = onXdgToplevelConfigure,
    .close = onXdgToplevelClose,
    .configure_bounds = onXdgToplevelConfigureBounds,
    .wm_capabilities = onXdgToplevelWmCapabilities,
};

// Live-window registry for surface routing. The first backend attaches a single
// gameplay window and selects one seat (deferred scope), but it tracks every
// created window so enter/leave can safely route by surface.
inline constexpr ludus::foundation::usize MAX_WAYLAND_WINDOWS = 16;
static WindowWayland* gWindows[MAX_WAYLAND_WINDOWS] = {};

static void registerWindow(WindowWayland* window) noexcept
{
    for (auto& slot : gWindows)
    {
        if (slot == nullptr)
        {
            slot = window;
            return;
        }
    }
    LUDUS_LOG_WARN(LOG_PLATFORM, "Wayland window registry full; surface routing may miss a window");
}

static void unregisterWindow(WindowWayland* window) noexcept
{
    for (auto& slot : gWindows)
    {
        if (slot == window)
        {
            slot = nullptr;
        }
    }
    if (gFocusedWindow == window)
    {
        gFocusedWindow = nullptr;
    }
}

static WindowWayland* windowForSurface(wl_surface* surface) noexcept;

// Deliver one normalized record to a window's attached sink, if any.
static void deliverRecord(WindowWayland* window, const ludus::input::KeyboardRecord& record) noexcept;
// Deliver a reset/baseline to a window's attached sink, if any.
static void deliverReset(WindowWayland* window,
                         ludus::input::ResetReason reason,
                         const ludus::input::FocusBaseline& baseline) noexcept;

WindowWayland::WindowWayland(CreateInfo&& info) noexcept
    : WindowBase(info.BaseCreateInfo), mSurface(std::move(info.Surface)), mXdgSurface(std::move(info.XdgSurface)),
      mXdgToplevel(std::move(info.XdgToplevel))
{
    mNativeWindowInfo = { .System = WindowSystem::Wayland, .Display = gDisplay.Get(), .Surface = mSurface.Get() };
}

WindowWayland::~WindowWayland() noexcept
{
    // Unregister surface routing before the surface is destroyed so a late
    // dispatch cannot route to freed window/sink state (K10).
    unregisterWindow(this);
}

WindowWayland* windowForSurface(wl_surface* surface) noexcept
{
    if (surface == nullptr)
    {
        return nullptr;
    }
    for (WindowWayland* window : gWindows)
    {
        if (window != nullptr && window->GetNativeWindowInfo().Surface == surface)
        {
            return window;
        }
    }
    return nullptr;
}

void deliverRecord(WindowWayland* window, const ludus::input::KeyboardRecord& record) noexcept
{
    if (window == nullptr)
    {
        return;
    }
    const KeyboardSink& sink = window->GetKeyboardSink();
    if (sink.OnRecord != nullptr)
    {
        sink.OnRecord(sink.UserData, record);
    }
}

void deliverReset(WindowWayland* window,
                  ludus::input::ResetReason reason,
                  const ludus::input::FocusBaseline& baseline) noexcept
{
    if (window == nullptr)
    {
        return;
    }
    const KeyboardSink& sink = window->GetKeyboardSink();
    if (sink.OnReset != nullptr)
    {
        sink.OnReset(sink.UserData, reason, baseline);
    }
}

bool InitializeWayland([[maybe_unused]] const WindowManager::InitializeInfo& info) noexcept
{
    wl_display* display = gDisplay.Get();
    if (display == nullptr)
    {
        display = wl_display_connect(nullptr);
    }

    if (display == nullptr)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM, "Failed to connect to Wayland display");
        return false;
    }

    wl_registry* registry = gRegistry.Get();
    if (registry == nullptr)
    {
        registry = wl_display_get_registry(display);
    }

    if (registry == nullptr)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM, "Failed to get Wayland registry");
        return false;
    }

    int result = wl_registry_add_listener(registry, &gRegistryListener, nullptr);
    if (result != 0)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM, "Failed to add Wayland registry listener. Result: {}", result);
        return false;
    }

    result = wl_display_roundtrip(display);
    if (result < 0)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM, "Failed to perform Wayland display roundtrip. Result: {}", result);
        return false;
    }

    gDisplay = UniquePtr<wl_display>(display);
    gRegistry = UniquePtr<wl_registry>(registry);

    return true;
}

void onGlobalRegistry([[maybe_unused]] void* data,
                      [[maybe_unused]] wl_registry* registry,
                      uint32_t name,
                      const char* interface,
                      uint32_t version) noexcept
{
    if (std::strcmp(interface, wl_compositor_interface.name) == 0)
    {
        wl_compositor* compositor =
            static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, version));
        if (compositor == nullptr)
        {
            LUDUS_LOG_ERROR(LOG_PLATFORM, "Failed to bind Wayland compositor");
            return;
        }
        gCompositor = UniquePtr<wl_compositor>(compositor);
    }
    if (std::strcmp(interface, xdg_wm_base_interface.name) == 0)
    {
        // Bind the newest version both sides implement. The generated protocol code
        // only has handlers and state values up to its own version, and a compositor
        // sends an event or toplevel state only when the bound version defines it:
        // the suspended state needs version 6 (xdg-shell.xml, xdg_toplevel.state).
        const uint32_t supportedVersion = static_cast<uint32_t>(xdg_wm_base_interface.version);
        const uint32_t boundVersion = version < supportedVersion ? version : supportedVersion;
        xdg_wm_base* xdgWmBase =
            static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, boundVersion));
        if (xdgWmBase == nullptr)
        {
            LUDUS_LOG_ERROR(LOG_PLATFORM, "Failed to bind XDG WM Base");
            return;
        }
        xdg_wm_base_add_listener(xdgWmBase, &gXdgWmBaseListener, nullptr);
        gXdgWmBase = UniquePtr<xdg_wm_base>(xdgWmBase);
        LUDUS_LOG_INFO(LOG_PLATFORM, "Bound xdg_wm_base: name={}, version={}", name, boundVersion);
    }
    if (std::strcmp(interface, wl_seat_interface.name) == 0 && gSeatSelected == false)
    {
        // Select the first (lowest registry name) seat deterministically and keep
        // it until removal. Bound the version to what the build headers support.
        const uint32_t bound = version < 7U ? version : 7U;
        wl_seat* seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, bound));
        if (seat == nullptr)
        {
            LUDUS_LOG_ERROR(LOG_PLATFORM, "Failed to bind Wayland seat");
            return;
        }
        gSeat = seat;
        gSeatName = name;
        gSeatSelected = true;
        wl_seat_add_listener(seat, &gSeatListener, nullptr);
        LUDUS_LOG_INFO(LOG_PLATFORM, "Selected Wayland seat: name={}, version={}", name, bound);
    }

    LUDUS_LOG_TRACE(LOG_PLATFORM, "Wayland global: name={}, interface={}, version={}", name, interface, version);
}

// `name` is only referenced by a LUDUS_LOG_TRACE call, which compiles out below
// the Trace level, so mark it maybe_unused to stay warning-clean in every build.
// Cancel the focused window and tear down the wl_keyboard, keeping the seat.
// Used on capability loss and (as part of) seat removal (K05, K10).
static void releaseKeyboard(ludus::input::ResetReason reason) noexcept
{
    if (gFocusedWindow != nullptr)
    {
        ludus::input::FocusBaseline empty;
        empty.Focused = false;
        deliverReset(gFocusedWindow, reason, empty);
        gFocusedWindow = nullptr;
    }
    if (gKeyboard != nullptr)
    {
        // version-correct destructor: release if available, else destroy.
        if (wl_keyboard_get_version(gKeyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
        {
            wl_keyboard_release(gKeyboard);
        }
        else
        {
            wl_keyboard_destroy(gKeyboard);
        }
        gKeyboard = nullptr;
    }
}

void onGlobalRegistryRemove([[maybe_unused]] void* data, [[maybe_unused]] wl_registry* registry, uint32_t name) noexcept
{
    // If the selected seat's global is removed, cancel input and drop the seat.
    // A replacement seat (advertised later) starts from reset state.
    if (gSeatSelected && name == gSeatName)
    {
        releaseKeyboard(ludus::input::ResetReason::SeatRemoved);
        if (gSeat != nullptr)
        {
            if (wl_seat_get_version(gSeat) >= WL_SEAT_RELEASE_SINCE_VERSION)
            {
                wl_seat_release(gSeat);
            }
            else
            {
                wl_seat_destroy(gSeat);
            }
            gSeat = nullptr;
        }
        gSeatSelected = false;
        gSeatName = 0;
        LUDUS_LOG_INFO(LOG_PLATFORM, "Wayland seat removed; keyboard input reset");
    }

    LUDUS_LOG_TRACE(LOG_PLATFORM, "Wayland global removed: name={}", name);
}

void onSeatCapabilities([[maybe_unused]] void* data, wl_seat* seat, uint32_t capabilities) noexcept
{
    if (seat != gSeat)
    {
        return; // ignore seats we did not select
    }
    const bool hasKeyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    if (hasKeyboard && gKeyboard == nullptr)
    {
        wl_keyboard* keyboard = wl_seat_get_keyboard(seat);
        if (keyboard == nullptr)
        {
            LUDUS_LOG_ERROR(LOG_PLATFORM, "Seat advertised keyboard but wl_seat_get_keyboard returned null");
            return;
        }
        gKeyboard = keyboard;
        wl_keyboard_add_listener(keyboard, &gKeyboardListener, nullptr);
        LUDUS_LOG_INFO(LOG_PLATFORM, "Wayland keyboard acquired");
    }
    else if (hasKeyboard == false && gKeyboard != nullptr)
    {
        // Capability lost: cancel input and release the keyboard; window stays
        // usable without a keyboard (K10).
        releaseKeyboard(ludus::input::ResetReason::CapabilityLost);
        LUDUS_LOG_INFO(LOG_PLATFORM, "Wayland keyboard capability lost");
    }
}

void onSeatName([[maybe_unused]] void* data, [[maybe_unused]] wl_seat* seat, [[maybe_unused]] const char* name) noexcept
{
    LUDUS_LOG_TRACE(LOG_PLATFORM, "Wayland seat name: {}", name);
}

// Signatures below are dictated by the wl_keyboard listener ABI; the adjacent
// same-typed parameters cannot be reordered, so bugprone-easily-swappable is
// suppressed on the parameter lines themselves.
void onKeyboardKeymap([[maybe_unused]] void* data,
                      [[maybe_unused]] wl_keyboard* keyboard,
                      [[maybe_unused]] uint32_t format, // NOLINT(bugprone-easily-swappable-parameters)
                      int32_t fd,
                      [[maybe_unused]] uint32_t size) noexcept
{
    // Physical-only milestone: we do NOT mmap/compile the keymap. We MUST still
    // close the fd on every path, including unsupported formats and when no sink
    // is attached (K10). no_keymap carries fd == -1; guard it.
    if (fd >= 0)
    {
        close(fd);
    }
}

void onKeyboardEnter([[maybe_unused]] void* data,
                     [[maybe_unused]] wl_keyboard* keyboard,
                     [[maybe_unused]] uint32_t serial,
                     wl_surface* surface,
                     wl_array* keys) noexcept
{
    WindowWayland* window = windowForSurface(surface);
    if (window == nullptr)
    {
        // Enter for a surface we do not own: ignore for gameplay (K06).
        return;
    }
    gFocusedWindow = window;

    // Copy/normalize the held-key array BEFORE returning; never retain the
    // borrowed wl_array (K06, design section 1). The array holds native evdev
    // codes of currently-pressed keys. BuildFocusBaseline is the shared,
    // unit-tested normalization used by both production and the headless tests.
    const uint32_t* codes = (keys != nullptr) ? static_cast<const uint32_t*>(keys->data) : nullptr;
    const ludus::foundation::usize count = (keys != nullptr) ? keys->size / sizeof(uint32_t) : 0;
    const ludus::input::FocusBaseline baseline = BuildFocusBaseline(codes, count);
    deliverReset(window, ludus::input::ResetReason::FocusEntered, baseline);
}

void onKeyboardLeave([[maybe_unused]] void* data,
                     [[maybe_unused]] wl_keyboard* keyboard,
                     [[maybe_unused]] uint32_t serial,
                     wl_surface* surface) noexcept
{
    WindowWayland* window = windowForSurface(surface);
    if (window == nullptr)
    {
        return;
    }
    ludus::input::FocusBaseline empty;
    empty.Focused = false;
    deliverReset(window, ludus::input::ResetReason::FocusLost, empty);
    if (gFocusedWindow == window)
    {
        gFocusedWindow = nullptr;
    }
}

void onKeyboardKey([[maybe_unused]] void* data,
                   [[maybe_unused]] wl_keyboard* keyboard,
                   [[maybe_unused]] uint32_t serial, // NOLINT(bugprone-easily-swappable-parameters)
                   uint32_t time,
                   uint32_t key,
                   uint32_t state) noexcept
{
    // Route strictly to the focused window; events for an unfocused/other window
    // never activate gameplay (K06).
    if (gFocusedWindow == nullptr)
    {
        return;
    }

    // Shared, unit-tested normalization. An unsupported/vendor/media code yields
    // false and is dropped with no state change (K01).
    ludus::input::KeyboardRecord record;
    if (BuildKeyRecord(key, state == WL_KEYBOARD_KEY_STATE_PRESSED, time, record) == false)
    {
        return;
    }
    deliverRecord(gFocusedWindow, record);
}

void onKeyboardModifiers([[maybe_unused]] void* data,
                         [[maybe_unused]] wl_keyboard* keyboard,
                         [[maybe_unused]] uint32_t serial, // NOLINT(bugprone-easily-swappable-parameters)
                         [[maybe_unused]] uint32_t modsDepressed,
                         [[maybe_unused]] uint32_t modsLatched,
                         [[maybe_unused]] uint32_t modsLocked,
                         [[maybe_unused]] uint32_t group) noexcept
{
    // Received safely; modifier keys are ordinary physical keys in this
    // milestone. No text/IME/lock/latch interpretation (K01, deferred scope).
}

void onKeyboardRepeatInfo([[maybe_unused]] void* data,
                          [[maybe_unused]] wl_keyboard* keyboard,
                          [[maybe_unused]] int32_t rate, // NOLINT(bugprone-easily-swappable-parameters)
                          [[maybe_unused]] int32_t delay) noexcept
{
    // Negotiated repeat parameters: we do not start a client repeat timer and do
    // not synthesize gameplay repeats (K04, K10).
}

void onXdgWmBasePing([[maybe_unused]] void* data, [[maybe_unused]] xdg_wm_base* xdgWmBase, uint32_t serial) noexcept
{
    xdg_wm_base_pong(xdgWmBase, serial);
}

UniquePtr<WindowWayland> CreateWindow(const Window::CreateInfo& info) noexcept
{
    WindowWayland::CreateInfo createInfo
    {
        .BaseCreateInfo = info,
        .Surface = nullptr,
        .XdgSurface = nullptr,
        .XdgToplevel = nullptr,
    };

    wl_surface* surface = wl_compositor_create_surface(gCompositor.Get());
    if (surface == nullptr)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM, "Failed to create Wayland surface");
        return nullptr;
    }

    xdg_surface* xdgSurface = xdg_wm_base_get_xdg_surface(gXdgWmBase.Get(), surface);
    if (xdgSurface == nullptr)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM, "Failed to create XDG surface");
        wl_surface_destroy(surface);
        return nullptr;
    }

    xdg_toplevel* xdgToplevel = xdg_surface_get_toplevel(xdgSurface);
    if (xdgToplevel == nullptr)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM, "Failed to create XDG toplevel");
        xdg_surface_destroy(xdgSurface);
        wl_surface_destroy(surface);
        return nullptr;
    }
    xdg_toplevel_set_title(xdgToplevel, info.Name.c_str());
    xdg_toplevel_set_app_id(xdgToplevel, "ludus");
    xdg_toplevel_set_min_size(xdgToplevel, 320, 200);
    xdg_toplevel_set_max_size(xdgToplevel, 0, 0);

    createInfo.Surface = UniquePtr<::wl_surface>(surface);
    createInfo.XdgSurface = UniquePtr<xdg_surface>(xdgSurface);
    createInfo.XdgToplevel = UniquePtr<xdg_toplevel>(xdgToplevel);

    // Engine code is exception-free (-fno-exceptions); use the non-throwing
    // new and null-check rather than relying on a bad_alloc exception.
    WindowWayland* window = new (std::nothrow) WindowWayland(std::move(createInfo)); // TODO: custom allocator
    if (window == nullptr)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM, "Failed to allocate Wayland window");
        return nullptr;
    }
    xdg_surface_add_listener(window->mXdgSurface.Get(), &gXdgSurfaceListener, window);
    xdg_toplevel_add_listener(window->mXdgToplevel.Get(), &gXdgToplevelListener, window);

    registerWindow(window);

    wl_surface_commit(window->mSurface.Get());
    wl_display_roundtrip(gDisplay.Get());

    return UniquePtr<WindowWayland>(window);
}

void onXdgSurfaceConfigure(void* data, [[maybe_unused]] xdg_surface* xdgSurface, uint32_t serial) noexcept
{
    WindowWayland* window = static_cast<WindowWayland*>(data);
    if (window == nullptr)
    {
        return;
    }

    xdg_surface_ack_configure(xdgSurface, serial);
}

// Signature is dictated by the Wayland xdg_toplevel listener; the adjacent
// width/height parameters cannot be reordered.
// Signature is dictated by the Wayland xdg_toplevel listener; the adjacent
// width/height parameters cannot be reordered. The suppression must sit on the
// parameter lines themselves (a leading NOLINTNEXTLINE does not cover a
// multi-line signature, since the diagnostic points at the parameters).
void onXdgToplevelConfigure(void* data,
                            [[maybe_unused]] xdg_toplevel* xdgToplevel,
                            [[maybe_unused]] int32_t width, // NOLINT(bugprone-easily-swappable-parameters)
                            [[maybe_unused]] int32_t height,
                            wl_array* statesOrNull) noexcept
{
    WindowWayland* window = static_cast<WindowWayland*>(data);
    if (window == nullptr)
    {
        return;
    }

    window->SetClosed(false);
    // Thanks to the xdg-shell authors (Kristian Høgsberg, Rafael Antognolli, Jasper
    // St. Pierre, Jonas Ådahl and contributors), wayland-protocols,
    // stable/xdg-shell/xdg-shell.xml, xdg_toplevel.configure: "The states listed in
    // the event specify how the width/height arguments should be interpreted, and
    // possibly how it should be drawn." Every configure carries the complete state
    // set, so a configure without the suspended state is the window becoming
    // visible again. https://gitlab.freedesktop.org/wayland/wayland-protocols
    if (statesOrNull != nullptr)
    {
        window->SetVisibility(ComputeToplevelVisibility(*statesOrNull));
    }
}

void onXdgToplevelClose(void* data, [[maybe_unused]] xdg_toplevel* xdgToplevel) noexcept
{
    WindowWayland* window = static_cast<WindowWayland*>(data);
    if (window == nullptr)
    {
        return;
    }

    window->SetClosed(true);
    // A closed window reports Occluded, as the Cocoa backend does.
    window->SetVisibility(WindowVisibility::Occluded);
}

WindowVisibility ComputeToplevelVisibility(const wl_array& states) noexcept
{
#if defined(XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION)
    // xdg-shell.xml, xdg_toplevel.state entry "suspended" (value 9, since version 6):
    // "The surface is currently not ordinarily being repainted; for example because
    // its content is occluded by another window, or its outputs are switched off
    // due to screen locking." The array packs one 32-bit state value per entry.
    const uint32_t* entries = static_cast<const uint32_t*>(states.data);
    const ludus::foundation::usize count = states.size / sizeof(uint32_t);
    for (ludus::foundation::usize index = 0; index < count; ++index)
    {
        if (entries[index] == XDG_TOPLEVEL_STATE_SUSPENDED)
        {
            return WindowVisibility::Occluded;
        }
    }
#else
    // wayland-protocols before 1.32 has no suspended state: never hidden.
    (void)states;
#endif
    return WindowVisibility::Visible;
}

// Signature is dictated by the Wayland xdg_toplevel listener; the adjacent
// width/height parameters cannot be reordered.
void onXdgToplevelConfigureBounds([[maybe_unused]] void* data,
                                  [[maybe_unused]] xdg_toplevel* xdgToplevel,
                                  [[maybe_unused]] int32_t width, // NOLINT(bugprone-easily-swappable-parameters)
                                  [[maybe_unused]] int32_t height) noexcept
{
}

void onXdgToplevelWmCapabilities([[maybe_unused]] void* data,
                                 [[maybe_unused]] xdg_toplevel* xdgToplevel,
                                 [[maybe_unused]] wl_array* capabilities) noexcept
{
}

// The nonblocking display pump. Unchanged behavior from the original
// HandleEvent: prepare/flush/poll(0)/read-or-cancel with EINTR/EAGAIN handling.
// Returns false when the window has closed or the display pump fails terminally.
static bool pumpDisplayOnce(WindowWayland* self) noexcept
{
    if (self->IsClosed())
    {
        return false;
    }

    wl_display* display = gDisplay.Get();
    if (display == nullptr)
    {
        return false;
    }

    // Drain queued callbacks before preparing a socket read. Never wait for a
    // compositor event here: the caller must be able to render an idle window.
    while (wl_display_prepare_read(display) != 0)
    {
        if (wl_display_dispatch_pending(display) < 0 || self->IsClosed())
        {
            return false;
        }
    }

    pollfd descriptor{ .fd = wl_display_get_fd(display), .events = POLLIN, .revents = 0 };
    if (wl_display_flush(display) < 0)
    {
        if (errno != EAGAIN)
        {
            wl_display_cancel_read(display);
            return false;
        }
        // Retry when writable, or on the next frame if the socket is still full.
        descriptor.events |= POLLOUT;
    }

    const foundation::int32 result = poll(&descriptor, 1, 0);
    if (result < 0)
    {
        const foundation::int32 pollError = errno;
        wl_display_cancel_read(display);
        return pollError == EINTR && self->IsClosed() == false;
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
        wl_display_cancel_read(display);
        return false;
    }

    // Every successful prepare must end in exactly one read or cancellation.
    if ((descriptor.revents & POLLIN) != 0)
    {
        if (wl_display_read_events(display) < 0)
        {
            return false;
        }
    }
    else
    {
        wl_display_cancel_read(display);
    }

    if ((descriptor.revents & POLLOUT) != 0 && wl_display_flush(display) < 0 && errno != EAGAIN)
    {
        return false;
    }
    if (wl_display_dispatch_pending(display) < 0)
    {
        return false;
    }

    return self->IsClosed() == false;
}

bool WindowWayland::HandleEvent([[maybe_unused]] const Event& event) noexcept
{
    const bool alive = pumpDisplayOnce(this);
    if (alive == false)
    {
        // The window is closing or the display pump failed terminally. Submit a
        // final reset to this window's sink (if attached and focused) so a held
        // action cancels before teardown (K05, K10). Done exactly once: clearing
        // the focus pointer prevents a repeat on a subsequent HandleEvent call.
        if (gFocusedWindow == this)
        {
            ludus::input::FocusBaseline empty;
            empty.Focused = false;
            deliverReset(this, ludus::input::ResetReason::WindowClosed, empty);
            gFocusedWindow = nullptr;
        }
    }
    return alive;
}
} // namespace ludus::platform::wayland
