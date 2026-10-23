# First container-system increment: evidence

The [architecture](../architecture/container-systems.md) selects storage by
workload. This increment supplies a contiguous `SortedMap` and repairs fallible
Array insertion/append. It does not establish a general lookup speedup.

## Reproducible measurement

Use the optional [benchmark](tools/container-bench.md). Measurements
below used Clang 18.1.3, libstdc++ from GCC 13.3.0, C++23, x86-64 Linux, and the
Development preset (`-O2 -g`, assertions enabled, exceptions disabled). Hardware
was an Intel Core i5-8265U, four cores/eight logical CPUs. This task's builds and
analysis had finished before measurement; other desktop activity and CPU
frequency were not controlled. These are local observations, not CI thresholds.

Each lookup sample contains 100,000 identical seeded queries, about 50% misses.
Insertion samples contain 500 descending-key rebuilds, including clear, with
capacity retained. Each reported number is the median of seven samples. Two
successive harness runs are shown as a range of their medians; this is not a
confidence interval. Columns give nanoseconds per operation.

| Entries | Sorted lookup | Hash lookup | Tree lookup | Sorted rebuild | Hash rebuild | Sorted backing bytes |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 10.32–11.75 | 10.26–11.17 | 4.89–5.21 | 5.60–5.61 | 24.35–38.20 | 16 |
| 4 | 20.16–20.31 | 14.68–14.91 | 14.47–14.54 | 9.05–9.31 | 24.61–25.16 | 64 |
| 16 | 30.37–31.38 | 13.54–16.33 | 19.19–19.89 | 12.90–13.17 | 28.57–30.26 | 256 |
| 64 | 44.72–44.79 | 14.92–17.11 | 28.58–30.98 | 17.71–18.50 | 33.17–42.74 | 1,024 |
| 256 | 61.97–65.21 | 13.53–15.63 | 35.61–36.94 | 33.42–39.23 | 33.08–37.27 | 4,096 |
| 1,024 | 70.57–71.86 | 15.31–16.09 | 42.29–43.58 | 93.97–103.28 | 35.34–39.73 | 16,384 |

Baselines are `std::unordered_map<uint64, uint64>` and
`std::map<uint64, uint64>`. Sorted storage excludes the 24-byte owning object.
Hash/tree allocator and bookkeeping bytes were not measured. Hash rebuilds
retain buckets but allocate/free nodes; sorted rebuilds retain their single
block. The workloads intentionally measure both lookup losses and mutation
costs. They do not cover string keys, adversarial collisions, cold caches,
deletion churn, other CPUs, or browser performance.

On this dataset, hashing generally wins lookup; sorted rebuilds win at 1–64
entries, overlap at 256, and lose at 1,024. There is no universal crossover
threshold. GameHost's supported clear-configuration fixture currently binds one
logical asset. Its migration is justified by explicit failure handling and
contiguous reusable storage; these timings do not demonstrate an engine frame
time improvement. Re-evaluate the selection when resource cardinality or the
access pattern changes.

## Deterministic correctness and allocation evidence

- 10,000 seeded map operations are compared after every mutation with an
  independent direct-index membership/value model, including ordered output.
- Lifetime ledgers verify insertion, relocation, copying, removal, clear, reset
  and destruction. Move-only and non-default-constructible values are covered;
  fallible Array append still supports non-assignable values.
- Failure injection rejects the existing nothrow allocation seam. Failed
  insertion and append preserve pointers, size, capacity and aliased/external
  move-only arguments; duplicate insertion consumes nothing. Oversize capacity
  requests fail without changing the map.
- Reserving 16 entries makes exactly one allocation. Inserting descending keys,
  finding each value, clearing and reusing capacity add none. Reset makes one
  matching free. The production allocator is not replaced or instrumented.
- GameHost tests check independent staged copies, updates, missing lookups and
  retirement. Capacity preparation precedes presenter commit; reload staging
  can reject allocation failure without changing the active registry.
- Both the installed native SDK consumer and the browser foundation probe
  instantiate the new public header and exercise fallible insertion. Timings
  are not asserted in tests.

Full warning-clean builds, unit tests, ASan/UBSan, formatting, pinned clang-tidy,
standalone headers, include boundaries and installed-SDK checks are the review
gates. Live Wayland/keyboard tests require a desktop session; local headless
runs report them as skipped. The PR's CI jobs supply the independent build
budget and browser/package validation. No budget is raised by this increment.

## M1 migration and policy gate

Recorded 2026-10-16 for [M1](../architecture/container-systems.md#m1-consumer-migration).
The base revision is `5117c8c`. The host was an Apple M5 (arm64) running macOS,
with the pinned Homebrew Clang/LLD 18.1.8 and libc++. This increment changes
ownership and failure handling, not lookup or growth policy, so it has no timing
claims. The first increment's measurements above are unchanged.

- **Inventory.** `python3 tools/check_container_policy.py` reports 209 findings
  in 42 first-party files on the base revision. Rebasing onto `db05ed6` exposed
  14 more findings in two filesystem test files added on `main` in the meantime.
  Those were migrated too, and the gate now reports none.
  `scripts/python/test_check_container_policy.py` covers every source root,
  spaced and `pmr` spellings, comments and literals (including raw strings),
  vendor/output trees, similarly named identifiers, line numbers and a stale
  exception. The gate runs from `./scripts/check --format` and in CI.
- **Allocation failure.** `ludus_text_alloc_tests` rejects the aligned nothrow
  allocation seam only. It verifies that creation, font loading, shaping and
  rasterization report `OutOfMemory` with no published owner or handle, cleared
  outputs and unchanged counters. Each operation succeeds after rejection is
  disarmed, and the font remains unloadable. GameHost and Audio Gym failure
  paths stage allocations before committing outputs. They were verified by
  review and by their existing success-path suites; no fault injection was
  added for them.
- **Validation.** The Development preset builds warning-clean, including
  `-Werror` on project targets. All ctest suites relevant to the change pass.
  The containers, containers allocation, localization and test-diagnostics
  executables are built but outside this preset's ctest selection, so they were
  run directly and pass. The pinned clang-tidy is clean on the 37 changed
  translation units in the compile database, with warnings treated as errors.
  `./scripts/check --format` and `scripts/python/source_policy.py --base HEAD`
  with the pinned formatter pass.
- **Sanitizers.** The ASan/UBSan preset builds warning-clean. Its tests could
  not run on this host: the LLVM 18 ASan runtime deadlocks during its own
  initialization on this macOS release, before test code runs. Sampling showed
  dyld shared-cache iteration re-entering ASan's allocator, and other
  worktrees' ASan runs hang the same way. Linux CI runs the sanitizer suites.
- **Not run locally.** Two suites failed for environmental reasons:
  `ludus_platform_cocoa_tests` skips every case without a WindowServer session,
  and `ludus_target_detection` lacks iOS/tvOS/watchOS SDKs. The renderer render
  tests are Linux/Slang-only, the editor tests need Qt, and the script-provider
  tests need cooked provider inputs. The filesystem benchmark is opt-in and was
  syntax-checked only. Linux CI and the full batch tier build these targets.

## M3 FixedArray and Input migration

Recorded 2026-10-17 for [M3](../architecture/container-systems.md#fixedarray-contract)
on the same host and pinned toolchain as M1. `FixedArray` adds no growth policy
or lookup algorithm, so there are no timing claims.

- **Correctness.** `[fixed-array]` in `ludus_foundation_containers_tests` has
  ten cases with about 327,000 assertions. They cover full status with an
  unchanged array, zero capacity, construction of occupied slots only,
  non-default-constructible and move-only values, aliased append and ordered
  insertion, ordered and swap removal, all-or-nothing range append, and copy,
  move and assignment in both growth directions with a balanced lifetime
  ledger. Over-aligned elements are also covered. A 20,000-operation seeded
  stream is compared after every step with a plain array-plus-count model and
  the live-object count.
- **Allocation.** `ludus_containers_alloc_tests` counts every global
  `operator new`/`delete`. A 64-element `FixedArray` filled by front insertion,
  rejected when full, copied, moved, shifted and cleared makes no allocation.
- **Consumer.** The Input reducer's binding table and per-step event buffer now
  use `FixedArray`, replacing raw arrays with hand-maintained counts. The
  existing reducer, action, trace and Input allocation suites pass unchanged.
- **SDK and browser.** `tests/sdk_consumer/containers.cpp`, which the installed
  SDK consumer and the browser foundation probe both build, exercises
  `FixedArray`. Those targets were not built locally; CI's SDK and browser jobs
  build them.

## M4 RingBuffer and Input pending queue

Recorded 2026-10-17 for [M4](../architecture/container-systems.md#ringbuffer-contract)
on the same host and pinned toolchain as M1. Inline storage with compile-time
capacity was selected. `RingBuffer` adds no algorithm choice beyond wrapping by
subtraction, so there are no timing claims.

- **Correctness.** `[ring-buffer]` in `ludus_foundation_containers_tests` has
  eight cases with about 287,000 assertions. They cover FIFO order with full
  status, wrapped leading/trailing spans and their reset to the first slot after
  draining, zero capacity, construction of occupied slots only, move-only and
  non-default-constructible values, aliased appends with stable borrows across
  wraparound, copy, move and assignment of wrapped contents, and over-aligned
  elements. A 20,000-operation seeded stream on a seven-slot ring (not a power
  of two) is compared after every step with a shifting-array model, covering
  indexing, both spans and the live-object count.
- **Allocation.** `ludus_containers_alloc_tests` counts every global
  `operator new`/`delete`. A 16-slot ring that wraps through 1,000 additions,
  then is copied, moved and cleared, makes no allocation.
- **Consumer.** The Input reducer's pending-transition queue now uses
  `RingBuffer`, replacing a raw array with a hand-maintained head and count.
  Overflow recovery still runs when the queue is full, and the rejected
  transition leaves the queue unchanged. The existing reducer, action, trace and
  Input allocation suites pass unchanged, including their overflow tests.
- **SDK and browser.** `tests/sdk_consumer/containers.cpp` exercises full
  rejection and wraparound, but those targets were not built locally; CI's SDK
  and browser jobs build them.

## M5 StaticBitSet and key and asset masks

Recorded 2026-10-17 for [M5](../architecture/container-systems.md#staticbitset-contract)
on the same host and pinned toolchain as M1. Only the fixed-size form shipped;
the dynamic `BitSet` is deferred because no consumer needs a run-time bit count.
There are no timing claims.

- **Correctness.** `[bit-set]` in `ludus_foundation_containers_tests` checks
  0, 1, 63, 64, 65 and 130 bits against a plain bool-array model. It covers
  first, last and word-edge bits, `FlipAll`/`SetAll`/`ResetAll` with tail
  masking, every left and right shift amount from 0 through `N + 2`, set
  operations, and `FindNextSet` visiting exactly the set bits. A
  20,000-operation seeded stream on 193 bits mixes set, reset, flip and shifts.
  Compile-time assertions check layout and size (one word for 64 bits, two for
  65), trivial copyability, `constexpr` use, and the popcount and trailing-zero
  helpers against direct loops and edge words.
- **Allocation.** `ludus_containers_alloc_tests` counts every global
  `operator new`/`delete`. Whole-set operations, shifts, complement and copies
  of a 200-bit set make no allocation.
- **Consumers.** The Input reducer's live-down, live-suppressed and
  reset-baseline masks, and the Resources manager's dependency-closure and
  catalog-duplicate sets, now use `StaticBitSet` instead of `bool` arrays. The
  existing Input and Resources suites pass unchanged. The reducer's write-only
  live-suppressed mask was later removed; suppression is step-time state. The public
  `KeyboardSnapshot` and `FocusBaseline` bool arrays were deliberately left
  alone to keep the SDK API and ABI stable.
- **SDK and browser.** `tests/sdk_consumer/containers.cpp` exercises a 65-bit
  set, but those targets were not built locally; CI's SDK and browser jobs build
  them.
