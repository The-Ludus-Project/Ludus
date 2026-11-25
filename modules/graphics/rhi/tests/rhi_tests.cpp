// Include the public API before Volk to catch native-handle namespace mistakes.
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/native_window.h>

#include <cstdlib>
#include <initializer_list>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <volk.h>

#include "internal/vulkan_diagnostics.h"

using namespace ludus;

static_assert(std::is_same_v<decltype(graphics::rhi::WindowInfo::Display), ::wl_display*>);
static_assert(std::is_same_v<decltype(graphics::rhi::WindowInfo::Surface), ::wl_surface*>);

TEST_CASE("VkResult keeps its signed code in logging and assertions", "[rhi][diagnostics]")
{
    const VkResult results[] = {VK_SUCCESS,
                                VK_TIMEOUT,
                                VK_SUBOPTIMAL_KHR,
                                VK_ERROR_DEVICE_LOST,
                                VK_ERROR_OUT_OF_DATE_KHR,
                                static_cast<VkResult>(-123456)};
    for (const VkResult result : results)
    {
        const auto diagnostic = foundation::diagnostics::detail::MakeDiagnosticArg(result);
        REQUIRE(diagnostic.Kind == foundation::diagnostics::detail::DiagnosticArgKind::Signed);
        CHECK(diagnostic.Value.Signed == static_cast<foundation::int32>(result));

        const foundation::logging::FormatArg logArg(result);
        REQUIRE(logArg.Tag == foundation::logging::ArgTag::I64);
        CHECK(logArg.I == diagnostic.Value.Signed);

        // Exercise argument packing through the public macro as well. The
        // condition succeeds so this must not invoke the failure handler.
        CHECK(LUDUS_CHECK_F(true, "Vulkan result: {}", result));
    }
}

TEST_CASE("Frames require a ready RHI session", "[rhi]")
{
    graphics::rhi::Shutdown();
    CHECK(graphics::rhi::BeginFrameStatus() == graphics::rhi::FrameStatus::NotReady);
    CHECK(graphics::rhi::EndFrameStatus() == graphics::rhi::FrameStatus::NotReady);
}

#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
namespace
{
namespace rhi = graphics::rhi;

platform::WindowManager& GetWaylandWindowManager()
{
    // Platform owns one Wayland registry for the process, including across Catch2 sections.
    static platform::WindowManager manager = [] {
        platform::WindowManager windowManager;
        REQUIRE(windowManager.Initialize({}) == true);
        return windowManager;
    }();
    return manager;
}

struct ShutdownGuard final
{
    ~ShutdownGuard()
    {
        rhi::Shutdown();
    }
};

VKAPI_ATTR VkResult VKAPI_CALL
LoseOnAcquire(VkDevice, VkSwapchainKHR, foundation::uint64, VkSemaphore, VkFence, foundation::uint32*) noexcept
{
    return VK_ERROR_SURFACE_LOST_KHR;
}

PFN_vkQueuePresentKHR presentOrNull = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL LoseOnPresent(VkQueue queue, const VkPresentInfoKHR* present) noexcept
{
    // Thanks to Khronos Group's Vulkan Specification, vkQueuePresentKHR:
    // https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html
    // A surface-lost result still enqueues the wait. Keep the real present and its fence
    // in flight and change only the result observed by the RHI.
    LUDUS_REQUIRE(presentOrNull != nullptr);
    const VkResult result = presentOrNull(queue, present);
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR ? VK_ERROR_SURFACE_LOST_KHR : result;
}
} // namespace
#endif

TEST_CASE("Wayland surface and device lifecycle", "[rhi][wayland]")
{
    // Opt in on hosts with a running compositor and an eligible Vulkan GPU.
    if (std::getenv("LUDUS_TEST_WAYLAND") == nullptr)
    {
        SKIP("Set LUDUS_TEST_WAYLAND=1 to exercise the live Wayland/Vulkan lifecycle.");
    }
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
    platform::WindowManager& manager = GetWaylandWindowManager();
    foundation::UniquePtr<platform::Window> window;
    // Destroy Vulkan objects before the platform window even on assertion failure.
    const ShutdownGuard guard;

    REQUIRE(manager.CreateWindow({ .Name = "Ludus Wayland lifecycle test" }, window));
    const platform::NativeWindowInfo native = window->GetNativeWindowInfo();
    REQUIRE(native.System == platform::WindowSystem::Wayland);
    CHECK(rhi::Start({}, { .System = platform::WindowSystem::WebCanvas }) == rhi::StartStatus::Failed);
    CHECK(rhi::GetStartup().Error == rhi::StartupError::InvalidWindow);
    rhi::Shutdown();
    platform::NativeWindowInfo invalid = native;
    invalid.Surface = nullptr;
    CHECK(rhi::Start({}, invalid) == rhi::StartStatus::Failed);
    CHECK(rhi::GetStartup().Error == rhi::StartupError::InvalidWindow);
    rhi::Shutdown();
    for (int iteration = 0; iteration < 2; ++iteration)
    {
        REQUIRE(rhi::Start({ .Name = "Wayland lifecycle test" }, native) == rhi::StartStatus::Ready);
        REQUIRE(rhi::GetStartup().State == rhi::StartupState::Ready);
        CHECK(vkCreateWaylandSurfaceKHR != nullptr);
        CHECK(rhi::Start({}, native) == rhi::StartStatus::Busy);
        rhi::Shutdown();
        rhi::Shutdown();
    }
#else
    SKIP("Wayland was disabled at build time.");
#endif
}

#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
TEST_CASE("Wayland swapchain resumes after a zero-sized frame target", "[rhi][wayland][surface][resize]")
{
    if (std::getenv("LUDUS_TEST_WAYLAND") == nullptr)
    {
        SKIP("Set LUDUS_TEST_WAYLAND=1 to run with a Wayland compositor.");
    }
    platform::WindowManager& manager = GetWaylandWindowManager();
    foundation::UniquePtr<platform::Window> window;
    const ShutdownGuard guard;
    REQUIRE(manager.CreateWindow({ .Name = "Wayland resume", .Width = 641, .Height = 481 }, window));
    const platform::NativeWindowInfo native = window->GetNativeWindowInfo();
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    REQUIRE(rhi::CreateDevice({}, native, {}, device, surface) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::SetFrameTarget(device, surface, { .Width = native.Width, .Height = native.Height }) ==
            rhi::DeviceStatus::Ready);
    REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::SetFrameTarget(device, surface, { .Width = 0, .Height = 0 }) == rhi::DeviceStatus::Ready);
    CHECK(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Skipped);

    REQUIRE(window->HandleEvent({}));
    const platform::NativeWindowInfo restored = window->GetNativeWindowInfo();
    REQUIRE(rhi::SetFrameTarget(device, surface, { .Width = restored.Width, .Height = restored.Height }) ==
            rhi::DeviceStatus::Ready);
    REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    rhi::FrameInfo frame;
    REQUIRE(rhi::GetFrameInfo(device, surface, frame) == rhi::DeviceStatus::Ready);
    CHECK(frame.Width == restored.Width);
    CHECK(frame.Height == restored.Height);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
}

TEST_CASE("R14b losing a Vulkan Wayland surface preserves the other swapchain", "[rhi][wayland][surface]")
{
    if (std::getenv("LUDUS_TEST_WAYLAND") == nullptr)
    {
        SKIP("Set LUDUS_TEST_WAYLAND=1 to exercise Vulkan presentation on a running Wayland compositor.");
    }

    platform::WindowManager& manager = GetWaylandWindowManager();
    foundation::UniquePtr<platform::Window> primaryWindow;
    foundation::UniquePtr<platform::Window> secondWindow;
    const ShutdownGuard guard;

    REQUIRE(manager.CreateWindow({ .Name = "R14b Wayland primary" }, primaryWindow));
    REQUIRE(manager.CreateWindow({ .Name = "R14b Wayland second" }, secondWindow));
    const platform::NativeWindowInfo primaryTarget = primaryWindow->GetNativeWindowInfo();
    const platform::NativeWindowInfo secondTarget = secondWindow->GetNativeWindowInfo();
    REQUIRE(primaryTarget.System == platform::WindowSystem::Wayland);
    REQUIRE(secondTarget.System == platform::WindowSystem::Wayland);

    rhi::DeviceHandle device;
    rhi::SurfaceHandle primary;
    REQUIRE(rhi::CreateDevice({}, primaryTarget, {}, device, primary) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::SetFrameTarget(device, primary, { .Width = primaryTarget.Width, .Height = primaryTarget.Height }) ==
            rhi::DeviceStatus::Ready);
    rhi::SurfaceHandle second;
    REQUIRE(rhi::CreateSurface(device, secondTarget, second) == rhi::DeviceStatus::Ready);

    REQUIRE(rhi::BeginFrame(device, primary) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::BeginFrame(device, second) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);

    foundation::uint64 submittedFrames = 1;
    SECTION("Acquisition fails before submission")
    {
        const PFN_vkAcquireNextImageKHR acquire = vkAcquireNextImageKHR;
        vkAcquireNextImageKHR = LoseOnAcquire;
        const rhi::DeviceStatus status = rhi::BeginFrame(device, second);
        vkAcquireNextImageKHR = acquire;
        REQUIRE(status == rhi::DeviceStatus::SurfaceLost);
    }
    SECTION("Presentation fails after submission")
    {
        REQUIRE(rhi::BeginFrame(device, second) == rhi::DeviceStatus::Ready);
        presentOrNull = vkQueuePresentKHR;
        vkQueuePresentKHR = LoseOnPresent;
        const rhi::DeviceStatus status = rhi::EndFrame(device);
        vkQueuePresentKHR = presentOrNull;
        presentOrNull = nullptr;
        REQUIRE(status == rhi::DeviceStatus::Ready);
        ++submittedFrames;
    }

    rhi::SurfaceInfo info;
    REQUIRE(rhi::GetSurfaceInfo(device, second, info) == rhi::DeviceStatus::Ready);
    CHECK(info.State == rhi::SurfaceState::Lost);
    CHECK(info.SubmittedFrames == submittedFrames);
    CHECK(info.RenderedFrames == submittedFrames);
    CHECK(info.PresentedFrames == 1);
    CHECK(rhi::BeginFrame(device, second) == rhi::DeviceStatus::SurfaceLost);

    rhi::DeviceInfo deviceInfo;
    REQUIRE(rhi::GetDeviceInfo(device, deviceInfo) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::GetSurfaceInfo(device, primary, info) == rhi::DeviceStatus::Ready);
    CHECK(info.State == rhi::SurfaceState::Ready);
    CHECK(info.SubmittedFrames == 1);
    CHECK(info.RenderedFrames == 1);
    CHECK(info.PresentedFrames == 1);
    REQUIRE(rhi::BeginFrame(device, primary) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::GetSurfaceInfo(device, primary, info) == rhi::DeviceStatus::Ready);
    CHECK(info.SubmittedFrames == 2);
    CHECK(info.PresentedFrames == 2);

    const rhi::SurfaceHandle stale = second;
    REQUIRE(rhi::DestroySurface(device, second) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::CreateSurface(device, secondTarget, second) == rhi::DeviceStatus::Ready);
    CHECK(rhi::BeginFrame(device, stale) == rhi::DeviceStatus::InvalidHandle);
    REQUIRE(rhi::GetSurfaceInfo(device, second, info) == rhi::DeviceStatus::Ready);
    CHECK(info.SubmittedFrames == 0);
    CHECK(info.RenderedFrames == 0);
    CHECK(info.PresentedFrames == 0);
    REQUIRE(rhi::BeginFrame(device, second) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::GetSurfaceInfo(device, second, info) == rhi::DeviceStatus::Ready);
    CHECK(info.SubmittedFrames == 1);
    CHECK(info.PresentedFrames == 1);
    REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
}
#endif

TEST_CASE("Native startup rejects invalid windows without loading a GPU", "[rhi][lifecycle]")
{
    graphics::rhi::Shutdown();
    CHECK(graphics::rhi::Start({}, { .System = platform::WindowSystem::WebCanvas }) ==
          graphics::rhi::StartStatus::Failed);
    CHECK(graphics::rhi::GetStartup().State == graphics::rhi::StartupState::Failed);
    CHECK(graphics::rhi::GetStartup().Error == graphics::rhi::StartupError::InvalidWindow);
    CHECK(graphics::rhi::BeginFrameStatus() == graphics::rhi::FrameStatus::NotReady);
    graphics::rhi::Shutdown();
}

TEST_CASE("Native startup rejects forced browser policies explicitly", "[rhi][capabilities]")
{
    for (const auto selection : {graphics::rhi::BackendSelection::WebGPU, graphics::rhi::BackendSelection::WebGL2})
    {
        graphics::rhi::Shutdown();
        CHECK(graphics::rhi::Start({}, {}, selection) == graphics::rhi::StartStatus::Failed);
        CHECK(graphics::rhi::GetStartup().Error == graphics::rhi::StartupError::BackendUnavailable);
        CHECK(graphics::rhi::GetStartup().Capabilities.MaxFrameDimension2D == 0);
    }
    graphics::rhi::Shutdown();
}
