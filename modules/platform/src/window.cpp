#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/platform/base/window.h>
#include <ludus/platform/config.h> // LUDUS_PLATFORM_WAYLAND / LUDUS_PLATFORM_HEADLESS

#include <utility>

#if defined(LUDUS_PLATFORM_BROWSER)
#    include "internal/window_web.hpp"
#elif defined(LUDUS_PLATFORM_COCOA)
#    include "internal/window_cocoa.hpp"
#elif defined(LUDUS_PLATFORM_WAYLAND)
#    include <ludus/platform/wayland/window.h>
#elif defined(LUDUS_PLATFORM_HEADLESS)
#    include <ludus/platform/headless/window.h>
#else
#    error "No platform windowing backend selected (expected LUDUS_PLATFORM_WAYLAND or LUDUS_PLATFORM_HEADLESS)"
#endif

namespace ludus::platform
{
bool WindowManager::Initialize(const InitializeInfo& info) noexcept
{
#if defined(LUDUS_PLATFORM_BROWSER)
    return browser::InitializeBrowser(info);
#elif defined(LUDUS_PLATFORM_COCOA)
    return cocoa::Initialize(info);
#elif defined(LUDUS_PLATFORM_WAYLAND)
    return wayland::InitializeWayland(info);
#elif defined(LUDUS_PLATFORM_HEADLESS)
    return headless::InitializeHeadless(info);
#endif
}

bool WindowManager::CreateWindow(const WindowBase::CreateInfo& info,
                                 ludus::foundation::core::UniquePtr<Window>& outWindow) noexcept
{
#if defined(LUDUS_PLATFORM_COCOA)
    // Reject before Reset(): destroying an existing Cocoa window off the main
    // thread would violate AppKit ownership even if creation itself is rejected.
    if (cocoa::OnMainThread() == false)
    {
        return false;
    }
#endif
    // Replacing an output window must destroy its callbacks before creation.
    outWindow.Reset();
#if defined(LUDUS_PLATFORM_BROWSER)
    auto window = browser::CreateWindow(info);
#elif defined(LUDUS_PLATFORM_COCOA)
    auto window = cocoa::CreateWindow(info);
#elif defined(LUDUS_PLATFORM_WAYLAND)
    auto window = wayland::CreateWindow(info);
#elif defined(LUDUS_PLATFORM_HEADLESS)
    auto window = headless::CreateWindow(info);
#endif
    if (window.Get() == nullptr)
    {
        outWindow.Reset();
        return false;
    }

    outWindow = std::move(window);
    return true;
}
} // namespace ludus::platform

#if !defined(LUDUS_PLATFORM_BROWSER)
#    include <ludus/platform/browser/window.h>
namespace ludus::platform::browser
{
// Canvas replacement is a browser-only recovery; native windowing has no canvas.
bool ReplaceCanvas(const char*) noexcept
{
    return false;
}
} // namespace ludus::platform::browser
#endif
