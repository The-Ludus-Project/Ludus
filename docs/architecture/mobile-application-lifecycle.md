# Mobile application lifecycle and surfaces

Status: Mobile M0 contract for
[#1229](https://github.com/The-Ludus-Project/Ludus/issues/1229), 2026-11-19.
No backend implements it yet. It binds the iOS/iPadOS UIKit host and the Android
GameActivity host in the [mobile support plan](../development/mobile-support-plan.md).
Desktop backends keep their current contracts ([macOS](macos-platform.md),
Wayland, Win32). A later change may move them onto this vocabulary.

This page owns the normalized application state, its bounded event queue,
surface generations and the drawable handoff, safe-area insets, memory pressure
and frame scheduling across suspension. It links to the owners of related rules
instead of restating them:

- Fixed ticks, clamps and lifecycle resets: [frame and tick updates](frame-update.md)
  and [frame and simulation integration](high-resolution-time.md#frame-and-simulation-integration).
- Native pacing waits: [native frame pacing](high-resolution-time.md#native-frame-pacing).
- Surface publication, zero-size surfaces and acquisition: [RHI threads and surfaces](rhi-gdi.md#threads-surfaces-and-rendering-conventions).
- Touch contacts and their cancellation: [touch input](touch-input.md).

Names in the code blocks are proposals that the M1 implementation PR may refine.
The semantics, capacities, threads and failure behavior are frozen here. Change
them by updating this page in a reviewed PR, not in the implementation.

## Rules every adapter keeps

1. **The snapshot is authoritative.** The queue explains how the snapshot was
   reached. A consumer that lost queued records resynchronizes from the snapshot
   and never guesses missing transitions. Keyboard focus baselines follow the same
   rule ([keyboard M0](keyboard-input-decision-log.md#m0-revalidate-checkout-and-freeze-the-small-api)).
2. **Surface loss is a synchronous handshake.** The OS callback that ends a
   surface does not return until Ludus has stopped using it. A queued
   notification on its own is never enough.
3. **No busy loops.** When no frame can be produced, the loop blocks in an OS
   wait: a paused display link, an infinite looper wait, or the pacer's wait.
4. **Suspension is not simulated time.** Resuming never catches up the time
   spent inactive, in the background or without a surface.
5. **Background is the last reliable callback.** The OS may end the process
   after it without any further callback. Persistence never depends on
   `Terminating`.
6. **Failures are explicit.** Adapters log and count unexpected OS ordering and
   normalize it. They do not assert on OS behavior. Engine misuse, such as
   presenting to a retired surface generation, returns a status.

## Application state

| State | Meaning | Rendering | Simulation and input |
| --- | --- | --- | --- |
| `Launching` | The process runs, but no scene or Activity is attached yet | None | None |
| `Inactive` | Visible, at least in part, but not receiving input | May present at the idle rate | Gameplay input is cancelled; the game's pause policy applies (default Paused) |
| `Active` | In the foreground with input focus | Full rate | Normal |
| `Background` | Not visible | No GPU submission and no drawable requests | No ticks; save now |
| `Terminating` | The OS asked for an orderly shutdown (best effort) | None | None |

Allowed transitions are `Launching → Inactive`, `Inactive ↔ Active`,
`Inactive ↔ Background` and any state to `Terminating`. If the OS skips a step,
for example going from active straight to backgrounded, the adapter emits the
intermediate `Inactive` record first. Consumers therefore see only adjacent
transitions. An OS sequence the adapter cannot normalize is counted and logged,
and the snapshot takes the state the OS reports.

| Ludus state | iOS/iPadOS (UIKit scene life cycle) | Android (GameActivity) |
| --- | --- | --- |
| `Inactive` | `sceneWillEnterForeground`, `sceneWillResignActive` | Started but not resumed, or resumed without window focus (`onPause`, `onWindowFocusChanged(false)`) |
| `Active` | `sceneDidBecomeActive` | `onResume` and window focus |
| `Background` | `sceneDidEnterBackground` | `onStop` |
| `Terminating` | `applicationWillTerminate`, `sceneDidDisconnect` for the last scene | `onDestroy` with `isFinishing()` |

Examples of `Inactive`: the app switcher, Control Center or Notification Center
pulled over the game, an incoming-call banner, a permission dialog, the Android
notification shade, and an unfocused Android multi-window pane. On Android,
window focus and the resumed state change independently. The adapter combines
them as shown in the table.

## Events and the bounded queue

```cpp
enum class ApplicationState : uint8 { Launching, Inactive, Active, Background, Terminating };
enum class MemoryPressure : uint8 { None, Warning, Critical };
enum class SurfaceTransform : uint8 { Identity, Rotate90, Rotate180, Rotate270 };

enum class ApplicationEventKind : uint8
{
    StateChanged,
    MemoryPressureRaised,
    SurfaceCreated,
    SurfaceResized,
    SurfaceDestroyed,
    SafeAreaChanged,
    Overflow,
};

struct EdgeInsetsPixel final { float32 Top; float32 Left; float32 Bottom; float32 Right; };

struct ApplicationSnapshot final
{
    ApplicationState State;
    MemoryPressure Pressure;            // highest level since the last acknowledgement
    uint64 SurfaceGeneration;           // 0 = no surface
    uint32 WidthPixel;                  // drawable extent of the current generation
    uint32 HeightPixel;
    float32 Scale;                      // drawable pixels per logical point (iOS) or dp (Android)
    SurfaceTransform Transform;
    EdgeInsetsPixel SafeArea;           // inset from the drawable edges, in drawable pixels
    uint64 LastSequence;
    uint64 LostRecordCount;
};
```

- **Records.** Each record is an owned value: its kind, a strictly increasing
  `uint64` sequence, and a copy of the snapshot fields it changed. A record never
  borrows an OS pointer. Native handles are reached only through
  `NativeWindowInfo` for the current generation.
- **Capacity.** `APPLICATION_EVENT_CAPACITY = 64` records, allocated at startup,
  with one more slot reserved for the overflow marker. One frame normally
  produces at most a handful of records. Nothing allocates after startup.
- **Overflow.** When the queue is full, new records are dropped but the snapshot
  is still updated. `LostRecordCount` increments, and one `Overflow` marker is
  appended in the reserved slot. A consumer that reads `Overflow` resynchronizes:
  it resets the frame clock, cancels input, and rebuilds extent-dependent
  resources whenever the generation or extent differs from what it last built.
- **Draining.** The loop drains the queue in `HandleEvent()` at the start of a
  frame, before input is stepped. Records arrive in OS callback order. Surface and
  state records are independent streams. For example, Android may deliver
  `TERM_WINDOW` before or after `onStop`, and consumers must not rely on a
  relative order.
- **Sequence exhaustion.** It is unreachable in practice, but it is still
  explicit: the adapter stops accepting records and reports a fatal host status.
  It never wraps.

### Threads

| Target | Where OS callbacks run | Where Ludus state is written | Cross-thread boundary |
| --- | --- | --- | --- |
| iOS/iPadOS | Main thread | Main thread. The game loop runs in the `CADisplayLink` callback on the main run loop | None. The queue is single-threaded and needs no lock |
| Android | Java UI thread | The native loop thread, when it processes the GameActivity glue command | The glue's command pipe. The UI thread never touches Ludus state |

The Android glue blocks the UI thread until the native thread has processed
`APP_CMD_TERM_WINDOW` and a few other commands. The surface handshake below
relies on that. M1 verifies this behavior against the pinned GameActivity
version before depending on it, and reports a blocker if it differs.

## Surfaces and generations

- A **surface generation** is a `uint64` that increments each time the platform
  obtains a new native surface object: an `ANativeWindow` from
  `APP_CMD_INIT_WINDOW`, or a new `CAMetalLayer`-backed view attached to the
  game scene. Zero means no surface. Resizing the same native surface keeps the
  generation and publishes `SurfaceResized`.
- **Native descriptors** gain `WindowSystem::UIKit`, with borrowed
  `UIKitViewOrNull` and `MetalLayerOrNull`, and `WindowSystem::Android`, with a
  borrowed `AndroidNativeWindowOrNull`. Both carry `SurfaceGeneration`. The
  handles are valid only on the loop thread, from `SurfaceCreated` until the
  destroy handshake completes. Platform does not create GPU objects.
- **RHI owns everything derived from a surface**: swapchain, drawables and
  presentation retirement. It records the generation it built from. Acquire and
  present calls for a different generation return an explicit status. A swapchain
  rebuild inside one generation (resize, `VK_ERROR_OUT_OF_DATE_KHR`, suboptimal
  presentation) is RHI's own state and does not change the generation.
  `VK_ERROR_SURFACE_LOST_KHR` retires the generation, and RHI waits for the next
  `SurfaceCreated`.
- **Destroy handshake, Android.** Before the native thread acknowledges
  `APP_CMD_TERM_WINDOW`, RHI waits, with bounded fence waits, for GPU work that
  references the swapchain. It then destroys `VkSwapchainKHR` and `VkSurfaceKHR`,
  and Platform releases its `ANativeWindow` reference. If a fence wait times out,
  RHI treats the device as lost for that surface: it submits and presents nothing
  more, and it still releases the surface before acknowledging.
- **Destroy handshake, iOS.** UIKit keeps the layer while the scene is in the
  background, but the app must not request or present drawables there. Metal
  rejects command buffers committed from the background. On `Background`, RHI
  stops acquiring drawables, waits until submitted command buffers are scheduled,
  and only then lets the main-thread callback return. `sceneDidDisconnect`
  releases the view and is the iOS destroy handshake. It also runs synchronously
  on the main thread.
- **Presentation retirement.** The current Vulkan backend requires
  `VK_EXT_swapchain_maintenance1` for safe retirement. The M0 inventory records
  whether the Galaxy driver exposes it. If it does not, M2 specifies and
  validates another retirement contract before presenting. Removing the check is
  not an option.
- **Drawable acquisition is bounded.** If `nextDrawable` returns nil, or it hits
  the layer's acquisition timeout, the frame is skipped with a status. The loop
  does not retry inside the same frame. A zero extent skips acquisition, as RHI
  already specifies.
- **Transform.** Android publishes the surface's current transform after
  rotation. M2 decides whether the swapchain pre-rotates. Either way, touch
  positions and UI layout use the orientation the user sees. On iOS, UIKit
  rotates the layer, so the transform is always `Identity`.

## Extent, scale, safe areas and configuration

- The drawable covers the whole window, edge to edge. Its extent is in drawable
  pixels. `Scale` converts logical units (iOS points, Android dp) to drawable
  pixels, for UI sizing only.
- Safe-area insets are distances in drawable pixels from each drawable edge.
  iOS reports `safeAreaInsets` multiplied by the layer scale. Android reports the
  union of system-bar and display-cutout `WindowInsets`. Insets affect UI layout
  and touch hit regions. They never move the drawable or the touch coordinate
  origin.
- iPad window resizing (Split View, Stage Manager), rotation and Galaxy
  unfolding produce `SurfaceResized` and `SafeAreaChanged`, or a new generation
  when the OS replaces the surface. When the Galaxy Flip is folded, its main
  display turns off, which arrives as `Background` and usually a surface
  destroy. The cover display is outside the first gate. There is no separate
  fold or posture event.
- **Android configuration changes.** The manifest proposed for M1 handles
  `orientation|screenSize|smallestScreenSize|screenLayout|density|keyboard|keyboardHidden|uiMode`
  in place, so rotation and folding do not destroy the Activity. M1 verifies the
  list on the Galaxy. Real Activity recreation remains possible. The game session
  lives in a process-lifetime host object. A new Activity attaches a new window
  and surface generation and enters at `Inactive`, without starting a new
  session. A cold process start restores from the save. M4 tests both paths
  separately.

## Memory pressure

| OS signal | Ludus level |
| --- | --- |
| iOS `didReceiveMemoryWarning` (one level) | `Warning` |
| Android `onTrimMemory(TRIM_MEMORY_BACKGROUND)` | `Warning` |
| Android `onTrimMemory(TRIM_MEMORY_RUNNING_CRITICAL or TRIM_MEMORY_COMPLETE)`, `onLowMemory` | `Critical` |
| Android `TRIM_MEMORY_UI_HIDDEN` | None. The `Background` transition already reports it |

The snapshot keeps the highest level seen since the consumer last called
`AcknowledgeMemoryPressure()`. The consumer releases caches on the loop thread,
during the next frame, without allocating, and then acknowledges. Platform
only reports pressure. M4 defines the order in which renderer pools, resource
caches and audio caches are released. Newer Android versions deprecate several
trim levels, so M4 records which levels the Galaxy actually delivers.

## Frame scheduling and time

- **iOS.** A `CADisplayLink` on the main run loop drives frames. The pacing
  policy chooses its `preferredFrameRateRange`. In `Background` the link is
  paused and the run loop sleeps. In `Inactive` it runs at the application's
  idle rate.
- **Android.** While `Active` with a surface, the native loop polls the looper
  without waiting, and frames are paced by FIFO presentation, or by the Platform
  pacer for frames that present nothing. Without a surface, or in `Background`,
  it waits on the looper indefinitely (`ALooper_pollOnce(-1, ...)`) until a
  command arrives. M2 decides whether to add Choreographer callbacks or the
  Android Frame Pacing library, and records that decision in the dependency
  inventory.
- **Resets.** Entering `Active`, receiving `Overflow`, and any generation change
  reset the frame-clock baseline and tick debt through the existing reset path
  ([frame and tick updates](frame-update.md)). The first frame afterwards
  simulates zero elapsed time. Diagnostics still show the excluded time.
  Whether `steady_clock` advances while the device sleeps depends on each
  target's standard library. The M0 probes record it, but the contract does not
  depend on it.
- **Input.** Leaving `Active` is a focus loss. It clears pending gameplay edges
  and cancels held actions and touch contacts, as the
  [input bridge](frame-update.md#input-across-frames-and-ticks) requires.

## Persistence and termination

Save on `Background`, which is the last callback the OS reliably delivers. Use
atomic replacement, so that killing the process mid-write leaves the previous
save intact. The OS allows only a short time for this. M3 decides whether long
saves request extra background execution time explicitly (iOS
`beginBackgroundTask`). `Terminating` is best effort: orderly teardown may never
run, and nothing may rely on it.

## Outside the first gate

- Multiple iOS scenes: `UIApplicationSupportsMultipleScenes` is `NO`, so the OS
  does not create additional scenes.
- Android picture-in-picture and secondary displays: not declared. The Flip
  cover display is not supported.
- Mac Catalyst. Simulator and emulator builds are separate profiles; see the
  [profile matrix](../development/mobile-support-plan.md#target-profiles).

## Required verification

- **Unit tests (M1), on synthetic OS sequences and without a device:**
  normalization of skipped steps, overflow and resynchronization, sequence
  exhaustion, rejection of a stale generation, ordering of the destroy handshake,
  and the frame-clock reset on resume.
- **Device journeys (M2 and M4), on all three devices:** rotation, lock and
  unlock, Home and app switching, the app switcher during play, repeated resume,
  surface replacement, iPad resizing, Galaxy fold and unfold, and Activity
  recreation. Each journey checks for stale handles, busy loops and catch-up
  ticks.
