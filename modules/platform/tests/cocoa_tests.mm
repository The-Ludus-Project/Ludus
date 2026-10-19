#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/input/key.h>
#include <ludus/input/keyboard.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/keyboard_sink.h>
#include <ludus/platform/native_window.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>

using namespace ludus::foundation;
using namespace ludus::platform;
using ludus::input::Key;
using ludus::input::KeyTransition;
using ludus::input::ResetReason;

namespace
{
void RequireLiveCocoa()
{
    if (std::getenv("LUDUS_TEST_COCOA") == nullptr)
    {
        SKIP("Set LUDUS_TEST_COCOA=1 to run native window tests in a WindowServer session");
    }
}

UniquePtr<Window> MakeWindow(const std::string& name = "Ludus Cocoa platform test")
{
    WindowManager manager;
    REQUIRE(manager.Initialize({}));
    REQUIRE(manager.Initialize({}));
    Window::CreateInfo info;
    info.Name = name;
    info.Width = 320;
    info.Height = 200;
    UniquePtr<Window> window;
    REQUIRE(manager.CreateWindow(info, window));
    REQUIRE(window);
    return window;
}

NSWindow* Native(const Window& window)
{
    return (__bridge NSWindow*)window.GetNativeWindowInfo().CocoaWindow;
}

void Focus(Window& window)
{
    NSWindow* native = Native(window);
    REQUIRE(window.HandleEvent({}));
    // macOS 14+ cooperative activation need not foreground a background test
    // process. Drive the real delegate boundary explicitly for input fixtures.
    [native.delegate windowDidBecomeKey:[NSNotification notificationWithName:NSWindowDidBecomeKeyNotification
                                                                      object:native]];
}

struct Probe final
{
    ludus::input::InputSystem Input;
    ludus::input::KeyboardRecord Records[64] = {};
    usize Count = 0;
    ResetReason LastReset = ResetReason::None;
    ludus::input::FocusBaseline Baseline;
    bool Overflow = false;

    KeyboardSink Sink() noexcept
    {
        return
        {
            .OnRecord =
                [](void* context, const ludus::input::KeyboardRecord& record) noexcept {
                    auto& probe = *static_cast<Probe*>(context);
                    if (probe.Count < 64)
                    {
                        probe.Records[probe.Count++] = record;
                    }
                    else
                    {
                        probe.Overflow = true;
                    }
                    (void)probe.Input.Ingest(record);
                },
            .OnReset =
                [](void* context, ResetReason reason, const ludus::input::FocusBaseline& baseline) noexcept {
                    auto& probe = *static_cast<Probe*>(context);
                    probe.LastReset = reason;
                    probe.Baseline = baseline;
                    probe.Input.RequestReset(reason, baseline);
                },
            .UserData = this,
        };
    }
};

// Deliberately use character "x" for every physical code: normalization must
// depend on keyCode even when the supplied character/layout disagrees.
void PostKey(NSWindow* native, NSEventType type, uint16 code, NSEventModifierFlags flags = 0, bool repeat = false)
{
    NSEvent* event = [NSEvent keyEventWithType:type
                                      location:NSZeroPoint
                                 modifierFlags:flags
                                     timestamp:0
                                  windowNumber:native.windowNumber
                                       context:nil
                                    characters:@"x"
                   charactersIgnoringModifiers:@"x"
                                     isARepeat:repeat
                                       keyCode:code];
    REQUIRE(event != nil);
    [NSApp postEvent:event atStart:NO];
}
} // namespace

TEST_CASE("Cocoa creates owned windows and publishes resized backing-pixel extents", "[cocoa][window]")
{
    RequireLiveCocoa();
    @autoreleasepool
    {
        std::string title = "Ludus Cocoa \xE2\x98\x83";
        auto window = MakeWindow(title);
        title.assign("changed caller storage");
        NSWindow* native = Native(*window);
        REQUIRE(native != nil);
        REQUIRE([native.title isEqualToString:@"Ludus Cocoa \u2603"]);
        REQUIRE(window->GetNativeWindowInfo().System == WindowSystem::Cocoa);
        REQUIRE(window->GetNativeWindowInfo().CocoaView == (__bridge void*)native.contentView);
        REQUIRE(native.visible);
        REQUIRE(native.contentView.wantsLayer);
        [native setContentSize:NSMakeSize(250, 140)];
        REQUIRE(window->HandleEvent({}));
        const auto extent = window->GetNativeWindowInfo();
        const NSRect backing = [native.contentView convertRectToBacking:native.contentView.bounds];
        CHECK(extent.Width == static_cast<uint32>(std::ceil(backing.size.width)));
        CHECK(extent.Height == static_cast<uint32>(std::ceil(backing.size.height)));
        CHECK(extent.Width >= 250);
        CHECK(extent.Height >= 140);
        CHECK(extent.Display == nullptr);
        CHECK(extent.Surface == nullptr);
        CHECK(extent.CanvasSelector == nullptr);
        [native miniaturize:nil];
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (native.miniaturized == false && std::chrono::steady_clock::now() < deadline)
        {
            REQUIRE(window->HandleEvent({}));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(native.miniaturized);
        REQUIRE(window->HandleEvent({}));
        CHECK(window->GetNativeWindowInfo().Width == 0);
        CHECK(window->GetNativeWindowInfo().Height == 0);
        CHECK(window->GetVisibility() == WindowVisibility::Minimized);
        [native deminiaturize:nil];
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        // AppKit can publish intermediate nonzero extents during the restore animation.
        // Wait for the requested size before checking that restoration preserved it.
        while ((window->GetNativeWindowInfo().Width != extent.Width ||
                window->GetNativeWindowInfo().Height != extent.Height) &&
               std::chrono::steady_clock::now() < deadline)
        {
            REQUIRE(window->HandleEvent({}));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(window->GetNativeWindowInfo().Width == extent.Width);
        CHECK(window->GetNativeWindowInfo().Height == extent.Height);
        CHECK(window->GetVisibility() != WindowVisibility::Minimized);
        [native performClose:nil];
        CHECK_FALSE(window->HandleEvent({}));
        CHECK_FALSE(window->HandleEvent({}));
        CHECK(window->GetNativeWindowInfo().CocoaWindow == nullptr);
        CHECK(window->GetNativeWindowInfo().CocoaView == nullptr);
        CHECK(window->GetVisibility() == WindowVisibility::Occluded);
        window.Reset();
        CHECK_FALSE(native.visible);
    }
}

TEST_CASE("Cocoa reports AppKit occlusion as window visibility", "[cocoa][window]")
{
    RequireLiveCocoa();
    @autoreleasepool
    {
        UniquePtr<Window> window = MakeWindow();
        NSWindow* native = Native(*window);
        const auto waitForVisibility = [&](WindowVisibility visibility) {
            const std::chrono::steady_clock::time_point deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (window->GetVisibility() != visibility && std::chrono::steady_clock::now() < deadline)
            {
                REQUIRE(window->HandleEvent({}));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return window->GetVisibility() == visibility;
        };
        // A background test process cannot activate itself (macOS 14+ cooperative activation), so a
        // normal-level window may open behind an opaque window and correctly report Occluded. Float it
        // above normal windows so the test controls its starting state. AppKit delivers occlusion
        // asynchronously; the published value follows its notification.
        native.level = NSFloatingWindowLevel;
        REQUIRE(waitForVisibility(WindowVisibility::Visible));
        CHECK((native.occlusionState & NSWindowOcclusionStateVisible) != 0);
        // Ordering the window out leaves no visible part: Occluded, not Minimized.
        [native orderOut:nil];
        CHECK(waitForVisibility(WindowVisibility::Occluded));
        CHECK(window->GetNativeWindowInfo().Width != 0);
        [native orderFront:nil];
        CHECK(waitForVisibility(WindowVisibility::Visible));
    }
}

TEST_CASE("Cocoa rejects invalid windows and calls made off the main thread", "[cocoa][lifecycle]")
{
    RequireLiveCocoa();
    auto window = MakeWindow();
    WindowManager manager;
    UniquePtr<Window> rejected;
    Window::CreateInfo info;
    info.Width = 0;
    CHECK_FALSE(manager.CreateWindow(info, rejected));
    CHECK_FALSE(rejected);
    info.Width = 16385;
    CHECK_FALSE(manager.CreateWindow(info, rejected));
    info.Width = 320;
    info.Name = "\xFF";
    CHECK_FALSE(manager.CreateWindow(info, rejected));
    info.Name = "worker thread";
    bool initialized = true;
    bool created = true;
    bool pumped = true;
    std::thread worker([&] {
        initialized = manager.Initialize({});
        created = manager.CreateWindow(info, window);
        pumped = window->HandleEvent({});
    });
    worker.join();
    CHECK_FALSE(initialized);
    CHECK_FALSE(created);
    CHECK_FALSE(pumped);
    CHECK_FALSE(rejected);
    REQUIRE(window);
    CHECK(window->HandleEvent({}));
}

TEST_CASE("Cocoa delivers physical keys and repeats through the existing input reducer", "[cocoa][input]")
{
    RequireLiveCocoa();
    @autoreleasepool
    {
        Probe probe;
        auto window = MakeWindow();
        window->AttachKeyboardSink(probe.Sink());
        Focus(*window);
        REQUIRE(probe.Baseline.Focused);
        NSWindow* native = Native(*window);
        PostKey(native, NSEventTypeKeyDown, 0x0D);
        PostKey(native, NSEventTypeKeyDown, 0x0D, 0, true);
        PostKey(native, NSEventTypeKeyUp, 0x0D);
        PostKey(native, NSEventTypeKeyDown, 0xFFFF);
        REQUIRE(window->HandleEvent({}));
        REQUIRE(probe.Count == 3);
        CHECK(probe.Records[0].PhysicalKey == Key::KeyW);
        CHECK(probe.Records[0].Source == ludus::input::RecordSource::Native);
        CHECK(probe.Records[1].Repeat);
        CHECK(probe.Records[2].Transition == KeyTransition::Up);
        REQUIRE(probe.Input.ConsumeStep(1) == ludus::input::StepStatus::Ok);
        const auto& snapshot = probe.Input.GetKeyboardSnapshot();
        const usize index = ludus::input::KeyIndex(Key::KeyW);
        CHECK(snapshot.Pressed[index]);
        CHECK(snapshot.Released[index]);
        CHECK_FALSE(snapshot.Down[index]);
        window->DetachKeyboardSink();
        PostKey(native, NSEventTypeKeyDown, 0x00);
        REQUIRE(window->HandleEvent({}));
        CHECK(probe.Count == 3);
    }
}

TEST_CASE("Cocoa routes other windows and modifier releases without inventing focus edges", "[cocoa][input]")
{
    RequireLiveCocoa();
    @autoreleasepool
    {
        Probe firstProbe;
        Probe secondProbe;
        auto first = MakeWindow("first routing window");
        auto second = MakeWindow("second routing window");
        first->AttachKeyboardSink(firstProbe.Sink());
        second->AttachKeyboardSink(secondProbe.Sink());
        Focus(*second);
        NSWindow* native = Native(*second);
        PostKey(native, NSEventTypeFlagsChanged, 0x38, static_cast<NSEventModifierFlags>(0x20002));
        PostKey(native, NSEventTypeFlagsChanged, 0x3C, static_cast<NSEventModifierFlags>(0x20006));
        PostKey(native, NSEventTypeFlagsChanged, 0x38, static_cast<NSEventModifierFlags>(0x20004));
        PostKey(native, NSEventTypeKeyDown, 0x00, NSEventModifierFlagCommand);
        PostKey(native, NSEventTypeKeyUp, 0x00, NSEventModifierFlagCommand);
        PostKey(native, NSEventTypeFlagsChanged, 0x39, static_cast<NSEventModifierFlags>(0x01010000));
        PostKey(native, NSEventTypeFlagsChanged, 0x39, NSEventModifierFlagCapsLock);
        // Pump the first window; records must still reach only the event owner.
        REQUIRE(first->HandleEvent({}));
        REQUIRE(secondProbe.Count == 7);
        CHECK(firstProbe.Count == 0);
        CHECK(secondProbe.Records[0].PhysicalKey == Key::ShiftLeft);
        CHECK(secondProbe.Records[1].PhysicalKey == Key::ShiftRight);
        CHECK(secondProbe.Records[2].PhysicalKey == Key::ShiftLeft);
        CHECK(secondProbe.Records[2].Transition == KeyTransition::Up);
        CHECK(secondProbe.Records[4].Transition == KeyTransition::Up);
        CHECK(secondProbe.Records[5].PhysicalKey == Key::CapsLock);
        CHECK(secondProbe.Records[5].Transition == KeyTransition::Down);
        CHECK(secondProbe.Records[6].Transition == KeyTransition::Up);
        [native.delegate windowDidResignKey:[NSNotification notificationWithName:NSWindowDidResignKeyNotification
                                                                          object:native]];
        REQUIRE(first->HandleEvent({}));
        CHECK(secondProbe.LastReset == ResetReason::FocusLost);
        CHECK_FALSE(secondProbe.Baseline.Focused);
        PostKey(native, NSEventTypeKeyDown, 0x00);
        REQUIRE(first->HandleEvent({}));
        CHECK(secondProbe.Count == 7);
        [native close];
        CHECK(secondProbe.LastReset == ResetReason::WindowClosed);
        CHECK_FALSE(second->HandleEvent({}));
        second.Reset();
        PostKey(native, NSEventTypeKeyDown, 0x00);
        REQUIRE(first->HandleEvent({}));
        CHECK(secondProbe.Count == 7);
        CHECK_FALSE(firstProbe.Overflow);
        CHECK_FALSE(secondProbe.Overflow);
        first->DetachKeyboardSink();
    }
}

TEST_CASE("Cocoa application quit closes windows while leaving teardown to the engine", "[cocoa][lifecycle]")
{
    RequireLiveCocoa();
    @autoreleasepool
    {
        auto first = MakeWindow();
        auto second = MakeWindow();
        REQUIRE([NSApp.delegate respondsToSelector:@selector(applicationShouldTerminate:)]);
        const NSApplicationTerminateReply reply = [NSApp.delegate applicationShouldTerminate:NSApp];
        CHECK(reply == NSTerminateCancel);
        CHECK_FALSE(first->HandleEvent({}));
        CHECK_FALSE(second->HandleEvent({}));
        first.Reset();
        second.Reset();
        // The application queue remains usable after the engine owns teardown.
        auto replacement = MakeWindow();
        CHECK(replacement->HandleEvent({}));
    }
}

TEST_CASE("Cocoa application deactivation cancels held input without release edges", "[cocoa][input]")
{
    RequireLiveCocoa();
    @autoreleasepool
    {
        Probe probe;
        auto window = MakeWindow();
        window->AttachKeyboardSink(probe.Sink());
        Focus(*window);
        PostKey(Native(*window), NSEventTypeKeyDown, 0x0D);
        REQUIRE(window->HandleEvent({}));
        REQUIRE(probe.Input.ConsumeStep(1) == ludus::input::StepStatus::Ok);
        const usize index = ludus::input::KeyIndex(Key::KeyW);
        REQUIRE(probe.Input.GetKeyboardSnapshot().Down[index]);
        [[NSNotificationCenter defaultCenter] postNotificationName:NSApplicationDidResignActiveNotification
                                                            object:NSApp];
        CHECK(probe.LastReset == ResetReason::FocusLost);
        CHECK_FALSE(probe.Baseline.Focused);
        REQUIRE(probe.Input.ConsumeStep(2) == ludus::input::StepStatus::Ok);
        const auto& snapshot = probe.Input.GetKeyboardSnapshot();
        CHECK_FALSE(snapshot.Down[index]);
        CHECK_FALSE(snapshot.Released[index]);
        PostKey(Native(*window), NSEventTypeKeyUp, 0x0D);
        REQUIRE(window->HandleEvent({}));
        CHECK(probe.Count == 1);
    }
}
