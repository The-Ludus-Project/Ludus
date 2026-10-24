#include <ludus/foundation/base/pointer.hpp>
#include <ludus/platform/base/window.h>
#include <ludus/platform/native_window.h>

#include <catch2/catch_test_macros.hpp>

#include <windows.h>
// The SDK maps CreateWindow to CreateWindowW; Ludus names its factory CreateWindow.
#undef CreateWindow

namespace
{
using ludus::foundation::UniquePtr;
using ludus::platform::Window;
using ludus::platform::WindowManager;

UniquePtr<Window> OpenWindow(const Window::CreateInfo& info)
{
    WindowManager manager;
    REQUIRE(manager.Initialize({}) == true);
    UniquePtr<Window> window;
    static_cast<void>(manager.CreateWindow(info, window));
    return window;
}
} // namespace

TEST_CASE("Win32 windows publish their HWND and client extent")
{
    UniquePtr<Window> window = OpenWindow({ .Name = "Ludus Win32 test", .Width = 320, .Height = 200 });
    REQUIRE(window.Get() != nullptr);
    REQUIRE(window->HandleEvent({}) == true);
    const ludus::platform::NativeWindowInfo info = window->GetNativeWindowInfo();
    CHECK(info.System == ludus::platform::WindowSystem::Win32);
    REQUIRE(info.Win32WindowOrNull != nullptr);
    CHECK(IsWindow(static_cast<HWND>(info.Win32WindowOrNull)) != FALSE);
    CHECK(info.Width > 0);
    CHECK(info.Height > 0);
    CHECK(info.Scale > 0.0F);
    // The requested extent is logical; the client extent is scaled by the window's DPI.
    const UINT dpi = GetDpiForWindow(static_cast<HWND>(info.Win32WindowOrNull));
    CHECK(info.Width == static_cast<ludus::foundation::uint32>(MulDiv(320, static_cast<int>(dpi), 96)));
    CHECK(info.Height == static_cast<ludus::foundation::uint32>(MulDiv(200, static_cast<int>(dpi), 96)));
}

TEST_CASE("Closing a Win32 window ends its event loop and clears its handles")
{
    UniquePtr<Window> window = OpenWindow({ .Name = "Ludus close test", .Width = 160, .Height = 120 });
    REQUIRE(window.Get() != nullptr);
    const HWND handle = static_cast<HWND>(window->GetNativeWindowInfo().Win32WindowOrNull);
    REQUIRE(PostMessageW(handle, WM_CLOSE, 0, 0) != FALSE);
    CHECK(window->HandleEvent({}) == false);
    const ludus::platform::NativeWindowInfo info = window->GetNativeWindowInfo();
    CHECK(info.Win32WindowOrNull == nullptr);
    CHECK(info.Width == 0);
    CHECK(info.Height == 0);
    CHECK(window->GetVisibility() == ludus::platform::WindowVisibility::Occluded);
    CHECK(IsWindow(handle) == FALSE);
    // Destroying a closed window is safe.
    window.Reset();
}

TEST_CASE("Minimized Win32 windows publish a zero extent")
{
    UniquePtr<Window> window = OpenWindow({ .Name = "Ludus minimize test", .Width = 160, .Height = 120 });
    REQUIRE(window.Get() != nullptr);
    const HWND handle = static_cast<HWND>(window->GetNativeWindowInfo().Win32WindowOrNull);
    ShowWindow(handle, SW_MINIMIZE);
    REQUIRE(window->HandleEvent({}) == true);
    CHECK(window->GetVisibility() == ludus::platform::WindowVisibility::Minimized);
    CHECK(window->GetNativeWindowInfo().Width == 0);
    CHECK(window->GetNativeWindowInfo().Height == 0);
    ShowWindow(handle, SW_RESTORE);
    REQUIRE(window->HandleEvent({}) == true);
    CHECK(window->GetVisibility() == ludus::platform::WindowVisibility::Visible);
    CHECK(window->GetNativeWindowInfo().Width > 0);
}

TEST_CASE("Win32 window creation rejects invalid requests")
{
    CHECK(OpenWindow({ .Name = "zero", .Width = 0, .Height = 120 }).Get() == nullptr);
    CHECK(OpenWindow({ .Name = "huge", .Width = 16385, .Height = 120 }).Get() == nullptr);
    CHECK(OpenWindow({ .Name = "bad \xff utf-8", .Width = 160, .Height = 120 }).Get() == nullptr);
}

TEST_CASE("Destroying an open Win32 window destroys its HWND")
{
    UniquePtr<Window> window = OpenWindow({ .Name = "Ludus destroy test", .Width = 160, .Height = 120 });
    REQUIRE(window.Get() != nullptr);
    const HWND handle = static_cast<HWND>(window->GetNativeWindowInfo().Win32WindowOrNull);
    window.Reset();
    CHECK(IsWindow(handle) == FALSE);
}
