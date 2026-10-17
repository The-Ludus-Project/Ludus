# Container systems

Status: M0 to M5 implemented. This design extends the implemented
[contiguous containers](containers.md#34-ludus-native-taxonomy-and-api-authoritative-naming)
and respects [memory ownership](memory-management.md). It does not replace
those contracts. Types marked planned below are architectural decisions, not
available SDK APIs. The [milestone roadmap](#milestone-roadmap) tracks the
remaining integration and implementation work.

## Requirements and evidence

The engine needs predictable storage, explicit failure, fast traversal, and
ownership that a debugger can explain. No design is universally fastest. Choose
by access pattern, cardinality, mutation frequency, ordering, and address
stability; benchmark the actual consumer before selecting a more complex
algorithm. C++23, Clang 18, no exceptions, fixed-width Ludus types, standalone
light public headers, and the existing native/browser SDK boundary are gates.

The initial audit at `52e29ce`, refreshed through `595042b`, finds an established
`Array`/`StaticArray` implementation,
`std::span` views, specialized concurrent logging/profiling queues, world-owned
entity/component storage, audio/UI-owned bounded slots, and one production
`std::unordered_map<uint64, uint64>` in GameHost's resource bindings. The
remaining unordered set is a reload-test observation set. Current main also
provides FoundationMemory allocation domains, FoundationHash, and Strings with
an exact intern table; reuse those APIs rather than invent competing hashing,
string ownership, or allocator systems. Array still uses its original byte seam.
Resource bindings are
written during setup and read through the host callback. That lookup-only
callback should never allocate; setup needs a fallible insertion path.

The first increment supplies `SortedMap`, backed by one `Array<Entry>`, and
migrates that registry. It also adds fallible insertion to `Array`, keeping
object lifetime and relocation in the existing audited helpers. This is a
useful vertical slice without a new allocator, SIMD portability layer, or a
second lifetime implementation.

## Caller-scratch stable ordering

`StableSort` in `ludus/foundation/containers/stable_sort.hpp` provides a nonallocating
stable merge sort for initialized caller-owned arrays. It takes separate scratch
storage, preserves equivalent input order and requires nonthrowing assignment and
comparison. Its declaration owns the exact input/failure contract. L0 Renderer uses
it during static view preparation; it is independently tested against a standard
stable-sort oracle on empty, duplicate-heavy and non-power-of-two sizes through 257.

## Baseline architecture, before the Gems review

```mermaid
flowchart BT
    Base[FoundationBase: types and assertions]
    Memory[FoundationMemory: allocation domains]
    Containers[FoundationContainers: owning storage and lookup]
    Consumers[World / Content / Audio / GameHost]
    Base --> Containers
    Base --> Memory
    Memory -. future Array integration .-> Containers
    Containers --> Consumers
```

Each container owns storage and element lifetimes. It does not own scheduling,
resource residency, serialization, locks, or subsystem policy. Keep views as
`std::span`/`std::string_view`; keep algorithms separate. Do not add containers
to `core.h`. New public headers opt in to the facilities they use, and export
through FoundationContainers' CMake header file set.

| Storage/access requirement | Type | Status / tradeoff |
| --- | --- | --- |
| Fixed count, contiguous | `StaticArray<T, N>` | Implemented; all N objects always live |
| Growable contiguous sequence | `Array<T>` | Implemented; reserve/reuse; pointers move on growth |
| Small or read-mostly ordered dictionary | `SortedMap<K, V, Compare>` | First increment; binary search, contiguous iteration, O(n) insert/remove |
| Runtime size within a hard bound | `FixedArray<T, N>` | Implemented (M3); live prefix, no heap, full is a status |
| FIFO with hard capacity | `RingBuffer<T, N>` | Implemented (M4); inline storage, single owner, two spans for wrapped storage |
| Dense index membership/flags | `StaticBitSet<N>`; `BitSet` | `StaticBitSet` implemented (M5); dynamic `BitSet` deferred for lack of a consumer |
| Larger mutable dictionary | `HashMap<K, V, Hash, Equal>` | Planned; flat storage, measurement gates below |
| Uniqueness without mapped values | `SortedSet<K>` / `HashSet<K>` | Planned; share the corresponding lookup policy |
| Dense traversal with durable identity | subsystem typed handle + `SlotMap<T>` | Planned; generation checking and swap removal |
| Dense components keyed by sparse IDs | `SparseSet<T>` | Planned only with a concrete World extraction |
| Rare address-stable objects | subsystem owned pool | Conditional; handles normally preferred |
| Tiny common sequence that occasionally grows | `SmallArray<T, N>` | Conditional; size/relocation cost must beat `Array` |

Use `Array` plus linear search for very small ephemeral tables; `SortedMap` when
ordered iteration or binary lookup is useful; `HashMap` for a measured large,
mutation-heavy table. These are workload choices, not a magic cardinality
threshold. Arrays remain the default; avoid pointer-linked lists and trees
unless ordering/range updates demonstrably justify their allocation/locality
costs. Spatial trees, render graphs, resource caches, and ECS layouts stay in
their own modules.

## Shared contracts

- Ordinary containers have one mutable owner. Concurrent reads require no
  concurrent mutation; synchronization is the caller's responsibility.
- `GetSize`, `IsEmpty`, `EnsureCapacity`, `Clear`, `Reset`, `Find`, `Contains`,
  `Remove`, and `TryAddInPlace` follow the established verb naming.
- Recoverable resource exhaustion is explicit. `Try` insertion does not consume
  arguments, change live elements, or invalidate pointers on allocation failure.
  Unchecked insertion calls the existing out-of-line fatal OOM path.
- `Clear` destroys live elements but retains capacity. `Reset` destroys them and
  releases storage. Neither method allocates. No hidden shrinking on removal.
- Element destruction/movement and comparison/hash callbacks must be nonthrowing
  for new lookup containers. Arbitrary user value construction must satisfy the
  same requirement. User element constructors that themselves allocate need
  their own explicit failure protocol; a container cannot recover that failure.
- Use the current aligned, nothrow byte seam for Array-based containers.
  FoundationMemory domains already exist for Strings; integrating Array and
  its dependent containers is a separate ABI/measurement change; do not embed an
  unrelated allocator
  framework in each new template or intercept global new in production.
- Pointers/references are temporary borrows. Expose invalidation in each API;
  durable identity uses a typed handle. A handle contains no owning pointer.
- No container stores debug strings or registers globally. Inspect normal
  fields in the debugger; assertions check contracts; sanitizer and lifetime
  tests verify raw storage. Optional expensive validators are explicit calls.
- In-memory container layout is not a file, network, or game-module ABI. Export
  logical records through explicit schemas; sort when deterministic order is
  required. Do not dump pointers, capacity, padding, buckets, or freelists.

## SortedMap contract

Representation is `Array<Entry>` with immutable-through-public-access keys,
mutable mapped values, and a stateless ordering policy. Keys must be nonthrowing
copyable/movable; owning move-only `String` keys need a future explicit cloning/
heterogeneous-borrow design and are outside this initial map contract. Values
must be nonthrowing movable/assignable/destructible,
including move-only non-default-constructible types. Copy operations exist only
when mapped values are nonthrowing copyable. This prevents unsupported operations
from masquerading as valid type traits.

`Compare` is a default-constructible empty policy with a const, nonthrowing
strict weak ordering. Equivalence is `!Compare(a,b) && !Compare(b,a)`, never a
second equality policy. Ordering must remain stable for the container's lifetime.
The default uses `<`; NaNs or mutable external ordering state do not satisfy the
contract. Heterogeneous lookup and stateful comparators can be added when a
consumer requires them, without charging every empty dictionary now.

The API intentionally exposes const entry spans for iteration, and `Find` returns
a mutable/const pointer to the mapped value. A writable entry span would let a
caller change keys and silently break lookup. There is no `operator[]`: reading
must not insert or require a default-constructible value. Insert returns
`{Value, Inserted}`: an existing pointer with false for duplicate, a new pointer
with true for insertion, null with false for OOM/overflow. Duplicate insertion
leaves both stored values and supplied arguments untouched.

Search uses overflow-safe binary lower bound. Insertion checks equivalence once,
then asks `Array::TryInsertAtInPlace` to construct the entry. On a growth path,
allocate new storage first, construct the new entry while aliases into the old
storage remain live, relocate prefix/suffix, and commit. On a reserved path,
materialize before shifting. This avoids a second sorted-map storage algorithm
and preserves failure semantics even for arguments referencing existing values.
`Array::TryAddInPlace` uses the shared growth helper, repairing its previous
consume-before-allocation behavior for an aliased rvalue on OOM.

| Operation | Complexity | Invalidation |
| --- | --- | --- |
| `Find` / `Contains` | O(log n) comparisons | None; no allocation |
| Duplicate insertion | O(log n) | None; no argument consumption |
| Insert with growth | O(n) moves/copies | All entry/value pointers |
| Insert without growth | O(n) moves after insertion index | At/after index |
| `Remove` | O(log n) search + O(n) moves | At/after removed index |
| `EnsureCapacity` | O(n) only if growing | All on growth |
| Failed `Try` operation | No live-element changes | None |
| `Clear` / `Reset` | O(n) destruction | All live elements |
| Move/Swap | O(1) | Borrows follow transferred storage; old move-assignment destination dies |

Entry references can still refer to external memory whose contents change; key
owners must prevent such mutation. Value-semantic keys such as integer IDs are
the normal choice. A span is a borrow and must not outlive the owning map.

## FixedArray contract

`FixedArray<T, N>` (in `fixed_array.hpp`) keeps its live elements as the prefix
of an uninitialized inline union, so constructing the array constructs no
element. Element types need only a nonthrowing destructor; copy, move,
insertion and removal are constrained on the operations they use. `N == 0`
selects an empty slot union, so the array holds only its size. `GetData()`
returns null, the array is always full, and every `Try` add fails. The array is
trivially destructible exactly when `T` is.

Additions report a full array through `Try` operations only: `false` or `nullptr`
with no element changed and no argument consumed. There is no OOM path.
Appending constructs in place without moving existing elements, so arguments may
alias the array. Ordered insertion materializes the value before shifting.
`TryAddRange` is all-or-nothing. Removal and ordered shifts reuse the
`detail/contiguous_storage.hpp` helpers shared with `Array`.

Borrows remain valid until their element is removed or shifted, because storage
never relocates. A copy or move transfers elements rather than storage, so
borrows refer to the source's slots. A moved-from array is empty, matching
`Array`. The M3 consumer is the Input reducer's binding table and per-step event
buffer. They replace raw arrays with hand-maintained counts, and a full
step-event buffer is asserted as unreachable by the reducer's capacity
invariant.

## RingBuffer contract

`RingBuffer<T, N>` (in `ring_buffer.hpp`) is a single-owner FIFO. M4's storage
decision is that capacity is a compile-time constant and storage is inline, the
same uninitialized slot union `FixedArray` uses (`detail/inline_slots.hpp`).
It never allocates, which preserves the Input consumer's one creation-time
allocation. A heap-backed ring whose capacity is chosen at run time remains
gated on a consumer that needs it.

Live elements occupy a possibly wrapped run starting at the head. Only those
slots hold objects, so element types need only a nonthrowing destructor. A full
ring rejects `TryAdd`/`TryAddInPlace` with no element changed and no argument
consumed. Appending never moves a live element, so arguments may alias the
ring, and a borrow stays valid until its element is removed. `RemoveFirst` is
O(1); a drained ring restarts at the first slot so the next run is contiguous.
Index 0 is always the oldest element. `GetLeadingSpan()` then `GetTrailingSpan()`
visit the live elements in FIFO order without pretending wrapped storage is
contiguous. Copies and moves preserve FIFO order and restart at the first slot.
A moved-from ring is empty. `N == 0` is always full.

The ring is not a concurrent queue: the audio, logging and profiling SPSC/MPSC
queues keep their own atomic publication and reclamation protocols. The M4
consumer is the Input reducer's pending-transition queue. Its overflow policy
is unchanged: a rejected `TryAdd` leaves the queue untouched, then the reducer
discards the batch and arms a reset.

## StaticBitSet contract

`StaticBitSet<N>` (in `static_bit_set.hpp`) packs `N` bits into
`(N + 63) / 64` `uint64` words held in a `StaticArray`. It never allocates and
is a trivially copyable `constexpr` value type. Bits at or above `N` in the last
word are always zero. Every operation that could set them (`SetAll`, `FlipAll`,
`operator~`, `operator<<=`) masks them again, so counts and comparisons never see
stale tail bits. A zero-bit set stores one always-zero word, so every operation
compiles without a separate specialization.

Bits are read with `IsSet` and written with `Set`, `Reset`, `Assign` and `Flip`.
There is no `bool&` proxy. Index arguments are preconditions asserted in checked
builds. Whole-set operations are `SetAll`, `ResetAll`, `FlipAll`, `CountSet`,
`IsAnySet`/`IsNoneSet`/`AreAllSet` and `FindNextSet`, the word-wise scan used to
visit set bits. Set operations `&`, `|`, `^` and `~` combine word by word.
Shifts move bits toward higher (`<<`) or lower (`>>`) indices, discard bits
shifted past either end, and reset every bit for a shift of `N` or more.
`GetWords()` exposes read-only words, least significant bit of word 0 first.
That in-memory layout is not a wire format; persist bits through an explicit
schema.

Population count and trailing-zero count are portable `constexpr`
implementations of the classic algorithms in Henry S. Warren, Jr., *Hacker's
Delight*, 2nd edition, sections 5-1 and 5-4. Neither `<bit>` nor compiler
intrinsics are used, because ADR 0003 does not list them. An intrinsic path
needs that policy change and a measured benefit first.

The M5 consumers are the Input reducer's private live-down and reset-baseline
key masks, and the Resources manager's dependency-closure
`Wanted` set and catalog-duplicate `seen` set. The public `KeyboardSnapshot`
and `FocusBaseline` bool arrays are unchanged, because packing them would change
the SDK's API and ABI. A dynamically sized `BitSet` with fallible growth is
deferred: no consumer needs a run-time bit count.

## Planned structures and decision gates

`HashMap` should compare a scalar flat open-addressed baseline against an
established grouped-control implementation before committing to SIMD. Grouped
control bytes/fingerprints reduce unnecessary full-key comparisons in Abseil's
[Swiss table design](https://abseil.io/about/design/swisstables), but tombstones,
probe termination, hash quality, and ISA paths add maintenance cost. Choose one
algorithm from mixed hit/miss, collision, deletion-churn, large-value, and native/
Wasm measurements. No blanket speed claim is warranted. Require bounded probing,
overflow-safe allocation, an occupied/deleted/empty invariant, key immutability,
transactional rehash, and failure injection at every backing allocation.

Hash and equality must agree and be nonthrowing; keys are trusted engine IDs by
default. Public/adversarial strings use FoundationHash
`KeyedTableHash64` with a caller-supplied secret plus a bounded admission policy.
Reuse `TableHash64` for trusted byte keys and exact key comparison. Do not use
a persisted fingerprint as collision-free dictionary identity. Deterministic
iteration must use a sorted output
view rather than rely on bucket order. Pointer stability is explicitly lost on
rehash; mutation/iteration rules will be documented for the selected algorithm.

`SlotMap` separates dense values from slot metadata and a dense-to-slot reverse
index. A handle carries a subsystem tag, index, and generation; stale lookup is
an ordinary null result. Removal repairs the reverse mapping after moving the
last value. Retire a slot on generation exhaustion instead of allowing an old
handle to become valid. Specify world/owner identity and cross-instance behavior
before extracting a generic primitive: index+generation alone does not identify
an owner. `Clear`, move, rollback of multi-array growth, and destruction must
preserve the stale-handle contract. The primary-source
[Bitsquid ID table article](https://bitsquid.blogspot.com/2011/09/)
provides the dense lookup model; owner identity and exhaustion are Ludus's
additional requirements.

`SparseSet` must reject IDs outside its bound before indexing, verify dense IDs
to avoid uninitialized sparse entries, repair back-links after swap removal, and
account for sparse address-space memory. Paged sparse metadata is conditional on
a measured sparse ID range. Keep ECS-specific component lifetime and query
policy in World. Pooling, inline spills, node containers, lock-free generic
collections, persistent structures, and platform virtual-memory tricks all need
a concrete consumer and measurements before implementation.

## Rollout and verification

### Milestone roadmap

Planning baseline: **2026-10-06**. M0 records implemented functionality, not a
fresh certification of every review gate. Later milestones are unfinished.
"Gated" means a consumer or design decision must be established before committing
to the public primitive. Numbers express the recommended order; the dependencies
below determine which increments can proceed independently.

| ID | Milestone | Status | Deliverable and exit criterion |
| --- | --- | --- | --- |
| M0 | Contiguous arrays and first ordered map | Implemented | `StaticArray`, `Array`, `SortedMap`, fallible insertion/append, GameHost resource bindings, model/lifetime/allocation tests, SDK probe and benchmark exist. Retain their contracts and scoped measurement evidence. |
| M1 | Restore migration and explicit allocation failure | Implemented | First-party `std::vector` uses were replaced with `Array`, including tests, tools and the exported Audio Gym API. Status-returning boundaries use fallible operations and preserve outputs on failure. `tools/check_container_policy.py` rejects new uses outside the benchmark/vendor exceptions; see [M1 consumer migration](#m1-consumer-migration). |
| M2 | Complete existing container API documentation | Implemented | Every public `Array`, `StaticArray`, `SortedMap` and relocation-trait declaration has a Doxygen contract, and their 158 baseline entries were removed from `api-undocumented.json`. The [container guide](../wiki/guides/containers.md) covers selection, failure handling and borrow rules, and separates shipped types from planned ones. Strict wiki/API builds and coverage checks pass. |
| M3 | `FixedArray<T, N>` and one Input migration | Implemented | `FixedArray` keeps a live prefix in inline raw storage, reports full explicitly and constructs only occupied slots ([contract](#fixedarray-contract)). The Input reducer's binding table and step-event buffer use it. Zero allocation, zero capacity, non-default/move-only values, aliasing, failure preservation and balanced lifetimes are tested, including against a seeded model. |
| M4 | Single-owner `RingBuffer<T, N>` | Implemented | Inline storage with compile-time capacity was selected ([contract](#ringbuffer-contract)); a heap-backed run-time-capacity ring stays gated on a consumer. Wrapped leading/trailing spans and explicit full/empty behavior are implemented. The Input reducer's pending-transition queue uses it with its overflow/reset policy unchanged. Wraparound and lifetimes are model-tested. |
| M5 | `StaticBitSet<N>`; dynamic `BitSet` | Implemented; dynamic `BitSet` deferred | `StaticBitSet` packs bits into `uint64` words with tail masking, single-bit and whole-set operations, `FindNextSet`, set operations and shifts ([contract](#staticbitset-contract)). The Input reducer's private key masks and the Resources manager's membership sets use it. 0/1/63/64/65/130-bit boundaries, tail masking, every shift amount and a seeded model are tested, with zero allocation. Dynamic `BitSet` with fallible growth is deferred until a consumer needs a run-time bit count. |
| M6 | Array allocation-domain integration | Planned; Memory prerequisites | Reuse FoundationMemory through the Array byte seam, then introduce domain ownership as a separately measured layout change. Test original-domain frees, copy/move/swap rules, nested owners, over-alignment and OOM preservation; version the SDK owning-layout boundary. |
| M7 | `SortedSet<K>` | Gated | Identify an ordered uniqueness consumer. Reuse the ordering/equivalence contract and contiguous lifetime helpers; verify duplicate insertion consumes nothing, keys remain immutable through public access, and ordered output matches an independent model. |
| M8 | `HashMap` and consumer-required `HashSet` | Gated | Compare a scalar flat table with grouped-control alternatives on a named mutable-lookup workload. Select and implement one algorithm, reuse FoundationHash, and migrate the consumer. Collision/deletion/rehash models, bounded probing and allocation-failure rollback pass; record native/Wasm results. |
| M9 | Typed handles and `SlotMap<T>` | Gated | Establish owner/cross-instance identity, generation exhaustion and subsystem ABI semantics first. Extract a reusable dense-value primitive with reverse indices; verify stale/foreign handles, removal, clear, move and multi-allocation rollback against a model. |
| M10 | `SparseSet<T>` extraction | Gated | Demonstrate reusable storage logic in World or another bounded-ID consumer. Extract membership/dense storage while retaining ECS policy in World; verify ID bounds, back-links, swap removal, failure preservation and sparse-address-space memory costs. |
| M11 | Research and measured improvements after the initial implementation | Planned; initial baseline required | Review the queued conference/journal sources below, record testable hypotheses, and trial promising alternatives against the validated initial implementation. Record adopt/defer/reject decisions with correctness, allocation, memory, performance and portability evidence. |

#### M1 consumer migration

M1 landed as consumer increments in one change. On base revision `5117c8c`, the
new gate reports 209 findings (`std::vector` uses and `<vector>` includes) in 42
first-party files. Migration repaired each
failure protocol rather than only renaming the container. Previously, Text's
[`LoadFont`](../../modules/text/src/font_system.cpp) called `std::vector::resize`
before an OOM size check that the exception-free build could never reach.

- **Text.** Font bytes, glyph records, slot metadata and raster scratch are
  `Array`s. `CreateFontSystem`, `LoadFont`, `ShapeRun` and `RasterizeGlyph`
  return `OutOfMemory` without publishing a handle, changing counters or
  consuming a slot. `RasterizeGlyph` publishes its bitmap and counter only after
  the scratch allocation succeeds. `ludus_text_alloc_tests` rejects the aligned
  nothrow seam to verify each path.
- **GameHost.** Codec fields, record batches, frames and the frame-reader
  backlog are `Array`s. `Message::Parse`, `GetRecords` and `EncodeFrame` stage
  their results and return false with outputs unchanged. Property snapshots and
  batches map allocation failure to `HostFailed` before anything is emitted.
  Reload checkpoint staging rejects the reload and keeps A running. Output and
  retained-result queues reserve capacity at construction. If a later append
  fails, the session latches a channel failure rather than silently dropping an
  outcome.
- **Audio Gym.** The public `Capture` and `Scenario` storage is `Array`.
  `Track`, `Advance` and the `FilterBy*` helpers return `Status`. `Advance`
  allocates scratch and capture storage before rendering, so `OutOfMemory`
  leaves the system, captures and tracked set unchanged. The filters write to a
  caller-owned output that is preserved on failure. Audio now links
  FoundationContainers publicly for this header.
- **Tests, tools and samples.** These use the infallible operations where
  failure aborts the test. The smoke text demo reports read-buffer allocation
  failure explicitly.

Two compromises remain. `GetRecords` keeps its `bool` contract, so an edit
batch that cannot be allocated is reported as `InvalidRequest`. Outgoing
`Message` setters stay infallible, like the `std::string` values they own.

The [container evidence](../development/container-systems-evidence.md#m1-migration-and-policy-gate)
records the commands and results. `./scripts/check --format` and the CI policy
step run the gate. Replacing `std::string`, filesystem facilities, `std::array`
or unrelated ownership types stays in their own workstreams.

M2 documented the containers that existed when it landed. Every subsequent
milestone includes Doxygen for its new/changed API and the relevant
[guide](../wiki/guides/containers.md) update in the same change, including moving
a type from its planned table to its shipped table. M3 delivered `FixedArray`,
M4 delivered `RingBuffer` and M5 delivered `StaticBitSet`.
Generic rings retain single-owner semantics and do not replace the
audio/logging/profiling publication protocols.

M6 follows the byte-boundary and attribution decisions in
[Memory phases 1 and 2](memory-management.md#17-implementation-phases). Existing
allocation domains do not by themselves establish that Array integration or
accounting/ABI prerequisites are complete. Split backend redirection from the
per-owner domain/layout change and retain the accepted ownership rules when
later containers are added. M6 can proceed independently of bounded inline
storage; a heap-owning milestone must state which allocation seam it uses.

M7 and M8 do not require each other. M8's benchmark/selection step precedes its
public implementation; `HashSet` shares the chosen policy when a consumer needs
it. M9's identity decision precedes any extraction. M10 can reuse existing World
identity without waiting for a generic SlotMap if a separate extraction pays.
Owning move-only String keys, heterogeneous lookup and stateful policies remain
consumer-driven contract extensions; none is required to declare M0 implemented.

`SmallArray`, address-stable pools, dynamic trees, generic concurrent collections
and virtual-memory storage remain a conditional backlog. Each needs a named
consumer and its own lifetime/ownership contract plus measured benefit before
receiving an implementation milestone.

### Research after the initial implementation

Recorded **2026-10-06**. Implement and validate the initial architecture through
the applicable M1-M10 delivery gates first, then begin M11's broader reading and
improvement pass. Consumer-gated milestones require either a selected consumer
and completed deliverable or an explicit deferral with its reason; the
conditional backlog does not need implementation to establish the baseline.
Record the baseline revision, completed/deferred scope, consumer workloads and
measurements before comparing research prototypes with it.

The algorithm-selection measurements and source checks already required by the
initial milestones remain part of their implementation. In particular, M8 still
compares scalar and grouped-control candidates before selecting its initial
hash table. M11 schedules later optimization and reconsideration of those
choices; it does not postpone existing correctness, ownership or measurement
gates.

Prioritize **CppCon** for practical C++ implementation and benchmarking talks,
**ACM Journal of Experimental Algorithmics** for measured data-structure
comparisons, and **Software: Practice and Experience** for implementation
studies. Screen **FOCS** selectively for hashing research that addresses a
measured problem. The older sparse-set paper below is a specific foundational
reading from *ACM Letters on Programming Languages and Systems*, rather than a
recommendation about an upcoming event or current journal schedule.

These venue priorities and experiments are Ludus's research plan. Source
metadata, talk descriptions and available abstracts were screened when making
the shortlist; the full talks/papers remain queued. This list does not claim
that their algorithms have been adopted or that their reported gains transfer
to Ludus.

Read CR-01, CR-02 and CR-03 first, then select the remaining readings by the
consumer's measured bottleneck. Milestone labels identify the affected initial
design; they do not move these trials ahead of the baseline.

| ID | Source and attribution | Full-review status | Question and possible experiment after review |
| --- | --- | --- | --- |
| CR-01 | Chandler Carruth, **Tuning C++: Benchmarks, and CPUs, and Compilers! Oh My!**, CppCon 2015. [Official recording](https://www.youtube.com/watch?v=nXaxk27zwlk) | Queued; talk description screened | Across milestones: are compiler transformations or the harness obscuring the consumer's costs? Review benchmark setup, distinguish dependent lookup latency from independent throughput, and compare warm/cold access and retained/rebuilt storage with reproducible inputs. |
| CR-02 | Pablo Halpern, **Allocators: The Good Parts**, CppCon 2017. [Official recording](https://www.youtube.com/watch?v=v3dz-AKOVL8) | Queued; talk description screened | M6: which allocator ownership and propagation ideas improve our domain integration? Compare domain retention and explicit fallible cross-domain copies, exercise copy/move/swap and nested owners, and measure allocation count, backing bytes and owner size. Retain Ludus's explicit failure protocol. |
| CR-03 | Matt Kulukundis, **Designing a Fast, Efficient, Cache-friendly Hash Table, Step by Step**, CppCon 2017. [Official recording](https://www.youtube.com/watch?v=ncHmEUmJZf4) | Queued; talk description screened | M8: would a different probing/control layout improve the selected baseline? Compare scalar and grouped-control candidates using mixed hits/misses, deletion churn, collisions, different key/value sizes and native/Wasm targets; include memory and rehash costs. |
| CR-04 | Paul-Virak Khuong and Pat Morin, **Array Layouts for Comparison-Based Searching**, *ACM Journal of Experimental Algorithmics* 22(1), article 1.3, 2017. [Author version](https://arxiv.org/abs/1509.05053); [DOI](https://doi.org/10.1145/3053370) | Queued; metadata/abstract screened | SortedMap/M7: does a different search layout help an immutable, read-heavy consumer? Compare binary search with Eytzinger or other reviewed layouts, including construction and cache behavior. Account for sorted contiguous iteration and mutation contracts before proposing a separate index or a representation change. |
| CR-05 | Preston Briggs and Linda Torczon, **An Efficient Representation for Sparse Sets**, *ACM Letters on Programming Languages and Systems* 2(1-4), pp.59-69, 1993. [DOI](https://doi.org/10.1145/176454.176484) | Queued; bibliographic metadata verified | M10: how do ID range, occupancy and clear/iteration frequency affect the initial dense/sparse representation? Compare membership, insertion, swap removal and reset costs across bounded dense and sparse ID distributions; measure sparse metadata bytes and preserve World ownership policy. |
| CR-06 | Samy Chambi, Daniel Lemire, Owen Kaser and Robert Godin, **Better Bitmap Performance with Roaring Bitmaps**, *Software: Practice and Experience* 46(5), pp.709-719, 2016. [Author version](https://arxiv.org/abs/1402.6407); [DOI](https://doi.org/10.1002/spe.2325) | Queued; metadata/abstract screened | M5/M10: does a large sparse membership consumer justify a compressed representation? Compare dense BitSet storage with reviewed sparse/compressed alternatives across occupancy, clustering, intersection and mutation workloads. Small bounded flags retain their existing consumer contract. |
| CR-07 | Michael A. Bender, Bradley C. Kuszmaul and William Kuszmaul, **Linear Probing Revisited: Tombstones Mark the Demise of Primary Clustering**, FOCS 2021, pp.1171-1182. [DOI](https://doi.org/10.1109/FOCS52979.2021.00115); [Author preprint](https://arxiv.org/abs/2107.01250), titled **Linear Probing Revisited: Tombstones Mark the Death of Primary Clustering** | Queued; metadata/abstract screened | M8: can a reviewed deletion/tombstone policy reduce churn costs without unacceptable memory or rebuild work? Trial load-factor and deletion sequences against the initial policy; measure probe lengths, insertion/lookup costs and rebuild frequency. Record the paper's hashing/model assumptions before applying any theoretical bound. |

For each completed review, record the source ID, date, exact pages/sections or
talk timestamps, relevant idea and assumptions, and a testable consumer-specific
hypothesis. Distinguish design inspiration from adapted code. A review may
justify deferring or rejecting an idea without a prototype.

For each trial, use the existing verification gates and append a dated result
to the [container evidence](../development/container-systems-evidence.md) with
the baseline/prototype revisions, commands, seeds, toolchain, hardware/backend,
operation mix and source ID. Record lookup/mutation timings, allocations,
backing/metadata bytes and relevant consumer effects, including regressions and
negative results. Retain the first increment's historical evidence unchanged;
keep generated captures in ignored `out/`.

Advance entries through **Queued**, **Reading**, **Reviewed**, **Trial planned**,
**Trial complete**, and **Adopted**, **Deferred** or **Rejected**. An adoption
needs a measured consumer benefit and unchanged or explicitly revised failure,
lifetime, ordering, portability and SDK contracts. Update this architecture and
the owning API documentation with any accepted change, and acknowledge the
consulted source near affected code. Native/Wasm validation, exception-free
errors and public-header build budgets remain requirements. No source in this
new queue has a completed trial or adoption record yet.

### Completion and evidence

For each increment run warning-clean pinned builds, unit/model/lifetime tests,
ASan/UBSan, format/tidy, standalone-header/include-boundary gates, SDK consumer,
and native/browser compilation where supported. Use exhaustive small-state
cases and seeded operation streams against a simple independent reference model.
Allocation tests separately interpose nothrow allocation and verify failure
leaves capacity, live content, and aliased/move-only arguments unchanged.

Performance reports must give compiler/build flavor, hardware, dataset/cardinality,
operation mix, sample method, allocation count and memory, and include a baseline.
Record wins and losses. Benchmarks are evidence, not noisy timing assertions in
CI. Measure lookup and mutation separately; optimizing one does not prove the
other improved. Build-budget checks remain unchanged. The
[first increment's evidence](../development/container-systems-evidence.md)
records measured lookup losses, small-table rebuild wins, and allocation tests.
Record later results by milestone with the tested revision, commands, consumer,
allocation/memory measurements and any skipped or incomplete checks. Preserve
the first increment's historical measurements. Mark a milestone complete only
when its deliverable, consumer migration where required, documentation and
applicable validation gates are satisfied.

## Gems review and resulting revisions

The initial architecture was written before reading the selected chapter bodies.
The source index is the local `references/game-dev-gems-toc.md`; the scanned
chapters were rendered and OCR-extracted, with title/code pages visually checked.
References are intentionally not required in a cloned SDK checkout. Printed
and PDF page numbers differ. These are design inputs, not copied implementations.

| Read source | Useful finding | Change from the baseline / rejected detail |
| --- | --- | --- |
| Brian Hawkins, **Handle-Based Smart Pointers**, *Game Programming Gems 3*, §1.5, printed pp.44–48, PDF pp.46–50 | Handle checks are useful when multiple borrowers observe an independently destroyed object; cached pointers can escape validation | Add explicit resolve-per-borrow scopes below. Retain owner/index/generation and exhaustion checks. Reject a global manager and persistent cached raw pointers for relocating dense storage. |
| Pete Isensee, **Custom STL Allocators**, *Game Programming Gems 3*, §1.6, printed pp.49–58, PDF pp.51–60 | Allocating storage and constructing objects are separate operations; allocator equivalence means compatible deallocation | Make backing-domain compatibility explicit for any future swap/splice and require tests for it. Keep reserve separate from construction. Reject the sample's exception-based overflow and old allocator boilerplate. |
| Paul Glinker, **Fight Memory Fragmentation with Templated Freelists**, *Game Programming Gems 4*, §1.5, printed pp.43–49, PDF pp.59–65 | Fixed-size, owner-local blocks can remove per-object allocation; access order still determines locality | Add explicit owner-local metadata and capacity admission rules. Reject default-constructing all pool objects and overwriting bytes in live objects; construct only occupied slots. Retain measured-consumer gate rather than adopting pooling everywhere. |
| Bill Budge, **A Generic Tree Container in C++**, *Game Programming Gems 4*, §1.6, printed pp.51–59, PDF pp.66–74 | Traversal should be reusable; maintaining a cached last-descendant link can make repeated append updates quadratic on a chain | Add tree-build complexity and cached-link invariants as review gates. Keep static hierarchies as cooked arrays. Reject a universal tree and static allocator per template; a dynamic hierarchy needs an owner and workload. |

Resulting contract amendments:

- A durable handle is resolved through its owner for one operation or explicit
  borrow scope. The caller may not cache the resolved pointer across insertion,
  compaction, retirement, or reset. Validation is part of release lookup too,
  rather than solely an assertion. A future scoped access object must enforce
  this rule without hidden global lifetime management.
- Future domain-aware containers may steal/swap backing storage only while
  retaining the original domain identity, or under proven deallocation
  compatibility. A cross-domain deep copy is explicit and fallible. Test distinct
  fake domains to detect freeing through the wrong owner. Do not assume two
  allocator objects with the same template type are interchangeable.
- Pools belong to an owner/workload. Freelist metadata is outside live payload
  objects; repeated free, foreign-owner free, and exhausted admission need
  explicit validation. Capacity slots are raw storage, not pre-created objects.
  Test non-default-constructible values and destructor balance. Fixed capacity
  requires a full status and a documented backpressure policy.
- Any later hierarchy exposes traversal centrally rather than duplicating it
  in every consumer. Validate parent/child/sibling or packed-range invariants,
  prohibit cycles, and measure deep-chain construction as well as traversal.
  A cached traversal link is a cost/complexity decision, not an automatic win.

These revisions refine the planned structures and domain integration. They do
not justify changing Array's measured growth policy, importing an old tree
library, or claiming a speedup for the first SortedMap consumer. The implemented
vertical slice follows the allocation/lifetime separation directly: allocate
before construction, stage resource-registry copies before commit, and verify
failed insertion leaves borrowed and move-only values intact.

## In-place ordering

`foundation::Sort(values, count, less)` in `containers/sort.hpp` uses an in-place
heap sort: O(count log count) comparisons, O(1) auxiliary storage, no allocation
and no stability guarantee. Elements must support nonthrowing move construction
and assignment, and the comparator must provide a nonthrowing strict weak
ordering. A zero count permits a null pointer. Element addresses remain valid;
values can move between positions. Use the existing `StableSort` with separate
initialized scratch storage when equal keys must retain their input order.
The Perfetto exporter uses that stable operation and returns failure if scratch
allocation fails.

## Borrowed contiguous views

`FoundationContainers` exports `BorrowedView<T>` in
`ludus/foundation/containers/borrowed_view.hpp`. It carries an address and element
count, allocates nothing, and neither owns storage nor extends its lifetime.
Built-in arrays preserve their extent on construction. For dynamic storage,
construct with `{array.GetData(), array.GetSize()}`. `BorrowedView<const T>`
prevents element writes through that view.

An empty view means absence. A null pointer with a nonzero count is invalid;
consumers check `IsValid()` and their required extent before indexing. This
validation cannot prove that the caller's declared storage is alive or large
enough. Indexing asserts validity and bounds. Renderer authored mesh channels
use this contract and copy the data before `CreateMesh` returns; callers may
release their storage after that call. Ownership-transfer APIs instead use a
Ludus-owned owner. Shared/weak pointer facilities remain unimplemented and are
not required for these synchronous borrowed inputs.
