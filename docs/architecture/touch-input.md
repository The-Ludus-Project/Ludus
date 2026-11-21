# Touch input contract

Status: Mobile M0 contract for
[#1229](https://github.com/The-Ludus-Project/Ludus/issues/1229), 2026-11-19.
It is not implemented yet; M3 implements it in `Ludus::Input`, and the M1/M2
host work must not contradict it. Input owns this contract. The
[application lifecycle](mobile-application-lifecycle.md) owns the surfaces and
state transitions that cancel contacts, and the [frame and tick
updates](frame-update.md#input-across-frames-and-ticks) page owns how sampled
input reaches gameplay ticks.

Touch follows the decisions already proved by the keyboard reducer
([research](keyboard-input-research.md#recommendation-and-limits),
[frozen M0 API](keyboard-input-decision-log.md#m0-revalidate-checkout-and-freeze-the-small-api)):
owned normalized records, one reducer shared by native, synthetic and replay
sources, bounded storage, explicit step consumption and cancellation that never
fabricates a release. Names below are proposals the M3 PR may refine; the
semantics, units and capacities are frozen here.

## Records

```cpp
enum class TouchPhase : uint8 { Began, Moved, Ended, Cancelled };
enum class ContactKind : uint8 { Finger, Stylus };

struct TouchRecord final
{
    RecordSource Source;          // Native, Synthetic or Replay, as for keyboard records
    TouchPhase Phase;
    ContactKind Kind;
    uint64 NativeContactKey;      // adapter-private identity; never exposed to gameplay
    uint64 SurfaceGeneration;     // generation whose drawable the position refers to
    float32 PositionPixelX;       // drawable pixels, top-left origin, +Y down
    float32 PositionPixelY;
    bool HasNativeTime;
    uint64 NativeTimeNanoseconds; // diagnostic only; never compared or sorted on
};
```

- **One record per contact transition.** An OS event carrying several pointers
  becomes several records in the OS's order. Adapters copy everything before the
  callback returns; nothing borrows a `UITouch`, `UIEvent` or `MotionEvent`.
- **Native identity.** iOS uses the `UITouch` object, which stays the same for
  the life of one touch ([UITouch](https://developer.apple.com/documentation/uikit/uitouch)).
  Android uses the pointer ID, which is stable while the pointer is down and is
  reused after it lifts ([MotionEvent](https://developer.android.com/reference/android/view/MotionEvent)).
  The adapter turns either into `NativeContactKey`; only the reducer maps it to
  a `TouchId`.
- **Stylus.** Apple Pencil and Android stylus contacts are ordinary contacts with
  `Kind = Stylus`; pressure, tilt, hover and palm rejection are outside the
  first gate. Indirect pointers (iPad trackpad or mouse, Android mouse) are not
  touch: the adapter ignores and counts them until a pointer contract exists.
- **No synthetic keyboard.** Touch never produces `KeyboardRecord`s. Games map
  touch to actions through the touch layer below.

## Contact identity

```cpp
struct TouchId final { uint64 Value; };  // 0 = none; never reused within a process
inline constexpr usize TOUCH_CONTACT_CAPACITY = 16;
```

- `Began` for an unknown native key allocates the next `TouchId` from a strictly
  increasing `uint64` counter and a free contact slot. Gameplay never observes a
  reused ID, even when Android reuses the pointer ID immediately. Counter
  exhaustion returns an explicit status and forces a reset; it never wraps.
- `Moved`, `Ended` and `Cancelled` for an unknown native key are
  `RejectedInvalid` and counted. A second `Began` for a live native key cancels
  the old contact (the OS lost its end) and begins a new one.
- **Capacity.** At most 16 simultaneous contacts. A `Began` with no free slot
  returns `RejectedCapacity`; that native key is ignored until its `Ended` or
  `Cancelled`, which also counts. The M0 inventory records the devices' actual
  simultaneous-contact limits; raise the constant only with that evidence.

## Coordinates

- Positions are `float32` **drawable pixels** of the surface generation in the
  record: top-left origin, +Y down, the same orientation the user sees. This is
  the pixel-rectangle convention of [RHI](rhi-gdi.md#threads-surfaces-and-rendering-conventions),
  so a position can be compared with a viewport or a UI rectangle without
  conversion.
- iOS converts `locationInView:` (points of the game view) by the layer scale.
  Android positions are already pixels; the adapter subtracts the game surface's
  origin in the window, which is normally zero because the GameActivity surface
  fills the window (M1 verifies it). If Android pre-rotates the
  swapchain (an [M2 decision](mobile-application-lifecycle.md#surfaces-and-generations)),
  positions still use the presented orientation, never the pre-transform space.
- Safe-area insets do not offset positions. UI hit-testing applies insets to its
  layout rectangles; gameplay that needs logical units divides by the published
  `Scale`.
- A record whose `SurfaceGeneration` differs from the current generation is
  `RejectedInvalid`: a touch from before a rotation or surface replacement never
  lands at a wrong position. A generation change cancels every contact.

## Queue, steps and overflow

- The pending ring holds `TOUCH_PENDING_CAPACITY = 256` records, as for keyboard.
  Consecutive `Moved` records of the same contact, with no other record of that
  contact between them, coalesce in place to the latest position, so a fast drag
  does not fill the ring. `Began`, `Ended` and `Cancelled` are never coalesced.
  Historical samples (`getHistoricalX`, `coalescedTouches`) are not ingested in
  the first gate.
- **Overflow.** When the ring is full, the pending batch is discarded, every
  live contact is cancelled, the ingest returns `RecoveredWithLoss` and the loss
  counter increments. Losing an `Ended` would otherwise leave a stuck contact.
  Contacts still physically down are ignored until they lift; new contacts begin
  normally.
- `ConsumeStep(stepId)` follows the keyboard rules: strictly increasing step IDs,
  pending records applied in order, edges published for the step, held state
  retained on later catch-up ticks and the queue retained across zero-tick frames.
- **Per-step view.** For each live or just-ended contact: `TouchId`, `Kind`,
  current and start positions, `IsDown`, and OR-accumulated `Began`, `Ended` and
  `Cancelled` flags. A contact that begins and ends within one step reports both.
  The ordered step events keep multiplicity for consumers such as gesture
  recognizers.

## Cancellation

A contact ends with `Cancelled`, never `Ended`, when:

- the OS cancels it (`touchesCancelled`, `ACTION_CANCEL`), for example for a
  system gesture or an incoming call;
- the application leaves `Active` or the surface generation changes
  ([lifecycle](mobile-application-lifecycle.md#frame-scheduling-and-time));
- the pending ring overflows, or Input resets for binding replacement or level
  activation (`RequestReset`, as for keyboard).

`Cancelled` does not fabricate a release: an action driven by a cancelled
contact reports `Cancelled`, not `Released`, matching
[`ActionState`](../../modules/input/include/ludus/input/actions.h). After
resuming, no contact from before suspension is restored; the OS delivers a new
`Began` for a finger that stays down, which allocates a new `TouchId`.

## Threads

The reducer runs on the loop thread, which is the only thread that calls
`Ingest`. On iOS, touches arrive on the main thread, which also runs the loop.
On Android, GameActivity buffers input events, and the native loop thread
reads them through the glue's input buffer swap. In both cases ingestion is
single-threaded and lock-free, and no OS callback calls gameplay.

## Touch to actions

M3 adds a small, data-defined touch layer that writes the existing
`ActionState` vocabulary: bounded rectangles in drawable pixels (virtual
buttons) and one drag region that produces an axis. A contact captures the
first region it begins in until it ends or cancels. General gesture
recognition, rebinding user interfaces and controller support are out of scope
for the first gate.

## Required verification

- Reducer tests on synthetic records: simultaneous contacts, Android pointer-ID
  reuse, capacity exhaustion, overflow cancellation, stale-generation
  rejection, a begin and end in one step, catch-up ticks, and resume without
  restored contacts.
- Device journeys (M3): two-finger input, rotation during a held touch, the app
  switcher and Control Center or notification shade while held, iPad resizing,
  Galaxy fold and unfold, with no stuck contacts or stretched coordinates.
