#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/platform/base/window.h>
#include <ludus/platform/browser/window.h>
#include <ludus/platform/native_window.h>

#include "internal/window_web.hpp"
#include "internal/window_web_bridge.hpp"

#include <new>
#include <string>

namespace ludus::platform::browser
{
namespace
{
using namespace ludus::foundation;
constexpr usize MAX_WINDOWS = 16;
constexpr usize EVENT_CAPACITY = 128;
constexpr uint32 MAX_FRAMEBUFFER_LIMIT = 32768;

struct Signal
{
    EventKind Kind;
    uint32 A;
    uint32 B;
    float64 X;
    float64 Y;
    float64 Z;
    float64 W;
};

class BrowserWindow;
struct Slot
{
    BrowserWindow* Window = nullptr;
    uint32 Token = 0;
};
Slot gWindows[MAX_WINDOWS];
uint32 gNextToken = 1;

class BrowserWindow final : public WindowBase
{
public:
    explicit BrowserWindow(const CreateInfo& info) noexcept
        : WindowBase(info), mSelector(info.CanvasSelector), mLimit(info.FramebufferLimit)
    {
        mNativeWindowInfo.System = WindowSystem::WebCanvas;
        mNativeWindowInfo.CanvasSelector = mSelector.c_str();
        mNativeWindowInfo.Width = 0;
        mNativeWindowInfo.Height = 0;
        // Mirrors WindowState::Visible, which stays false until the bridge reports the canvas.
        mVisibility = WindowVisibility::Occluded;
    }

    ~BrowserWindow() noexcept override
    {
        for (auto& slot : gWindows)
        {
            if (slot.Window == this)
            {
                slot.Window = nullptr; // invalidate before foreign cleanup
                LudusBrowserDetach(slot.Token);
                slot.Token = 0;
            }
        }
    }

    bool Attach(bool capture) noexcept
    {
        if (gNextToken == 0)
        {
            return false; // refuse generation wrap
        }
        for (auto& slot : gWindows)
        {
            if (slot.Window == nullptr)
            {
                mToken = gNextToken++;
                slot = {this, mToken};
                return LudusBrowserAttach(mToken, mSelector.c_str(), mLimit, capture ? 1 : 0) != 0;
            }
        }
        return false;
    }

    bool HandleEvent(const Event&) noexcept override
    {
        if (LudusBrowserRefresh(mToken, mLimit) == 0)
        {
            ResetInput();
            mState.Attached = false;
            mState.Visible = false;
            mVisibility = WindowVisibility::Occluded;
            mState.Focused = false;
            mNativeWindowInfo.Width = 0;
            mNativeWindowInfo.Height = 0;
            mState.FramebufferWidth = 0;
            mState.FramebufferHeight = 0;
            return false;
        }
        return mState.Attached;
    }

    bool GetBrowserState(WindowState& out) const noexcept override
    {
        out = mState;
        return true;
    }

    bool PollBrowserInput(InputEvent& out) noexcept override
    {
        if (mEventCount == 0)
        {
            return false;
        }
        out = mEvents[mReadIndex];
        mReadIndex = (mReadIndex + 1) % EVENT_CAPACITY;
        --mEventCount;
        return true;
    }

    bool SetBrowserFramebufferLimit(uint32 limit) noexcept override
    {
        if (limit == 0 || limit > MAX_FRAMEBUFFER_LIMIT)
        {
            return false;
        }
        mLimit = limit;
        return HandleEvent({});
    }

    void Receive(const Signal& packet) noexcept
    {
        InputEvent event;
        event.Kind = packet.Kind;
        switch (packet.Kind)
        {
            case EventKind::Resize:
                mState.CssWidth = packet.X;
                mState.CssHeight = packet.Y;
                mState.PixelRatio = packet.Z;
                mState.Attached = packet.W != 0;
                mState.FramebufferWidth = packet.A;
                mState.FramebufferHeight = packet.B;
                mNativeWindowInfo.Width = packet.A;
                mNativeWindowInfo.Height = packet.B;
                break;
            case EventKind::Focus:
                mState.Focused = packet.A != 0;
                if (mState.Focused == false)
                {
                    ResetInput();
                }
                break;
            case EventKind::Visibility:
                mState.Visible = packet.A != 0;
                mVisibility = mState.Visible == true ? WindowVisibility::Visible : WindowVisibility::Occluded;
                if (mState.Visible == false)
                {
                    ResetInput();
                }
                break;
            case EventKind::KeyDown:
            case EventKind::KeyUp:
                if (packet.A < static_cast<uint32>(Key::Count))
                {
                    event.PhysicalKey = static_cast<Key>(packet.A);
                    if (packet.A != 0)
                    {
                        mState.Keys[packet.A] = packet.Kind == EventKind::KeyDown;
                    }
                }
                event.Repeat = packet.B != 0;
                break;
            case EventKind::PointerDown:
            case EventKind::PointerUp:
            case EventKind::PointerMove:
                mState.Buttons = packet.A;
                mState.PointerX = packet.X;
                mState.PointerY = packet.Y;
                event.Buttons = packet.A;
                event.X = packet.X;
                event.Y = packet.Y;
                break;
            case EventKind::Wheel:
                event.WheelX = packet.X;
                event.WheelY = packet.Y;
                break;
            case EventKind::PointerCancel:
                mState.Buttons = 0;
                break;
            case EventKind::InputReset:
                ResetInput();
                return;
        }
        ++mState.Revision;
        PushEvent(event);
    }

private:
    void PushEvent(const InputEvent& event) noexcept
    {
        if (mEventCount == EVENT_CAPACITY)
        {
            ++mState.DroppedEvents;
            return;
        }
        mEvents[(mReadIndex + mEventCount) % EVENT_CAPACITY] = event;
        ++mEventCount;
    }
    void ResetInput() noexcept
    {
        for (bool& held : mState.Keys)
        {
            held = false;
        }
        mState.Buttons = 0;
        ++mState.Revision;
        PushEvent({ .Kind = EventKind::InputReset });
    }

    std::string mSelector;
    uint32 mLimit;
    uint32 mToken = 0;
    WindowState mState;
    InputEvent mEvents[EVENT_CAPACITY];
    usize mReadIndex = 0;
    usize mEventCount = 0;
};

void Deliver(uint32 token, const Signal& packet) noexcept
{
    for (const auto& slot : gWindows)
    {
        if (slot.Token == token && slot.Window != nullptr)
        {
            slot.Window->Receive(packet);
            return;
        }
    }
}
} // namespace

bool InitializeBrowser(const WindowManager::InitializeInfo&) noexcept
{
    return true; // no synchronous native endpoint or worker startup
}

UniquePtr<Window> CreateWindow(const Window::CreateInfo& info) noexcept
{
    if (info.CanvasSelector.empty() || info.CanvasSelector.size() > 256 ||
        info.CanvasSelector.find('\0') != std::string_view::npos || info.FramebufferLimit == 0 ||
        info.FramebufferLimit > MAX_FRAMEBUFFER_LIMIT)
    {
        return {};
    }
    auto* window = new (std::nothrow) BrowserWindow(info);
    if (window == nullptr)
    {
        return {};
    }
    if (window->Attach(info.CaptureBrowserInput) == false)
    {
        delete window;
        return {};
    }
    Window* base = window;
    return UniquePtr<Window>(base);
}
bool ReplaceCanvas(const char* selector) noexcept
{
    return selector != nullptr && LudusBrowserReplaceCanvas(selector) != 0;
}
} // namespace ludus::platform::browser

// Fixed numeric ABI for the private JS bridge, not an engine call-site API.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
extern "C" EMSCRIPTEN_KEEPALIVE void LudusBrowserEvent(ludus::foundation::uint32 token,
                                                       ludus::foundation::uint32 kind,
                                                       ludus::foundation::uint32 a,
                                                       ludus::foundation::uint32 b,
                                                       ludus::foundation::float64 x,
                                                       ludus::foundation::float64 y,
                                                       ludus::foundation::float64 z,
                                                       ludus::foundation::float64 w) noexcept
{
    if (kind <= static_cast<ludus::foundation::uint32>(ludus::platform::browser::EventKind::PointerCancel))
    {
        ludus::platform::browser::Deliver(token,
                                          {static_cast<ludus::platform::browser::EventKind>(kind), a, b, x, y, z, w});
    }
}
