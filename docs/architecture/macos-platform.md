# macOS Platform backend

The Platform module selects Cocoa on macOS. AppKit ownership and Objective-C++
stay in `window_cocoa.mm`; public SDK headers remain ordinary C++. This slice
provides windows and input. Metal device, layer configuration and presentation
remain responsibilities of the RHI backend.

## Ownership and initialization

Initialize, create, pump, query and destroy windows on the main thread.
Initialization requires a WindowServer session and is repeatable. Calls to
initialize, create or pump from a worker thread fail. An off-thread create call
preserves its output rather than destroying an existing main-thread window.
Main-thread creation replaces its output and leaves it empty on failure.

Standalone processes create the shared NSApplication, install a minimal Quit
menu and finish launching. An existing host application's delegate, menu and
activation policy are preserved. Cocoa retains each window, content view and
delegate until the Ludus window is destroyed. Delegate routing is disconnected
before native objects are released. A registry admits at most 16 live windows;
creation beyond that bound fails rather than dropping input routing.

Titles are copied from valid UTF-8, with a 4096-byte admission limit. Requested
content width and height are logical points, each from 1 through 16384. The
borrowed `CocoaWindow` and `CocoaView` handles identify an NSWindow and NSView.
They must not be released by consumers. Closing clears both handles and the
published dimensions. The content view is layer-backed, ready for RHI to attach
the RHI Metal layer; Platform does not create a GPU object.

## Events and dimensions

`HandleEvent` pumps up to 256 queued events from the shared NSApplication queue,
then updates windows. AppKit can run its own tracking loops during native
interaction such as a window drag. Pumping one Ludus window can dispatch events
for another; routing always follows the native event's owning window.

Window delegate notifications track focus, close, resize, backing-scale changes
and minimization. Cocoa native descriptors report content dimensions in backing
pixels, using the view's backing conversion rather than assuming a Retina scale.
Minimized windows publish zero drawable dimensions; restoration refreshes them.
`GetVisibility()` reports `Minimized` for a miniaturized window and `Occluded`
while AppKit's window occlusion state has no visible part (fully covered by
opaque windows, ordered out, a hidden application or another Space); the occlusion
notification refreshes it. A process that cannot activate itself (macOS 14
cooperative activation) may open behind another window and correctly start
`Occluded`. Native loops use it to [lower their pacing rate](high-resolution-time.md#native-frame-pacing).
`HandleEvent` returns false after close. Quit requests close all Ludus windows
and let the engine perform teardown instead of exiting inside AppKit.

## Physical keyboard input

Key-down, key-up and modifier changes feed the existing `KeyboardSink`. Native
virtual key positions map to Ludus physical keys independently of character data
or keyboard layout. Left/right modifiers use device-specific modifier bits;
Caps Lock uses its stateless physical bit rather than its persistent latch.
OS repeats are marked as repeats for the Input reducer. Unmapped ISO/JIS, Fn,
media and keypad-equals positions are ignored. Text input and IME are deferred.

Focus entry copies the held-key baseline using CoreGraphics key-state queries.
Window focus loss, application deactivation and close deliver an empty, unfocused reset; they do not fabricate
release events. Attaching or replacing a sink synchronizes its current baseline
on the next pump. Physical events normalize before AppKit dispatch, including
Command shortcuts and key-up events that AppKit may otherwise route specially.
When an existing host such as Qt pumps AppKit, the owned view's key/modifier
responders normalize events too. Events already normalized by Ludus's pump are
suppressed in that responder path, so native delivery remains exactly once.
Callbacks ingest input only: they must not destroy a window during dispatch.

## Validation

Portable key mapping tests run on every host. Native integration tests opt in
with `LUDUS_TEST_COCOA=1` and require a real WindowServer session:

```bash
LUDUS_TEST_COCOA=1 out/host-tools/venv/bin/ctest --preset macos-clang-development \
  --label-regex 'platform|macos' --no-tests=error
```

The macOS CI job enables these checks. They exercise title ownership, native
handles, resize/minimize/close, thread rejection, physical input/repeats,
multi-window routing, focus cancellation and orderly Quit. Keyboard fixtures
post native NSEvents and explicitly drive focus delegate/application notifications,
so they do not depend on macOS granting foreground activation to a test process.
Resize, minimize, restore and close exercise actual AppKit lifecycle operations.
The same production
backend is linked by applications and tests; only test targets enable C++
exceptions. Objective-C exceptions are disabled for both.

Thanks to Apple, *Cocoa Event Handling Guide*,
[Event Architecture](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/EventOverview/EventArchitecture/EventArchitecture.html)
and [Handling Key Events](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/EventOverview/HandlingKeyEvents/HandlingKeyEvents.html),
for the shared-queue and responder model used here. Native key positions and
modifier masks follow Apple's macOS SDK `HIToolbox/Events.h` and
`IOKit/hidsystem/IOLLEvent.h`; no text interpretation or upstream backend code
is copied.
