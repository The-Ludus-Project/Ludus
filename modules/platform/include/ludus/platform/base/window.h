#pragma once

#include <ludus/foundation/base/core.h>      // LUDUS_INLINE + foundational vocabulary
#include <ludus/foundation/base/pointer.hpp> // ludus::foundation::core::UniquePtr
#include <ludus/platform/config.h>
#include <ludus/platform/keyboard_sink.h>
#include <ludus/platform/native_window.h>

#include <string> // owned strings are required for stable window names
#include <string_view>

namespace ludus::platform
{
namespace browser
{
struct WindowState;
struct InputEvent;
} // namespace browser

/// @brief Whether any part of a window can currently be seen.
///
/// A loop that should save power lowers its rate while a window is not Visible
/// (docs/architecture/high-resolution-time.md, "Native frame pacing").
enum class WindowVisibility : ludus::foundation::uint8
{
    /// Some part of the window may be on screen, or the backend cannot tell.
    Visible,
    /// The window is open but no part is on screen: fully covered by opaque windows,
    /// on another Space, or ordered out (including a hidden application). A closed
    /// window also reports Occluded.
    Occluded,
    /// The window is minimized (miniaturized into the Dock on macOS). Its published extent is zero.
    Minimized,
};

class WindowBase
{
public:
    struct CreateInfo final
    {
        /// Window title; Cocoa accepts valid UTF-8 of at most 4096 bytes and copies it at creation.
        std::string Name;
        /// Requested content width; Cocoa interprets this in logical points (1..16384).
        ludus::foundation::uint32 Width = 800;
        /// Requested content height; Cocoa interprets this in logical points (1..16384).
        ludus::foundation::uint32 Height = 600;
        /// @brief Browser only.
        ///
        /// Selector is copied at creation; the matching canvas is borrowed and must remain in the DOM until
        /// this window is destroyed.
        std::string_view CanvasSelector = "#canvas";
        ludus::foundation::uint32 FramebufferLimit = 4096;
        bool CaptureBrowserInput = true;
    };

    struct Event final
    {
    };

public:
    virtual ~WindowBase() = default;
    /// Pumps native events; false means the window closed or the call cannot proceed.
    /// Cocoa pumps the shared application queue and may deliver input to other windows.
    /// Create, pump, query and destroy Cocoa windows on the main thread; sinks must not destroy them during dispatch.
    virtual bool HandleEvent(const Event& event) noexcept = 0;

    /// @brief Browser snapshots/events are copied into caller-owned storage.
    ///
    /// Native backends return false. Call HandleEvent once per animation frame to refresh CSS size/DPR/DOM
    /// attachment; callbacks also update state between frames.
    virtual bool GetBrowserState(browser::WindowState&) const noexcept
    {
        return false;
    }
    virtual bool PollBrowserInput(browser::InputEvent&) noexcept
    {
        return false;
    }
    /// Supply the actual device texture-dimension limit after GPU startup.
    virtual bool SetBrowserFramebufferLimit(ludus::foundation::uint32) noexcept
    {
        return false;
    }

    /// Returns borrowed backend handles and current dimensions; Cocoa dimensions are backing pixels.
    /// Cocoa snapshots must be queried on the main thread and refreshed after pumping events.
    [[nodiscard]] LUDUS_INLINE NativeWindowInfo GetNativeWindowInfo() const noexcept
    {
        return mNativeWindowInfo;
    }

    /// @brief Return the visibility observed by the latest HandleEvent().
    ///
    /// Cocoa reports Minimized for a miniaturized window and Occluded while AppKit's
    /// occlusion state says no part is visible; it is refreshed by HandleEvent() and
    /// window-delegate notifications, on the main thread. Browser windows report
    /// Occluded while the canvas is hidden, detached or zero-sized, matching
    /// browser::WindowState::Visible. Wayland and headless windows always report
    /// Visible because those backends do not observe occlusion.
    /// @note Occlusion is a power hint, not a guarantee: partial or translucent cover
    /// still reports Visible, and AppKit delivers changes asynchronously, so a newly
    /// opened window may report Occluded until the window server has placed it.
    [[nodiscard]] LUDUS_INLINE WindowVisibility GetVisibility() const noexcept
    {
        return mVisibility;
    }

    /// @brief Attach a normalized keyboard sink to THIS window (K10).
    ///
    /// The sink and its user data are borrowed: detach (or destroy the window) before the sink's user data goes
    /// away. Backends that deliver keyboard transitions (Wayland) route native events to the sink of the
    /// attached gameplay window; backends without keyboard delivery (headless) simply retain it. Attaching
    /// replaces any previous sink. This does not synthesize events by itself.
    LUDUS_INLINE void AttachKeyboardSink(const KeyboardSink& sink) noexcept
    {
        mKeyboardSink = sink;
    }
    LUDUS_INLINE void DetachKeyboardSink() noexcept
    {
        mKeyboardSink = KeyboardSink{};
    }
    [[nodiscard]] LUDUS_INLINE const KeyboardSink& GetKeyboardSink() const noexcept
    {
        return mKeyboardSink;
    }

protected:
    WindowBase() = delete;
    /// Copies the window name and requested dimensions; the backend publishes its native extent.
    /// The caller's CreateInfo may be released after construction.
    explicit WindowBase(const CreateInfo& info) noexcept;

protected:
    std::string mName;
    NativeWindowInfo mNativeWindowInfo = {};
    KeyboardSink mKeyboardSink = {};
    WindowVisibility mVisibility = WindowVisibility::Visible;
};

using Window = WindowBase;

class WindowManager final
{
public:
    struct InitializeInfo final
    {
    };

public:
    WindowManager() = default;
    ~WindowManager() = default;

    /// Initializes the selected backend; Cocoa requires the main thread and a WindowServer session.
    /// Repeated Cocoa initialization is safe; an existing host application's delegate and menu are preserved.
    bool Initialize(const InitializeInfo& info) noexcept;
    /// Creates a window, replacing outWindow; failure leaves it empty.
    /// Cocoa requires successful initialization and main-thread ownership through destruction.
    /// Off-main-thread Cocoa calls fail without changing outWindow; at most 16 Cocoa windows may be alive.
    bool CreateWindow(const WindowBase::CreateInfo& info,
                      ludus::foundation::core::UniquePtr<Window>& outWindow) noexcept;
};
} // namespace ludus::platform
