#pragma once

#include <ludus/platform/config.h>

#if !defined(LUDUS_PLATFORM_WAYLAND)
#    error "Wayland support requires LUDUS_PLATFORM_WAYLAND to be enabled."
#endif

#include <ludus/foundation/base/core.h> // LUDUS_INLINE + foundational vocabulary
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/platform/base/window.h>

#include <utility>

struct wl_surface;
struct wl_array;
struct xdg_surface;
struct xdg_toplevel;

namespace ludus::foundation::core
{
template <>
struct DefaultDeleter<wl_surface>
{
    void operator()(wl_surface* ptr) const noexcept;
};

template <>
struct DefaultDeleter<xdg_surface>
{
    void operator()(xdg_surface* ptr) const noexcept;
};

template <>
struct DefaultDeleter<xdg_toplevel>
{
    void operator()(xdg_toplevel* ptr) const noexcept;
};
} // namespace ludus::foundation::core

namespace ludus::platform::wayland
{
using ludus::foundation::UniquePtr;

class WindowWayland;

bool InitializeWayland(const WindowManager::InitializeInfo& info) noexcept;
UniquePtr<WindowWayland> CreateWindow(const Window::CreateInfo& info) noexcept;

/// @brief Map the states of an xdg_toplevel.configure event to a visibility.
///
/// @param states The event's `states` argument: packed 32-bit `xdg_toplevel_state` values.
/// @return Occluded when the array carries `XDG_TOPLEVEL_STATE_SUSPENDED`, which xdg-shell
/// (xdg_wm_base version 6) sends while "the surface is currently not ordinarily being
/// repainted; for example because its content is occluded by another window, or its outputs
/// are switched off due to screen locking", and which compositors also set for a minimized
/// window; otherwise Visible. The protocol does not distinguish minimization, so Minimized is
/// never returned. A build against wayland-protocols older than 1.32 has no suspended state
/// and always returns Visible.
[[nodiscard]] WindowVisibility ComputeToplevelVisibility(const wl_array& states) noexcept;

class WindowWayland final : public WindowBase
{
public:
    friend UniquePtr<WindowWayland> CreateWindow(const Window::CreateInfo& info) noexcept;

public:
    ~WindowWayland() noexcept override;

    bool HandleEvent(const Event& event) noexcept override;
    [[nodiscard]] bool IsClosed() const noexcept
    {
        return mIsClosed;
    }
    void SetClosed(bool value) noexcept
    {
        mIsClosed = value;
    }
    /// Record the visibility derived from the latest xdg_toplevel configure or close event.
    /// Called by the protocol listeners on the thread that pumps HandleEvent().
    void SetVisibility(WindowVisibility visibility) noexcept
    {
        mVisibility = visibility;
    }

private:
    struct CreateInfo final
    {
        WindowBase::CreateInfo BaseCreateInfo;
        UniquePtr<wl_surface> Surface;
        UniquePtr<xdg_surface> XdgSurface;
        UniquePtr<xdg_toplevel> XdgToplevel;
    };

private:
    explicit WindowWayland(CreateInfo&& info) noexcept;

private:
    UniquePtr<wl_surface> mSurface;
    UniquePtr<xdg_surface> mXdgSurface;
    UniquePtr<xdg_toplevel> mXdgToplevel;
    bool mIsClosed = false;
};
} // namespace ludus::platform::wayland
