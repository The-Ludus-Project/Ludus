#include "internal/window_cocoa.hpp"
#include "internal/cocoa_keymap.hpp"

#include <ludus/foundation/base/core.h>
#include <ludus/input/keyboard_event.h>
#include <ludus/platform/keyboard_sink.h>
#include <ludus/platform/native_window.h>

#include <cmath>
#include <limits>
#include <new>

#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>

#if !defined(LUDUS_PLATFORM_COCOA) || !defined(LUDUS_PLATFORM_MACOS)
#    error "The Cocoa backend requires a macOS platform target"
#endif

// Thanks to Apple, Cocoa Event Handling Guide, "Event Architecture" and
// "Handling Key Events": use NSApplication's shared queue and responder routing,
// with native keyCode normalization rather than character/text interpretation.
// https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/EventOverview/EventArchitecture/EventArchitecture.html
// https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/EventOverview/HandlingKeyEvents/HandlingKeyEvents.html
// NSWindowDelegate notifications track focus/close/resize; all AppKit ownership
// stays here on the main thread. Metal layer creation belongs to RHI.
namespace ludus::platform::cocoa
{
class CocoaWindow;
} // namespace ludus::platform::cocoa

@interface LudusCocoaWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) ludus::platform::cocoa::CocoaWindow* owner;
@end

@interface LudusCocoaView : NSView
@end

@interface LudusCocoaApplicationDelegate : NSObject <NSApplicationDelegate>
- (void)requestQuit:(id)sender;
@end

@interface LudusCocoaApplicationObserver : NSObject
- (void)didResignActive:(NSNotification*)notification;
- (void)didBecomeActive:(NSNotification*)notification;
@end

namespace ludus::platform::cocoa
{
using foundation::usize;
namespace
{
constexpr usize MAX_WINDOWS = 16;
constexpr uint32 MAX_CONTENT_EXTENT = 16384;
constexpr usize MAX_EVENTS_PER_PUMP = 256;
CocoaWindow* gWindows[MAX_WINDOWS] = {};
bool gInitialized = false;
bool gOwnsApplication = false;
// A host such as Qt may dispatch AppKit events without entering PumpEvents.
// Responder routing below covers that path; this pointer prevents duplicate
// physical records when our own pump already normalized the same event.
NSEvent* gNormalizedEvent = nil;
LudusCocoaApplicationDelegate* gApplicationDelegate = nil;
// NSNotificationCenter does not retain observers, so this strong ARC reference
// keeps the registered observer alive. It is written but never read, which
// Clang 23 reports as -Wunused-but-set-global.
[[maybe_unused]] LudusCocoaApplicationObserver* gApplicationObserver = nil;

bool MainThread() noexcept
{
    return [NSThread isMainThread];
}

uint32 PixelExtent(CGFloat value) noexcept
{
    if (std::isfinite(value) == false || value <= 0 || value > std::numeric_limits<uint32>::max())
    {
        return 0;
    }
    return static_cast<uint32>(std::ceil(value));
}

input::FocusBaseline CurrentBaseline(bool focused) noexcept
{
    input::FocusBaseline baseline;
    baseline.Focused = focused;
    if (focused)
    {
        for (uint32 code = 0; code < KEYCODE_COUNT; ++code)
        {
            const input::Key key = MapKeyCode(code);
            if (input::IsValidKey(key))
            {
                baseline.Keys[input::KeyIndex(key)] =
                    CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, static_cast<CGKeyCode>(code));
            }
        }
    }
    return baseline;
}
} // namespace

class CocoaWindow final : public WindowBase
{
public:
    explicit CocoaWindow(const CreateInfo& info) noexcept : WindowBase(info) {}
    ~CocoaWindow() noexcept override;
    bool Open() noexcept;
    bool HandleEvent(const Event&) noexcept override;
    void RefreshExtent() noexcept;
    void RefreshVisibility() noexcept;
    void FocusChanged(bool focused) noexcept;
    void Closed() noexcept;
    void SynchronizeSink() noexcept;
    void KeyEvent(NSEvent* event) noexcept;
    [[nodiscard]] NSWindow* NativeWindow() const noexcept
    {
        return mWindow;
    }

private:
    void Reset(input::ResetReason reason, bool focused) noexcept;
    NSWindow* mWindow = nil;
    LudusCocoaWindowDelegate* mDelegate = nil;
    KeyboardSink mSynchronizedSink{};
    bool mFocused = false;
    bool mClosed = false;
};

namespace
{
CocoaWindow* FindWindow(NSWindow* native) noexcept
{
    if (native != nil)
    {
        for (CocoaWindow* window : gWindows)
        {
            if (window != nullptr && window->NativeWindow() == native)
            {
                return window;
            }
        }
    }
    return nullptr;
}

void PumpEvents() noexcept
{
    @autoreleasepool
    {
        // Service ready run-loop sources/timers as well as queued NSEvents;
        // AppKit's asynchronous minimize/restore and activation use both.
        (void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
        for (CocoaWindow* window : gWindows)
        {
            if (window != nullptr)
            {
                window->SynchronizeSink();
            }
        }
        for (usize count = 0; count < MAX_EVENTS_PER_PUMP; ++count)
        {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event == nil)
            {
                break;
            }
            if (event.type == NSEventTypeKeyDown || event.type == NSEventTypeKeyUp ||
                event.type == NSEventTypeFlagsChanged)
            {
                CocoaWindow* window = FindWindow(event.window);
                if (window != nullptr)
                {
                    // Normalize before dispatch so Command/menu shortcuts and
                    // AppKit's special key-up routing cannot leave held keys stuck.
                    window->KeyEvent(event);
                }
            }
            NSEvent* previous = gNormalizedEvent;
            gNormalizedEvent = event;
            [NSApp sendEvent:event];
            gNormalizedEvent = previous;
        }
        [NSApp updateWindows];
    }
}
} // namespace

void DispatchResponderKey(NSEvent* event) noexcept
{
    if (MainThread() == false || event == gNormalizedEvent)
    {
        return;
    }
    if (auto* window = FindWindow(event.window))
    {
        window->KeyEvent(event);
    }
}

void CloseAllWindows() noexcept
{
    for (CocoaWindow* window : gWindows)
    {
        if (window != nullptr)
        {
            window->Closed();
            [window->NativeWindow() close];
        }
    }
}

void ApplicationFocusChanged(bool active) noexcept
{
    for (CocoaWindow* window : gWindows)
    {
        if (window != nullptr)
        {
            window->FocusChanged(active && window->NativeWindow().keyWindow);
        }
    }
}

bool OnMainThread() noexcept
{
    return MainThread();
}

bool Initialize([[maybe_unused]] const WindowManager::InitializeInfo& info) noexcept
{
    if (MainThread() == false)
    {
        return false;
    }
    @autoreleasepool
    {
        if (gInitialized)
        {
            return true;
        }
        gOwnsApplication = gOwnsApplication || NSApp == nil;
        NSApplication* application = [NSApplication sharedApplication];
        if (application == nil || [NSScreen screens].count == 0)
        {
            return false;
        }
        LudusCocoaApplicationObserver* observer = [[LudusCocoaApplicationObserver alloc] init];
        if (observer == nil)
        {
            return false;
        }
        if (gOwnsApplication)
        {
            if ([application setActivationPolicy:NSApplicationActivationPolicyRegular] == false)
            {
                return false;
            }
            gApplicationDelegate = [[LudusCocoaApplicationDelegate alloc] init];
            if (gApplicationDelegate == nil)
            {
                return false;
            }
            application.delegate = gApplicationDelegate;
            NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Ludus"];
            NSMenuItem* applicationItem = [[NSMenuItem alloc] initWithTitle:@"Ludus" action:nullptr keyEquivalent:@""];
            NSMenu* applicationMenu = [[NSMenu alloc] initWithTitle:@"Ludus"];
            NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:@"Quit Ludus"
                                                          action:@selector(requestQuit:)
                                                   keyEquivalent:@"q"];
            if (menu == nil || applicationItem == nil || applicationMenu == nil || quit == nil)
            {
                application.delegate = nil;
                gApplicationDelegate = nil;
                return false;
            }
            quit.target = gApplicationDelegate;
            [applicationMenu addItem:quit];
            applicationItem.submenu = applicationMenu;
            [menu addItem:applicationItem];
            application.mainMenu = menu;
            [application finishLaunching];
        }
        gApplicationObserver = observer;
        NSNotificationCenter* notifications = [NSNotificationCenter defaultCenter];
        [notifications addObserver:observer
                          selector:@selector(didResignActive:)
                              name:NSApplicationDidResignActiveNotification
                            object:application];
        [notifications addObserver:observer
                          selector:@selector(didBecomeActive:)
                              name:NSApplicationDidBecomeActiveNotification
                            object:application];
        gInitialized = true;
        return true;
    }
}

foundation::UniquePtr<Window> CreateWindow(const Window::CreateInfo& info) noexcept
{
    if (MainThread() == false || gInitialized == false || info.Width == 0 || info.Height == 0 ||
        info.Width > MAX_CONTENT_EXTENT || info.Height > MAX_CONTENT_EXTENT || info.Name.size() > 4096)
    {
        return nullptr;
    }
    for (CocoaWindow*& slot : gWindows)
    {
        if (slot == nullptr)
        {
            CocoaWindow* allocated = new (std::nothrow) CocoaWindow(info);
            foundation::UniquePtr<CocoaWindow> window(allocated);
            if (window.Get() == nullptr || window->Open() == false)
            {
                return nullptr;
            }
            slot = window.Get();
            Window* base = window.Release();
            return foundation::UniquePtr<Window>(base);
        }
    }
    return nullptr;
}

bool CocoaWindow::Open() noexcept
{
    @autoreleasepool
    {
        NSString* title = [[NSString alloc] initWithBytes:mName.data()
                                                   length:mName.size()
                                                 encoding:NSUTF8StringEncoding];
        if (title == nil)
        {
            return false;
        }
        const NSRect rectangle = NSMakeRect(0, 0, mNativeWindowInfo.Width, mNativeWindowInfo.Height);
        mWindow = [[NSWindow alloc] initWithContentRect:rectangle
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        // ARC owns the window even if a later delegate/view allocation fails.
        mWindow.releasedWhenClosed = NO;
        mDelegate = [[LudusCocoaWindowDelegate alloc] init];
        LudusCocoaView* view = [[LudusCocoaView alloc] initWithFrame:rectangle];
        if (mWindow == nil || mDelegate == nil || view == nil)
        {
            return false;
        }
        mDelegate.owner = this;
        mWindow.delegate = mDelegate;
        mWindow.title = title;
        mWindow.contentView = view;
        view.wantsLayer = YES;
        [mWindow makeFirstResponder:view];
        [mWindow center];
        mNativeWindowInfo.System = WindowSystem::Cocoa;
        mNativeWindowInfo.CocoaWindow = (__bridge void*)mWindow;
        mNativeWindowInfo.CocoaView = (__bridge void*)view;
        [mWindow makeKeyAndOrderFront:nil];
        [NSApp activate];
        FocusChanged(mWindow.keyWindow && NSApp.active);
        RefreshExtent();
        return true;
    }
}

CocoaWindow::~CocoaWindow() noexcept
{
    LUDUS_REQUIRE(MainThread(), "Cocoa windows must be destroyed on the main thread");
    @autoreleasepool
    {
        for (CocoaWindow*& slot : gWindows)
        {
            if (slot == this)
            {
                slot = nullptr;
            }
        }
        Closed();
        mDelegate.owner = nullptr;
        mWindow.delegate = nil;
        [mWindow close];
        mWindow = nil;
        mDelegate = nil;
    }
}

bool CocoaWindow::HandleEvent([[maybe_unused]] const Event& event) noexcept
{
    if (MainThread() == false || mClosed)
    {
        return false;
    }
    PumpEvents();
    RefreshExtent();
    return mClosed == false;
}

void CocoaWindow::RefreshExtent() noexcept
{
    if (mClosed)
    {
        return;
    }
    const NSRect backing = [mWindow.contentView convertRectToBacking:mWindow.contentView.bounds];
    const bool minimized = mWindow.miniaturized;
    mNativeWindowInfo.Width = minimized ? 0 : PixelExtent(backing.size.width);
    mNativeWindowInfo.Height = minimized ? 0 : PixelExtent(backing.size.height);
    RefreshVisibility();
}

void CocoaWindow::RefreshVisibility() noexcept
{
    if (mClosed)
    {
        return;
    }
    // Thanks to Apple, AppKit Release Notes for OS X 10.9 Mavericks, "Application and
    // Window Occlusion": occlusionState is a bitfield tested with NSWindowOcclusionStateVisible;
    // a window is occluded when fully covered by opaque windows, ordered off screen,
    // minimized or on another Space, and partial occlusion counts as visible. Ludus
    // reports minimization separately because the published extent is then zero.
    // https://developer.apple.com/library/archive/releasenotes/AppKit/RN-AppKitOlderNotes/index.html
    if (mWindow.miniaturized == YES)
    {
        mVisibility = WindowVisibility::Minimized;
    }
    else if ((mWindow.occlusionState & NSWindowOcclusionStateVisible) == 0)
    {
        mVisibility = WindowVisibility::Occluded;
    }
    else
    {
        mVisibility = WindowVisibility::Visible;
    }
}

void CocoaWindow::Reset(input::ResetReason reason, bool focused) noexcept
{
    const input::FocusBaseline baseline = CurrentBaseline(focused);
    const KeyboardSink sink = GetKeyboardSink();
    if (sink.OnReset != nullptr)
    {
        sink.OnReset(sink.UserData, reason, baseline);
    }
}

void CocoaWindow::SynchronizeSink() noexcept
{
    const KeyboardSink sink = GetKeyboardSink();
    if (sink.UserData != mSynchronizedSink.UserData || sink.OnRecord != mSynchronizedSink.OnRecord ||
        sink.OnReset != mSynchronizedSink.OnReset)
    {
        mSynchronizedSink = sink;
        Reset(mFocused ? input::ResetReason::FocusEntered : input::ResetReason::FocusLost, mFocused);
    }
}

void CocoaWindow::FocusChanged(bool focused) noexcept
{
    if (mClosed || mFocused == focused)
    {
        return;
    }
    mFocused = focused;
    Reset(focused ? input::ResetReason::FocusEntered : input::ResetReason::FocusLost, focused);
}

void CocoaWindow::Closed() noexcept
{
    if (mClosed == false)
    {
        mClosed = true;
        mFocused = false;
        mNativeWindowInfo.CocoaWindow = nullptr;
        mNativeWindowInfo.CocoaView = nullptr;
        mNativeWindowInfo.Width = 0;
        mNativeWindowInfo.Height = 0;
        mVisibility = WindowVisibility::Occluded;
        Reset(input::ResetReason::WindowClosed, false);
    }
}

void CocoaWindow::KeyEvent(NSEvent* event) noexcept
{
    if (mFocused == false || mClosed)
    {
        return;
    }
    const input::Key key = MapKeyCode(event.keyCode);
    if (input::IsValidKey(key) == false)
    {
        return;
    }
    bool down = event.type == NSEventTypeKeyDown;
    if (event.type == NSEventTypeFlagsChanged)
    {
        const uint64 mask = ModifierMask(key);
        if (mask == 0)
        {
            return;
        }
        down = (static_cast<uint64>(event.modifierFlags) & mask) != 0;
    }
    input::KeyboardRecord record;
    record.Source = input::RecordSource::Native;
    record.PhysicalKey = key;
    record.Transition = down ? input::KeyTransition::Down : input::KeyTransition::Up;
    record.Repeat = event.type == NSEventTypeKeyDown && event.ARepeat;
    const KeyboardSink sink = GetKeyboardSink();
    if (sink.OnRecord != nullptr)
    {
        sink.OnRecord(sink.UserData, record);
    }
}
} // namespace ludus::platform::cocoa

@implementation LudusCocoaWindowDelegate
- (void)windowDidBecomeKey:(NSNotification*)notification
{
    (void)notification;
    if (self.owner != nullptr)
    {
        self.owner->FocusChanged(true);
    }
}
- (void)windowDidResignKey:(NSNotification*)notification
{
    (void)notification;
    if (self.owner != nullptr)
    {
        self.owner->FocusChanged(false);
    }
}
- (void)windowWillClose:(NSNotification*)notification
{
    (void)notification;
    if (self.owner != nullptr)
    {
        self.owner->Closed();
    }
}
- (void)windowDidResize:(NSNotification*)notification
{
    (void)notification;
    if (self.owner != nullptr)
    {
        self.owner->RefreshExtent();
    }
}
- (void)windowDidChangeBackingProperties:(NSNotification*)notification
{
    [self windowDidResize:notification];
}
- (void)windowDidMiniaturize:(NSNotification*)notification
{
    [self windowDidResize:notification];
}
- (void)windowDidDeminiaturize:(NSNotification*)notification
{
    [self windowDidResize:notification];
}
- (void)windowDidChangeOcclusionState:(NSNotification*)notification
{
    (void)notification;
    if (self.owner != nullptr)
    {
        self.owner->RefreshVisibility();
    }
}
@end

@implementation LudusCocoaView
- (BOOL)acceptsFirstResponder
{
    return YES;
}
- (void)keyDown:(NSEvent*)event
{
    ludus::platform::cocoa::DispatchResponderKey(event);
}
- (void)keyUp:(NSEvent*)event
{
    ludus::platform::cocoa::DispatchResponderKey(event);
}
- (void)flagsChanged:(NSEvent*)event
{
    ludus::platform::cocoa::DispatchResponderKey(event);
}
@end

@implementation LudusCocoaApplicationDelegate
- (void)requestQuit:(id)sender
{
    (void)sender;
    ludus::platform::cocoa::CloseAllWindows();
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender
{
    [self requestQuit:sender];
    // The engine owns teardown; do not let AppKit terminate the process here.
    return NSTerminateCancel;
}
@end

@implementation LudusCocoaApplicationObserver
- (void)didResignActive:(NSNotification*)notification
{
    (void)notification;
    ludus::platform::cocoa::ApplicationFocusChanged(false);
}
- (void)didBecomeActive:(NSNotification*)notification
{
    (void)notification;
    ludus::platform::cocoa::ApplicationFocusChanged(true);
}
@end
