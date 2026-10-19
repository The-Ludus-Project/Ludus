#pragma once

#include <ludus/foundation/base/types.h>

struct wl_display;
struct wl_surface;

namespace ludus::platform
{
class HostedSurfaceChannel;

enum class WindowSystem : ludus::foundation::uint8
{
    Headless,
    Wayland,
    WebCanvas,
    /// Native macOS window and content view, owned by the Cocoa platform backend.
    Cocoa,
    /// @brief Window whose events come from a host channel (HOSTED_WINDOW_ENVIRONMENT_VARIABLE).
    ///
    /// It has no native handles. RHI renders into the hosted surface reached through
    /// NativeWindowInfo::HostedSurfaceOrNull; see docs/architecture/editor-gui-systems.md,
    /// "The hosted surface and frame ownership".
    Hosted,
};

/// Borrowed handles. The platform window must outlive its RHI connection.
struct NativeWindowInfo final
{
    WindowSystem System = WindowSystem::Headless;
    ::wl_display* Display = nullptr;
    ::wl_surface* Surface = nullptr;
    ludus::foundation::uint32 Width = 800;
    ludus::foundation::uint32 Height = 600;
    /// @brief WebCanvas: borrowed selector owned by the platform window.
    ///
    /// The matching DOM canvas and window must both outlive the RHI connection. Keep the selector identifying
    /// that same canvas until window destruction. No GPU handle.
    const char* CanvasSelector = nullptr;
    /// Borrowed NSWindow on macOS; nullptr for other backends or a closed window.
    /// Access only on the main thread while the owning Ludus window remains alive.
    void* CocoaWindow = nullptr;
    /// Borrowed NSView for attaching the Metal presentation layer; no GPU object is created here.
    /// Main-thread access only; lifetime matches CocoaWindow. Cocoa Width/Height are backing pixels.
    void* CocoaView = nullptr;
    /// @brief Backing pixels per logical point.
    ///
    /// Hosted windows publish the scale their host sent with each resize. Native backends
    /// do not observe a scale yet and publish 1.
    ludus::foundation::float32 Scale = 1.0F;
    /// @brief Hosted only: the window's surface channel, borrowed from the hosted window.
    ///
    /// Valid while that window lives; null for every other backend. RHI publishes its
    /// hosted images and frames through it. Use it on the thread that pumps the window.
    HostedSurfaceChannel* HostedSurfaceOrNull = nullptr;
};
} // namespace ludus::platform
