# Ludus Core Containers — Static and Dynamic Array Design

> For the broader family, lookup contracts and staged implementation, see
> [Container systems](container-systems.md). That design supersedes the
> historical future taxonomy in §26; §34 remains authoritative for the arrays.
>
> Status: **Implemented.** This document began as the design/architecture
> deliverable; the containers were implemented, tested, benchmarked, and the
> first-party codebase migrated at that point. The 2026-10-06 source review found
> reintroduced `std::vector` uses in newer modules and tests. Milestone M1 of the
> [container roadmap](container-systems.md#m1-consumer-migration) migrated them
> on 2026-10-15. `tools/check_container_policy.py` now rejects new uses. See §34.3/§34.4 for the authoritative policy and permitted
> exceptions. Earlier sections that described a *sequenced*
> migration with deferred/"stay STL" container uses (§3, §4, §23, §24, §32.6) are
> historical and are superseded by §34.3/§34.4 where they conflict.
>
> **§34 is the current authoritative naming and API.** An architectural
> correction renamed the types to the Ludus-native taxonomy
> (`StaticArray<T, N>` fixed-size, `Array<T>` dynamic — freeing `Vector` for
> mathematics) and moved the member API off STL spellings onto the engine's
> verb-oriented convention (`GetSize`/`IsEmpty`/`GetData`/`EnsureCapacity`/`Add`/
> `RemoveLast`/…). Where §§1–33 use the old names (`Array<T,N>`/`Vector<T>`,
> `Size()`/`PushBack()`/`Reserve()`), read them through §34's mapping. §32
> (measured results) and §33 (audit corrections) remain valid for behaviour and
> the growth-factor decision (2×).
>
> Audience: Ludus engine contributors and reviewers.
> Scope: two owning contiguous containers — a fixed-size static array and a
> dynamically sized array — plus the supporting traits, the future container
> taxonomy they must not corner, and a concrete migration plan.
>
> This document deliberately separates four kinds of claim, and labels them
> inline where it matters:
>
> - **[Standard]** — a requirement or guarantee of the C++23 language/library.
> - **[Library]** — observed behaviour of an existing implementation
>   (libstdc++, libc++, LLVM, Folly, EASTL, Unreal). Not normative; evidence of
>   what is possible and what trade-offs others chose.
> - **[Measured]** — something we will only assert once benchmarked on the
>   pinned toolchain. In this design phase these are stated as hypotheses to be
>   confirmed by the §22 benchmark suite.
> - **[Judgment]** — an architectural decision made for Ludus with reasons, not
>   derivable from the above alone.

---

## 1. Executive summary

Ludus should own two foundational contiguous containers:

- **`ludus::foundation::Array<ElementType, Count>`** — a fixed-size, owning,
  stack/inline aggregate. Broad analogue of `std::array`.
- **`ludus::foundation::Vector<ElementType>`** — a dynamically sized, owning,
  heap-backed contiguous array. Broad analogue of `std::vector`.

Both are thin, unsurprising, cache-friendly types whose optimized-build codegen
should be indistinguishable from (or better than) the equivalent standard
container for the operations Ludus actually uses. They are **not** more clever
than `std::vector`; they are more *aligned with Ludus*: exception-free by
construction, explicit about allocation failure, integrated with the Ludus
assertion/build-flavor system for zero-cost release bounds behaviour, spelled in
Ludus fixed-width types and naming conventions, and positioned as the single
seam through which a future Ludus allocator is introduced.

The single most important reason to own them is **not** raw performance — a
tuned `std::vector` is already excellent. It is **control of the allocation and
error-handling seam** under a hard `-fno-exceptions` policy, combined with a
**coherent, stable, engine-native container vocabulary** that a custom allocator
(ADR 0003 makes it inevitable), a fixed-capacity vector, and a small/inline
vector can all attach to later without churning call sites again.

The current inventory is small (12 non-test `std::vector` uses, 4 `std::array`
uses, 7 `std::span` uses), and several of those uses are legitimately entangled
with facilities that are themselves slated for replacement (`std::string`,
`std::filesystem`, `std::unique_ptr`). This means the migration is cheap to
sequence and easy to keep green, and that some uses should *stay* STL-based
until their neighbours (String, VFS) are addressed. The design is therefore
paired with an explicit, subsystem-ordered migration plan and a short list of
justified STL-boundary exceptions.

`std::span` is **kept**. It is the non-owning view/interop vocabulary the owning
containers convert *into*; replacing it is out of scope and would weaken
ecosystem interoperability.

---

## 2. Current Ludus constraints

These are discovered facts about the repository as it exists today. They are the
hard boundary conditions on the design.

| # | Constraint | Source | Design consequence |
|---|---|---|---|
| C1 | **No C++ exceptions.** Engine builds with `-fno-exceptions` (`/EHs-c-`), enforced by the compiler; `bugprone-std-exception-baseclass` is a secondary guard. | `AGENTS.md`, `cmake/EngineOptions.cmake`, ADR 0003 | Containers must be exception-free. No `throw`. Allocation failure and precondition violations cannot be reported by throwing. Failure must be in the signature or handled fatally. Mark hot/infra methods `noexcept`. |
| C2 | **Fixed-width Ludus type spellings.** `uint8..uint64`, `int8..int64`, `usize` (= `std::size_t`), `isize` (= `std::ptrdiff_t`), `float32/float64` from `ludus/foundation/base/types.h`. `std::size_t`/`std::uint32_t` spellings are banned in engine code. | `AGENTS.md`, `types.h`, ADR 0003 | Public API uses `usize` for sizes/indices, `isize` for differences. Internal size/capacity representation is a decision (§10–§11), but the *spelling* is Ludus aliases. |
| C3 | **Naming and style.** PascalCase types and methods; `m`-prefixed members; template parameters spelled out (`ElementType`); PascalCase concepts; `[[nodiscard]]`, `[[no_unique_address]]`; `#pragma once`. | `pointer.hpp` (`UniquePtr`), `window.h`, `.clang-tidy` | `Vector::PushBack`, `Vector::Data`, `Vector::Size`, `mData/mSize/mCapacity`. Not `push_back`. STL-shaped lowercase aliases are provided only where needed for range-for/ranges interop (§20). |
| C4 | **Namespace + re-export idiom.** Foundation primitives live in `ludus::foundation::core` and are re-exported into `ludus::foundation`. | `pointer.hpp`, `types.h` | `core::Array`/`core::Vector` with `using ludus::foundation::Array = core::Array<...>` aliases. |
| C5 | **Module layout & dependency direction.** `modules/<layer>/<module>/`, public headers under `include/ludus/<layer>/<module>/`, private under `src/internal/`. `FoundationBase` depends on nothing higher. Static libs by default. | `AGENTS.md`, ADR 0002 | The containers belong in a foundation module at or below the level of everything that will use them. See §9 for placement. |
| C6 | **Build flavors drive assertion policy, not `NDEBUG`.** `LUDUS_ENABLE_ASSERTS` is 1 for Debug/Development, 0 for Profile/Release. `LUDUS_ASSERT` compiles out entirely when disabled; `LUDUS_REQUIRE`/`LUDUS_FATAL` are always live; `LUDUS_CHECK` returns a `bool`. Assertions use an **independent** emergency path, never Logging. | `assert.hpp`, `EngineBuildFlavor.cmake`, `assert_config.hpp.in`, ADR 0003/0006 | Bounds/invariant checks use `LUDUS_ASSERT` → zero release cost, rich debug/dev diagnostics for free. Unconditionally-fatal conditions (e.g. OOM in the infallible API) use `LUDUS_FATAL`/`LUDUS_REQUIRE`. |
| C7 | **Warnings-as-errors.** `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wundef -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough -Woverloaded-virtual -Wnon-virtual-dtor`, CI `-Werror`. | `EngineCompilerWarnings.cmake` | Templates must be warning-clean across all instantiations, including empty types and over-aligned types. |
| C8 | **Build-time budgets; no heavy STL in public headers.** Default 2000 ms average per-include parse budget; heavy templates type-erased behind `.cpp` (ADR 0004/0005). | `build_budget.json`, ADR 0004/0005 | Container headers include only light headers (`<type_traits>`, `<utility>`, `<new>`, and Ludus base headers). Cold/error paths (growth math, OOM handling) are candidates for out-of-line non-template helpers. `<memory>`, `<vector>`, `<algorithm>`, `<initializer_list>`-heavy pulls avoided. |
| C9 | **No engine allocator exists yet.** ADR 0003 states `std::vector`→"Ludus `Array`/`Span` with allocator support" *once a custom allocator exists*. `UniquePtr` already uses global `new`/`delete`. | ADR 0003, `pointer.hpp` | v1 must not invent an allocator framework, but must be designed so the allocator seam can be added without an API break (§14). |
| C10 | **Cross-platform intent.** Milestone 0 is Linux/Clang-only (FoundationBase `CMakeLists` hard-errors otherwise), but the engine targets Windows, Linux, macOS/Apple Silicon, Vulkan, Metal. | `README.md`, prompt | No Linux/Clang-only constructs in the container. Alignment, `memcpy`/`memmove`, `operator new` behaviour must be portable. Compiler intrinsics only behind the existing `compiler.hpp`/`defines.h` macros. |
| C11 | **`std::span` is allowed and used** as the contiguous view. | ADR 0003, logging | Keep `std::span`. Owning containers convert to it; do not build a competing owning-view type. |

---

## 3. Current container inventory

Counts are over `modules/` and `apps/` (engine code) plus tests, gathered by
grep on the current tree. "Engine" excludes `*/tests/`.

- `std::vector`: **19** occurrences total — **12 engine (non-test)**, 7 test.
- `std::array`: **4** occurrences — all engine `.cpp`, all fixed-size stack
  buffers.
- `std::span`: **7** occurrences — all in Logging, all non-owning views.
- `std::initializer_list`: **0**.
- Raw built-in arrays used *as containers*: only fixed-capacity buffers (see
  `TraceChunk`), not growable containers.
- APIs accepting/returning STL containers across a module boundary: the Logging
  backend (`std::vector<std::unique_ptr<ILogSink>>&&`) and the `std::span`
  format APIs.

### 3.1 `std::vector` — engine (non-test)

| Site | Declaration(s) | Hotness | Entanglement |
|---|---|---|---|
| `foundation/profiling/src/export_perfetto.cpp` | `vector<FlatEvent>`, `vector<TraceChunk*>`, `vector<uint64>`, `vector<std::string>` | **Cold** (offline trace export) | `std::string`, `std::stable_sort`; file explicitly documents deliberate STL use here |
| `foundation/logging/src/sinks/file_sink.cpp` | `vector<std::filesystem::path>`, `vector<Group>`, `vector<Group*>` | **Cold** (log retention/pruning) | `std::filesystem` (slated for a VFS layer) |
| `foundation/logging/src/internal/backend.hpp` + `backend.cpp` | `vector<std::unique_ptr<ILogSink>> mSinks`; `Start(vector<std::unique_ptr<ILogSink>>&&)` | Warm-ish (set up once; drained by worker) | `std::unique_ptr` (Ludus `UniquePtr` exists); header already carries a build-budget override partly due to this member |
| `foundation/logging/src/logger.cpp` | `vector<std::unique_ptr<internal::ILogSink>> Sinks` | Warm-ish (setup) | `std::unique_ptr` |

### 3.2 `std::array` — engine

| Site | Declaration | Notes |
|---|---|---|
| `foundation/base/src/diagnostic.cpp` | `array<char,512> buffer`, `array<char,16> lineDigits` | **Emergency assertion path.** Must remain dependency-free and allocation-free; extreme care. |
| `foundation/logging/src/logger.cpp` | `array<char, kMaxMessageBytes> buffer` | Producer-owned stack format buffer. |

### 3.3 `std::span` — engine (all Logging, all views — keep)

`log_format.hpp` `SubmitFormat(..., std::span<const FormatArg>)`;
`format_engine.{hpp,cpp}` `FormatInto(std::span<char>, ..., std::span<const FormatArg>)`
and internal callers.

### 3.4 Tests (low priority)

`std::vector<std::thread>` thread pools in logging/profiling regression and
scope/clock tests; `std::vector<TraceEvent>`/`<TraceChunk*>` drain buffers in
profiling tests. These may keep STL indefinitely; tests already re-enable
exceptions and are not bound by the engine STL policy.

### 3.5 Notable non-container evidence

`foundation/profiling/src/internal/trace_chunk.hpp` uses a **fixed-capacity**
built-in array `TraceEvent Events[kEventsPerChunk]` on a `alignas(64)` struct,
with a *drop-on-full, never-grow, never-allocate* policy. This is not a
`std::vector`/`std::array` use, but it is direct evidence that Ludus already
wants a **fixed-capacity vector** idiom (bounded count with a live size, no
growth). It is a signal for the future taxonomy (§26), **not** an argument for
adding small-buffer optimization to the fundamental `Vector`.

---

## 4. STL usage classification (A–E)

Using the prompt's taxonomy. Column "v1 need" states whether the initial
container design must support this to migrate the site.

| Site | Class | Rationale | v1 need |
|---|---|---|---|
| `backend`/`logger` `vector<unique_ptr<ILogSink>>` | **B** (dynamic replace) + **E** (element should be `UniquePtr`) | Owning growable list of sinks; small N; set up once. Pairs naturally with Ludus `UniquePtr`. Migrating may *reduce* `backend.hpp` include weight (removes a `std::vector<unique_ptr>` instantiation that pulls `<format>` transitively on libstdc++). | `Vector<T>` with move-only element support; `PushBack`/`EmplaceBack`, move-assign, range-for. |
| `export_perfetto` `vector<FlatEvent>`, `vector<TraceChunk*>`, `vector<uint64>` | **B** | Trivially-copyable / pointer elements; cold path; classic dynamic array. | `Vector<T>`; `PushBack`, `Reserve`, iteration, `std::stable_sort` over `begin/end` (needs contiguous iterators). |
| `export_perfetto` `vector<std::string>` | **D** (external/entangled) | Element type is `std::string`, itself slated for a Ludus `String`. Migrating the outer container without the inner type is low value and risks churn. | Defer until `String` exists; then `Vector<String>`. |
| `file_sink` `vector<std::filesystem::path>` / `vector<Group>` / `vector<Group*>` | **D** + **E** | Entangled with `std::filesystem` (slated for a VFS). Cold retention logic. | Defer to the VFS milestone; `Group*`/`Group` parts could migrate earlier but low value while `path` remains. |
| `diagnostic.cpp` `array<char,512>`, `array<char,16>` | **A**, but **sensitive** | Emergency assertion path; must not gain any dependency, allocation, or header weight. | A `FixedArray`/`Array<char,N>` would fit *only if* it is provably as light and dependency-free as the raw buffer. Otherwise keep as-is. Not a v1 migration target. |
| `logger.cpp` `array<char, kMaxMessageBytes>` | **A** | Stack format buffer. | `Array<char, N>` migrates cleanly if header-light. Low urgency. |
| Logging `std::span<...>` APIs | **not applicable** — keep | Non-owning views; the interop vocabulary. | `Vector`/`Array` must convert *to* `std::span`. |
| Test `std::vector<std::thread>` etc. | **tests** | Not engine code; not bound by STL policy. | Optional, opportunistic. |

**Reading of the inventory.** Only two clusters are clean, high-value `Vector`
migrations: the Logging **sink lists** (Category B, pairs with `UniquePtr`) and
the profiling **exporter's POD/pointer vectors** (Category B). Everything else is
either entangled with a not-yet-replaced facility (String, filesystem) or is a
fixed stack buffer better served by `Array` or left alone. This is a *small,
sequenced* migration, not a sweeping one.

---

## 5. Research / reference review

Principles extracted (not code copied). Sources in §30. Claims labelled by kind.

### 5.1 Standard library baseline

- **[Standard]** `std::array<T,N>` is an aggregate with no indirection; `N==0`
  is allowed and `data()` may return an unspecified non-dereferenceable
  pointer. It is `constexpr`-friendly and trivially copyable iff `T` is.
- **[Standard]** `std::vector<T>` guarantees contiguous storage, amortized O(1)
  `push_back`, and precise iterator/reference invalidation rules (any
  reallocation invalidates all; `push_back` invalidates all iff it reallocates;
  `erase` invalidates from the erase point onward). `data()`/`size()` expose the
  pointer+count needed by C and GPU APIs.
- **[Standard]** Object lifetime in raw storage begins with placement `new` /
  `std::construct_at` and ends with an explicit destructor call /
  `std::destroy_at`. For **trivially copyable** types, copying the object
  representation with `memcpy`/`memmove` is well-defined *today*. For general
  types, moving bytes is UB unless the type is (a language notion not yet
  available in Clang 18) *trivially relocatable*.
- **[Standard]** `std::span<T>` (C++20) is the non-owning contiguous view;
  pointers are `contiguous_iterator` and model `std::ranges::contiguous_range`,
  so exposing `data()`+`size()` and pointer iterators yields full ranges /
  algorithm interoperability at zero abstraction cost.
- **[Standard]** Under `-fno-exceptions`, the default *throwing* `operator new`
  still exists; on allocation failure it will attempt to throw, which with
  exceptions disabled terminates the process. The `std::nothrow` overload
  returns `nullptr` instead. (See §5.4 / §14.)

### 5.2 Production implementations

- **LLVM `SmallVector`** — **[Library]** stores **32-bit** size and capacity
  ("32 bit size would limit the vector to ~4GB") to shrink the object, and
  *splits storage* so the inline-capacity detail is type-erased out of the
  size-agnostic base. Growth doubles. Lesson: **the width of size/capacity is a
  deliberate footprint/perf trade**, and the "impl base + typed derived" split
  is how they get a stable non-templated core. Exception-unsafe by design.
- **Folly `fbvector`** — **[Library]** uses an opt-in `IsRelocatable<T>` trait
  to `memcpy` on reallocation, and (Folly PR #2216) upgrades to
  `std::is_trivially_relocatable` when the P1144 feature-test macro is present.
  Documents the growth-factor trade: any factor > 1 is amortized O(1), too small
  (≈1.1) reallocates too often, too large (3–4) wastes memory. Lesson:
  **relocation is opt-in and trait-driven today; the growth factor is a real
  tunable, not folklore.**
- **EASTL** — **[Library]** engine-oriented `vector`: no exceptions, explicit
  allocators as a template policy, `memcpy` fast paths for POD/relocatable
  types, and `push_back`/`resize` variants that avoid value-initialization.
  Lesson: **an engine container is allowed to make different default choices
  from the STL (allocator visible, exceptions absent) but should stay
  STL-*shaped* enough to interoperate.**
- **Unreal `TArray`** — **[Library]** 32-bit (`int32`) size/capacity, allocator
  policy, `memcpy`-relocatable element assumption for POD, no exceptions.
  Lesson: an entire AAA engine runs on a 32-bit-indexed array; but its API
  diverges from STL and interop suffers. Ludus should *not* copy the divergent
  API surface.
- **Boost.Container / libstdc++ / libc++** — **[Library]** allocator-aware
  vectors; libstdc++ and libc++ grow **2×**, MSVC/Dinkumware **1.5×**. libc++
  `vector<bool>` and small-string-optimization show the cost of cleverness in
  debuggability. Lesson: **default STL vectors are already very good; a custom
  type earns its keep through policy control, not micro-optimization.**
- **LLVM** is actively **[Library]** rewriting vector operations "in terms of
  relocation" for ~50% speedups on reallocation-heavy paths — evidence the
  relocation fast path is worth designing *for*, even if v1 only enables it for
  trivially-copyable types.

### 5.3 Growth-factor evidence

**[Library/Judgment]** The classic argument: with factor 2, the sum of all
previously freed blocks is always strictly less than the next request, so a
general allocator can never coalesce freed blocks to satisfy the next growth;
with a factor below the golden ratio φ≈1.618, earlier freed blocks can be
reused. This favours ≈1.5 for memory locality and allocator reuse; 2× favours
fewer reallocations. Both are amortized O(1). This is a **[Measured]** decision
for Ludus (§15): the design proposes 1.5× as the default hypothesis and gates
the final value on the §22 benchmark.

### 5.4 Exception-free allocation

**[Standard/Judgment]** Because the throwing `operator new` terminates under
`-fno-exceptions` (§5.1), Ludus containers must allocate through a **nothrow**
path and check for `nullptr`, then either (a) report failure in the signature
(the *fallible* API) or (b) treat OOM as unrecoverable via `LUDUS_FATAL` (the
*infallible* default API). This is the crux of the exception-free design (§14,
§18).

---

## 6. Standard-library baseline (what we are measured against)

The bar is not "beat `std::vector`." The bar is: for the operations Ludus uses,
**generate equivalent optimized code, allocate no more often, use no more
memory, and be at least as debuggable** — while adding the Ludus policy
integration the STL cannot provide (exception-free failure, assertion-integrated
bounds, allocator seam, Ludus spellings). A design that is materially *worse*
than `std::vector` on any representative workload (§22) is rejected.

`std::array` is a near-zero-cost aggregate; `Array<T,N>` must match it exactly in
codegen and size, differing only in name, method casing, bounds-check policy,
and `std::span` conversion ergonomics.

---

## 7. Why custom containers (challenging the premise — Phase 4)

Direct answers to the nine questions. Honesty over agreement.

1. **Why custom instead of `std::array`/`std::vector` directly?** The dominant
   reason is **policy control at one seam**, not speed. Ludus has a hard
   `-fno-exceptions` rule and *will* get a custom allocator (ADR 0003 says so
   explicitly). `std::vector`'s allocation, growth, and failure behaviour are
   controlled by the standard library and its allocator model — an allocator
   model Ludus does not want to adopt wholesale (it is verbose, and
   `-fno-exceptions` makes `std::vector`'s `length_error`/`bad_alloc` paths
   terminate rather than report). Owning the container lets Ludus make
   allocation failure explicit, make bounds checks ride the existing build-flavor
   assertion system at zero release cost, and introduce the engine allocator
   later behind a stable API. Secondary reasons: a coherent Ludus vocabulary
   (`Array`/`Vector`/future `FixedVector`/`SmallVector`/`String`) and consistent
   spelling/naming.

2. **Concrete benefits.** (a) Exception-free, signature-level allocation-failure
   handling. (b) Zero-cost release bounds/invariant checking via `LUDUS_ASSERT`,
   rich in Debug/Development. (c) A single insertion point for the future engine
   allocator without touching call sites. (d) `usize`/PascalCase consistency.
   (e) Opt-in `memcpy` relocation fast path controlled by Ludus, upgradable to
   `std::is_trivially_relocatable` later. (f) Reduced public-header include
   weight vs `std::vector<unique_ptr<...>>` on libstdc++ (relevant to the
   existing `backend.hpp` budget override).

3. **Which benefits are measurable?** (b) release codegen size and bounds-check
   cost, (e) reallocation throughput for relocatable types, and (f) header parse
   time are all directly **[Measured]** via §22 and the existing build-budget
   tooling. (a) and (c) are correctness/architecture properties, verified by
   tests and by the absence of terminate-on-OOM, not by a benchmark.

4. **Which advantages are folklore / unlikely to matter?** "Custom containers
   are faster than `std::vector`" is largely folklore for optimized builds — a
   good `std::vector` on trivially-copyable types is already near-optimal, and we
   should *not* promise to beat it. "Always use 2× (or always 1.5×) growth" is
   folklore without measurement. "SBO everywhere is a win" is folklore — SBO
   changes move semantics, pointer stability, and `sizeof`, and is a *different
   type's* job. "32-bit indices are always faster" is folklore; the win is
   footprint, and it costs a 4G-element ceiling and `usize` divergence.

5. **Which engine requirements justify ownership?** The *hard* justifications:
   `-fno-exceptions` (STL vector's error paths terminate), and the *committed*
   future allocator (ADR 0003). The *soft* justifications (naming, taxonomy,
   header weight) would not alone justify the maintenance burden, but they make
   ownership clearly net-positive given the hard ones.

6. **Could thin wrappers do the same with less maintenance?** A wrapper around
   `std::vector` could fix *naming* and *bounds policy*, but it **cannot** fix
   the two hard problems: it still terminates on OOM (the underlying allocation
   still goes through `std::allocator`/throwing `new`), and it still forces the
   `std::allocator` model when the engine allocator arrives — the wrapper would
   have to be rewritten into a real container at that point anyway. A wrapper
   also inherits `std::vector`'s full API and ABI, defeating the "coherent small
   API" goal. **Judgment: a wrapper defers, but does not avoid, the real work,
   and adds a throwaway layer.** Owning a small real container is the lower
   lifetime cost.

7. **Long-term maintenance/correctness burden.** Owning `Vector` means owning
   exception-free reallocation, lifetime correctness for non-trivial types,
   overflow-safe growth, alignment for over-aligned types, and sanitizer/debug
   ergonomics — permanently. This is real and non-trivial. It is mitigated by:
   keeping the API small, leaning on `std::construct_at`/`std::destroy_at` for
   lifetime, restricting `memcpy` to types where it is provably legal, and a
   heavy test/benchmark suite (§21–§22). `Array` is nearly free to maintain.

8. **Is replacing existing STL usage worth the migration cost?** Partly. The
   *whole-repo* replacement is not urgent — the inventory is tiny and several
   uses are entangled. The right posture is: **build the containers now**
   (because they unblock the allocator and set the vocabulary), **migrate the two
   clean clusters** (sink lists, exporter POD vectors) as proof and dogfooding,
   and **defer entangled uses** until their neighbours (String, VFS) are ready.

9. **Uses that should intentionally stay STL-based?** Yes: the emergency
   assertion buffers (§4, unless a `FixedArray` is provably as light), the
   filesystem-entangled `file_sink` vectors (until VFS), the `std::string`-typed
   exporter vector (until `String`), the `std::span` view APIs (kept by design),
   and test-only `std::vector<std::thread>`. See §24.

**Conclusion of Phase 4:** custom containers are justified for Ludus —
*primarily* by the exception policy and the committed allocator, *secondarily* by
vocabulary and header hygiene — provided the design stays small, standards-clean,
and interoperable, and the migration is sequenced rather than sweeping.


---

## 8. Alternatives considered

> **Decision:** Own two purpose-built containers (`Array<T,N>`, `Vector<T>`) in a
> foundation module, standards-clean, exception-free, allocator-seam-ready.
>
> **Alternatives considered:**
>
> - **A0 — Keep using `std::array`/`std::vector`.** Rejected. Under
>   `-fno-exceptions`, `std::vector`'s failure paths terminate rather than
>   report, and the standard allocator model does not match the coming engine
>   allocator (ADR 0003). Zero migration cost now, but leaves the two hard
>   problems unsolved and forces a bigger change later.
> - **A1 — Thin `std::vector` wrapper (rename + bounds policy only).** Rejected as
>   the destination; useful only as a stopgap. Cannot change allocation-failure
>   behaviour or the allocator model without becoming a real container (see
>   §7.6). Inherits full STL API/ABI, contradicting the "small coherent API"
>   goal.
> - **A2 — Adopt EASTL wholesale.** Rejected. Large dependency, its own build and
>   ABI surface, divergent API, and it still needs adaptation to the Ludus
>   assertion/type/allocator systems. Extract its *principles* instead.
> - **A3 — One über-vector with a growth policy + inline-capacity + allocator all
>   as template parameters (EASTL/Boost style).** Rejected for v1. Contaminates
>   the common case with policy machinery, bloats the type, hurts debuggability,
>   and corners the taxonomy. The future family (§26) achieves the same coverage
>   with *separate, honest types*.
> - **A4 — Add small-buffer optimization to `Vector` (Folly/`SmallVector`
>   style).** Rejected for the fundamental dynamic array (explicit prompt
>   constraint, and sound): SBO changes pointer stability, move cost, `sizeof`,
>   and moved-from semantics, and makes the type harder to reason about. It is a
>   *different type* (`SmallVector`, §26).
> - **A5 — 32-bit size/capacity like `SmallVector`/`TArray`.** Rejected as the
>   default for the general `Vector`; it diverges from the `usize` policy (C2) and
>   from `std::vector`/`std::span` interop, for a footprint win that matters most
>   in the *small/fixed* types. Revisited as an option for `FixedVector`/
>   `SmallVector` where element counts are bounded (§26).
>
> **Trade-offs:** Owning two small types costs permanent maintenance (§7.7) and a
> test/benchmark burden (§21–§22), in exchange for solving the two hard problems
> at one seam and establishing a coherent, allocator-ready vocabulary.
>
> **Evidence:** `-fno-exceptions` + throwing `new` terminates **[Standard]**;
> ADR 0003 commits to the allocator and to `Array`/`Span` **[repo]**; SBO and
> 32-bit costs are **[Library]** (Folly, LLVM, Unreal); "std::vector is already
> good" is **[Library/Judgment]**.

---

## 9. Proposed architecture

### 9.1 Placement and module

Create a new foundation module **`FoundationContainers`** (directory
`modules/foundation/containers/`), sibling to `base`, `logging`, `profiling`,
exporting `Ludus::FoundationContainers`.

> **Decision:** New module, header-only for the templates, depending only on
> `Ludus::FoundationBase`.
>
> **Rationale:** The containers depend on `FoundationBase` for types
> (`types.h`), assertions (`assert.hpp`), and compiler macros
> (`compiler.hpp`/`defines.h`). They must sit *above* Base (they use its
> assertion API) and *below* Logging/Profiling/Platform (which will consume
> them). A dedicated module keeps the dependency direction (C5) clean and lets
> the future allocator either live here or in a sibling `FoundationMemory` module
> that Containers depends on.
>
> **Alternatives considered:** (a) Put containers *in* `FoundationBase`.
> Rejected: Base must depend on nothing higher and hosts the sensitive emergency
> assertion path; keeping the container templates out of Base protects Base's
> build weight and dependency purity. (b) A single mixed "core" module. Rejected:
> the repo's convention is one module per concern.
>
> **Trade-offs:** One more module to wire; but it isolates build cost and makes
> the eventual allocator dependency explicit.
>
> **Evidence:** module conventions and dependency rule are **[repo]** (C5,
> `AGENTS.md`, ADR 0002).

Header layout (public):

```
modules/foundation/containers/
  include/ludus/foundation/containers/
    array.hpp        # Array<T,N>
    vector.hpp       # Vector<T>
    relocation.hpp   # IsTriviallyRelocatable trait + LUDUS_TRIVIALLY_RELOCATABLE
    containers.hpp   # umbrella convenience include (optional)
  src/
    vector_support.cpp   # out-of-line cold helpers (growth math, OOM handler)
    internal/
      contiguous_storage.hpp  # shared raw-storage/lifetime helpers (private)
  tests/
    array_tests.cpp
    vector_tests.cpp
    relocation_tests.cpp
    lifetime_type.hpp        # instrumented element type (see §21)
  benchmarks/
    vector_bench.cpp         # (see §22)
```

`FoundationBase` stays Linux/Clang-gated for now (C10); the container module
must compile with the same portable subset so it is ready when other backends
are enabled. No new module should replicate Base's platform hard-error unless it
has a platform-specific reason (it does not).

### 9.2 Shared internals

A private `contiguous_storage.hpp` provides the lifetime/relocation primitives
used by `Vector` (and reusable by future `FixedVector`/`SmallVector`):

- `ConstructAt(ptr, args...)` → `std::construct_at` **[Standard]**.
- `DestroyRange(first, last)` → `std::destroy_at` per element; a no-op (compiled
  away) when `std::is_trivially_destructible_v<T>`.
- `UninitializedRelocate(dst, src, count)` — the central relocation primitive:
  - if `IsTriviallyRelocatable<T>` (which includes trivially copyable): a single
    `memcpy` of `count * sizeof(T)` bytes, then **no** destructor calls
    (relocation consumes the source); legal today for trivially-copyable, and
    for opt-in relocatable types by Ludus contract (§16).
  - else: per-element move-construct into `dst` then `destroy` the source
    (strong-enough behaviour under `-fno-exceptions`, where element moves are
    required `noexcept` — see §16/§18).
- `UninitializedCopy`, `UninitializedMove`, `UninitializedValueConstruct`,
  `UninitializedDefaultConstruct` — the standard building blocks, each with a
  trivial fast path.

These helpers are the *only* place `memcpy`/`memmove` and lifetime calls live, so
the correctness argument is centralized and testable.

---

## 10. Static-array design — `Array<ElementType, Count>`

### 10.1 Purpose

A fixed-size, owning, contiguous aggregate whose element count is a compile-time
constant. It **represents**: a value-semantic bundle of exactly `Count`
elements with no indirection and no allocation. It **deliberately does not
represent**: anything growable, anything with runtime size, or a non-owning view
(that is `std::span`).

### 10.2 Ownership

Owns its `Count` elements inline, by value. Copy/move/destroy are elementwise and
`= default` (aggregate-like), so `Array<T,N>` is trivially copyable / trivially
destructible **iff** `T` is — matching `std::array` **[Standard]**.

### 10.3 Storage model

```
ElementType mData[Count];   // for Count > 0
```

For `Count == 0`, no storage is materialized and `Data()` returns a valid but
non-dereferenceable pointer (see §10.5). The type is an aggregate: no
user-declared constructors, enabling aggregate init `Array<int,3>{1,2,3}` and
`constexpr` use.

> **Decision:** Keep `Array` an aggregate wrapping a C array; do not add a size
> field (it is compile-time `Count`).
>
> **Rationale:** Zero overhead, `constexpr`, structured-binding friendly, exact
> `std::array` parity. `sizeof(Array<T,N>) == sizeof(T[N])` and alignment
> follows `T`.
>
> **Alternatives considered:** A non-aggregate class with explicit constructors —
> rejected: loses aggregate init and adds nothing.
>
> **Trade-offs:** Aggregate-ness means we cannot run a custom bounds-checked
> constructor, but element access checks live in `operator[]` where they belong.
>
> **Evidence:** `std::array` semantics **[Standard]**.

### 10.4 Size / index type

Public size/index type is `usize` (C2). `Size()`/`Count` are `constexpr`
returning `usize`.

### 10.5 Public API (smallest coherent, high-value)

`constexpr`/`noexcept` throughout where possible.

- `Data()` / `Data() const` → `ElementType*` / `const ElementType*`. For
  `Count==0`, returns a non-null but non-dereferenceable pointer (parity with
  `std::array`). **[Standard-aligned]**
- `Size()` / `constexpr usize Size()` (always `Count`); `Empty()` (`Count==0`);
  `static constexpr usize Capacity()` == `Count`.
- `operator[](usize)` / const — **`LUDUS_ASSERT(index < Count)`** in
  Debug/Development, zero cost in Profile/Release (C6).
- `Front()`, `Back()` — asserted non-empty in checked builds.
- `begin()/end()/cbegin()/cend()` — pointer iterators (lowercase for range-for /
  ranges, see §20).
- `Fill(const ElementType&)`, `Swap(Array&)`.
- Conversion to `std::span<ElementType>` / `std::span<const ElementType>`
  (implicit for const view, see §20).
- Comparison: `operator==` / `operator<=>` defaulted (so `Array` is a usable
  value/key type) — **[Standard]** defaulted comparisons.

Not included: `at()` (its whole purpose is throwing; replaced by the asserted
`operator[]` plus an optional `TryGet` returning a pointer/`std::optional` if a
call site needs graceful bounds handling — added only if the inventory demands
it; it does not today).

### 10.6 Semantics

- Complexity: all O(1) except `Fill` (O(N)) and comparisons (O(N)).
- Invalidation: references/iterators are stable for the lifetime of the `Array`
  object (no reallocation exists). Moving an `Array` moves elements in place;
  addresses of a *moved-into* object differ from the source.
- No allocation, ever. `noexcept` throughout.

---

## 11. Dynamic-array design — `Vector<ElementType>`

### 11.1 Purpose

A dynamically sized, owning, heap-backed contiguous array with amortized O(1)
append. It **represents**: an owning, growable, contiguous sequence — the
default choice when the element count is not known at compile time. It
**deliberately does not represent**: a fixed-capacity buffer (future
`FixedVector`), a small-buffer-optimized vector (future `SmallVector`), a
non-owning view (`std::span`), or a node-based/stable-address container.

### 11.2 Ownership

Owns a single heap allocation and the live elements within it. Destructor
destroys `[0, mSize)` and frees the block. Move transfers the block and leaves
the source **empty and valid** (§11.7). Copy performs a fresh allocation +
elementwise copy.

### 11.3 Storage model

> **Decision:** Three-pointer-equivalent representation: **`ElementType* mData;
> usize mSize; usize mCapacity;`**
>
> **Rationale:** This is the clearest, most debuggable representation (a debugger
> shows data/size/capacity directly), matches `std::vector`'s mental model,
> gives O(1) `Size`/`Capacity`/`Data`, and keeps the empty state trivially
> representable (`{nullptr, 0, 0}`). `usize` matches the Ludus type policy (C2)
> and `std::vector`/`std::span` interop, and removes any 4G-element ceiling.
>
> **Alternatives considered:**
> - *Pointer + two `uint32`* (LLVM `SmallVector`/UE `TArray` style) **[Library]**.
>   Smaller object (16 bytes vs 24 on 64-bit) and cheaper size math, but caps at
>   4G elements, diverges from `usize`/`std::span`, and complicates
>   conversion/interop. **Rejected for the general `Vector`; reconsider for
>   `FixedVector`/`SmallVector`** where counts are bounded (§26).
> - *Three pointers* (`begin/end/capacity_end`, libc++/libstdc++ internal style).
>   Equivalent; makes `end()` a load rather than an add. Neutral; the
>   size/capacity form is more debugger-legible and matches how the rest of Ludus
>   reasons about sizes. **Judgment:** prefer explicit `mSize`/`mCapacity`.
>
> **Trade-offs:** 24-byte object on 64-bit (vs a 16-byte 32-bit design). Accepted:
> footprint of the *container object* is rarely the bottleneck for a general
> dynamic array, and the debuggability/interop/consistency wins dominate.
>
> **Evidence:** `usize` policy **[repo]** (C2); 32-bit width is a footprint trade
> **[Library]** (SmallVector RFC, `TArray`).

### 11.4 Size / capacity / index types

`usize` throughout the public API and the representation. Internal growth math
uses `usize` with explicit overflow checks (§15). `isize` for iterator
differences.

### 11.5 Public API (smallest coherent, high-value — not the full `std::vector`)

Grouped; `[[nodiscard]]` on observers; `noexcept` where the operation cannot
fail. **Two failure modes** are offered where allocation is involved: an
*infallible* default (OOM → `LUDUS_FATAL`, matching how the rest of the engine
treats unrecoverable infrastructure failure) and a *fallible* `Try*` variant
returning `bool` for the rare caller that must degrade gracefully. (§18 justifies
this split.)

**Construction / lifetime**
- `Vector() noexcept` — empty, no allocation.
- `explicit Vector(usize count)` — `count` value-initialized elements.
- `Vector(usize count, const ElementType&)` — `count` copies.
- `Vector(const Vector&)` / `Vector& operator=(const Vector&)` — deep copy.
- `Vector(Vector&&) noexcept` / `Vector& operator=(Vector&&) noexcept` — steal.
- `~Vector()` — destroy live elements, free.
- `Array(std::initializer_list<ElementType>)` — copy elements into owned storage;
  requires a copy-constructible element type (see §12).
- Range/span construction: `static Array FromRange(std::span<const ElementType>)`.

**Capacity**
- `Size()`, `Capacity()`, `Empty()`, `Data()`/const.
- `Reserve(usize)` / `bool TryReserve(usize)` — ensure capacity ≥ n; reallocates
  at most once; no-op if already sufficient.
- `Resize(usize)` / `Resize(usize, const ElementType&)` and `TryResize` — grow
  (value- or copy-init new elements) or shrink (destroy tail).
- `ShrinkToFit()` — reallocate down to `mSize` (may be a no-op; never grows).
- `Clear() noexcept` — destroy elements, keep capacity, `mSize = 0`.

**Element access**
- `operator[](usize)` / const — `LUDUS_ASSERT(index < mSize)` (C6).
- `Front()`, `Back()` — asserted non-empty.

**Modifiers**
- `PushBack(const ElementType&)` / `PushBack(ElementType&&)` and
  `bool TryPushBack(...)`.
- `template<class... Args> ElementType& EmplaceBack(Args&&...)` and
  `TryEmplaceBack`.
- `PopBack() noexcept` — asserted non-empty; destroy last.
- `Append(std::span<const ElementType>)` / range append, and `TryAppend` — bulk
  insert at end (single capacity check + relocation).
- `Insert(usize index, ...)` / `Erase(usize index)` / `EraseRange(first,last)` —
  order-preserving; **plus** an explicit `EraseUnordered(index)` (swap-with-last
  + pop) as the engine-preferred O(1) removal when order is irrelevant (this is a
  frequent game-engine idiom; naming it prevents the accidental O(N) `erase`).
- `Swap(Vector&) noexcept`.

**Uninitialized / advanced (opt-in, clearly named)**
- `AppendUninitialized(usize count)` → returns a `std::span<ElementType>` over
  the newly reserved-but-**value-initialized** region for trivially-constructible
  types only (constrained by a concept), to serve GPU staging / bulk-fill
  workloads that fill immediately. Named to make the "you must fill this" contract
  explicit; guarded so it cannot be used to create objects whose lifetime is not
  begun. (§16 explains why this is safe only for the constrained set.)

**Interop**
- Conversion to `std::span<ElementType>` / `std::span<const ElementType>`;
  `begin()/end()` pointer iterators (§20).

Deliberately **excluded** from v1: allocator-parameterized constructors (added
with the allocator, §14), `assign`, `insert` overload zoo, `emplace` at arbitrary
position beyond `Insert`, `resize`-with-default-init subtleties beyond the two
forms above, reverse iterators (add if a call site needs them), `data()`/`at()`
STL spellings beyond the interop aliases. The rule (§12): **add an API member
only when a real Ludus call site needs it or it is broadly, obviously useful —
not to mechanically mirror `std::vector`.**

### 11.6 Semantics — complexity

- `PushBack`/`EmplaceBack`: amortized O(1), worst-case O(n) on reallocation.
- `PopBack`, `Back`, `operator[]`, `Data`, `Size`, `Capacity`, `EraseUnordered`:
  O(1).
- `Insert`/`Erase`/`EraseRange` (ordered): O(n) shift.
- `Reserve`/`Resize`/`ShrinkToFit`/copy: O(n).
- `Append(span)`: O(k) for k appended, amortized.

### 11.7 Semantics — moved-from and empty-state guarantees

- A moved-from `Vector` is left **empty and valid**: `{nullptr, 0, 0}`. It may be
  reused (assigned to, pushed into) or destroyed. This is stronger than the
  standard's "valid but unspecified" and is a **[Judgment]** we adopt because it
  is cheap, easy to reason about, and easy to debug.
- A default-constructed and a cleared `Vector` never own an allocation
  (`Capacity()` may be > 0 after `Clear`, 0 after default construction / move).

---

## 12. API specification — migration-compatibility mapping

The API in §10–§11 is justified against the actual inventory (§3–§4). For each
STL dependency the inventory relies on, we state the decision per the prompt's
four options: (1) support an equivalent because broadly useful; (2) rewrite the
call site into a better Ludus-native form; (3) use an adapter/span; (4) keep the
boundary STL-based.

| STL feature relied on | Where | Decision | Ludus form |
|---|---|---|---|
| `push_back(unique_ptr)` / move-only element | logging sink lists | **(1)** support | `PushBack(UniquePtr&&)` / `EmplaceBack(...)`; `Vector` supports move-only elements |
| `vector(&&)` move, `operator=(&&)` | `backend.Start(vector&&)` | **(1)** support | move ctor/assign; `Start(Vector<UniquePtr<ILogSink>>&&)` |
| range-for over sinks | backend worker | **(1)** support | pointer iterators + range-for |
| `push_back`, `reserve` over POD/pointer | exporter | **(1)** support | `PushBack`, `Reserve` |
| `begin()/end()` for `std::stable_sort` | exporter sort | **(3)** adapter via std | pointer iterators are `contiguous_iterator`; `std::stable_sort(v.begin(), v.end(), cmp)` works unchanged (§20) |
| `.empty()`, `.front()`, `.size()`, `.push_back`/`.pop_back` on the open-scope stacks | exporter | **(1)** support / **(2)** rewrite | `Empty()`, `Back()`, `PopBack()`; the parallel `openTicks`/`openNames` stacks are a candidate to **rewrite** into a single `Vector<OpenScope>` (better Ludus-native form) |
| `vector<std::string>` | exporter | **(4)** keep until `String` | keep STL element until Ludus `String` exists |
| `vector<std::filesystem::path>` etc. | file_sink | **(4)** keep until VFS | keep STL until VFS milestone |
| `std::array<char,N>` stack buffer | logger, diagnostic | **(1)**/**(4)** | `Array<char,N>` where header-light; **keep** in the emergency assertion path unless proven equally light |
| `std::span<...>` params | logging format APIs | **keep** | not a container; `Vector`/`Array` convert *to* `std::span` |

`initializer_list`: supported by `Array` for explicitly requested brace
initialization, including lists of designated aggregate elements. The constructor
copies the list's const elements into one exact-size allocation; an empty list
does not allocate. It uses the existing fatal-on-OOM/overflow constructor policy.
The array owns its copies and does not retain the list's backing storage.

Use `Array<int32>{3}` for one element with value 3, `Array<int32>(3)` for three
value-initialized elements, and `Array<int32>(3, 7)` for three copies of 7. List
construction requires copyable elements; use `Add`/`AddInPlace` for move-only
elements. `FromRange(std::span<const T>)` remains available for contiguous views.

This mapping is the guard against API bloat: every member in §11.5 traces to a
real need or a broadly-useful core operation, and the entangled/STL-shaped
features are explicitly deferred or kept rather than mechanically reproduced.


---

## 13. Memory / lifetime model

**[Standard]-grounded rules the implementation must obey:**

- Storage for `Vector` is raw, untyped memory obtained from the allocation seam
  (§14). Object lifetime begins with `std::construct_at` (placement `new`) and
  ends with `std::destroy_at` (explicit destructor call). `Vector` never
  default-constructs `T` into the whole capacity; only `[0, mSize)` are live
  objects.
- Element access (`operator[]`, iterators, `Data`) returns pointers/references to
  live objects only within `[0, mSize)`. Accessing `[mSize, mCapacity)` is a
  precondition violation, caught by `LUDUS_ASSERT` in checked builds.
- Reallocation moves live objects from the old block to the new block via
  `UninitializedRelocate` (§9.2), then frees the old block. After a successful
  reallocation, **all** pointers, references, and iterators into the vector are
  invalidated (§19).
- For non-trivially-destructible `T`, shrinking (`Resize` down, `PopBack`,
  `Clear`, `Erase`) calls `std::destroy_at` on the removed elements before the
  size is reduced.
- `std::launder` is applied where the standard requires it (reusing storage that
  previously held a different-typed object); centralized in
  `contiguous_storage.hpp` so the reasoning is in one place.
- Over-aligned `T`: the allocation seam must honour `alignof(T)`. §14 requires the
  seam to accept alignment; v1 uses the aligned `operator new`
  (`::operator new(bytes, std::align_val_t{alignof(T)}, std::nothrow)`)
  **[Standard]** so over-aligned element types (e.g. SIMD-aligned math structs,
  GPU-upload structs) are correct on all targets.
- Empty base / empty element types: `Array<Empty, N>` and `Vector<Empty>` must
  compile and behave; `[[no_unique_address]]` is used for any stateless policy
  members (e.g. a future allocator) so empty policies cost zero bytes (C3,
  mirrors `UniquePtr`'s `[[no_unique_address]] mDeleter`).

**Constexpr:** `Array` is fully `constexpr`. `Vector` is designed so a
`constexpr` implementation is *possible* later (allocation via
`std::allocator`-style constexpr path is intricate under the no-exceptions/custom
seam), but v1 does **not** promise `constexpr Vector`; it is an explicit
non-goal (§27) to avoid contorting the runtime design for compile-time use that
no call site needs yet.

---

## 14. Allocation strategy

> **Decision:** v1 allocates through a **single internal seam** — a tiny pair of
> functions `AllocateBytes(usize bytes, usize alignment) noexcept` /
> `FreeBytes(void*, usize bytes, usize alignment) noexcept` — implemented today
> in terms of the C++ **nothrow, alignment-aware** `operator new`/`operator
> delete`. There is **no** allocator template parameter, no allocator object,
> and no allocator framework in v1.
>
> **Rationale:**
> - The nothrow path lets the container **check for `nullptr`** and handle OOM
>   explicitly, instead of the default throwing `new` that terminates under
>   `-fno-exceptions` **[Standard]** (§5.4). This is the core exception-free
>   requirement (C1).
> - A single seam is exactly the insertion point ADR 0003 anticipates: when the
>   engine allocator lands, `AllocateBytes`/`FreeBytes` are re-pointed at it (or
>   an allocator handle is threaded through), **without changing any container
>   call site**.
> - No allocator template parameter keeps `sizeof(Vector)` minimal, keeps the
>   type simple and debuggable, avoids allocator-propagation complexity, and
>   avoids template bloat (C8) — none of which any current call site needs.
>
> **Alternatives considered:**
> - *STL allocator model (`Allocator` template param + `allocator_traits`).*
>   Rejected for v1: verbose, propagation rules are a well-known footgun, and it
>   presumes a model the engine allocator may not match. Can be layered later if
>   ever needed, but the seam is enough.
> - *`std::pmr`-style polymorphic allocator handle stored in the vector.*
>   Deferred: attractive when the engine allocator exists (per-vector arena/tag),
>   but premature now; adds a pointer to every vector for zero present benefit.
>   The seam is forward-compatible with adding an optional memory-resource handle
>   later.
> - *Raw `malloc`/`free`.* Rejected: does not honour over-alignment portably and
>   bypasses any future `new`-based interposer/tooling (the repo already uses
>   `--wrap=malloc`/`new` interposition in assertion allocation tests; going
>   through `operator new` keeps those hooks meaningful).
>
> **Trade-offs:** No per-vector allocator now means workloads that will
> eventually want arena/tag allocation must wait for the allocator milestone; but
> the seam guarantees they will not need a call-site rewrite. OOM policy (fatal
> vs fallible) is handled at the API layer (§18), not the seam.
>
> **Evidence:** throwing-`new` termination under `-fno-exceptions` **[Standard]**;
> ADR 0003 commitment to a Ludus allocator **[repo]**; alignment-aware `operator
> new` **[Standard]**; existing allocation interposition tests **[repo]**.

**Allocation-failure behaviour (v1):**
- *Infallible API* (`PushBack`, `Reserve`, `Resize`, ...): on `nullptr` from the
  seam, call `LUDUS_FATAL("Ludus::Vector allocation failed")`. Rationale: for
  the overwhelming majority of engine code, heap exhaustion is unrecoverable and
  should fail fast with a diagnostic, consistent with how the engine treats
  infrastructure failure.
- *Fallible API* (`TryPushBack`, `TryReserve`, `TryResize`, `TryAppend`): return
  `false` and leave the container unchanged (strong guarantee, §18) for callers
  that must degrade (e.g. streaming/asset paths that can drop or retry).

---

## 15. Growth strategy

> **Decision (proposed, benchmark-gated — REVISED to 2× after measurement; see
> §32.2):** geometric growth, overflow-checked, never shrinking on growth, with a
> small **minimum first capacity**. The design proposed 1.5× as a hypothesis; the
> §22/§32.4 measurement selected **2×** (≈40% fewer reallocations, ≈30% fewer
> bytes copied, negligible peak-memory difference). The paragraphs below are the
> original 1.5× reasoning, preserved; §32.2 records why 2× won.
>
> **Rationale:**
> - Any factor > 1 gives amortized O(1) append **[Standard/Library]**.
> - A factor below the golden ratio (φ≈1.618) lets a general allocator reuse
>   previously freed blocks, improving locality and reducing fragmentation
>   **[Library]** (Folly/Dinkumware reasoning). 1.5× is the widely used
>   memory-friendly point (MSVC/Dinkumware).
> - Integer-only `oldCap + oldCap/2` avoids floating point and is cheap.
>
> **Alternatives considered:**
> - *2×* **[Library]** (libstdc++/libc++). Fewer reallocations (fewer relocations
>   of non-trivial elements), but higher peak memory and no freed-block reuse.
>   Better when relocation is expensive (non-relocatable element types) or append
>   counts are huge; worse for memory. Kept as the leading alternative; the
>   benchmark (§22, "repeated growth", "push N POD/non-trivial") decides.
> - *Golden-ratio-ish (≈1.6)* — marginal; not worth the odd constant vs a clean
>   1.5×.
> - *Adaptive/size-class-aware growth* — deferred to the allocator milestone,
>   where the allocator's size classes are known; premature now.
>
> **Growth mechanics (all builds):**
> - First growth from capacity 0 jumps to a floor (proposed: `max(1,
>   ceil(kMinBytes / sizeof(T)))` with a small `kMinBytes`, so tiny elements do
>   not reallocate on the first few pushes and large elements start at 1). Exact
>   floor is **[Measured]**.
> - `Reserve(n)`/`Append`/`Resize` that request more than the geometric next step
>   allocate exactly the requested amount (no over-allocation beyond the request),
>   matching `std::vector` intuition and avoiding surprise memory use.
> - Growth math checks `usize` overflow: a requested capacity that overflows
>   `usize` or exceeds a `MaxSize()` (= `PTRDIFF_MAX / sizeof(T)` analogue) is a
>   fatal error in the infallible API / `false` in the fallible API — never a
>   silent wrap. This is the exception-free replacement for `std::length_error`.
>
> **Trade-offs:** 1.5× reallocates slightly more often than 2× (more relocations)
> in exchange for lower peak memory and allocator reuse. For trivially-relocatable
> elements (the common engine case) relocation is a cheap `memcpy`, so the extra
> reallocations are inexpensive — which is precisely why 1.5× is the proposed
> default. For expensive-to-move element types, 2× might win; the benchmark
> settles it, and the factor is a single named constant so revising it is trivial.
>
> **Evidence:** amortization **[Standard]**; factor trade-offs and the φ argument
> **[Library]** (Folly, StackOverflow analyses); relocation-cost dependence
> **[Measured]** (to be produced by §22).

---

## 16. Type-specific optimizations — classified

The prompt's required classification. **Never require UB for performance.**

### 16.1 Portable and standards-compliant *now* (ship in v1)

- **Trivially destructible `T`:** skip destructor loops entirely
  (`if constexpr (std::is_trivially_destructible_v<T>)` → the loop compiles
  away). **[Standard]**
- **Trivially copyable `T`:** relocation and range copy via `memcpy`/`memmove`.
  Copying the object representation of a trivially copyable type is well-defined
  **[Standard]**; `memmove` for overlapping ordered `Insert`/`Erase` shifts is
  the correct primitive. libstdc++/libc++/MSVC all do this **[Library]**.
- **Ludus opt-in relocation trait `IsTriviallyRelocatable<T>`:** default
  `std::is_trivially_copyable_v<T>`; a type author can opt in for a move-safe,
  no-internal-pointer type via `LUDUS_TRIVIALLY_RELOCATABLE(Type)` (a
  specialization macro), exactly like Folly's `IsRelocatable` **[Library]**. When
  true, reallocation uses `memcpy` and omits source destructors. This is the
  central engine win (POD-heavy and move-cheap types). Correctness rests on the
  *contract* the author asserts (the type is bitwise-relocatable); it is not
  inferred unsafely.
- **`noexcept`-move requirement:** for the non-relocatable reallocation path,
  `Vector` requires `T`'s move constructor to be `noexcept` (or falls back to
  copy). Under `-fno-exceptions` element moves cannot throw anyway, but requiring
  `noexcept` (via a `static_assert`/concept) documents intent and matches the
  strong-guarantee reasoning (§18). **[Standard/Judgment]**

### 16.2 Potentially useful *later* (design leaves room; do not ship in v1)

- **`std::is_trivially_relocatable` (P1144/P2786) fast path:** when a supported
  toolchain defines the feature-test macro, make `IsTriviallyRelocatable<T>`
  default to it, widening the `memcpy` set automatically **[Standard-future]**
  (Folly PR #2216 pattern **[Library]**). Gated behind the macro so Clang 18
  behaviour is unchanged.
- **`realloc`-based growth for trivially-relocatable types** once the engine
  allocator exposes a `Reallocate` that can grow in place — avoids copying when
  the block can extend. Deferred to the allocator milestone.
- **`constexpr Vector`** — see §13; possible, not promised.

### 16.3 Premature (explicitly *not* now)

- Small-buffer optimization (belongs to `SmallVector`, §26 — prompt constraint).
- 32-bit size/capacity for the general `Vector` (§11.3).
- Allocator template parameter / propagation (§14).
- SIMD-accelerated bulk copy beyond what `memcpy` already yields.

### 16.4 Unsafe / UB (never)

- `memcpy`-relocating a **non**-trivially-copyable type that has **not** opted
  into `IsTriviallyRelocatable` (e.g. a type with self-referential pointers, or
  whose move has side effects). UB; forbidden. The trait default is the
  conservative `is_trivially_copyable_v`, so the unsafe path is *impossible to
  reach by accident* — it requires an explicit, auditable opt-in.
- `reinterpret_cast`-based type punning of storage without `std::launder` where
  the standard requires it.
- Treating `AppendUninitialized` storage as live objects for non-trivial types
  (hence its concept constraint to trivially-constructible types, §11.5).

---

## 17. Debugging strategy

Intentional per-flavor semantics, riding the existing build-flavor system (C6)
so **Release/Profile pay nothing** and Debug/Development are richly checked.

| Facility | Debug (ID1) | Development (ID2) | Profile/Release (ID3/4) |
|---|---|---|---|
| Bounds checks (`operator[]`, `Front`/`Back`, `PopBack` non-empty, `Insert`/`Erase` index) | `LUDUS_ASSERT` (live, resumable, rich site) | `LUDUS_ASSERT` (live) | compiled out (zero cost) |
| Invariants (`mSize ≤ mCapacity`, `mData != nullptr` iff `mCapacity > 0`) checked on mutation | `LUDUS_ASSERT` | `LUDUS_ASSERT` | out |
| OOM / overflow (infallible API) | `LUDUS_FATAL` (always) | `LUDUS_FATAL` | `LUDUS_FATAL` (always live) |
| Memory poisoning of freed/relinquished storage | fill freed block + moved-from elements with a poison pattern before free | optional | none |
| Debug iterator checking (detect use-after-invalidation) | optional checked-iterator wrapper (see below) | off | off (raw pointer iterators) |
| Sanitizer cooperation | see §21 | — | — |

- **Bounds/invariant checks use `LUDUS_ASSERT`** (not `LUDUS_CHECK`/`REQUIRE`):
  precondition violations are programmer errors; they should be loud in
  dev/debug and *free* in shipping, which is exactly `LUDUS_ASSERT`'s contract
  (C6). OOM and capacity-overflow in the infallible API are unrecoverable and use
  `LUDUS_FATAL`.
- **Poisoning:** in Debug, overwrite freed element storage (and moved-from source
  bytes) with a recognizable pattern (e.g. `0xDD`) so use-after-free reads are
  obvious in a debugger; this is *in addition to* ASan, which already catches the
  access. Zero cost outside Debug.
- **Debug iterators:** raw pointer iterators are the default (best codegen,
  ranges-compatible). An *optional* checked iterator (generation counter on the
  vector; iterator stores container + generation, asserts on deref/compare after
  invalidation) can be enabled in Debug only. Proposed as a follow-up, not v1
  core, to keep the first implementation small — but the storage model reserves
  the design room (a generation counter can be added behind
  `LUDUS_ENABLE_ASSERTS` without ABI concern since the engine is static-linked,
  ADR 0002). Listed as an open question (§28).
- **ASan is first-class:** because storage is a single `operator new` block and
  freed storage is released promptly, ASan detects OOB and UAF on the container
  buffer directly. The design avoids "cache the whole capacity as live objects"
  tricks that would blind ASan.
- **Debugger legibility:** the `{mData, mSize, mCapacity}` layout (§11.3) is
  trivially inspectable; a natvis / pretty-printer is a cheap follow-up (§28).

---

## 18. Error / exception semantics

**No exceptions anywhere (C1).** Failure taxonomy:

- **Precondition violations** (index out of range, `Back()`/`PopBack()` on empty,
  bad `Insert` position): programmer errors → `LUDUS_ASSERT` (checked builds),
  UB-if-disabled *by contract* in shipping (documented; same contract as
  `std::vector::operator[]`). We do **not** pay for these in Release.
- **Allocation failure / capacity overflow:**
  - *Infallible API* → `LUDUS_FATAL` (fail fast, always live). This is the
    exception-free analogue of `std::bad_alloc`/`std::length_error` that
    `std::vector` would throw (and which would terminate under `-fno-exceptions`
    anyway) — but with a Ludus diagnostic and a defined, auditable site.
  - *Fallible `Try*` API* → returns `false`, container **unchanged**.
- **Exception guarantees (adapted to a no-throw world):**
  - `Try*` operations provide the **strong guarantee**: on failure the container
    is exactly as before (achieved by allocating the new block and fully
    populating it *before* releasing the old one; the old state is only replaced
    on success).
  - Infallible operations either succeed or terminate; there is no partial-failure
    observable state.
  - `PopBack`, `Clear`, `Size`, `Data`, `Swap`, move operations are `noexcept`
    and cannot fail.
- **Element operations:** if a `T` copy/move/constructor could fail, it must do so
  without exceptions (engine types cannot throw). `Vector` requires `noexcept`
  move for the fast reallocation path (§16.1). If an element *copy* (in the copy
  constructor / `Resize`-with-value) has side effects that can leave `T` in a bad
  state, that is `T`'s concern; `Vector` guarantees it will not leak or
  double-destroy: it tracks exactly how many elements were constructed and
  destroys precisely those on teardown.

This makes failure part of the signature (`Try*` returns `bool`) or fatal and
diagnosable (`LUDUS_FATAL`), exactly as `AGENTS.md` mandates.

---

## 19. Iterator / reference invalidation rules

Documented as a hard contract (matching `std::vector` intuition so migration is
safe, with Ludus-specific notes):

- **`Array<T,N>`:** iterators/references never invalidate for the lifetime of the
  object. Moving the `Array` object does not preserve addresses across the move
  (elements live inline).
- **`Vector<T>`:**
  - Any operation that **reallocates** (`Reserve`/`Resize`/`ShrinkToFit`/growth in
    `PushBack`/`EmplaceBack`/`Append`, copy-assign) invalidates **all** iterators,
    pointers, and references.
  - `PushBack`/`EmplaceBack`/`Append` invalidate all **iff** they reallocate
    (i.e. `Size()` would exceed `Capacity()`); otherwise only `end()` moves.
  - `PopBack` invalidates only the removed element and `end()`.
  - Ordered `Insert`/`Erase`/`EraseRange` invalidate iterators/references at and
    after the modification point (and all of them if `Insert` reallocates).
  - **`EraseUnordered`** invalidates the erased slot and the last element (which
    moves into the slot) and `end()`. This differs from `std::vector::erase` and
    is documented prominently because it is an intentional Ludus idiom.
  - `Swap` invalidates nothing (pointers follow the storage to the other object;
    end iterators are conceptually swapped).
  - `Clear` invalidates all references (elements destroyed) but not the capacity.
- **Debug detection:** the optional checked-iterator (§17) turns
  use-after-invalidation from UB into a `LUDUS_ASSERT` failure in Debug.

---

## 20. Standard interoperability

Owning Ludus containers must not isolate Ludus from the ecosystem (explicit
goal). Concretely:

- **`std::span` conversion.** Both containers convert to `std::span<T>` and
  `std::span<const T>`. `const`-view conversion is implicit (safe, cheap);
  mutable-view conversion is via an explicit `AsSpan()` / implicit as decided in
  review (implicit is ergonomic and matches how `std::vector` interoperates via
  `std::span`'s range constructor). This is the primary bridge to the existing
  logging `std::span` APIs and to GPU/C APIs (`span.data()`, `span.size()`).
- **Contiguous iterators / ranges.** `begin()/end()` return **raw pointers**,
  which are `std::contiguous_iterator` and make the container a
  `std::ranges::contiguous_range` **[Standard]**. Therefore `std::sort`,
  `std::stable_sort` (used by the exporter), `std::find`, ranges views, and
  range-for all work with no adapter. Lowercase `begin`/`end` (and `data`/`size`
  free-function support via ADL / the members) are provided **specifically** for
  this interop, alongside the PascalCase `Data`/`Size` used by Ludus code. This
  is the one sanctioned lowercase-alias concession (C3).
- **C / GPU APIs (pointer + count).** `Data()` + `Size()` is exactly the
  Vulkan/Metal upload idiom (`pCode`/`codeSize`, buffer `contents` + `length`).
  No copy or adapter needed. `AppendUninitialized` (§11.5) targets the
  staging-buffer fill pattern.
- **Structured bindings** work for `Array` (aggregate) via `std::tuple_size` /
  `std::tuple_element` / `get` specializations if we choose to provide them
  (optional, low priority; `std::array` supports it — add only if a call site
  wants it).
- **What we do *not* do:** we do not implement full `std::vector`-compatible
  allocator traits, `std::erase`/`std::erase_if` free-function overloads
  (provide Ludus `Erase`/`EraseUnordered` members instead), or reverse iterators
  in v1 (add on demand). We do not chase `constexpr` container interop (§13).

---


## 21. Testing strategy

Tests use Catch2 v3 (test exceptions re-enabled per `ludus_enable_test_exceptions`),
run under the Debug, Development, and ASan/UBSan presets, plus a
zero-allocation check reusing the existing allocation-interposer pattern
(`--wrap=malloc/calloc/realloc`, and the `LD_PRELOAD` interposer) already present
for assertions.

### 21.1 Instrumented element types (test scaffolding)

A `tests/lifetime_type.hpp` providing element types that make lifetime bugs
observable:

- **`LifetimeTracked`** — a global/thread-local counter set tracking
  construct/copy/move/destroy counts and live-instance count; every test asserts
  the ledger balances (constructions == destructions, no leaks, no
  double-destroy). Each instance carries a canary/id to detect
  use-after-move/use-after-free.
- **`MoveOnly`** — deleted copy; verifies move-only element support (the sink-list
  case: `UniquePtr` is move-only).
- **`NonTrivial`** — user dtor + heap member; forces the non-relocatable path and
  ASan/leak detection.
- **`ThrowingLikeError`** — since we cannot throw, a type whose copy can *report
  failure by leaving a flag*; used to test the strong-guarantee `Try*` paths and
  precise destroy-count on partial fills. (No real exceptions; validates the
  no-throw error model.)
- **`Relocatable`** — a type that opts into `LUDUS_TRIVIALLY_RELOCATABLE`;
  verifies the `memcpy` fast path is taken (via a static-assert probe / counting
  hook) and correctness after relocation.
- **`OverAligned`** — `alignas(64)` (cache line) and `alignas(128)`; verifies the
  aligned allocation seam; ASan/UBSan alignment checks.
- **`Empty`** — stateless type; verifies empty-type handling and (with a policy)
  `[[no_unique_address]]`.

### 21.2 Coverage matrix (minimum)

Per container, across trivial scalar / POD struct / non-trivial / move-only /
over-aligned / empty element types:

- **Sizes:** zero-size, one element, many elements, exactly-at-capacity,
  one-past-capacity (forces growth), large N.
- **Growth:** repeated `PushBack` triggering multiple reallocations; verify
  capacity sequence follows the chosen factor; verify element values and order
  preserved; verify no leaks across every reallocation.
- **Capacity ops:** `Reserve` (grow, no-op, shrink-request ignored), `Resize`
  up/down, `ShrinkToFit`, `Clear` (capacity retained, elements destroyed).
- **Modifiers:** `PushBack`/`EmplaceBack` (lvalue/rvalue), `PopBack`,
  `Insert`/`Erase`/`EraseRange` at begin/mid/end boundaries, `EraseUnordered`,
  `Append(span)` empty/one/many, self-append aliasing guard.
- **Copy/move:** copy ctor/assign (deep, independent), move ctor/assign
  (moved-from is empty+valid), self-copy-assign and self-move-assign safety,
  `Swap`.
- **Access:** `operator[]`, `Front`/`Back`, `Data`, iteration (range-for,
  explicit iterators), const-correctness.
- **Lifetime:** ledger balances in every test; destruction of a non-empty vector;
  destruction after moves; exact destroy count after partial-fill failure.
- **Aliasing:** `Insert`/`Append` where source overlaps the vector.
- **Boundaries:** capacity/overflow — a `Reserve`/`Resize` that exceeds `MaxSize`
  triggers `LUDUS_FATAL` (death test) / `Try*` returns `false` (unchanged).
- **Interop:** `std::span` round-trip; `std::stable_sort`/`std::find` over
  `begin/end`; `std::ranges` view; pointer+count handed to a fake "GPU upload"
  sink.
- **Failure model:** `Try*` OOM simulation (interposer forces `nullptr`) →
  `false` + strong guarantee; infallible OOM → death test hitting `LUDUS_FATAL`.

### 21.3 Sanitizers, leaks, and allocation

- All tests pass under **ASan + UBSan** (existing preset) and, where relevant,
  **TSan** is out of scope (containers are not thread-safe by contract — a
  documented non-goal, §27; concurrent use is the caller's responsibility, as
  with `std::vector`).
- **Leak checks:** the `LifetimeTracked` ledger + ASan leak detector.
- **Zero-allocation assertions:** using the interposer, assert that `Array` never
  allocates, that a default-constructed / `Reserve(0)` `Vector` never allocates,
  that `Clear` does not free/allocate, and that `Reserve(n)` then N `PushBack`s
  performs exactly **one** allocation.
- **Relocation-path assertion:** a test that a `memcpy` (not per-element move) is
  used for a relocatable/trivially-copyable type — verified via a move-counting
  element that must observe **zero** moves across a reallocation.

### 21.4 `Array` specifics

Aggregate init, `constexpr` use (static_assert on a `constexpr` `Array`),
structured/`std::span` conversion, `Fill`, comparison operators, `Count==0` edge.

---

## 22. Benchmark strategy

Primary comparison: **`Ludus::Vector` vs `std::vector`** and **`Ludus::Array` vs
`std::array`**, on the `linux-clang-release` and `linux-clang-development`
presets with the pinned Clang 18. Goal is **not** to beat `std::vector`; it is to
prove we are not materially worse and to settle the §15 growth factor with data.

### 22.1 Workloads (representative)

- Construction (empty; `Reserve(N)`; `Vector(N)`).
- Push N `uint32` (trivial scalar).
- Push N POD structs (e.g. a 32-byte transform-like struct — trivially copyable).
- Push N non-trivial objects (heap-owning element — non-relocatable path).
- Push N move-only (`UniquePtr`-like) — the sink-list workload.
- `Reserve` + append vs unreserved append (measures reallocation cost).
- Repeated growth from empty (measures growth-factor behaviour: allocation count,
  peak capacity, bytes).
- Iteration (sum/transform — should be identical codegen to `std::vector`).
- Random access.
- Bulk `Append(span)` of N elements.
- `Insert`/`Erase` at front/mid; `EraseUnordered`.
- Copy whole; move whole; destruction of large vector.

### 22.2 Metrics

- Wall-clock runtime (Catch2/nanobench-style or a small harness).
- **Allocation count and total bytes** (via the interposer) — the honest measure
  of growth policy.
- **Peak capacity** reached for a given push sequence (growth factor validation).
- `sizeof(Vector<T>)`, `sizeof(Array<T,N>)` (footprint parity/deltas).
- **Generated code for tiny common ops** — inspect assembly for `operator[]`,
  `PushBack` fast path (no reallocation), `Size`, `Data`, iteration loop; compare
  to `std::vector`. Expectation **[Measured]**: identical or fewer instructions in
  Release; `operator[]` bounds check fully elided in Release.
- Debug vs Release delta (confirm checks vanish in Release/Profile).
- **Header parse time** via the existing `./scripts/profile-build` +
  build-budget tooling — confirm `vector.hpp`/`array.hpp` stay within the default
  2000 ms budget and that migrating `backend.hpp` off `std::vector<unique_ptr>`
  does not regress (ideally improves) its parse time.

### 22.3 Acceptance gates

- No representative workload is materially slower than `std::vector` in Release.
- Trivially-copyable reallocation performs a single `memcpy` (zero element moves).
- Allocation count for `Reserve(N)`+N pushes is exactly 1.
- `Array` codegen and `sizeof` match `std::array`.
- Container headers within build budget.
- The growth-factor decision (§15) is recorded with the measured allocation
  count / peak-bytes / runtime table (1.5× vs 2× on the POD and non-trivial push
  workloads).

---

## 23. Codebase-wide migration strategy

Principles: **migrate by subsystem, keep every stage green** (builds
warning-clean, unit tests pass, ASan/UBSan clean, format/tidy pass — the
`AGENTS.md` "ready for review" bar), **prove with the clean clusters first**, and
**defer entangled uses**. No giant blind text replacement. Each stage is a
separate PR.

### 23.1 Stages (ordered)

**Stage 0 — Build the containers (no migration).**
Implement `Array`, `Vector`, `relocation.hpp`, tests (§21), benchmarks (§22) in
`FoundationContainers`. Land behind its own module; nothing else depends on it
yet. Green by construction. *Risk: none to existing code.*

**Stage 1 — Dogfood: logging sink lists (Category B, clean).**
Migrate `logger.cpp` `Sinks` and `backend.{hpp,cpp}` `mSinks` /
`Start(...)` from `std::vector<std::unique_ptr<ILogSink>>` to
`Vector<UniquePtr<ILogSink>>` (pairs with the existing Ludus `UniquePtr`).
- Requires: `Vector` move-only element support, `PushBack`/`EmplaceBack`, move
  ctor/assign, range-for, `Size`/`Empty`.
- Mechanical? **Mostly**, but touches a public-ish internal header
  (`backend.hpp`) and its build-budget override — must re-measure header parse
  time (§22.2) and *tighten/remove the override if it improves*.
- Manual refactor value: none beyond the type swap.
- *Risk: low-medium* (worker ownership semantics; covered by existing logging
  regression tests + ASan).

**Stage 2 — Profiling exporter POD/pointer vectors (Category B, clean).**
Migrate `export_perfetto.cpp` `vector<FlatEvent>`, `vector<TraceChunk*>`,
`vector<uint64>` to `Vector<...>`. Consider **rewriting** the parallel
`openTicks`/`openNames` stacks into one `Vector<OpenScope>` (option (2),
Ludus-native improvement) — *but* `openNames` holds `std::string`, so this
rewrite is only clean once `String` exists; until then, keep those two as STL and
migrate only the POD/pointer vectors.
- Requires: `PushBack`, `Reserve`, `Empty`, `Back`, `PopBack`, `begin/end` for
  `std::stable_sort`, `std::span` conversion.
- Mechanical? **Yes** for the POD/pointer vectors (sort works unchanged via
  contiguous iterators).
- *Risk: low* (cold path; export tests cover it).

**Stage 3 — `Array` for stack format buffers (Category A, non-sensitive).**
Migrate `logger.cpp` `std::array<char, kMaxMessageBytes>` to `Array<char, N>`
*iff* §22 confirms `array.hpp` is header-light and codegen-identical.
- *Risk: low.* Skip if it adds any header weight to the logging hot path.

**Stage 4 (deferred, gated on other milestones):**
- `export_perfetto.cpp` `vector<std::string>` → `Vector<String>` — **after
  `String` exists**.
- `file_sink.cpp` `vector<std::filesystem::path>` / `Group` / `Group*` — **after
  the VFS layer** replaces `std::filesystem`.
- Emergency assertion buffers in `diagnostic.cpp` — **only** if a `FixedArray`
  is proven equally light and dependency-free; otherwise **keep STL permanently**
  (this path must not gain dependencies).
- Test `std::vector<std::thread>` — optional/opportunistic; not required.

### 23.2 Per-usage migration table

| # | Site | Current | Proposed | Required API | Mechanical? | Manual preferred? | Boundary exception? | Risk | Stage |
|---|---|---|---|---|---|---|---|---|---|
| 1 | `logger.cpp` `Sinks` | `vector<unique_ptr>` | `Vector<UniquePtr>` | move-only, PushBack/EmplaceBack, range-for | yes | no | no | Low-Med | 1 |
| 2 | `backend.{hpp,cpp}` `mSinks`, `Start(&&)` | `vector<unique_ptr>` | `Vector<UniquePtr>` | move, move-only, iterate | mostly | re-measure header budget | no | Med | 1 |
| 3 | `export_perfetto` `vector<FlatEvent>` | `vector<POD>` | `Vector<FlatEvent>` | PushBack, Reserve, iter | yes | no | no | Low | 2 |
| 4 | `export_perfetto` `vector<TraceChunk*>` | `vector<ptr>` | `Vector<TraceChunk*>` | PushBack, range-for | yes | no | no | Low | 2 |
| 5 | `export_perfetto` `vector<uint64> openTicks` | `vector<uint64>` | `Vector<uint64>` (or fold into `Vector<OpenScope>`) | PushBack/PopBack/Back/Empty | yes | maybe rewrite w/ #6 | no | Low | 2 |
| 6 | `export_perfetto` `vector<std::string> openNames` | `vector<string>` | `Vector<String>` | — | no | wait for `String` | **yes (until String)** | Low | 4 |
| 7 | `logger.cpp` `array<char,N>` | `std::array` | `Array<char,N>` | Data/Size/span | yes | only if header-light | maybe | Low | 3 |
| 8 | `diagnostic.cpp` `array<char,512/16>` | `std::array` | keep / maybe `FixedArray` | — | no | keep | **yes (emergency path)** | — | 4/never |
| 9 | `file_sink.cpp` `vector<path>`, `Group`, `Group*` | `vector` | `Vector` after VFS | — | no | wait for VFS | **yes (until VFS)** | Low | 4 |
| 10 | logging/profiling tests `vector<thread>` | `vector` | optional | — | n/a | optional | tests exempt | — | opportunistic |
| 11 | logging format APIs `std::span<...>` | `span` | **keep** | — | n/a | n/a | **kept by design** | — | never |

### 23.3 Where the API must NOT imitate STL merely to ease conversion

- **`erase`:** provide `Erase` (ordered, O(n)) *and* the explicitly-named
  `EraseUnordered` (O(1)); do **not** silently make `Erase` swap-and-pop, and do
  **not** add `std::erase_if` free functions just because they exist. The exporter
  and future engine code should reach for `EraseUnordered` when order is
  irrelevant — naming it prevents accidental O(n).
- **`at()`:** not provided (its reason for existing is to throw). Bounds safety is
  the asserted `operator[]`; graceful bounds handling, if ever needed, is an
  explicit `TryGet`.
- **`initializer_list` ctor:** not added preemptively (§12).
- **Allocator constructors / `get_allocator`:** not added until the allocator
  exists (§14).
- The parallel-stack idiom in the exporter (`openTicks` + `openNames`) is an
  invitation to a **Ludus-native rewrite** (one `Vector` of a small struct), not
  a reason to mirror two `std::vector`s verbatim.

---

## 24. Explicit STL-boundary exceptions

> **Superseded by §34.3/§34.4.** This section listed container-level exceptions
> (the `file_sink` filesystem vectors and the test `std::vector<std::thread>`
> pools) that have since been migrated to `Ludus::Array`. `std::vector` is now
> **prohibited** in all first-party code, engine and tests alike; the only
> permitted `std::vector`/`std::array` uses are the benchmark comparison
> baseline and third-party/vendor code (§34.3). The rows below are corrected to
> the current state.

Cases that correctly **remain STL-based**, each with a concrete reason. These
are not container migrations that were skipped; they are either non-container
facilities or hard architectural boundaries.

| Exception | Reason | Revisit when |
|---|---|---|
| `std::span<...>` in the logging format APIs and as the container view/interop type | Non-owning view; the sanctioned ecosystem vocabulary; replacing it weakens ranges/GPU/C interop and is out of scope | never (kept by design) |
| `std::string` **element** in `export_perfetto`/`file_sink` (the container is `Ludus::Array`) | The *element type* is `std::string`, slated for a Ludus `String`; the owning container has already migrated | element migrates when `String` lands |
| `std::filesystem::path` **element** in `file_sink` (the container is `Ludus::Array`) | The *element type* is entangled with `std::filesystem`, slated for the VFS layer; the owning container has already migrated | element migrates when the VFS layer lands |
| `std::array<char,N>` in the **emergency assertion path** (`diagnostic.cpp`) | Must stay allocation-free, dependency-free, and header-light; `FoundationBase` cannot depend on `FoundationContainers` (circular), and the path must not run `LUDUS_ASSERT`. It is `std::array`, not `std::vector` | only if a dependency-free `StaticArray` variant is proven equally light; otherwise never |
| `std::vector`/`std::array` in `array_bench.cpp` | The comparison baseline; benchmarking against the STL is the entire purpose. Not in the engine build graph | never (kept by design) |
| `std::stable_sort`/`std::find`/ranges over Ludus containers | Algorithms are explicitly out of scope; Ludus containers interoperate *with* them via contiguous iterators | not a replacement target |

The default is absolute: **all first-party code (engine and tests) uses Ludus
containers, never `std::vector`.** The only `std::vector` permitted anywhere in
the repository is the benchmark baseline and third-party/vendor code.

---

## 25. (folded into §12/§20 — API + interop specification)

See §11.5 (API), §12 (migration-compatibility mapping), §20 (standard interop).

---

## 26. Future container taxonomy

Proposed so current naming does not corner the architecture. **Not designed here;
sketched to reserve names and semantics.**

| Type (proposed name) | Category | One-line purpose | Distinct from `Vector` how |
|---|---|---|---|
| `Array<T, N>` | static/fixed array | compile-time-sized inline owning array | no allocation, size is a type parameter |
| `Vector<T>` | dynamic array | growable heap-backed owning array | this document |
| `FixedVector<T, N>` (a.k.a. static/inline-capacity) | fixed-capacity vector | runtime size ≤ compile-time capacity `N`, **no heap** | never allocates; `PushBack` past `N` is a fatal/`Try` failure; directly serves the `TraceChunk`/bounded-buffer idiom (§3.5) |
| `SmallVector<T, N>` | small/inline vector | inline storage for ≤ N, spills to heap beyond | SBO changes pointer stability & `sizeof`; a *separate* type, never a mode of `Vector` (prompt) |
| `Span<T>` → **use `std::span`** | view | non-owning contiguous view | not owning; kept as `std::span` (do not build a competitor) |
| `String` / `StringView` | string | owning UTF-8 string / view | ADR 0003; backed by the allocator; `StringView` may alias `std::string_view` |
| flat/open-addressed hash map, flat set | hash / flat | cache-friendly associative | ADR 0003 ("flat/open-addressed hash map"); separate design |

Naming coherence: static things are `Array`/`FixedVector`, the growable default is
`Vector`, inline-optimized is `SmallVector`, views are `std::span`. `FixedVector`
and `SmallVector` are the natural homes for the **32-bit size/capacity** and
**inline-storage** ideas rejected for the general `Vector` (§11.3, §16.3), and
they can share `contiguous_storage.hpp` (§9.2). This taxonomy is explicitly *not*
committed by v1 beyond reserving the names and confirming `Vector` must not absorb
their roles.

---

## 27. Non-goals

- **Not** a replacement for `std::span`, algorithms, ranges, iterators, smart
  pointers (`UniquePtr` already exists), strings, or hash containers. Those are
  separate decisions (prompt).
- **Not** thread-safe. Concurrent mutation is the caller's responsibility, as
  with `std::vector`. (The lock-free structures in profiling/logging are
  purpose-built and stay as they are.)
- **No** small-buffer optimization in `Vector` (that is `SmallVector`).
- **No** allocator framework / allocator template parameter in v1 (single seam
  only, §14).
- **No** `constexpr Vector` guarantee in v1 (§13).
- **No** 32-bit size/capacity for the general `Vector` (§11.3).
- **No** *view* replacement: `std::span` stays by design (§24). Owning
  containers, however, have been fully migrated — no first-party `std::vector`
  remains outside the benchmark baseline (§34.3). Element types that are still
  STL facilities (`std::string`, `std::filesystem::path`, `std::thread`) migrate
  with their own subsystems, not by keeping the container as `std::vector`.
- **No** exceptions, RTTI dependence, or heavy-header inclusion in public
  container headers.
- **Not** a `std::vector` API clone — only the small, high-value surface in §11.5.

---

## 28. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Relocation fast path applied to a type that is not actually bitwise-relocatable (author mis-opts-in `LUDUS_TRIVIALLY_RELOCATABLE`) | High (UB) | Default trait is conservative (`is_trivially_copyable`); opt-in is explicit and auditable; relocation tests with `LifetimeTracked`/canaries; ASan; document the contract loudly |
| Lifetime bugs (double-destroy / leak / use-after-free) in the non-trivial path | High | `std::construct_at`/`std::destroy_at` only; centralized `contiguous_storage.hpp`; `LifetimeTracked` ledger in every test; ASan/UBSan; poisoning in Debug |
| Integer overflow in growth math → tiny allocation + OOB writes | High | Overflow-checked growth; `MaxSize` cap; fatal/`Try`-false on overflow; boundary tests |
| OOM behaviour surprises callers (fatal vs fallible) | Medium | Two clearly-named API families; documented; death tests for the fatal path, interposer tests for the fallible path |
| Header/build-time regression from a template in a public header | Medium | Minimal includes; out-of-line cold helpers; measured against build budget (§22.2); the `backend.hpp` migration is a chance to *improve* it |
| Growth factor chosen wrong for real workloads | Medium | Decision is benchmark-gated (§15/§22); factor is one named constant, trivially revised |
| Maintenance burden of owning a container forever | Medium | Small API; heavy tests; principles-not-clone approach; `Array` is nearly free |
| Cross-platform correctness (alignment, `operator new` semantics) before non-Linux backends are enabled | Medium | Portable subset only (C10); aligned nothrow `operator new`; no compiler-specific tricks outside `compiler.hpp`; revisit when Windows/macOS backends land |
| Debug-iterator absence lets use-after-invalidation stay UB in Debug | Low-Medium | ASan catches most; optional checked-iterator reserved as a follow-up (§17/§28 open question) |
| Divergent `EraseUnordered` semantics surprise a reader expecting `std::vector::erase` | Low | Distinct name; prominent invalidation docs (§19) |

---

## 29. Open questions (need decisions / your approval)

1. **Growth factor: 1.5× (proposed) vs 2×.** Final value gated on §22 benchmark.
   Do you want 1.5× as the default hypothesis, or start from 2× (fewer
   reallocations, matches libstdc++/libc++)?
2. **Size/capacity width: `usize` (proposed) vs 32-bit for `Vector`.** Proposed
   `usize` for policy consistency and interop; a 32-bit variant is reserved for
   `FixedVector`/`SmallVector`. Confirm `usize` for the general `Vector`.
3. **OOM policy default: infallible-fatal (`LUDUS_FATAL`) + fallible `Try*`
   (proposed).** Confirm this dual model, or prefer fallible-only / fatal-only.
4. **Module placement: new `FoundationContainers` module (proposed) vs folding
   into `FoundationBase`.** Confirm a new module.
5. **Checked (debug) iterators in v1 or as a follow-up?** Proposed follow-up
   (raw-pointer iterators in v1). Confirm.
6. **`AppendUninitialized` in v1?** Useful for GPU staging but adds a
   sharp-edged API; proposed to include it constrained to trivially-constructible
   types. Include now or defer until a real GPU-upload call site exists?
7. **Should this design also produce an ADR (0007) recording the "own the
   containers / single allocation seam" decision**, alongside this architecture
   doc? (The repo pairs big decisions with ADRs.)
8. **Naming: `Vector`/`Array` (proposed, matches ADR 0003 wording and STL mental
   model) — confirm** vs alternatives (`DynArray`, `List`, `TArray`-style). ADR
   0003 already says "Ludus `Array`/`Span`", which argues for `Array`/`Vector`.

---

## 30. Implementation plan

Sequenced, each step green and reviewable (matches §23 stages):

1. **ADR + review.** (If §29.7 approved) write ADR 0007 recording the decision;
   get this design reviewed; resolve §29.
2. **Module scaffold.** `modules/foundation/containers/` with CMake
   (`Ludus::FoundationContainers`, depends on `Ludus::FoundationBase`), install
   rules, test target (Catch2 + `ludus_enable_test_exceptions`), ASan/UBSan wiring,
   build-budget entry.
3. **`relocation.hpp`.** `IsTriviallyRelocatable` trait +
   `LUDUS_TRIVIALLY_RELOCATABLE` macro + tests (including the future feature-test
   upgrade path behind a macro).
4. **`Array<T,N>`.** Implement + full test/bench; confirm `std::array` parity
   (codegen, `sizeof`, constexpr). Lowest risk, validates conventions.
5. **`contiguous_storage.hpp`** (private) — lifetime/relocation primitives +
   focused tests with `LifetimeTracked`/`OverAligned`/`Relocatable`.
6. **`Vector<T>`.** Implement the §11.5 API on top of the seam and storage
   helpers; out-of-line cold helpers in `vector_support.cpp`; full §21 tests; §22
   benchmarks; **record the growth-factor decision with data**.
7. **Verify gates.** Warning-clean, unit tests, ASan/UBSan, format/tidy,
   build-budget — the `AGENTS.md` bar — on the pinned toolchain.
8. **Stage 1 migration** (logging sink lists) + re-measure `backend.hpp` budget.
9. **Stage 2 migration** (profiling exporter POD/pointer vectors).
10. **Stage 3** (`Array` for the logging format buffer, if header-light).
11. **Document deferred stages** (String/VFS-gated) as tracked follow-ups; do not
    execute until their milestones.

No step lands unless the whole engine still builds warning-clean and all tests
(incl. ASan/UBSan) pass.

---

## 31. References

Grouped by claim type. Attribution provided; content paraphrased/summarized for
licensing compliance (no source quoted beyond brief identifiers). *Content was
rephrased for compliance with licensing restrictions.*

**Repository (authoritative for Ludus constraints):**
- `AGENTS.md`, `.kiro/steering/coding-standards.md` — coding standards, no
  exceptions, types, naming.
- `docs/decisions/0002-static-library-default.md`,
  `0003-standard-library-usage-policy.md` (commits to Ludus `Array`/`Span` +
  allocator), `0004-logging-format-type-erasure.md`,
  `0005-build-time-budgets.md`, `0006-resumable-development-assertions.md`.
- `cmake/EngineOptions.cmake`, `EngineBuildFlavor.cmake`,
  `EngineCompilerWarnings.cmake`, `EngineSanitizers.cmake`, `assert_config.hpp.in`,
  `config/build_budget.json`.
- `modules/foundation/base/include/ludus/foundation/base/`:
  `types.h`, `assert.hpp`, `pointer.hpp` (`UniquePtr`), `compiler.hpp`,
  `defines.h`.
- `modules/foundation/profiling/src/internal/trace_chunk.hpp` (fixed-capacity
  idiom evidence).

**Standard C++ [Standard]:**
- cppreference: `std::array`, `std::vector`, `std::span`, `std::construct_at`,
  `std::destroy_at`, `std::launder`, `contiguous_iterator`,
  `std::ranges::contiguous_range`, `operator new` (`std::nothrow`,
  `std::align_val_t`), defaulted comparisons.
- WG21 P1144 (`is_trivially_relocatable`, O'Dwyer):
  https://open-std.org/jtc1/sc22/wg21/docs/papers/2024/p1144r12.html
- WG21 P2786 (Trivial Relocatability for C++26, Bloomberg):
  https://open-std.org/jtc1/sc22/wg21/docs/papers/2025/p2786r13.html
- Arthur O'Dwyer, "STL algorithms for trivial relocation" (relocation ≈ memmove
  for trivially-relocatable):
  https://quuxplusone.github.io/blog/2023/03/03/relocate-algorithm-design/

**Production implementations [Library]:**
- LLVM `SmallVector` (32-bit size/capacity rationale; split storage):
  https://www.llvm.org/docs/doxygen/SmallVector_8h_source.html ; RFC "Should
  SmallVectors be smaller?" (footprint trade); LLVM issue #129328 (vector ops in
  terms of relocation, ~50% speedups):
  https://github.com/llvm/llvm-project/issues/129328
- Folly `IsRelocatable`/`fbvector` (opt-in memcpy relocation; growth factor
  discussion): https://github.com/facebook/folly/blob/main/folly/docs/Traits.md ,
  https://github.com/facebook/folly/blob/main/folly/docs/FBVector.md ; Folly PR
  #2216 (use `std::is_trivially_relocatable` when P1144 macro present):
  https://github.com/facebook/folly/pull/2216
- EASTL `vector` (engine container principles: no-exceptions, allocator policy,
  POD memcpy fast paths) — EA Standard Template Library documentation.
- Unreal Engine `TArray` (32-bit `int32` size, allocator policy, POD relocation).
- Growth-factor survey (libstdc++/libc++ 2×, MSVC/Dinkumware 1.5×; golden-ratio
  reuse argument): StackOverflow answers 20481237 and 27672798.
- `-fno-exceptions` + `operator new` termination vs `nothrow` returns null:
  StackOverflow 6049563 / 21605092.

*All external sources were used for principles and trade-offs only; no
implementation code was copied.*


---

## 32. Implementation & migration results (post-implementation addendum)

> Status update: the containers designed above were **implemented, tested,
> benchmarked, and the Ludus-owned codebase was migrated** onto them. This
> section records what was actually built, the one design decision that changed
> under measurement, and the migration outcome. It supersedes the "not yet
> implemented" banner at the top for everything it covers.

### 32.1 What was built

Module `modules/foundation/containers/` (`Ludus::FoundationContainers`, depends
only on `Ludus::FoundationBase`):

- `include/ludus/foundation/containers/relocation.hpp` — `IsTriviallyRelocatable`
  trait (default = `std::is_trivially_copyable_v`), `TriviallyRelocatable`
  concept, `LUDUS_TRIVIALLY_RELOCATABLE(Type)` opt-in macro.
- `include/ludus/foundation/containers/array.hpp` — `Array<T, N>` (+ `Array<T,0>`
  specialization).
- `include/ludus/foundation/containers/vector.hpp` — `Vector<T>`.
- `include/ludus/foundation/containers/detail/contiguous_storage.hpp` —
  lifetime/relocation primitives (installed detail header; the header template
  needs it at instantiation). Placed under `detail/` rather than `internal/`
  because the SDK-install check forbids installing headers whose path contains
  `internal`.
- `include/ludus/foundation/containers/containers.hpp` — umbrella.
- `src/vector_support.cpp` — out-of-line allocation seam, growth math, OOM/overflow
  fatal sinks.
- `tests/` — `lifetime_type.hpp` (instrumented element types), `array_tests.cpp`,
  `vector_tests.cpp`, `relocation_tests.cpp`, `allocation_tests.cpp`.
- `benchmarks/vector_bench.cpp` — comparison harness vs `std::vector`/`std::array`.

The API matches §11.5 / §10.5 as designed. All lifetime work goes through
`std::construct_at`/`std::destroy_at`; the `memcpy` relocation fast path is
taken only for `IsTriviallyRelocatable` types and guarded against
constant-evaluation.

### 32.2 Design decision that changed under measurement — growth factor

> Decision (revised): **2× geometric growth**, not the 1.5× hypothesis proposed
> in §15.
>
> Rationale: §15 explicitly made the factor benchmark-gated. A reallocation/
> byte-count simulation (100k pushes, element sizes 4 B and 32 B) and the runtime
> benchmark (§32.4) showed 2× performs **~40% fewer reallocations** and **~30%
> fewer total bytes copied** than 1.5×, with negligible peak-memory difference,
> and matches libstdc++/libc++. The 1.5×-for-allocator-block-reuse argument
> depends on an allocator Ludus does not yet have; the extra reallocation cost of
> 1.5× is real today. The factor is a single named constant
> (`kGrowthNum`/`kGrowthDen` in `vector_support.cpp`) and is trivial to revisit
> when the engine allocator lands.
>
> Evidence: measured, recorded in §32.4. This resolves design open question #1.

All other design decisions were implemented as specified. Open questions #2
(`usize` size type), #3 (dual fatal + `Try*` OOM), #4 (new module), #8
(`Vector`/`Array` names) were implemented as proposed. #5 (checked debug
iterators) and #6 (`AppendUninitialized`) were deliberately **not** implemented
(kept as future work, §27 non-goals honored); #7 (ADR 0007) is left to the
reviewer.

### 32.3 Migration results

Toolchain: pinned **Clang 18.1.8**, libstdc++, LLD, CMake/Ninja/Conan, Catch2 3.

**`std::vector` call sites migrated (engine):** 6 owning uses across 3 files →
`Ludus::Vector`:
- `logging/src/logger.cpp` — sink list (`Vector<UniquePtr<ILogSink>>`).
- `logging/src/internal/backend.{hpp,cpp}` — worker-owned sink list + `Start()`
  parameter.
- `profiling/src/export_perfetto.cpp` — `Vector<FlatEvent>`, `Vector<TraceChunk*>`
  (plus a semantic cleanup: two parallel dead-value stacks collapsed to one
  `usize openDepth` counter, which removed a `std::vector<std::string>` entirely).

**`std::array` call sites migrated (engine):** 1 → `Ludus::Array`:
- `logging/src/logger.cpp` — `Array<char, kMaxMessageBytes>` producer format
  buffer.

**Test dogfood:** `profiling/tests/scope_tests.cpp` drain buffers
(`Vector<TraceEvent>`, `Vector<TraceChunk*>`).

**Modules affected:** FoundationLogging, FoundationProfiling (+ new
FoundationContainers). FoundationBase unchanged (correctly — see exceptions).

**Notable API cleanups during migration:**
- Introduced a `MakeSink<T>()` helper wrapping `new`-ed sinks in `UniquePtr`
  (the sink list is now `Vector<UniquePtr<ILogSink>>`, no `std::unique_ptr`).
- Collapsed the exporter's two parallel `push`/`pop` stacks (whose stored values
  were never read) into a single depth counter — clearer and dependency-lighter.

### 32.4 Benchmark results (Clang 18, `-O2`, ns/element, best of 7)

| Workload | Ludus | std | ratio |
|---|---:|---:|---:|
| push N uint32 (unreserved) | 2.25 | 1.96 | 1.15 |
| reserve + push N uint32 | 0.63 | 0.54 | 1.16 |
| push N POD32 (unreserved) | 18.21 | 17.77 | 1.03 |
| push N non-trivial (std::string) | 10.31 | 9.82 | 1.05 |
| iterate + sum | 0.10 | 0.13 | 0.75 |
| random access | 0.76 | 0.76 | 1.00 |
| bulk append (span) | 0.08 | 0.08 | 1.01 |
| copy N POD32 | 1.97 | 1.95 | 1.01 |

`sizeof(Vector<int>) == sizeof(std::vector<int>) == 24`;
`sizeof(Array<int,16>) == sizeof(std::array<int,16>) == 64`.

Reading: at or near parity on every workload except **small-scalar `push_back`,
which is ~15% slower**. Root cause (confirmed by inspecting `-O2` assembly): the
debuggable `{data,size,capacity}` representation writes `mSize` back to memory
each push and reloads it for the capacity compare, because the out-of-line
`GrowForOne` call forces `mSize` to be memory-current; libstdc++ keeps its
end-pointer in a register. This is an inherent, ~0.09 ns/element cost of the
representation chosen in §11.3 for debuggability; it is reported, not hidden, and
was judged not worth trading debuggability for. Single-call `PushBack` codegen is
otherwise identical to `std::vector` (load size, compare capacity, store,
increment).

### 32.5 Verification performed

- **Container unit tests** (Debug): 42 cases / 3178 assertions pass.
- **Allocation tests** (Debug, counting `operator new`/`delete`): 6 cases pass —
  default `Vector` and `Array` never allocate; `Reserve(n)`+n pushes = exactly 1
  allocation; `Clear` frees/allocates nothing; growth is O(log n) reallocations;
  alloc/free symmetric.
- **Relocation tests**: trivially-relocatable/POD elements reallocate via `memcpy`
  with **zero** element move-constructions; non-relocatable elements use
  move+destroy.
- **ASan + UBSan** (`detect_leaks=1`, `halt_on_error=1`): container tests + full
  migrated logging + full migrated profiling suites pass with zero UB/leak/
  lifetime findings.
- **Full engine build matrix:** Debug (17/17 ctest), Release (whole engine +
  smoke app compile/link clean, asserts compiled out), ASan+UBSan (14/14 ctest;
  the alloc test and sanitizer-gated death tests are excluded by design), and a
  `-Werror` (CI-equivalent) build of the container + migrated targets — all clean.

### 32.6 Remaining intentional STL exceptions (post-migration)

> **Superseded by §34.3/§34.4.** This table reflected an earlier, partial
> migration that still deferred the `file_sink` retention containers and the
> test thread pools. Those have since been migrated to `Ludus::Array`; the
> element types (`std::filesystem::path`, `std::string`, `std::thread`) stay STL
> on their own replacement tracks while the *containers* are Ludus-owned. The
> current, authoritative exception list is §34.4.

Only two categories now remain (see §34.4 for the full statement):

| Location | Kept STL | Reason |
|---|---|---|
| `foundation/base/src/diagnostic.cpp` | `std::array<char,512>` ×2, `std::array<char,16>`, `<array>` | Emergency assertion path in FoundationBase. Dependency direction forbids Base depending on FoundationContainers, and the emergency path must be dependency-free and must not run `LUDUS_ASSERT` (which `Array::operator[]` uses). Genuine hard boundary — and it is `std::array`, not `std::vector`. |
| `containers/benchmarks/array_bench.cpp` | `std::vector`/`std::array`, `<vector>`/`<array>` | The comparison baseline; using the STL types is the point. Not in the engine build graph. |

`std::string` in `export_perfetto.cpp`/`file_sink.cpp` is an element/facility
(slated for a Ludus `String`, ADR 0003), not a container, and is out of scope
for the container-migration policy.

No first-party `std::vector` remains outside the benchmark baseline. No
undocumented owning-STL-container usage remains.


---

## 33. Independent audit corrections (post-implementation)

An independent adversarial audit of the implementation and migration was
performed. Summary of what it found and what changed; §33 is authoritative where
it overlaps earlier sections.

### 33.1 P0 fixed — self-referential insertion was undefined behaviour

The original `EmplaceBack`/`PushBack`/`EmplaceAt`/`Insert` reallocated (or
shifted) **before** materializing the new element from its arguments. When an
argument aliased an existing element — e.g. `v.PushBack(v.Back())`,
`v.EmplaceAt(i, v[j])`, `v.Insert(i, v[k])` — the referenced element was freed
(reallocation) or moved (shift) before it was read. Consequences, both confirmed
under ASan/UBSan:

- **Heap-use-after-free** when the operation reallocated.
- **Silent wrong value** when a shift moved the referenced element first.

`std::vector` guarantees these self-referential forms are well-defined, so this
was also a latent migration hazard (any future `v.PushBack(v.Back())` — a very
common idiom — would have been UB). No *existing* migrated call site triggered
it (all push fresh/temporary values), so it was latent, not an active crash.

**Fix.** The new element is now materialized from its arguments **before** any
reallocation or shift:
- `EmplaceBack`/`TryEmplaceBack`: on the (cold) grow branch, construct a local
  from the arguments, then grow, then move the local into place. The hot
  (no-grow) path is unchanged and still constructs in place. The local lives only
  in the cold branch, so the common append path pays nothing (verified by
  codegen and benchmark — see §33.3).
- `EmplaceAt`/`Insert`: construct a local from the arguments before shifting or
  growing, then move it into the opened gap.

The earlier code comment on `EmplaceAt` claiming self-references were "supported"
was false and has been corrected.

### 33.2 Cost of the fix — bounded extra moves on the grow path

For a **non-trivially-copyable but opt-in-relocatable** element pushed by value,
the grow path now performs one extra move-construction of the *pushed* element
per reallocation (the local → slot move). This is O(log n) total moves across a
full fill, **not** per element, and does not affect the existing elements (which
are still relocated by `memcpy`). For trivially-copyable elements the "move" is a
trivial copy and is elided/negligible. The relocation regression test was
tightened accordingly: it asserts existing-element relocation performs no
per-element moves (bulk moves bounded by the reallocation count), and a second
test confirms the reserved (no-grow) path performs zero moves.

### 33.3 Performance re-verified after the fix

The append hot path is unchanged; benchmark (Clang 18, −O2, Release assert
config) after the fix matches the pre-fix numbers: reserved push ≈1.13×,
unreserved push ≈1.18×, POD/non-trivial/iterate/random-access/bulk-append/copy at
or near parity — i.e. the §32.4 profile holds. (An interim version of the fix
that forwarded arguments to an out-of-line grow helper regressed reserved push to
~3.4× by forcing a per-iteration argument spill in the hot loop; that approach
was rejected in favour of the cold-branch local above.)

### 33.4 Doc drift fixed

The `vector.hpp` header comment still described "1.5x" growth; corrected to 2×
(the shipped value; §15/§32.2).

### 33.5 Audit items confirmed clean (no change needed)

Copy/move assignment (incl. self-assignment and capacity reuse), moved-from
state, `EraseRange`/`Erase`/`EraseUnordered` lifetime, `Append` from another
vector across reallocation, over-aligned allocation, empty/zero-size handling,
`EraseRange(0,0)` on an empty (null-data) vector (no null-pointer arithmetic),
overflow/`MaxSize` defenses, `std::span` conversions, and the exception-free
allocation-failure model were all exercised adversarially under ASan/UBSan and
found correct. The two remaining engine STL-container usages (`diagnostic.cpp`
emergency path — blocked by the `FoundationBase`→`FoundationContainers`
dependency direction; `file_sink.cpp` — entangled with `std::filesystem`, pending
the VFS layer) were re-verified as genuine, not convenience.


---

## 34. Ludus-native taxonomy and API (authoritative naming)

An architectural correction moved the containers off STL-mimicking names onto
Ludus's own conventions. This section is authoritative for naming; earlier
sections describe the same designs under their original working names.

### 34.1 Type taxonomy

> **Decision:** two owning contiguous types, named so the *storage discipline* is
> obvious at a glance, with `Vector` deliberately reserved for mathematics.
>
> | Concept | Name | Was |
> |---|---|---|
> | fixed-size (compile-time count) contiguous owning storage | `ludus::foundation::StaticArray<T, N>` | `Array<T, N>` |
> | dynamically-sized contiguous owning storage | `ludus::foundation::Array<T>` | `Vector<T>` |
> | mathematical vector | *(reserved — future `Vec2`/`Vec3`/`Vec4`)* | — |
>
> **Rationale.** `Vector` is the single most overloaded word in a rendering/
> physics engine (position, direction, velocity, `Vec3`…); using it for a
> growable byte container guarantees a lifetime of "which vector?" ambiguity.
> Reserving `Vector` for math and naming the growable sequence `Array` removes
> that collision. `Array<T>` reads naturally as "an array of T" and is the common
> case, so it earns the short name; `StaticArray<T, N>` makes the fixed,
> compile-time size explicit at every use. This is a coherent long-term taxonomy
> (the future `FixedVector`/`SmallVector` in §26 become `FixedArray`/
> `SmallArray`, keeping "Array" as the sequence family and "Vector" as math).
>
> **Trade-offs.** `Array<T>` (dynamic) reads slightly less "obviously heap" than
> `Vector<T>` would to an STL native; mitigated by the taxonomy being consistent
> and documented, and by `StaticArray` making the fixed sibling unmistakable.

### 34.2 Member API — verb-oriented, not STL spellings

Ludus functions begin with a verb/verb-phrase (`GetX`, `IsX`, `EnsureX`,
`AddX`, `RemoveX`), a convention already established across the engine
(`GetCurrentThreadId`, `IsInitialized`, `HasPending`, `SetGlobalLevel`). The
container API now follows it. STL spellings are used only where they are
*independently* the clearest Ludus choice and are needed for ecosystem interop
(`operator[]`, `begin/end`, `data/size`, `std::span` conversion, `operator==`).

| Old (STL-ish) | Ludus | Notes |
|---|---|---|
| `Size()` | `GetSize()` | |
| `Capacity()` | `GetCapacity()` | |
| `Empty()` | `IsEmpty()` | predicate → `Is` |
| `Data()` | `GetData()` | |
| `MaxSize()` | `GetMaxSize()` | |
| `Front()` | `GetFirst()` | |
| `Back()` | `GetLast()` | |
| `Reserve(n)` | `EnsureCapacity(n)` / `TryEnsureCapacity(n)` | name states the postcondition: `GetCapacity() >= n`; never shrinks; no-op if already satisfied |
| `ShrinkToFit()` | `TrimCapacity()` | releases excess; best-effort |
| `PushBack(v)` | `Add(v)` / `TryAdd(v)` | |
| `EmplaceBack(args…)` | `AddInPlace(args…)` / `TryAddInPlace(args…)` | |
| `PopBack()` | `RemoveLast()` | |
| `Insert(i,v)` | `InsertAt(i,v)` | |
| `EmplaceAt(i,args…)` | `InsertAtInPlace(i,args…)` | |
| `Erase(i)` | `RemoveAt(i)` | order-preserving, O(n) |
| `EraseRange(f,l)` | `RemoveRange(f,l)` | order-preserving, O(n) |
| `EraseUnordered(i)` | `RemoveAtSwap(i)` | O(1) swap-with-last; name makes the reorder explicit |
| `Append(span)` | `AddRange(span)` / `TryAddRange(span)` | |
| `From(span)` | `FromRange(span)` | static factory |
| `Resize`, `Clear`, `Swap`, `Fill` | unchanged | already verbs |
| `operator[]`, `begin/end/cbegin/cend`, `data/size`, `AsSpan`, `operator==`/`<=>` | unchanged | interop surface (range-for, ranges, `std::sort`, GPU pointer+count) |

`EnsureCapacity(n)` is deliberately **not** `Reserve(n)` or `SetCapacity(n)`: the
name states its contract (ensure at least `n`; do nothing if already met; never
shrink). An exact-capacity `SetCapacity` was **not** introduced — Ludus has no
call site that needs an exact resulting capacity, and exact-shrink semantics
would be a sharper, less obviously-correct tool; releasing excess is the explicit
`TrimCapacity()`.

Files were renamed to match: `static_array.hpp` (`StaticArray`), `array.hpp`
(`Array`, formerly `vector.hpp`), `src/array_support.cpp`. The detail header
`detail/contiguous_storage.hpp` and the relocation trait are unchanged in
behaviour.

### 34.3 Migration completeness (requirement: actually migrate)

**Policy (authoritative, current).** `std::vector` is **prohibited** in all
first-party Ludus code — engine source *and* tests alike. The only permitted
occurrences are:

1. **Benchmark comparison baselines** — the container benchmark
   (`containers/benchmarks/array_bench.cpp`) deliberately declares `std::vector`
   and `std::array` to measure `Ludus::Array`/`StaticArray` against the STL. The
   STL types *are* the point of that translation unit, which is not part of the
   engine build graph.
2. **Third-party / vendor / generated code** — code Ludus does not own.

There is **no** "convenience", "cold path", "filesystem-related", "test-only",
"temporary", or "will be rewritten later" exception. The element type being a
not-yet-Ludus facility (`std::filesystem::path`, `std::string`, `std::thread`)
does **not** justify keeping the *container* as `std::vector`: the owning
container migrates to `Ludus::Array` regardless, and only the element type stays
STL until its own replacement lands.

The original migration replaced every first-party owning `std::vector` then
present, **including the log-retention path and the test thread pools that prior
revisions of this document had deferred**:

- `logging/logger.cpp`: sink list → `Array<UniquePtr<ILogSink>>`; producer
  format buffer → `StaticArray<char, N>`.
- `logging/internal/backend.{hpp,cpp}`: worker sink list → `Array<UniquePtr<…>>`.
- `logging/sinks/file_sink.cpp`: the retention grouping (`std::vector<Group>`,
  `std::vector<Group*>`, and `Group::Segments` `std::vector<path>`) → Ludus
  `Array`. The **element** types `std::filesystem::path` and `std::string` stay
  STL — they are `<filesystem>`/String facilities on their own replacement track,
  not container replacements. Pointer stability is preserved (the `Group*`
  snapshot is built only after `groups` stops growing), and `std::sort` still
  operates over the array's contiguous iterators.
- `profiling/export_perfetto.cpp` and `profiling/tests/scope_tests.cpp`:
  event/chunk buffers → `Array<…>`.
- `graphics/rhi/src/rhi.cpp`: Vulkan extension enumeration/enable buffers
  (`std::vector<VkExtensionProperties>`, `std::vector<std::string>`,
  `std::vector<const char*>`) → `Array<…>`. Owning storage is genuinely
  required (Vulkan writes counts + out-params via `GetData()`/`GetSize()`), so
  these are owning `Array`, not `std::span` views.
- `profiling/tests/{scope_tests,clock_tests}.cpp` and
  `logging/tests/regression_tests.cpp`: `std::vector<std::thread>` thread pools
  → `Array<std::thread>`. `std::thread` is move-only; `Array` handles it
  correctly (verified — see §34.5).
- `docs/architecture/logging-review-evidence/probe.cpp`: the review evidence
  probe's `std::vector<std::thread>` → `Array<std::thread>`.

The 2026-10-06 source review found later first-party `std::vector` uses in Text,
GameHost protocol/session storage, Audio Gym and tests. They are migration debt
under this policy. [Milestone M1](container-systems.md#milestone-roadmap) tracks
consumer-by-consumer repair and an automated regression gate.

### 34.4 Remaining STL exceptions (verified genuine)

The original audit retained two categories of exceptions:

- `foundation/base/src/diagnostic.cpp` — `std::array<char, N>` (×3) in the
  emergency diagnostic/assertion path. This is a **hard** boundary, not
  convenience: `FoundationBase` must not depend on `FoundationContainers` (the
  containers module depends on Base; using `StaticArray` there would be a
  circular module dependency), and the emergency path must stay dependency-free
  and must not invoke `LUDUS_ASSERT` (which `StaticArray::operator[]` uses). It
  is `std::array` (fixed-size, no allocation), not `std::vector`, and it is
  correct that it stays.
- `containers/benchmarks/array_bench.cpp` — `std::vector`/`std::array`
  comparison baselines. Their whole purpose is to benchmark against the STL, and
  the translation unit is not in the engine build graph.

The later first-party `std::vector` uses found by the 2026-10-06 review were
migrated by [M1](container-systems.md#m1-consumer-migration). The tree again
contains only these exceptions, and `tools/check_container_policy.py` enforces
the `std::vector` exception list. It runs from `./scripts/check --format` and
in CI. The checker reports any listed exception that no longer needs the type.

### 34.5 `Array` with move-only / non-trivial element types (verified)

The thread-pool migration exercised `Array<std::thread>`, a **move-only,
non-trivially-destructible** element type. `Array` handled it correctly with no
container change required: `EnsureCapacity` + `AddInPlace(lambda)` construct
threads in place, growth relocates via moves (never copies), and destruction
runs each element's destructor. This was confirmed both by the migrated
multithreaded tests passing under ASan/UBSan and by a standalone probe that
spawned and joined 28 threads through `Array<std::thread>`, alongside
`Array<std::string>` and `Array<const char*>`, with no leaks or UB. Because no
correctness deficiency was exposed, the container behaviour was **not**
redesigned (per the task's contract: change the container only if migration
reveals a real bug).
