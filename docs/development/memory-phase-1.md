# Memory Phase 1: Domain Counters, Registry and Gauges

This page records phase 1 of the
[memory profiling architecture](../architecture/memory-profiling.md): checked
counters on the existing system-backed `AllocationDomain`, a stable domain
registry, the explicit `Array` migration, a coverage manifest and a quiescent
gauge export. It follows the metric contracts in that design's
[questions and metric contracts](../architecture/memory-profiling.md#questions-and-metric-contracts)
and the counter tier of the
[memory management design](../architecture/memory-management.md#113-tiers-and-cost).
[Memory phase 0](memory-phase-0.md) supplied the benchmark tool and baselines
used below.

No allocator, live table, allocation journal, site attribution or memory
capture session is introduced. The system backend is unchanged.

## What changed

| Area | Change |
| --- | --- |
| [`allocation_domain.hpp`](../../modules/foundation/memory/include/ludus/foundation/memory/allocation_domain.hpp) | Every domain carries checked counters. `GetContainerAllocationDomain()` returns a second built-in domain. A zero-byte request with a valid alignment is an empty success without a block. |
| [`allocation_statistics.hpp`](../../modules/foundation/memory/include/ludus/foundation/memory/allocation_statistics.hpp) | Opt-in queries: `GetAllocationStatistics`, the registry (`TryRegisterAllocationDomain`, `TryGetRegisteredAllocationDomain`) and the coverage exclusion manifest. |
| Memory `src/internal/accounting.*` | Private relaxed-atomic counter updates, the constant-initialized registry and built-in domains. |
| [`array_support.cpp`](../../modules/foundation/containers/src/array_support.cpp) | The `Array` allocation seam calls the `Containers` domain. |
| [`memory_gauges.hpp`](../../modules/foundation/profiling/include/ludus/foundation/profiling/memory_gauges.hpp) | `RecordMemoryGauges()` samples every registered domain onto the CPU trace timeline; `ExportPerfettoTrace` writes the samples and the coverage manifest. |
| [`tools/memory-bench`](../../tools/memory-bench/main.cpp) | Raw-backend and four-thread contended churn workloads. |

Dependencies keep the design's direction: Memory depends only on Base.
Containers links Memory privately; no public container header names it.
Profiling samples Memory's counters outside allocation hooks, and Memory never
calls Profiling or Logging.

## Counter contract

Each domain has these counters. Requested bytes are the sizes passed to
`TryAllocate`, never backend usable, committed or resident bytes.

| Counter | Meaning |
| --- | --- |
| `LiveRequestedBytes`, `LiveBlocks` | Allocated and not yet freed. |
| `PeakRequestedBytes` | High-water of this domain's `LiveRequestedBytes`. |
| `AllocatedBytes`, `AllocationCount` | Cumulative successful nonzero allocations. |
| `FreedBytes`, `FreeCount` | Cumulative non-null frees. |
| `FailureCount` | Valid requests the backend could not satisfy. |
| `RejectedCount` | Requests rejected before the backend: invalid alignment or a missing callback. |

- **Ordering.** Allocation accounting follows backend success and precedes the
  pointer return. A free subtracts before the backend releases the block, so a
  reused address cannot charge an older lifetime.
- **Origin.** Counters live in the domain, so a free on another thread, or after
  the allocating thread exited, charges the domain that allocated the block.
- **Samples.** Every update is a relaxed atomic read-modify-write on that
  domain's own counters. A query is a per-field sample, not a transaction.
  `LiveRequestedBytes == AllocatedBytes - FreedBytes` and
  `LiveBlocks == AllocationCount - FreeCount` hold only at a caller-established
  quiescent boundary.
- **Peaks.** The peak is raised from the exact counter value each allocation
  produced. It is a per-domain history; summing domain peaks does not give a
  simultaneous process peak.
- **Validity.** Overflow of any counter sets a sticky `Saturated` state. Freeing
  more bytes or blocks than are live, or a free with a size or alignment no
  allocation can have, sets a sticky `AccountingError` state. An underflow after
  a saturation is treated as a consequence of the wrapped value, not as a new
  error. Arithmetic never restores `Valid`. There is no counter reset in this
  phase.
- **Zero and null.** A zero-byte request with a valid alignment returns null and
  changes no counter. A null free changes no counter.
- **Footprint.** The counters add 80 bytes to each domain on 64-bit targets
  (24 to 104 bytes). The registry is 1,792 bytes of static storage.

The near-limit cases are tested with injected counter values rather than
impossible allocations.

## Registry and coverage

The registry is a bounded, constant-initialized table of 32 process-lifetime
domains. It is usable before dynamic initialization and through static
teardown. `System` (ID 1) and `Containers` (ID 2) are registered without any
initializer. `TryRegisterAllocationDomain` copies a name of up to 47 ASCII
letters, digits, `.`, `_` or `-`. Registering the identical domain and name
again returns its ID. A reused name, a second name for one domain, an invalid
name or a full registry is an explicit status with the output unchanged.
Registration is cold and serialized. It never affects allocation: an
unregistered domain still counts, but it is not enumerated or exported.

Domain totals cover only allocations made through registered domains. The
manifest from `GetAllocationCoverageExclusion` names the known paths outside
them. It is not a measured denominator, so no coverage percentage is reported.

| Domain or exclusion | Covers or excludes |
| --- | --- |
| `System` | Engine storage that uses `GetSystemAllocationDomain()`: config, string dictionary, trace slab and other explicit callers. |
| `Containers` | `Array` buffers. Capacity bytes are charged, including the old and new buffers while growth relocates. |
| `unregistered-domains` | Custom domains that were never registered. |
| `direct-engine-allocation` | Engine `new`/`delete`, `malloc` and `UniquePtr` default deleters that bypass a domain. |
| `standard-library-storage` | `std::string`, `std::function`, `std::thread` and other standard-library internals. |
| `third-party-libraries` | FreeType, HarfBuzz, decoders and other native heaps without adapted callbacks. |
| `game-host-module-backing` | GameHost module allocations, reported separately as host backing bytes. |
| `graphics-driver-and-gpu` | Driver CPU heaps, GPU memory and browser graphics implementations. |
| `process-runtime` | Static storage, thread stacks, thread-local storage, loader and C runtime memory. |

## Array migration

The seam keeps its signature and the aligned nothrow `operator new`/`delete`
family, so blocks built before and after the change are interchangeable and
`Array`'s 24-byte layout, growth and failure behavior are unchanged. Array
storage no longer appears in the `System` domain. Moves and `Swap` create no
allocation or free events; `Clear` keeps the charged capacity; trimming an empty
array frees it; a copy allocates only the copied size. The existing global
`operator new` counting tests still observe Array allocations, because the
backend family did not change. A per-array domain is phase 2 of the memory
management plan and is not added here.

## Gauge export

`RecordMemoryGauges()` records one timestamped group of trace events per
registered domain while a CPU capture is active. `ExportPerfettoTrace` writes
each group as one counter event:

```json
{"pid":1,"tid":0,"ts":280063271532.958,"ph":"C","name":"Memory/Containers",
 "args":{"Validity":0,"LiveRequestedBytes":4096,"LiveBlocks":1,"PeakRequestedBytes":6144,
         "AllocatedBytes":8128,"FreedBytes":4032,"AllocationCount":7,"FreeCount":6,
         "FailureCount":0,"RejectedCount":0}}
```

A domain that is not `Valid` exports only `Validity` (1 saturated, 2 accounting
error), never plausible byte values. The trace's top-level `metadata` object
gains `ludus-memory-gauges`: schema version, metric kind, sampling and peak
semantics, the sampled domains and the exclusion manifest. Gauge events share
the capture's bounded pool; a dropped event omits that metric and is counted in
`TraceHealth::EventsDropped`.

The values reconcile exactly only when the caller has stopped allocation and
free in every registered domain for the duration of the call. The function
cannot detect a violation. Domain accounting itself is independent of
`LUDUS_PROFILING_ENABLED`; in a profiling-disabled browser build
`RecordMemoryGauges()` returns false. See the
[FoundationProfiling guide](../modules/foundation/profiling.md#memory-gauges).

## Overhead

Host: Apple M5 (4 performance, 6 efficiency cores), macOS 26.6, Clang 23.1.3
from the prepared host tools, `macos-clang-profile` (RelWithDebInfo, Profile flavor). Both
variants link the same benchmark source. `before` is base `ec577ca4` with only
the new benchmark source; `after` is this change. Six alternating rounds ran one
fresh process per workload and variant, seven samples each, so every median
is over 42 samples. Other worktrees' builds and tests kept the one-minute load
average near 10–11 on 10 cores throughout, so absolute values are specific to
this loaded host.

| Workload | Before median ns/op (IQR) | After median ns/op (IQR) | Change in median |
| --- | ---: | ---: | ---: |
| `system-small-churn` | 30.41 (29.87–34.17) | 29.45 (28.23–32.33) | −3.1% |
| `raw-small-churn` (unchanged code) | 26.73 (24.47–34.36) | 24.23 (22.62–26.43) | −9.4% |
| `system-contended-churn-4-threads` | 23.83 (21.97–25.25) | 158.87 (149.70–185.13) | **+567%** |
| `raw-contended-churn-4-threads` (unchanged code) | 22.79 (21.76–23.99) | 23.93 (22.09–25.54) | +5.0% |
| `system-remote-free` | 13.91 (11.74–15.39) | 13.77 (12.49–14.76) | −1.0% |
| `system-large-blocks` | 5,738 (5,128–5,868) | 5,240 (5,188–5,282) | −8.7% |
| `array-append-unreserved` | 0.66 (0.58–0.89) | 0.53 (0.52–0.54) | −19.8% |
| `array-append-reserved` | 0.50 (0.46–0.52) | 0.46 (0.40–0.47) | −7.1% |
| `array-append-aligned64` | 5.02 (4.10–5.41) | 4.31 (4.07–4.62) | −14.0% |

The raw workloads run identical code in both variants, yet their medians moved
by −9% and +5%. Changes of that size are noise on this host. Within that
noise, these samples show no single-threaded cost from the counters, including
remote frees and Array growth; they cannot rule out a cost of a few nanoseconds
per operation. The faster `after` Array rows are not a speedup claim. Median
peak RSS of the idle process was 1,408 KiB before and 1,360 KiB after.

**Contention is the measured cost.** When four threads allocate and free in one
domain, every operation writes the same counter cache line, and wall time per
operation rose about 6.7 times. Two experimental builds, which were not
committed, bounded the alternatives
([raw samples](memory-phase-1-evidence/contention-variants.csv); 21 samples per
row, two batches):

| Accounting variant | Atomic writes per allocate / free | Contended median ns/op |
| --- | --- | ---: |
| No accounting (base) | 0 / 0 | 22.7, 22.9 |
| Live bytes and peak only | 1 / 1 | 76.7, 64.9 |
| Derived freed bytes and live blocks | 3 / 2 | 122.4 |
| This change | 4 / 4 | 189.4, 143.8 |

Even one shared write per operation costs about three times the base under this
contention, so reducing the counter set does not remove the cost. The design
anticipates this: it allows persistent origin-owned counter shards, but only
with a documented weaker peak and snapshot contract. This change keeps the exact
per-domain contract. Its acceptance gate, no supported frame-time regression
above 2% in representative release workloads, has not been measured: no such
workload exists yet. Separate subsystem domains (memory management phase 2)
spread contention across domains; a sharded mode remains a separate reviewed
decision.

Raw samples and per-round metadata:
[before](memory-phase-1-evidence/before/metadata.json) and
[after](memory-phase-1-evidence/after/metadata.json), with their
`benchmarks.csv` files in the same directories.

## Validation

| Check | Result |
| --- | --- |
| Full `macos-clang-development` build, warnings as errors | Warning-clean. |
| `macos-clang-development` CTest | 108 of 108 pass; `ludus_platform_cocoa_tests` skips without a window session, as before. Memory now has 29 cases, including the shared-owner tests from `develop`. |
| AddressSanitizer + UndefinedBehaviorSanitizer (`macos-clang-asan-ubsan`, Clang 23.1.3) | Memory, Containers, Profiling and Strings pass with no reports, three runs each. |
| ThreadSanitizer (`macos-clang-development` with `LUDUS_ENABLE_TSAN=ON`, Clang 23.1.3) | Memory, Containers, Profiling and Strings pass with no reports, three runs each. |
| Installed SDK consumer (`./scripts/install-sdk macos-clang-development`) | Pass. |
| Build-time budget (`./scripts/check-build-budget --profile`) | Pass; every budgeted header within budget. |
| Format, naming, container policy, `source_policy.py` against the merge base, clang-tidy 23 | Pass. |
| `check-docs`, strict MkDocs, `build-api` (Doxygen 1.18.0), `check-wiki --documentation-only`, docs and wiki unit tests | Pass; no new undocumented public symbols. |

The public domain header adds no namespace: the counters are a private nested
type, so code that combines `using namespace ludus::foundation` with its own
`detail` alias is unaffected.

## Limitations

- Results come from one loaded Apple Silicon host and synthetic workloads. They
  are not engine frame-time results.
- The contended workload is a worst case: four threads doing nothing but small
  allocations in one domain.
- Gauges are samples at caller-chosen points. They are not an allocation
  history, a live set or a leak report; those are phases 2 and 3 of the
  profiling design.
- Only macOS was built and tested locally. Linux, Windows and browser builds
  rely on CI.

## Reproducing

Build the benchmark as in [memory phase 0](memory-phase-0.md#reproducing), then
run the new workloads with the same runner:

```bash
out/host-tools/venv/bin/python scripts/python/memory_baseline.py --binary out/build/macos-clang-profile/tools/memory-bench/ludus_memory_bench --cmake-cache out/build/macos-clang-profile/CMakeCache.txt --output-dir out/memory-phase-1 --processes 1 --workload system-small-churn --workload raw-small-churn --workload system-contended-churn-4-threads --workload raw-contended-churn-4-threads
```

For a before/after comparison, build the same benchmark source against the base
revision in a separate worktree and alternate the runner between the two
binaries, as recorded in the metadata above.
