---
description: Choose owning storage, handle allocation failure and keep borrows valid with Ludus containers.
---

# Choose and use containers

Use `Ludus::FoundationContainers` for owning sequences, small ordered
dictionaries and allocation-free sorting. The library is available to native and
browser SDK consumers. Include each header explicitly; Base's `core.h` does not
include containers. Containers never throw. Fallible `Try` operations return
`false` or `nullptr`, and their infallible counterparts terminate on allocation
failure. In an SDK game target, load the installed package and link the target:

```cmake
find_package(Ludus CONFIG REQUIRED)
target_link_libraries(my_game PRIVATE Ludus::FoundationContainers)
```

First-party engine code, tests and tools must not use `std::vector`. A
repository check enforces this; the only exception is the container benchmark
baseline. See the [migration policy](../../architecture/containers.md#343-migration-completeness-requirement-actually-migrate).

## Choose the storage

These types ship today. Each has its reference page in the
[API reference](../reference/index.md).

| Need | Use | Header | Contract summary |
| --- | --- | --- | --- |
| Count fixed at compile time | `StaticArray<T, N>` | `static_array.hpp` | Inline aggregate; all `N` elements are always live; never allocates |
| Runtime size within a hard bound | `FixedArray<T, N>` | `fixed_array.hpp` | Live prefix of inline storage; only occupied slots are constructed; full is a status; never allocates |
| Growable contiguous sequence | `Array<T>` | `array.hpp` | One heap block; 2x growth; growth invalidates every element borrow |
| Small or read-mostly ordered dictionary | `SortedMap<K, V>` | `sorted_map.hpp` | Sorted `Array` of entries; O(log n) lookup, O(n) insert/remove |
| Borrowed read or write of caller storage | `BorrowedView<T>` or `std::span<T>` | `borrowed_view.hpp` | Pointer and count only; never owns or extends the storage's lifetime |
| In-place ordering, no stability | `Sort` | `sort.hpp` | Heap sort; O(1) extra storage; equal elements may reorder |
| Stable ordering | `StableSort` | `stable_sort.hpp` | Merge sort into caller-supplied scratch; no allocation |
| Bounded first-in, first-out queue | `RingBuffer<T, N>` | `ring_buffer.hpp` | Inline storage; full is a status; wrapped contents as two spans; single owner; never allocates |
| Flags or membership over a fixed index range | `StaticBitSet<N>` | `static_bit_set.hpp` | `N` bits packed into `uint64` words; set operations and shifts; trivially copyable; never allocates |

`Array` is the default. Use linear search over a small `Array` for short-lived
tables. Use `SortedMap` when ordered iteration or binary lookup helps. There is
no universal size at which a map wins; measure the actual consumer. The
[architecture](../../architecture/container-systems.md#baseline-architecture-before-the-gems-review)
explains the selection.

These structures are **planned but deferred, not available**. Each milestone
(M6–M10) has an explicit deferral with its reason and the condition that reopens
it. Do not write code against these types yet. The
[milestone roadmap](../../architecture/container-systems.md#milestone-roadmap)
tracks their status and gating consumers.

| Planned type | Intended use | Deferral and reopen condition |
| --- | --- | --- |
| `BitSet` | Packed bits with a run-time size | Deferred (M5): reopens when a consumer needs a run-time bit count |
| `HashMap`, `HashSet` | Larger mutable dictionaries | Deferred (M8): reopens when a named consumer appears; needs a measured mutable-lookup workload before the algorithm is selected |
| `SortedSet<K>` | Ordered uniqueness without mapped values | Deferred (M7): reopens when a named ordered-uniqueness consumer appears |
| `SlotMap<T>` with typed handles | Dense traversal with generation-checked durable identity | Deferred (M9): reopens once the object-handles / FoundationIdentity workstream settles owner and cross-instance identity |
| `SparseSet<T>` | Dense storage keyed by bounded sparse IDs | Deferred (M10): reopens once object-handles / FoundationIdentity settles, then with a concrete World extraction |

## Report allocation failure without partial output

At a function that returns a status, use the `Try` operations. Build into a
local, then swap it into the output only after every step succeeds. A failure
then leaves the caller's array unchanged. Swapping never allocates.

```cpp
#include <ludus/foundation/containers/array.hpp>

#include <span>

enum class CollectStatus : ludus::foundation::uint8
{
    Ok,
    OutOfMemory,
};

// Replace outIds with the IDs of every visible object. On failure outIds keeps
// its previous contents.
[[nodiscard]] CollectStatus CollectVisible(std::span<const ludus::foundation::uint32> ids,
                                           std::span<const bool> isVisible,
                                           ludus::foundation::Array<ludus::foundation::uint32>& outIds) noexcept
{
    using namespace ludus::foundation;
    Array<uint32> visible;
    if (visible.TryEnsureCapacity(ids.size()) == false)
    {
        return CollectStatus::OutOfMemory;
    }
    for (usize i = 0; i < ids.size(); ++i)
    {
        if (isVisible[i] == true)
        {
            visible.Add(ids[i]); // within the reserved capacity: cannot allocate
        }
    }
    outIds.Swap(visible);
    return CollectStatus::Ok;
}
```

A failed `TryAdd`, `TryAddInPlace`, `TryInsertAt`, `TryResize` or `TryAddRange`
leaves size, capacity, elements and borrows unchanged. It also leaves the
argument untouched, including a move-only value or one that refers to an
element of the same array. Use `Add`, `Resize` and `EnsureCapacity` only where
running out of memory may end the process, such as tests and tools.

## Bound a buffer without allocating

`FixedArray<T, N>` is for a size that varies at run time but must never exceed
a known bound, such as an event batch or a binding table. Its storage is inline,
so it suits hot paths that must not allocate. Element types need not be
default-constructible, because only occupied slots hold objects. There is no
infallible `Add`: every add or insert is a `Try` operation that returns `false`
or `nullptr` when the array is full, leaving it unchanged.

```cpp
#include <ludus/foundation/containers/fixed_array.hpp>

#include <span>

constexpr ludus::foundation::usize MAX_TOUCHES = 10;

struct Touch
{
    ludus::foundation::uint32 Id;
    ludus::foundation::float32 X;
    ludus::foundation::float32 Y;
};

// Returns false, keeping every earlier touch, when the frame's bound is reached.
[[nodiscard]] bool RecordTouch(ludus::foundation::FixedArray<Touch, MAX_TOUCHES>& inoutTouches,
                               const Touch& touch) noexcept
{
    return inoutTouches.TryAdd(touch);
}
```

`TryAddRange` appends a whole span or nothing. Storage never moves, so element
borrows stay valid until that element is removed or shifted by an ordered
insert or `RemoveAt`. Copying or moving the array copies or moves its elements;
borrows do not follow, and a moved-from `FixedArray` is empty. Use
`StaticArray` instead when every element is always live.

## Queue work in arrival order

`RingBuffer<T, N>` is a bounded FIFO: `TryAdd` appends at the back and
`RemoveFirst` removes the oldest element. Like `FixedArray`, it never allocates,
only occupied slots hold objects, and a full ring rejects `TryAdd` without
changing anything. Decide the overflow policy at the call site, such as
dropping, coalescing, or recovering, as Input does.

```cpp
#include <ludus/foundation/containers/ring_buffer.hpp>

#include <initializer_list>
#include <span>

constexpr ludus::foundation::usize MAX_PENDING_COMMANDS = 32;

struct Command
{
    ludus::foundation::uint32 Kind;
    ludus::foundation::uint32 Argument;
};

using CommandQueue = ludus::foundation::RingBuffer<Command, MAX_PENDING_COMMANDS>;

// Applies every queued command in arrival order, oldest first.
template <typename ApplyFunction>
void DrainCommands(CommandQueue& inoutQueue, const ApplyFunction& apply) noexcept
{
    while (inoutQueue.IsEmpty() == false)
    {
        apply(inoutQueue.GetFirst());
        inoutQueue.RemoveFirst();
    }
}

// Reads without removing: the oldest elements, then any that wrapped around.
[[nodiscard]] ludus::foundation::uint64 SumArguments(const CommandQueue& queue) noexcept
{
    ludus::foundation::uint64 sum = 0;
    for (const std::span<const Command> part : {queue.GetLeadingSpan(), queue.GetTrailingSpan()})
    {
        for (const Command& command : part)
        {
            sum += command.Argument;
        }
    }
    return sum;
}
```

The ring is not contiguous once it wraps, so it has no `begin`/`end`. Index it
with `operator[]` (0 is the oldest) or walk the two spans. It is single-owner,
not a thread-safe queue.

## Track flags and membership in bits

Use `StaticBitSet<N>` instead of a `bool` array when an index range is known at
compile time: a key per input key, an asset per slot, a flag per channel. It
uses an eighth of the memory, copies as a plain value, and supports whole-set
operations. Read bits with `IsSet` and write them with `Set`, `Reset`,
`Assign` or `Flip`. There is no `bits[i] = true` proxy.

```cpp
#include <ludus/foundation/containers/static_bit_set.hpp>

constexpr ludus::foundation::usize MAX_ASSETS = 64;
using AssetSet = ludus::foundation::StaticBitSet<MAX_ASSETS>;

// Visits every asset that is wanted but not yet resident, in index order.
template <typename VisitFunction>
void VisitMissing(const AssetSet& wanted, const AssetSet& resident, const VisitFunction& visit) noexcept
{
    const AssetSet missing = wanted & ~resident;
    for (ludus::foundation::usize asset = missing.FindNextSet(0); asset < missing.GetSize();
         asset = missing.FindNextSet(asset + 1))
    {
        visit(asset);
    }
}
```

`FindNextSet` returns `GetSize()` when no further bit is set, which ends the
loop. Indices past the end are asserted in checked builds. `GetWords()` exposes
the packed words for word-wise algorithms; their layout is not a file format.

## Register and look up by key

`SortedMap` insertion reports a duplicate separately from allocation failure.
Keys are read-only through the map; use `Find` to change a value. There is no
`operator[]`, because a lookup must never insert.

```cpp
#include <ludus/foundation/containers/sorted_map.hpp>

using ludus::foundation::SortedMap;
using ludus::foundation::uint32;
using ludus::foundation::uint64;

enum class BindStatus : ludus::foundation::uint8
{
    Ok,
    AlreadyBound,
    OutOfMemory,
};

[[nodiscard]] BindStatus BindAsset(SortedMap<uint64, uint32>& inoutBindings, uint64 asset, uint32 slot) noexcept
{
    const SortedMap<uint64, uint32>::InsertResult result = inoutBindings.TryAddInPlace(asset, slot);
    if (result.Value == nullptr)
    {
        return BindStatus::OutOfMemory; // map unchanged
    }
    return result.Inserted == true ? BindStatus::Ok : BindStatus::AlreadyBound;
}

[[nodiscard]] bool TryGetSlot(const SortedMap<uint64, uint32>& bindings, uint64 asset, uint32& outSlot) noexcept
{
    const uint32* slotOrNull = bindings.Find(asset);
    if (slotOrNull == nullptr)
    {
        return false;
    }
    outSlot = *slotOrNull;
    return true;
}
```

When the key set is known during setup, call `TryEnsureCapacity` first. Later
insertions up to that capacity then never allocate. A range-for over
`const SortedMap<K, V>::Entry&`, or `AsSpan()`, visits entries in ascending key
order. The default ordering uses `operator<`; keys that are NaN, or that change
order while stored, break lookup.

## Keep borrows valid

Pointers, references, iterators and spans into a container are temporary
borrows. The container does not track them.

- `Array`: growth beyond the current capacity (by insertion, `Resize` or
  `EnsureCapacity`) invalidates every borrow. Ordered insertion or removal invalidates borrows at
  and after that position. `RemoveAtSwap` moves the last element into the hole.
- `SortedMap`: the same rules apply to entries and to the value pointers that
  `Find` returns. Failed or duplicate insertion invalidates nothing.
- `Clear` destroys elements but keeps capacity. `SortedMap::Reset` also
  releases storage. Neither allocates, and removal never shrinks capacity.
- Moving an `Array` or `SortedMap` transfers the storage. Borrows then refer to
  the destination, and the source is empty.

A durable reference to an element that may move needs an index the owner
revalidates, or a future typed handle. Never keep a raw pointer to it across
mutation. Containers are single-owner: concurrent reads are safe only while no
thread mutates, and the caller provides any synchronization.

## Sort without allocating

`Sort(values, count, less)` sorts in place and may reorder equal elements.
`StableSort(values, scratch, count, less)` keeps equal elements in input order,
using a separate initialized scratch array of at least `count` elements; reuse
it across calls. It returns `false` for a null nonempty array or when
`scratch` is the same array, leaving `values` unchanged. Both need a
nonthrowing strict weak ordering and nonthrowing element moves.

## Implementation and further reading

- [Container systems architecture and roadmap](../../architecture/container-systems.md)
- [Array and StaticArray design and API decisions](../../architecture/containers.md)
- [Container measurements and validation evidence](../../development/container-systems-evidence.md)
- [Container benchmark](../../development/tools/container-bench.md)
- [Foundation reading path](../architecture/foundation.md)
