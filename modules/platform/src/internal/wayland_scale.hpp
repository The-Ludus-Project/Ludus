#pragma once

#include <ludus/platform/base/window.h>

struct wl_registry;
struct wp_fractional_scale_v1;
struct wp_viewport;

namespace ludus::platform::wayland
{
void BindScaleGlobal(wl_registry* registry,
                     foundation::uint32 name,
                     const char* interface,
                     foundation::uint32 version) noexcept;
void RemoveScaleGlobal(foundation::uint32 name) noexcept;

struct SurfaceScale final
{
    explicit SurfaceScale(NativeWindowInfo& inoutInfo, WindowVisibility& outVisibility) noexcept;
    ~SurfaceScale() noexcept;
    bool Attach() noexcept;
    void Publish() noexcept;
    void Configure(foundation::int32 width, foundation::int32 height, WindowVisibility visibility) noexcept;
    void Acknowledge() noexcept;

    NativeWindowInfo& Snapshot;
    WindowVisibility& Visibility;
    foundation::uint32 LogicalWidth;
    foundation::uint32 LogicalHeight;
    foundation::int32 PendingWidth = 0;
    foundation::int32 PendingHeight = 0;
    WindowVisibility PendingVisibility = WindowVisibility::Visible;
    foundation::uint32 FractionalScale = 0;
    bool HasPublished = false;
    foundation::int32 PreferredIntegerScale = 0;
    wp_fractional_scale_v1* FractionalOrNull = nullptr;
    wp_viewport* ViewportOrNull = nullptr;
};
} // namespace ludus::platform::wayland
