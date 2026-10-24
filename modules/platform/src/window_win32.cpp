#include "internal/window_win32.hpp"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/platform/base/window.h>
#include <ludus/platform/log_categories.h>
#include <ludus/platform/native_window.h>

#include <new>

#if !defined(LUDUS_PLATFORM_WINDOWS)
#    error "The Win32 window backend requires Windows"
#endif

#include <windows.h>
// The SDK maps CreateWindow to CreateWindowW; Ludus names its factory CreateWindow.
#undef CreateWindow

// Thanks to Microsoft, "Writing the Window Procedure" and "Managing Application State",
// Win32 programming guide, for the per-window state stored with GWLP_USERDATA:
// https://learn.microsoft.com/windows/win32/learnwin32/managing-application-state-
// and to Microsoft, "WM_DPICHANGED message", for resizing to the suggested rectangle:
// https://learn.microsoft.com/windows/win32/hidpi/wm-dpichanged
namespace ludus::platform::win32
{
namespace
{
using foundation::float32;
using foundation::int32;
using foundation::uint32;
using foundation::usize;

constexpr wchar_t WINDOW_CLASS_NAME[] = L"LudusWin32Window";
/// Content extent accepted at creation, in logical (96 DPI) units; matches Cocoa.
constexpr uint32 MAX_CONTENT_EXTENT = 16384;
/// Longest window title, in UTF-8 bytes; matches Cocoa.
constexpr usize MAX_WINDOW_NAME_BYTES = 4096;
/// UTF-16 never needs more code units than the UTF-8 bytes it decodes, plus a terminator.
constexpr usize MAX_WINDOW_NAME_UNITS = MAX_WINDOW_NAME_BYTES + 1;
constexpr DWORD WINDOW_STYLE = WS_OVERLAPPEDWINDOW;
constexpr DWORD WINDOW_EXTENDED_STYLE = WS_EX_APPWINDOW;

bool isInitialized = false;

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter) noexcept;

class Win32Window final : public WindowBase
{
public:
    LUDUS_INLINE explicit Win32Window(const CreateInfo& info) noexcept
        : WindowBase(info), mOwnerThread(GetCurrentThreadId())
    {
    }
    ~Win32Window() noexcept override;
    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    bool HandleEvent(const Event& event) noexcept override;
    bool Open() noexcept;
    /// Messages arrive during CreateWindowExW, before Open() stores the window handle.
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter) noexcept;

private:
    void refreshExtent() noexcept;
    void close() noexcept;

    HWND mWindowOrNull = nullptr;
    DWORD mOwnerThread = 0;
    bool mIsClosed = false;
};

[[nodiscard]] bool IsOwnerThread(DWORD owner) noexcept
{
    return GetCurrentThreadId() == owner;
}

[[nodiscard]] float32 GetScale(UINT dpi) noexcept
{
    return dpi == 0 ? 1.0F : static_cast<float32>(dpi) / static_cast<float32>(USER_DEFAULT_SCREEN_DPI);
}

[[nodiscard]] int32 ScaleExtent(uint32 logical, UINT dpi) noexcept
{
    // MulDiv rounds; the result fits because both inputs are bounded (16384 * 96x DPI).
    return MulDiv(static_cast<int32>(logical), static_cast<int32>(dpi), USER_DEFAULT_SCREEN_DPI);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter) noexcept
{
    if (message == WM_NCCREATE)
    {
        // Win32 carries pointers through LPARAM and LONG_PTR; these casts are its ABI.
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        const CREATESTRUCTW* creation = reinterpret_cast<const CREATESTRUCTW*>(longParameter);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr): GWLP_USERDATA holds the owner stored at WM_NCCREATE.
    Win32Window* ownerOrNull = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (ownerOrNull == nullptr)
    {
        return DefWindowProcW(window, message, wordParameter, longParameter);
    }
    return ownerOrNull->HandleMessage(window, message, wordParameter, longParameter);
}

Win32Window::~Win32Window() noexcept
{
    LUDUS_REQUIRE(IsOwnerThread(mOwnerThread), "Win32 windows must be destroyed on the thread that created them");
    if (mWindowOrNull != nullptr)
    {
        // Detach first so DestroyWindow's messages no longer reach this object.
        HWND window = mWindowOrNull;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        close();
        DestroyWindow(window);
    }
}

bool Win32Window::Open() noexcept
{
    wchar_t title[MAX_WINDOW_NAME_UNITS]{};
    if (mName.empty() == false)
    {
        const int32 units = MultiByteToWideChar(CP_UTF8,
                                                MB_ERR_INVALID_CHARS,
                                                mName.data(),
                                                static_cast<int32>(mName.size()),
                                                title,
                                                static_cast<int32>(MAX_WINDOW_NAME_UNITS - 1));
        if (units <= 0)
        {
            LUDUS_LOG_ERROR(LOG_PLATFORM, "Win32: window title is not valid UTF-8");
            return false;
        }
    }
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    mWindowOrNull = CreateWindowExW(WINDOW_EXTENDED_STYLE,
                                    WINDOW_CLASS_NAME,
                                    title,
                                    WINDOW_STYLE,
                                    CW_USEDEFAULT,
                                    CW_USEDEFAULT,
                                    CW_USEDEFAULT,
                                    CW_USEDEFAULT,
                                    nullptr,
                                    nullptr,
                                    instance,
                                    this);
    if (mWindowOrNull == nullptr)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM, "Win32: CreateWindowExW failed ({})", static_cast<uint32>(GetLastError()));
        return false;
    }
    // Size the client area for the window's monitor DPI now that the window exists.
    const UINT dpi = GetDpiForWindow(mWindowOrNull);
    RECT bounds =
    {
        .left = 0,
        .top = 0,
        .right = ScaleExtent(mNativeWindowInfo.Width, dpi),
        .bottom = ScaleExtent(mNativeWindowInfo.Height, dpi),
    };
    if (AdjustWindowRectExForDpi(&bounds, WINDOW_STYLE, FALSE, WINDOW_EXTENDED_STYLE, dpi) == FALSE ||
        SetWindowPos(mWindowOrNull,
                     nullptr,
                     0,
                     0,
                     bounds.right - bounds.left,
                     bounds.bottom - bounds.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) == FALSE)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM, "Win32: sizing the window failed ({})", static_cast<uint32>(GetLastError()));
        return false;
    }
    mNativeWindowInfo.System = WindowSystem::Win32;
    mNativeWindowInfo.Win32WindowOrNull = mWindowOrNull;
    ShowWindow(mWindowOrNull, SW_SHOW);
    refreshExtent();
    return true;
}

bool Win32Window::HandleEvent([[maybe_unused]] const Event& event) noexcept
{
    if (IsOwnerThread(mOwnerThread) == false || mIsClosed == true)
    {
        return false;
    }
    // Win32 delivers a window's messages only to its creating thread. Pumping the
    // thread queue also dispatches messages for this thread's other windows.
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
    {
        if (message.message == WM_QUIT)
        {
            // Leave the quit request for the application's own loop.
            PostQuitMessage(static_cast<int32>(message.wParam));
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    refreshExtent();
    return mIsClosed == false;
}

LRESULT Win32Window::HandleMessage(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter) noexcept
{
    switch (message)
    {
        case WM_CLOSE:
            // Closing ends the window; the owner observes it through HandleEvent.
            close();
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            close();
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            mWindowOrNull = nullptr;
            break;
        case WM_SIZE:
            refreshExtent();
            return 0;
        case WM_DPICHANGED: {
            // NOLINTNEXTLINE(performance-no-int-to-ptr): WM_DPICHANGED passes the suggested RECT in LPARAM.
            const RECT* suggested = reinterpret_cast<const RECT*>(longParameter);
            SetWindowPos(window,
                         nullptr,
                         suggested->left,
                         suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            refreshExtent();
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(window, message, wordParameter, longParameter);
}

void Win32Window::refreshExtent() noexcept
{
    if (mIsClosed == true || mWindowOrNull == nullptr)
    {
        return;
    }
    const bool isMinimized = IsIconic(mWindowOrNull) != FALSE;
    RECT client{};
    if (isMinimized == true || GetClientRect(mWindowOrNull, &client) == FALSE)
    {
        mNativeWindowInfo.Width = 0;
        mNativeWindowInfo.Height = 0;
    }
    else
    {
        mNativeWindowInfo.Width = static_cast<uint32>(client.right - client.left);
        mNativeWindowInfo.Height = static_cast<uint32>(client.bottom - client.top);
    }
    mNativeWindowInfo.Scale = GetScale(GetDpiForWindow(mWindowOrNull));
    // Win32 offers no portable occlusion signal; a hidden window reports Occluded.
    mVisibility = isMinimized == true                       ? WindowVisibility::Minimized
                  : IsWindowVisible(mWindowOrNull) == FALSE ? WindowVisibility::Occluded
                                                            : WindowVisibility::Visible;
}

void Win32Window::close() noexcept
{
    if (mIsClosed == true)
    {
        return;
    }
    mIsClosed = true;
    mNativeWindowInfo.Win32WindowOrNull = nullptr;
    mNativeWindowInfo.Width = 0;
    mNativeWindowInfo.Height = 0;
    mVisibility = WindowVisibility::Occluded;
}
} // namespace

bool Initialize([[maybe_unused]] const WindowManager::InitializeInfo& info) noexcept
{
    if (isInitialized == true)
    {
        return true;
    }
    // Per-monitor v2 makes client extents physical pixels. A manifest or an earlier
    // call may already have fixed the process mode; keep that choice.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const WNDCLASSEXW windowClass =
    {
        .cbSize = sizeof(WNDCLASSEXW),
        .style = CS_OWNDC,
        .lpfnWndProc = WindowProcedure,
        .cbClsExtra = 0,
        .cbWndExtra = 0,
        .hInstance = GetModuleHandleW(nullptr),
        .hIcon = nullptr,
        .hCursor = LoadCursorW(nullptr, IDC_ARROW),
        .hbrBackground = nullptr,
        .lpszMenuName = nullptr,
        .lpszClassName = WINDOW_CLASS_NAME,
        .hIconSm = nullptr,
    };
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM, "Win32: RegisterClassExW failed ({})", static_cast<uint32>(GetLastError()));
        return false;
    }
    isInitialized = true;
    return true;
}

foundation::UniquePtr<Window> CreateWindow(const Window::CreateInfo& info) noexcept
{
    if (isInitialized == false || info.Width == 0 || info.Height == 0 || info.Width > MAX_CONTENT_EXTENT ||
        info.Height > MAX_CONTENT_EXTENT || info.Name.size() > MAX_WINDOW_NAME_BYTES)
    {
        return nullptr;
    }
    Win32Window* allocated = new (std::nothrow) Win32Window(info);
    foundation::UniquePtr<Win32Window> window(allocated);
    if (window.Get() == nullptr || window->Open() == false)
    {
        return nullptr;
    }
    Window* base = window.Release();
    return foundation::UniquePtr<Window>(base);
}
} // namespace ludus::platform::win32
