#include "internal/wayland_scale.hpp"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/platform/log_categories.h>

#include <cstring>
#include <wayland-client.h>
#if defined(LUDUS_WAYLAND_FRACTIONAL_SCALE)
#    include <fractional-scale-v1-client-protocol.h>
#    include <fractional-scale-v1-protocol.cpp> // NOLINT(bugprone-suspicious-include)
#    include <viewporter-client-protocol.h>
#    include <viewporter-protocol.cpp> // NOLINT(bugprone-suspicious-include)
#endif

namespace ludus::platform::wayland
{
using namespace foundation;
namespace
{
// Thanks to the Wayland authors for wayland.xml, wl_output.scale / wl_surface.enter,
// and wayland-protocols, fractional-scale-v1.xml: preferred scale is in 120ths,
// toplevel pixel extents round to nearest, and the viewport destination is logical.
// https://gitlab.freedesktop.org/wayland/wayland-protocols/-/tree/main/staging/fractional-scale
constexpr uint32 SCALE_DENOMINATOR = 120;
struct Output final
{
    uint32 Name;
    wl_output* Handle;
    int32 Scale = 1;
    Array<wl_surface*> Surfaces;
};
Array<Output> gOutputs;
#if defined(LUDUS_WAYLAND_FRACTIONAL_SCALE)
wp_viewporter* gViewporterOrNull = nullptr;
wp_fractional_scale_manager_v1* gFractionalManagerOrNull = nullptr;
uint32 gViewporterName = 0;
uint32 gFractionalManagerName = 0;
#endif

Output* FindOutput(wl_output* handle) noexcept
{
    for (Output& output : gOutputs)
    {
        if (output.Handle == handle)
        {
            return &output;
        }
    }
    return nullptr;
}
void Leave(void*, wl_surface* surface, wl_output* handle) noexcept
{
    Output* outputOrNull = FindOutput(handle);
    if (outputOrNull != nullptr)
    {
        for (usize index = 0; index < outputOrNull->Surfaces.GetSize(); ++index)
        {
            if (outputOrNull->Surfaces[index] == surface)
            {
                outputOrNull->Surfaces.RemoveAtSwap(index);
                return;
            }
        }
    }
}
const wl_surface_listener SURFACE_LISTENER =
{
    .enter =
        [](void*, wl_surface* surface, wl_output* handle) {
            Output* outputOrNull = FindOutput(handle);
            if (outputOrNull != nullptr)
            {
                outputOrNull->Surfaces.Add(surface);
            }
        },
    .leave = Leave,
#if defined(WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
    .preferred_buffer_scale =
        [](void* data, wl_surface*, int32 scale) { static_cast<SurfaceScale*>(data)->PreferredIntegerScale = scale; },
    .preferred_buffer_transform = [](void*, wl_surface*, uint32) {},
#endif
};
const wl_output_listener OUTPUT_LISTENER =
{
    .geometry = [](void*, wl_output*, int32, int32, int32, int32, int32, const char*, const char*, int32) {},
    .mode = [](void*, wl_output*, uint32, int32, int32, int32) {},
    .done = [](void*, wl_output*) {},
    .scale =
        [](void*, wl_output* handle, int32 scale) {
            Output* outputOrNull = FindOutput(handle);
            if (outputOrNull != nullptr)
            {
                outputOrNull->Scale = scale;
            }
        },
};

int32 GetIntegerScale(wl_surface* surface, int32 preferred) noexcept
{
    if (wl_surface_get_version(surface) < WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)
    {
        return 1;
    }
    if (preferred > 0)
    {
        return preferred;
    }
    int32 scale = 1;
    for (const Output& output : gOutputs)
    {
        for (wl_surface* entered : output.Surfaces)
        {
            if (entered == surface && output.Scale > scale)
            {
                scale = output.Scale;
            }
        }
    }
    return scale;
}
uint32 GetPixels(uint32 logical, uint64 numerator) noexcept
{
    if (logical == 0 || numerator > static_cast<uint64>(INT32_MAX) * SCALE_DENOMINATOR / logical)
    {
        return 0;
    }
    return static_cast<uint32>((logical * numerator + SCALE_DENOMINATOR / 2) / SCALE_DENOMINATOR);
}
} // namespace

void BindScaleGlobal(wl_registry* registry, uint32 name, const char* interface, uint32 version) noexcept
{
    if (std::strcmp(interface, wl_output_interface.name) == 0)
    {
        // Version 2 delivers scale and done; later output events are unnecessary.
        wl_output* handleOrNull =
            static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, version < 2 ? version : 2));
        if (handleOrNull != nullptr)
        {
            gOutputs.Add(Output{ .Name = name, .Handle = handleOrNull, .Surfaces = {} });
            wl_output_add_listener(handleOrNull, &OUTPUT_LISTENER, nullptr);
        }
    }
#if defined(LUDUS_WAYLAND_FRACTIONAL_SCALE)
    else if (gViewporterOrNull == nullptr && std::strcmp(interface, wp_viewporter_interface.name) == 0)
    {
        gViewporterOrNull = static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
        gViewporterName = name;
    }
    else if (gFractionalManagerOrNull == nullptr &&
             std::strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0)
    {
        gFractionalManagerOrNull = static_cast<wp_fractional_scale_manager_v1*>(
            wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
        gFractionalManagerName = name;
    }
#endif
}

void RemoveScaleGlobal(uint32 name) noexcept
{
    for (usize index = 0; index < gOutputs.GetSize(); ++index)
    {
        if (gOutputs[index].Name == name)
        {
            wl_output_destroy(gOutputs[index].Handle);
            gOutputs.RemoveAtSwap(index);
            return;
        }
    }
#if defined(LUDUS_WAYLAND_FRACTIONAL_SCALE)
    if (gViewporterOrNull != nullptr && name == gViewporterName)
    {
        wp_viewporter_destroy(gViewporterOrNull);
        gViewporterOrNull = nullptr;
    }
    if (gFractionalManagerOrNull != nullptr && name == gFractionalManagerName)
    {
        wp_fractional_scale_manager_v1_destroy(gFractionalManagerOrNull);
        gFractionalManagerOrNull = nullptr;
    }
#endif
}

SurfaceScale::SurfaceScale(NativeWindowInfo& inoutInfo, WindowVisibility& outVisibility) noexcept
    : Snapshot(inoutInfo), Visibility(outVisibility), LogicalWidth(inoutInfo.Width), LogicalHeight(inoutInfo.Height)
{
}
bool SurfaceScale::Attach() noexcept
{
    if (wl_surface_add_listener(Snapshot.Surface, &SURFACE_LISTENER, this) != 0)
    {
        return false;
    }
#if defined(LUDUS_WAYLAND_FRACTIONAL_SCALE)
    if (gViewporterOrNull != nullptr && gFractionalManagerOrNull != nullptr)
    {
        static constexpr wp_fractional_scale_v1_listener LISTENER =
        {
            .preferred_scale = [](void* data,
                                  wp_fractional_scale_v1*,
                                  uint32 scale) { static_cast<SurfaceScale*>(data)->FractionalScale = scale; },
        };
        ViewportOrNull = wp_viewporter_get_viewport(gViewporterOrNull, Snapshot.Surface);
        FractionalOrNull =
            wp_fractional_scale_manager_v1_get_fractional_scale(gFractionalManagerOrNull, Snapshot.Surface);
        return ViewportOrNull != nullptr && FractionalOrNull != nullptr &&
               wp_fractional_scale_v1_add_listener(FractionalOrNull, &LISTENER, this) == 0;
    }
#endif
    return true;
}
SurfaceScale::~SurfaceScale() noexcept
{
    for (Output& output : gOutputs)
    {
        Leave(nullptr, Snapshot.Surface, output.Handle);
    }
#if defined(LUDUS_WAYLAND_FRACTIONAL_SCALE)
    if (FractionalOrNull != nullptr)
    {
        wp_fractional_scale_v1_destroy(FractionalOrNull);
    }
    if (ViewportOrNull != nullptr)
    {
        wp_viewport_destroy(ViewportOrNull);
    }
#endif
}
void SurfaceScale::Configure(int32 width, int32 height, WindowVisibility visibility) noexcept
{
    PendingWidth = width;
    PendingHeight = height;
    PendingVisibility = visibility;
}
void SurfaceScale::Acknowledge() noexcept
{
    if (PendingWidth > 0)
    {
        LogicalWidth = static_cast<uint32>(PendingWidth);
    }
    if (PendingHeight > 0)
    {
        LogicalHeight = static_cast<uint32>(PendingHeight);
    }
    Visibility = PendingVisibility;
}
void SurfaceScale::Publish() noexcept
{
    const int32 integerScale = GetIntegerScale(Snapshot.Surface, PreferredIntegerScale);
    const uint64 numerator =
        FractionalScale != 0 ? FractionalScale : static_cast<uint64>(integerScale) * SCALE_DENOMINATOR;
    const uint32 width = GetPixels(LogicalWidth, numerator);
    const uint32 height = GetPixels(LogicalHeight, numerator);
    const float32 scale = static_cast<float32>(numerator) / SCALE_DENOMINATOR;
    if (HasPublished == true && width == Snapshot.Width && height == Snapshot.Height && scale == Snapshot.Scale)
    {
        return;
    }
    HasPublished = true;
    Snapshot.Width = width;
    Snapshot.Height = height;
    Snapshot.Scale = scale;
#if defined(LUDUS_WAYLAND_FRACTIONAL_SCALE)
    if (ViewportOrNull != nullptr)
    {
        if (width != 0 && height != 0)
        {
            wp_viewport_set_destination(ViewportOrNull,
                                        static_cast<int32>(LogicalWidth),
                                        static_cast<int32>(LogicalHeight));
        }
    }
    else
#endif
        if (wl_surface_get_version(Snapshot.Surface) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)
    {
        wl_surface_set_buffer_scale(Snapshot.Surface, integerScale);
    }
    LUDUS_LOG_INFO(LOG_PLATFORM,
                   "Wayland logical {}x{}, backing {}x{}, scale {}",
                   LogicalWidth,
                   LogicalHeight,
                   width,
                   height,
                   scale);
}
} // namespace ludus::platform::wayland
