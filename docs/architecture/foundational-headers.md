# Foundational Headers, Include Strategy, and the Ludus Core Vocabulary

> **Status:** Implemented. The two-tier architecture below (Candidate D) has
> been built and the repository migrated: `core.h`/`config.h`/`compiler.h`
> exist, `defines.h`/`defines.hpp`/`compiler.hpp` are removed, the modules are
> migrated to explicit includes, the opt-in foundational PCH exists, and the
> header-self-sufficiency and include-boundary gates run in CI. See ADR 0007 and
> the "Implementation status" note at the end of this document. Sections 1-2
> preserve the original as-built audit that motivated the design.
>
> **Scope:** How Ludus should expose its universal engine vocabulary
> (primitive types, build/compiler/platform configuration, core macros and
> attributes, assertion plumbing) versus its heavier facilities (containers,
> strings, logging, math, threading); the role of a minimal foundational
> header, umbrella headers, module public APIs, a PCH, and the transitive- and
> include-ordering policies that keep the module graph honest.
>
> **Relationship to existing decisions:** This builds on and does not override
> ADR 0002 (static libraries), ADR 0003 (standard-library usage policy),
> ADR 0004 (type-erase heavy templates at the ABI boundary), and ADR 0005
> (build-time budgets). Where a rule here becomes durable it should graduate to
> an ADR (see §18). `AGENTS.md` remains authoritative for coding standards.

---

## 1. Current-state audit

### 1.1 Module and dependency layout

Ludus is organized as static library modules under `modules/`, plus a
development-only diagnostics library under `tools/`, consumed by a single
`apps/smoke` executable. All engine libraries are `STATIC` (ADR 0002). The
dependency direction is strictly downward toward `FoundationBase`, with no
cycles:

```
FoundationBase   (leaf; depends on no engine module; Linux/Clang-only guard today)
  ├── FoundationContainers      → Base
  ├── FoundationLogging         → Base, Containers
  ├── FoundationProfiling       → Base, Containers, Threads   (deliberately NOT Logging)
  ├── Platform                  → Base, Logging               (own layer: ludus/platform/…)
  ├── GraphicsRhi               → Base (public), Logging (private), volk (private)
  └── tools/DiagnosticsIntegration → Base
```

Public headers live under `include/ludus/<layer>/<module>/…`; private
implementation headers live under `src/internal/` (and `src/sinks/`). Only
public headers are installed/exported via CMake `FILE_SET`s. Compile options,
warnings, and `-fno-exceptions` are applied through the interface targets
`ludus_project_options` / `ludus_project_warnings`, attached to every target by
`ludus_apply_project_defaults()`.

Three headers are generated at configure time:

- `assert_config.hpp` (base) — SDK-owned assertion policy macros.
- `build_metadata.hpp` (base) — version, git revision, compiler identity.
- `profiling_config.hpp` (profiling) — `LUDUS_PROFILING_ENABLED`.

### 1.2 Existing foundational vocabulary (`FoundationBase`)

| Header | Contents | Notes |
| --- | --- | --- |
| `types.h` | `uint8/16/32/64`, `int8/16/32/64`, `usize`, `isize`, `float32/float64` (exact `<cstdint>`/`<cstddef>` aliases). Declared in `ludus::foundation::core`, re-exported into `ludus::foundation`. | The de-facto universal header (see §2). Cheap: only `<cstdint>`/`<cstddef>`. |
| `defines.h` | `LUDUS_INLINE`, `nullptr_t`, `DerivedFrom` concept. Includes `<cstddef>`, `<type_traits>`. | Duplicated by `defines.hpp` (see §3). |
| `defines.hpp` | `LUDUS_INLINE`, `nullptr_t`, **plus** build-flavor macros (`LUDUS_BUILD_*`, `LUDUS_IS_*_BUILD`). | Near-duplicate of `defines.h`; not listed in the CMake `FILE_SET`. |
| `compiler.hpp` | `LUDUS_NOINLINE`, `LUDUS_COLD` (compiler attribute macros). | Compiler detection only. |
| `assert.hpp` | `LUDUS_ASSERT/REQUIRE/CHECK/FATAL`, `AssertionSite`, `FailureKind`. Includes `assert_config.hpp`, `compiler.hpp`, `types.h`. | The engine's error-signaling contract. |
| `assert_format.hpp` | Assertion message formatting (opt-in layer). | |
| `diagnostic.hpp` / `diagnostic_output.hpp` | Base emergency-report primitive (`EmergencyReport`, `EmergencyNote`), independent of Logging. | Uses `<source_location>`, `<string_view>`. |
| `pointer.hpp` | `UniquePtr<T>` (Ludus-owned, replaces `std::unique_ptr`). | |
| `version.hpp` | `Version` struct + `version()/version_string()/git_revision()/compiler_identity()`. | `snake_case` free functions — inconsistent with the verb-first PascalCase used elsewhere (pre-existing). |

Notably, **OS/platform detection** (`LUDUS_PLATFORM_WINDOWS/MACOS/LINUX/DESKTOP`)
does **not** live in `FoundationBase`; it lives *above* Base in
`modules/platform/include/ludus/platform/config.h`. Compiler-attribute detection
(`compiler.hpp`) and inline attributes (`defines.h`) live *in* Base. So today
"how do I detect my compiler" and "how do I detect my OS" answer from two
different layers.

### 1.3 Conventions (authoritative — from `AGENTS.md`, ADR 0003, and code)

- **Namespaces:** implementation in `ludus::foundation::core`, re-exported into
  `ludus::foundation` for short-scoped names; subsystem namespaces
  `ludus::foundation::{base,diagnostics,logging,profiling}`.
- **Naming:** types and methods `PascalCase`; **functions are verb-first**
  (`Get`, `Release`, `Reset`, `EmergencyReport`, `ShouldLog`, `SubmitText`);
  members `mXxx`; macros `LUDUS_` prefixed. Naming is enforced *by example*,
  not by `.clang-tidy` (no `readability-identifier-naming` checks configured).
- **Header suffixes:** `.h` for C-compatible / macro / type-alias headers
  (`types.h`, `defines.h`, `config.h`, `rhi.h`, `log_categories.h`); `.hpp` for
  C++ headers.
- **Primitives:** use the Ludus aliases; `std::uint32_t`/`std::size_t`/raw
  `float`/`double` spellings are banned outside `types.h`.
- **No exceptions** (`-fno-exceptions`), C++23, Clang 23 / LLD, `#pragma once`.
- **Heavy headers** (`<format>`, `<chrono>`, `<filesystem>`, `<regex>`,
  `<iostream>`) are banned from public headers; expensive facilities are
  type-erased/PIMPL'd behind a `.cpp` (ADR 0004). Build time is budgeted per
  header (ADR 0005).
- **Slated for replacement, do not spread:** `std::string`, `std::vector`,
  `std::unordered_map`, `std::format`, `<filesystem>`, `<cstring>` (ADR 0003).

### 1.4 Existing aggregation and PCH state

- Exactly **one** umbrella header exists: `containers.hpp`, which re-exports
  `array.hpp` / `relocation.hpp` / `vector.hpp` and is explicitly documented as
  a convenience for call sites that use several — "prefer the specific header."
- There is **no** `Core.h` / `Common.h` / `prelude` / god-header anywhere.
- There is **no** PCH anywhere (`target_precompile_headers` appears zero times).
- Build hygiene is governed by measured budgets, not include-cleaner gating
  (ADR 0005); include-cleaner is advisory only.

---

## 2. Include / dependency measurements

Measured on the current tree (`grep` over `#include <ludus/…>` and `<std>` in
`modules/`, `apps/`, `tools/`; 49 `.cpp` translation units total).

### 2.1 Most-included Ludus headers (by number of files that include them)

| Rank | Header | Files (all) | Direct in `.cpp` TUs |
| ---: | --- | ---: | ---: |
| 1 | `foundation/base/types.h` | **32** | 2 |
| 2 | `foundation/logging/log.hpp` | 9 | 8 |
| 2 | `foundation/logging/log_format.hpp` | 9 | 8 |
| 2 | `foundation/containers/vector.hpp` | 9 | 7 |
| 2 | `foundation/base/diagnostic_output.hpp` | 9 | 7 |
| 2 | `foundation/base/assert.hpp` | 9 | 5 |
| 7 | `foundation/base/pointer.hpp` | 8 | 5 |
| 8 | `foundation/logging/log_system.hpp` | 7 | 7 |
| 8 | `foundation/logging/level.hpp` | 7 | – |
| 8 | `foundation/base/defines.h` | 7 | 3 |
| … | `foundation/base/compiler.hpp` | 4 | 1 |

### 2.2 The central finding: an *accidental* foundational header

`types.h` is included by **32** files — every module touches it — yet only **2**
`.cpp` files include it *directly*. It reaches essentially every translation
unit **transitively**, pulled in by ~28 headers: `assert.hpp`, `diagnostic*.hpp`,
every containers header, logging `category/level/log_format/log_system`,
profiling `clock/zone/trace_system`, `rhi.h`, and their private headers.

This is the crux of the problem. Ludus *already has* a universal foundational
vocabulary — the fixed-width types — but its universality is **accidental and
transitive**, not **explicit and contractual**. A TU gets `uint32` because it
happened to include something that happened to include `types.h`. Remove one
edge in that chain and unrelated files stop compiling. That is the exact
"accidental transitive include" anti-pattern the deliverable asks us to
distinguish from a deliberate foundational header (§ mapping to categories 1
and 7 in the "critically distinguish" list).

### 2.3 Standard-library include frequency (engine, non-test)

| Header | Files | Header | Files |
| --- | ---: | --- | ---: |
| `<string_view>` | 23 | `<utility>` | 5 |
| `<string>` | 17 | `<source_location>` | 5 |
| `<cstring>` | 14 | `<mutex>` | 5 |
| `<atomic>` | 14 | `<algorithm>` | 5 |
| `<cstdlib>` | 10 | `<format>` | 4 |
| `<cstdio>` | 10 | `<filesystem>` | 4 |
| `<type_traits>` | 9 | `<cstddef>` | 4 |
| `<thread>` | 7 | `<span>` | 7 |
| `<new>` | 7 | `<vector>` | 6 |
| `<chrono>` | 7 | | |

`<string_view>` (23) and `<type_traits>`/`<utility>` are pervasive and cheap —
candidates for the foundational layer. `<string>` (17), `<vector>` (6),
`<format>` (4), `<filesystem>` (4), `<chrono>` (7) are heavier and/or slated for
replacement — they must stay opt-in.

### 2.4 Include clusters and boilerplate

- Files including ≥3 distinct `base/*` headers: only `vector.hpp` (4),
  `rhi.cpp` (3), `vector_support.cpp` (3), `assert.cpp` (3), `assert.hpp` (3).
  Base boilerplate is modest but real.
- `apps/smoke/main.cpp` still opens with a **9-line** `ludus/…` include block
  plus `<string>` — the concrete "every file starts with boilerplate" evidence
  the brief calls out, at the application layer where cohesion matters most.

### 2.5 Notable transitive chains and hygiene observations

- `assert.hpp` → `assert_config.hpp` (generated) + `compiler.hpp` + `types.h`.
  Anything that asserts transitively acquires the type vocabulary.
- `vector.hpp` → `detail/contiguous_storage.hpp` → `types.h`; plus `<span>`,
  `<type_traits>`, `<utility>`. Kept deliberately light (placement-new instead
  of `<memory>`, per the containers budget note).
- `profiling.hpp` → `defines.hpp` (the `.hpp`, not `.h`) + `zone.hpp`
  (`<string_view>` + `<source_location>`) + `<atomic>`. This is the only
  consumer that depends on `defines.hpp`'s build-flavor macros through that
  spelling, which is why the `.h`/`.hpp` split is load-bearing today (§3).
- Minor: `<memory>` appears in `logging/src/logger.cpp` (slated-for-replacement
  facility, contained to one TU); `<sstream>` appears in three logging **test**
  files (tests are exempt per ADR 0003).

### 2.6 Cost characterization (why the split matters)

`types.h`, `compiler.hpp`, `defines.*`, and `version.hpp` are all *cheap* to
parse (they pull only `<cstdint>`/`<cstddef>`/`<type_traits>`/`<string_view>`).
The expensive headers by prior measurement are the ones that historically
pulled `<format>`/`<chrono>` (`log.hpp` was ~2240 ms on CI before the logging
redesign split it; `backend.hpp` still carries a documented 4000 ms override).
The foundational vocabulary is therefore *safe to make universal* — its
intrinsic weight is near-zero — while the heavy facilities must stay explicitly
opt-in. This asymmetry is the entire basis of the recommended design.

---

## 3. Problems discovered

1. **Accidental universality of `types.h`.** The single most important
   vocabulary header reaches ~every TU transitively, not by contract (§2.2).
   There is no header a file can include and *know* it has the Ludus
   vocabulary; there is no compile guarantee it will keep arriving.

2. **`defines.h` vs `defines.hpp` duplication.** Two near-identical headers both
   define `LUDUS_INLINE` and `nullptr_t`. `defines.hpp` additionally owns the
   build-flavor macros (`LUDUS_BUILD_*`, `LUDUS_IS_*_BUILD`); `defines.h` owns
   the `DerivedFrom` concept. They are not consistently interchangeable, the
   `FILE_SET` lists `defines.h` but omits `defines.hpp` (which is on disk and
   included by `profiling.hpp`), and callers must know which spelling carries
   which macro. This is a latent correctness/packaging bug.

3. **Foundational configuration is split across layers.** Compiler detection and
   inline attributes are in Base; OS detection (`LUDUS_PLATFORM_*`) is above
   Base in the `platform` module. A file that needs "am I on macOS?" must depend
   on a whole windowing module. This blocks the stated future goals (macOS,
   Vulkan/Metal/D3D12) which need OS/arch detection at the *foundation*, not at
   the window layer.

4. **No explicit "what is always available" contract.** Nothing states, in one
   place, the vocabulary every Ludus TU may assume. New contributors reverse-
   engineer it from transitive includes; the "cohesion" the brief wants is
   unspecified.

5. **Application-layer boilerplate is real** (§2.4) while there is no sanctioned
   way to reduce it that would not reintroduce a god-header.

6. **No PCH, no PCH policy.** Build time is well-governed by budgets, but there
   is no decision on whether/what to precompile, and — importantly — no
   guardrail preventing a future PCH from silently becoming the semantic
   dependency model (the exact hazard the brief warns about).

7. **Minor consistency:** `version.hpp` uses `snake_case` free functions,
   diverging from the verb-first PascalCase convention.

None of these are emergencies — the repo is disciplined and the budgets catch
regressions. They are architectural debt that compounds as modules multiply.

---

## 4. Design goals

Ranked, and traceable to the brief's "desired properties":

1. **Explicit, contractual foundational vocabulary** — a single, tiny,
   cheap-to-parse header a TU can include to *guarantee* it has the Ludus
   language (types, core macros, build/compiler/platform config, assertion
   plumbing). Universality by contract, not by accident.
2. **Extremely low dependency cost** — the foundational header must add
   negligible parse time and pull no heavy STL, no allocator, no containers,
   no strings, nothing that instantiates templates.
3. **Predictable, incremental-friendly build times** — nothing implicit may
   grow the dependency footprint; the heavy facilities stay opt-in; budgets
   (ADR 0005) remain the enforcement mechanism.
4. **Explicit architectural layering** — the module graph must remain
   readable from the include graph; no umbrella or PCH may hide a real
   cross-module dependency.
5. **Engine cohesion / ergonomics** — a file that includes the foundational
   header should feel like it is writing Ludus, not std C++, without a wall of
   boilerplate for the truly-universal concepts.
6. **Portability & future backends** — OS/arch/compiler detection belongs at
   the foundation so macOS + Vulkan/Metal/D3D12 can branch on it without a
   window-module dependency.
7. **Maintainability, debuggability, testability, IDE tooling, long-term
   scale** — one obvious home per concept; forwardable types; no god-header to
   confuse tooling; a PCH that is a pure build accelerator.

Explicit non-goal: **minimizing `#include` line count is not itself a goal.**
Fewer includes is a side effect of a correct foundation, never the objective.

---

## 5. Candidate architectures considered

### Candidate A — Status quo (accidental transitive `types.h`)

Keep relying on `types.h` arriving transitively; add nothing.

- **Pros:** zero work; already "cohesive" in practice for types.
- **Cons:** fails goal 1 and 4; fragile (an include-cleaner pass or a header
  refactor can break unrelated TUs); no home for platform/arch detection; the
  `defines.h/.hpp` bug persists.

### Candidate B — Single god-header (`Ludus.h` / `Common.h` / `Engine.h`)

One umbrella that pulls types + macros + containers + strings + logging + …
Every `.cpp` includes it.

- **Pros:** maximal "cohesion"; minimal include lines.
- **Cons:** the explicitly-forbidden outcome. It couples every TU to heavy
  facilities (`<format>`, containers, logging), destroys incremental builds
  (touch logging → recompile the world), hides the real dependency graph
  (goal 4), and violates ADR 0003/0004/0005. Rejected by the brief and by every
  mature codebase's experience (UE's IWYU exists specifically to escape
  `Engine.h`; general guidance is that gratuitous umbrellas *increase* build
  time). **Rejected.**

### Candidate C — Chromium-style pure IWYU, no foundational header

Every file includes exactly the header declaring each symbol it uses; forward-
declare aggressively; no umbrella; no transitive reliance.

- **Pros:** best raw build scalability; crystal-clear dependencies.
- **Cons:** maximal boilerplate for truly-universal concepts (every file
  re-includes the types/macros header); fights the engine-cohesion goal; higher
  friction than warranted at Ludus's scale. Chromium accepts this because its
  scale makes any implicit cost catastrophic; Ludus is not there and values
  cohesion. **Rejected as the whole model, but adopted as the rule *above* the
  foundation line** (see recommendation).

### Candidate D — Two-tier: minimal foundational header + optional curated preludes (RECOMMENDED)

Mirrors the Unreal split (`CoreTypes.h` → `CoreMinimal.h`, and *never*
`Engine.h`), adapted to Ludus naming and layering:

- A tiny, always-cheap **foundational header** in Base that *explicitly* and
  *contractually* provides the universal vocabulary (types, core macros,
  build/compiler/platform config, attributes, assertion entry points). This is
  the one thing allowed to be relied upon everywhere.
- Everything else (containers, strings, logging, profiling, math, threading) is
  **included explicitly** by the files that use it — Chromium/Abseil discipline
  *above* the foundation line.
- Optional, **curated, per-boundary umbrella headers** where a module's public
  surface is genuinely used as a set (the existing `containers.hpp` is the
  template) — never a cross-module god-header.
- A **PCH that mirrors the foundational header set** (plus the stable heavy STL
  a TU already pays for), as a pure build accelerator that never changes what a
  TU is *allowed* to name.

- **Pros:** satisfies every goal; makes the accidental `types.h` universality
  intentional; gives platform/arch detection a home; keeps heavy facilities
  opt-in and budgeted; keeps the dependency graph readable; matches the most
  relevant industry analogue without cargo-culting it.
- **Cons:** requires a one-time, mechanical migration and new enforcement.

### Candidate E — C++20 modules / header units

Convert the foundation (and eventually modules) to named C++20 modules.

- **Pros:** the "right" long-term answer to include cost; no textual re-parsing.
- **Cons:** toolchain risk on the pinned Clang 18 + CMake + Conan + Ninja +
  sanitizer + coverage matrix; immature ecosystem support for static-lib export
  and IDE tooling in 2024–2025; would be a large, risky bet layered on top of an
  unsolved *design* question. **Deferred** — the recommended header design is
  explicitly forward-compatible with a later modules migration (§ recommended
  architecture and §remaining questions).

---

## 6. Trade-off analysis

| Criterion | A: Status quo | B: God-header | C: Pure IWYU | **D: Two-tier (rec.)** | E: C++20 modules |
| --- | --- | --- | --- | --- | --- |
| Dependency cost | Low but fragile | **Very high** | Lowest | **Low, contractual** | Lowest (eventually) |
| Build predictability | Fragile | **Worst** | Best | **Very good** | Best (eventually) |
| Incremental builds | OK until refactor | **Worst** | Best | **Very good** | Best |
| Explicit layering | Hidden/accidental | **Hidden** | Explicit | **Explicit** | Explicit |
| Engine cohesion / ergonomics | Accidental | Max (bad kind) | **Worst** | **High (good kind)** | High |
| Portability / future backends | No home for arch/OS | N/A | Manual everywhere | **Foundation owns it** | Foundation owns it |
| Debugging | OK | Hard (huge TUs) | Good | **Good** | Good |
| IDE tooling | OK | Poor | Good | **Good** | Immature today |
| Migration cost | None | Low (but wrong) | High | **Moderate** | **High + risky** |
| ADR 0003/0004/0005 compat | OK | **Violates** | OK | **Reinforces** | OK |

Candidate **D** is the only option that scores well on cohesion *and* build
health simultaneously, which is precisely the boundary the brief asks us to
find. It also strictly generalizes the pattern the repo already uses
(`containers.hpp`) and the header the repo already treats as universal
(`types.h`), so it is evolutionary, not a rewrite.


---

## 7. Recommended architecture

Adopt **Candidate D**. Concretely, introduce an explicit three-band model. The
bands map one-to-one onto the "critically distinguish" concepts in the brief.

```
┌─────────────────────────────────────────────────────────────────────────┐
│ BAND 0 — implicit configuration (macros only, no code, effectively free)  │
│   ludus/foundation/base/config.h        OS + arch + build-flavor macros   │
│   ludus/foundation/base/compiler.h      compiler id + attribute macros    │
│   ludus/foundation/base/assert_config.hpp (generated) assertion policy    │
├─────────────────────────────────────────────────────────────────────────┤
│ BAND 1 — the foundational header (THE universal vocabulary; contractual)  │
│   ludus/foundation/base/core.h                                            │
│     → types.h (uint32/usize/float32/…)                                    │
│     → config.h + compiler.h (Band 0)                                      │
│     → the small always-safe macro/attribute set + Move/Forward            │
│     → assertion entry points (LUDUS_ASSERT/REQUIRE/CHECK/FATAL)           │
│   This is the ONE header a TU may rely on being universally available.    │
├─────────────────────────────────────────────────────────────────────────┤
│ BAND 2 — everything else: explicit, per-file, no transitive reliance      │
│   containers (Vector/Array), strings, logging, profiling, math,           │
│   threading, filesystem, smart pointers beyond the core…                  │
│   Included by the files that use them. Curated module umbrellas allowed   │
│   at a single module's public boundary (like containers.hpp).             │
└─────────────────────────────────────────────────────────────────────────┘

              A PCH may precompile Band 0+1 (and the stable heavy STL a TU
              already pays for) as a BUILD ACCELERATOR only — it never changes
              what a TU is allowed to name (§10).
```

Key decisions:

- **The foundational header is `ludus/foundation/base/core.h`.** It is the
  intentional replacement for the accidental universality of `types.h`. It stays
  cheap: it pulls only `<cstdint>`/`<cstddef>` (via `types.h`),
  `<type_traits>`/`<utility>` (for `Move`/`Forward`), and the generated assertion
  policy — no `<string>`, no `<string_view>` even, no containers, no
  `<source_location>` in the header itself beyond what assertions already need.
  (`.h` suffix because it is the C-compatible/macro/type-alias tier, consistent
  with `types.h`/`defines.h`/`config.h`.)
- **`types.h` continues to exist** and remains includable directly; `core.h`
  includes it. Files that want *only* the numeric types may keep including
  `types.h`. `core.h` is the "and everything a Ludus file assumes" superset.
- **OS/arch detection moves into Base** as `ludus/foundation/base/config.h`
  (macros only, zero code). `platform/config.h` becomes a thin shim that
  includes the Base header and adds *windowing-backend* selection only
  (`LUDUS_PLATFORM_WAYLAND`/`HEADLESS`), which is genuinely a platform-module
  concern. This unblocks macOS/Metal/D3D12 branching at the foundation.
- **`defines.h` and `defines.hpp` are merged** and the pieces re-homed:
  `LUDUS_INLINE` + attributes → `compiler.h`; build-flavor macros → `config.h`;
  `DerivedFrom` and `nullptr_t` → a small `traits.h`/kept in `core.h`. The
  duplicate is deleted (§18 migration).
- **Above the foundation line, Chromium/Abseil discipline applies:** include the
  header that declares what you name; do not rely on transitive includes (§14).
- **Curated module umbrellas** (like `containers.hpp`) are permitted at a single
  module's public boundary, are opt-in, and never cross module boundaries (§11).

This is forward-compatible with C++20 modules (Candidate E): `core.h` is the
natural first module interface unit; the Band-2 explicit-include discipline maps
directly onto `import` boundaries later.

---

## 8. Exact foundational-header contents

`core.h` is the contract. It contains **only** facilities that are (a)
universally useful, (b) effectively free to parse, (c) free of allocation,
templates-of-substance, ABI surface, and platform runtime dependencies.

Proposed `modules/foundation/base/include/ludus/foundation/base/core.h`:

```cpp
#pragma once

// Ludus foundational vocabulary. Including this header GUARANTEES a translation
// unit has the universal Ludus language: fixed-width types, build/compiler/
// platform configuration, core attribute macros, Move/Forward, and the
// assertion entry points. This is the ONE header a Ludus file may assume is
// universally available; everything heavier is included explicitly (see
// docs/architecture/foundational-headers.md).
//
// Cost contract: this header pulls only <cstdint>/<cstddef>/<type_traits>/
// <utility>. It MUST NOT include <string>, <string_view>, <format>, <chrono>,
// <filesystem>, containers, logging, or anything that instantiates a
// substantial template. Enforced by the build-time budget (ADR 0005) and the
// forbidden-include check (§9/§19).

#include <ludus/foundation/base/types.h>     // uint32/usize/float32/… (Band 0)
#include <ludus/foundation/base/config.h>     // OS/arch/build-flavor macros (Band 0)
#include <ludus/foundation/base/compiler.h>   // compiler id + attribute macros (Band 0)
#include <ludus/foundation/base/assert.hpp>   // LUDUS_ASSERT/REQUIRE/CHECK/FATAL

#include <type_traits>
#include <utility>

namespace ludus::foundation::core
{
// Move: cast to rvalue. Ludus-named spelling of std::move; verb-first, matches
// the engine vocabulary rather than importing the std name into call sites.
template <typename T>
[[nodiscard]] constexpr std::remove_reference_t<T>&& Move(T&& value) noexcept
{
    return static_cast<std::remove_reference_t<T>&&>(value);
}

// Forward: perfect-forward. Two overloads mirror std::forward's contract.
template <typename T>
[[nodiscard]] constexpr T&& Forward(std::remove_reference_t<T>& value) noexcept
{
    return static_cast<T&&>(value);
}
template <typename T>
[[nodiscard]] constexpr T&& Forward(std::remove_reference_t<T>&& value) noexcept
{
    static_assert(!std::is_lvalue_reference_v<T>, "Forward must not turn an rvalue into an lvalue");
    return static_cast<T&&>(value);
}
} // namespace ludus::foundation::core

namespace ludus::foundation
{
using core::Forward;
using core::Move;
} // namespace ludus::foundation
```

Proposed `config.h` (Band 0; macros only; **moved/expanded from
platform/config.h + defines.hpp**):

```cpp
#pragma once

// OS family (detection only — does NOT select a windowing backend; that is the
// platform module's job).
#if defined(_WIN32)
#    define LUDUS_PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
#    define LUDUS_PLATFORM_MACOS 1
#elif defined(__linux__)
#    define LUDUS_PLATFORM_LINUX 1
#endif
#if defined(LUDUS_PLATFORM_WINDOWS) || defined(LUDUS_PLATFORM_MACOS) || defined(LUDUS_PLATFORM_LINUX)
#    define LUDUS_PLATFORM_DESKTOP 1
#endif

// CPU architecture (needed for SIMD/math/atomics decisions across backends).
#if defined(__x86_64__) || defined(_M_X64)
#    define LUDUS_ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#    define LUDUS_ARCH_ARM64 1
#endif

// Build flavor (relocated verbatim from the former defines.hpp; SDK-owned
// values still arrive from assert_config.hpp / EngineBuildFlavor.cmake).
#if !defined(LUDUS_BUILD_DEBUG) && !defined(LUDUS_BUILD_DEVELOPMENT) &&                                                 \
    !defined(LUDUS_BUILD_PROFILE) && !defined(LUDUS_BUILD_RELEASE)
#    if defined(_DEBUG) || defined(DEBUG)
#        define LUDUS_BUILD_DEBUG 1
#    elif defined(NDEBUG)
#        define LUDUS_BUILD_RELEASE 1
#    else
#        define LUDUS_BUILD_DEVELOPMENT 1
#    endif
#endif
// LUDUS_IS_*_BUILD convenience flags follow (as today).
```

Proposed `compiler.h` (Band 0; **renamed from compiler.hpp + inline macro from
defines.h**):

```cpp
#pragma once

// Compiler identity.
#if defined(__clang__)
#    define LUDUS_COMPILER_CLANG 1
#elif defined(__GNUC__)
#    define LUDUS_COMPILER_GCC 1
#elif defined(_MSC_VER)
#    define LUDUS_COMPILER_MSVC 1
#endif

// Inlining / codegen attributes.
#if defined(_MSC_VER)
#    define LUDUS_INLINE   __forceinline
#    define LUDUS_NOINLINE __declspec(noinline)
#    define LUDUS_COLD
#else
#    define LUDUS_INLINE   inline __attribute__((__always_inline__))
#    define LUDUS_NOINLINE __attribute__((noinline))
#    define LUDUS_COLD     __attribute__((cold))
#endif

// Branch-prediction and optimizer hints (new; today the code uses [[likely]]/
// [[unlikely]] inline — these give a named, greppable engine spelling and a
// portable debug-break for the assertion/diagnostic paths).
#define LUDUS_LIKELY(x)   (__builtin_expect(!!(x), 1))
#define LUDUS_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#if defined(__clang__) || defined(__GNUC__)
#    define LUDUS_DEBUG_BREAK() __builtin_debugtrap()
#    define LUDUS_UNREACHABLE() __builtin_unreachable()
#else
#    define LUDUS_DEBUG_BREAK() ((void)0)
#    define LUDUS_UNREACHABLE() ((void)0)
#endif
```

**Membership rationale (Tier evaluation):**

| Category | Tier | In `core.h`? | Why |
| --- | --- | :---: | --- |
| Fixed-width types, `usize`/`isize` | A | ✅ (via `types.h`) | Universal, free, already de-facto universal (§2.2). |
| Compiler/platform/arch/build macros | A | ✅ (Band 0) | Universal, zero code, needed for portability & future backends. |
| Attributes, force/no-inline, branch hints, debug-break | A | ✅ (`compiler.h`) | Free macros; used by hot/diagnostic paths engine-wide. |
| Assertion entry points | A | ✅ (`assert.hpp`) | The engine's error contract; already ~universal transitively; cheap. |
| `Move`/`Forward` | B | ✅ | Trivial, free, verb-first engine vocabulary; avoids importing `std::move`. |
| Small traits (`DerivedFrom`, `nullptr_t`) | B | ✅ (kept minimal) | Cheap, already in `defines.*`; move here on merge. |
| `Span`/view types | B | ❌ (own header) | Cheap-ish but not universal; keep as `ludus/foundation/base/span.h` opt-in. |
| Result/Status, lightweight IDs/handles | B | ❌ (own headers) | Valuable but not yet present; introduce as opt-in Base headers, not implicit. |
| `String`/`Array`/`Vector`/hash maps/sets | C | ❌ | Allocation + template + ABI + slated-for-replacement (ADR 0003). Never implicit. |
| `optional`/`variant`, smart pointers beyond `UniquePtr` | C | ❌ | Template/parse cost; `UniquePtr` stays its own `pointer.hpp`, not implicit. |
| filesystem, math, logging, formatting, threading | C | ❌ | Heavy / ABI / platform / allocator coupling; explicit opt-in only. |

---

## 9. What is explicitly forbidden from the foundational header

`core.h` (and its Band-0 includes) must **never**:

1. Include a **heavy STL header**: `<format>`, `<chrono>`, `<filesystem>`,
   `<regex>`, `<iostream>`, `<sstream>`, `<locale>`, `<random>`. (Reinforces
   ADR 0004 and the `AGENTS.md` heavy-header rule.)
2. Include **containers or strings** — Ludus's or std's: no `vector.hpp`,
   `array.hpp`, `<vector>`, `<string>`, and not even `<string_view>` in `core.h`
   itself. (Files that need text include `<string_view>` or the future
   `StringView` explicitly.)
3. Instantiate or define a **substantial template** (anything beyond the trivial
   `Move`/`Forward`), or anything that forces template instantiation on include.
4. Introduce an **allocator dependency** or anything that allocates at static-
   init or on include.
5. Pull in **logging, profiling, platform-runtime, or graphics** — i.e. anything
   above Base. `core.h` lives in the leaf module and must stay a leaf.
6. Add **ABI surface** (no non-inline function definitions, no global objects
   with static storage duration beyond `constexpr`).
7. Depend on a **specific windowing backend** (`LUDUS_PLATFORM_WAYLAND` etc.
   stay in the platform module).

If a facility is *frequently used* but violates any of the above, that is
evidence it belongs in Band 2 (opt-in), not in the foundation — frequency is not
a membership criterion (§4 non-goal).

---

## 10. PCH strategy

**Principle:** a PCH is a build-performance mechanism, **not** the semantic
dependency model. It must never change what a TU is *allowed* to name, and the
codebase must compile identically with the PCH disabled.

Recommendation:

1. **Introduce a PCH only after `core.h` exists and after a measurement shows a
   win** (ADR 0005 discipline). Do not add it speculatively in the migration.
2. **PCH contents = Band 0 + Band 1 (`core.h`) + the stable heavy STL a TU
   already pays for anyway** (e.g. `<type_traits>`, `<utility>`, and — only for
   the internal targets that already include them — `<atomic>`). The PCH mirrors
   the foundation; it does **not** add containers/strings/logging that a TU did
   not already include.
3. **Scope the PCH per target**, applied through a helper in
   `cmake/EngineTargets.cmake` (e.g. `ludus_apply_pch(target)`), never as a
   global `Engine.pch`. A target that only needs `core.h` gets a `core.h` PCH; a
   target that legitimately uses `<atomic>` engine-wide (logging/profiling) may
   opt into a slightly larger PCH. Test targets may use the same PCH but must
   still compile without it.
4. **Enforce PCH-optional builds in CI:** a `no-pch` configuration (or the
   existing budget job) compiles with `target_precompile_headers` disabled to
   guarantee no file has come to rely on a symbol only the PCH provided. This is
   the guardrail that prevents the PCH from becoming a stealth god-header.
5. **The PCH is invisible to the include graph and to tooling.** Include-cleaner
   and the transitive-include policy (§14) are evaluated as if the PCH does not
   exist.
6. **Editor exception.** The optional Qt editor targets (`apps/editor`) precompile
   stable Qt Core and standard library headers by default
   (`LUDUS_EDITOR_PRECOMPILED_HEADERS`). No Ludus header is listed, so this
   section's rules for engine headers are unchanged. The batch editor job builds
   with it off, which is the no-PCH guard of point 4 for the editor.

Rationale: this captures the real build win (re-parsing the cheap-but-ubiquitous
foundation once per target) without letting the PCH define the architecture —
exactly the separation the brief demands.

---

## 11. Module umbrella-header strategy

- **Umbrella headers are permitted only at a single module's public boundary**,
  aggregating that module's own public headers, and only where call sites
  genuinely use several together. The existing
  `foundation/containers/containers.hpp` is the canonical, approved example and
  the pattern to copy.
- **Naming:** `<module>.hpp` at the module include root
  (e.g. a future `logging.hpp` aggregating `log.hpp`/`log_format.hpp`/
  `log_system.hpp`; a future `profiling.hpp` already partly plays this role).
- **Rules for every umbrella:**
  1. It aggregates **only its own module's** public headers — never another
     module's, never a std heavy header directly.
  2. It carries a header comment stating it is a convenience and that callers
     should prefer the specific header (mirroring `containers.hpp`).
  3. It must stay within its module's build-time budget; if aggregating makes it
     heavy, that is a signal not to provide the umbrella.
  4. It is **opt-in**. No target includes another module's umbrella implicitly,
     and no umbrella is added to the PCH.
- **There is no engine-wide umbrella.** `Ludus.h` / `Engine.h` / `Core.h`
  (as a god-header) are forbidden (§9, §5-B). `core.h` is a *foundational*
  header, not an umbrella: it exposes vocabulary, not a module's API surface.

The distinction the brief asks for, made concrete:

| Concept | Ludus artifact | Role |
| --- | --- | --- |
| 1. Minimal foundational header | `ludus/foundation/base/core.h` | Universal vocabulary; may be relied on everywhere. |
| 2. Umbrella header | `containers.hpp`, future `<module>.hpp` | Convenience aggregation of *one* module's public API. |
| 3. Module public API header | `log.hpp`, `window.h`, `rhi.h`, … | The real, minimal public surface of a subsystem. |
| 4. Module-private convenience header | `src/internal/*.hpp` | Implementation-only; never installed. |
| 5. Precompiled header | per-target PCH mirroring Band 0+1 | Build accelerator only; semantically invisible. |
| 6. C++20 module / header unit | (future) `core.h` as first unit | Deferred (Candidate E). |
| 7. Accidental transitive include | the current `types.h` reach | The anti-pattern this design eliminates. |

---

## 12. Rules for strings and containers

Strings and containers are **Band 2** — explicitly opt-in, never implicit — for
concrete, evidence-based reasons:

- **Compilation cost:** `<string>` appears in 17 files and `<vector>` in 6;
  both are materially heavier than the foundation and historically dragged in
  `<format>`/allocation machinery. Making them implicit would tax every TU.
- **Allocator dependency:** ADR 0003 slates `std::string`/`std::vector`/
  `std::unordered_map` for replacement by Ludus-owned, allocator-aware types.
  A foundational header must not bind the whole engine to an allocator decision
  that has not been made.
- **ABI/template-instantiation cost:** containers instantiate per element type;
  putting them in the foundation multiplies instantiations across every TU.
- **Discoverability:** a file that uses `Vector` should *say* so with an
  `#include`. That include is documentation of a real dependency.

Rules:

1. **Never** include a container or string header from `core.h` or any Band-0
   header, or add them to the PCH.
2. Use `ludus::foundation::core::Vector` / `Array` from `foundation/containers`
   via an explicit `#include`. Prefer the specific header (`vector.hpp`) over the
   `containers.hpp` umbrella unless you use several.
3. For read-only text, include `<string_view>` explicitly today; when Ludus
   grows a `StringView`/`String` (ADR 0003), it lives in a `foundation/strings`
   module and is opt-in exactly like containers — it does **not** join `core.h`.
4. `UniquePtr` stays in `pointer.hpp` (opt-in). It is common but not universal
   and carries a (trivial) template; it is not added to `core.h`.
5. Follow the engine naming, not the STL: verb-first methods, `PascalCase`
   types. Do **not** replicate STL member-function names purely to imitate it
   (per the brief's constraint); match existing Ludus container APIs.

---

## 13. Public / private header policy

Formalize the existing practice:

- **Public headers** live under `modules/<layer>/<module>/include/ludus/…` and
  are the *only* headers exported via the CMake `FILE_SET public_headers`. They
  are the module's contract.
- **Private headers** live under `src/internal/` (and `src/sinks/`, etc.), are
  listed as PRIVATE sources, and are **never installed**. A public header must
  not `#include` a private header.
- **A public header may include:** `core.h` and other Band-0/Base public
  headers; its own module's public headers; another module's public headers
  **only** if that dependency is declared `PUBLIC` in CMake
  (`target_link_libraries`). The include graph and the CMake link graph must
  agree.
- **A public header must not:** include a heavy STL header (ADR 0004); instantiate
  a heavy template; include a private header; include a header from a module it
  does not `PUBLIC`-link.
- **`.h` vs `.hpp`:** keep the existing convention — `.h` for
  macro/type-alias/C-compatible headers (`core.h`, `types.h`, `config.h`,
  `compiler.h`, `rhi.h`), `.hpp` for C++ API headers.
- **Private-link dependencies** (e.g. GraphicsRhi → Logging PRIVATE, volk
  PRIVATE) must appear **only** in `.cpp`/private headers, never in a public
  header — this is what keeps `volk`/Logging out of RHI's public surface.

---

## 14. Transitive-include policy

The rule that resolves the central problem (§2.2):

1. **Above the foundation line, no reliance on transitive includes.** If a file
   names a symbol, it includes the header that declares it — directly. This is
   the Chromium/Abseil discipline and it is the enforcement counterpart to
   having an explicit foundational header. A file must compile if every include
   it does *not* list were pruned from its direct includes' transitive sets.
2. **The single sanctioned exception is `core.h`.** Its contents (types, core
   macros, config, assertion entry points, `Move`/`Forward`) *may* be relied on
   as universally available — because a TU either includes `core.h` directly or
   includes a header that is *contractually required* to expose it. To make that
   contract real: **every public engine header includes `core.h` first** (see
   §15), so `core.h`'s vocabulary is guaranteed, not accidental.
3. **`types.h` reliance is folded into the `core.h` contract.** After migration,
   files should include `core.h` (or `types.h` directly if they want only the
   numeric types). They must not rely on `types.h` arriving via, say,
   `vector.hpp`.
4. **Enforcement is measurement + advisory tooling, not a hard IWYU gate**
   (consistent with ADR 0005's reasoning that include-cleaner is too noisy to
   gate): `./scripts/check --include-cleaner` remains advisory; the build-time
   budget remains the hard gate; a new lightweight forbidden-include check
   (§19) hard-fails only on the specific, unambiguous violations (heavy header
   in a public header; container/string in `core.h`).

This is the precise boundary the brief asks for: the foundational vocabulary is
*allowed* to be implicit (by contract); **everything else is explicit.**

---

## 15. Include ordering policy

Adopt a single, `clang-format`-friendly ordering (Ludus uses grouped includes
already, e.g. in `vector.hpp` and `smoke/main.cpp`). Within a `.cpp` or `.hpp`:

1. (In a `.cpp`) its own module header first, or (in any engine header) an
   `#include <ludus/foundation/base/core.h>` **first**, to establish the
   foundational contract and catch missing-include errors early.
2. Other **Ludus** headers, grouped by layer, most-foundational first
   (`foundation/base` → `foundation/*` → `platform` → `graphics` → app),
   alphabetized within a group.
3. **Standard library** headers.
4. **Third-party** headers (volk, wayland, Catch2 in tests).

Each group separated by a blank line (matches `.clang-format` include-block
behavior). Generated headers (`build_metadata.hpp`, `*_config.hpp`) sort with
their module group. Rationale for `core.h` first: it makes the universal
vocabulary an explicit, self-documenting dependency and surfaces accidental
reliance on ordering. This does not require the STL "own header first" rule to be
mechanical — it is about establishing the foundation, then widening outward.

---

## 16. Dependency-layer diagram

```
                          ┌───────────────────────────┐
        apps/smoke ─────► │        Application         │
                          └───────────────────────────┘
                             │      │        │      │
             ┌───────────────┘      │        │      └──────────────┐
             ▼                      ▼        ▼                     ▼
     ┌──────────────┐      ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
     │   Platform   │      │ GraphicsRhi  │ │  (future)    │ │ Diagnostics  │
     │ Base+Logging │      │ Base(pub)    │ │  gameplay…   │ │ Integration  │
     └──────┬───────┘      │ Logging(priv)│ └──────────────┘ │  Base(pub)   │
            │              │ volk(priv)   │                  └──────┬───────┘
            │              └──────┬───────┘                         │
            ▼                     │                                 │
     ┌──────────────┐   ┌─────────┴─────┐   ┌──────────────┐        │
     │FoundationLog │   │FoundationProf │   │FoundationCont│        │
     │ Base+Cont    │   │ Base+Cont+Thr │   │  Base        │        │
     └──────┬───────┘   └───────┬───────┘   └──────┬───────┘        │
            │  (Logging→Cont)   │                  │                │
            └───────────┬───────┴──────────────────┘                │
                        ▼                                           │
              ┌───────────────────────────────────────┐            │
              │            FoundationBase              │◄───────────┘
              │  core.h ─ types.h ─ config.h ─         │
              │  compiler.h ─ assert(.hpp) ─           │
              │  diagnostic ─ pointer ─ version        │
              │        (the foundational layer)        │
              └───────────────────────────────────────┘
```

Every arrow points downward; `FoundationBase`/`core.h` is the single sink. The
diagram is identical whether or not a PCH exists (the PCH sits outside it).

---

## 17. Naming / folder proposal

Keep the established layout; add/rename within Base only.

```
modules/foundation/base/include/ludus/foundation/base/
  core.h            NEW — the foundational header (Band 1)
  types.h           kept  — fixed-width types (included by core.h)
  config.h          NEW/moved — OS + arch + build-flavor macros (Band 0)
  compiler.h        renamed from compiler.hpp — compiler id + attribute macros
  assert.hpp        kept  — assertion macros + entry points
  assert_config.hpp kept  — generated assertion policy (Band 0)
  assert_format.hpp kept
  diagnostic.hpp / diagnostic_output.hpp   kept
  pointer.hpp       kept  — UniquePtr (opt-in, NOT in core.h)
  version.hpp       kept  — (optionally align funcs to verb-first, §18)
  build_metadata.hpp kept — generated
  # DELETED: defines.h and defines.hpp (contents re-homed to core.h/config.h/compiler.h)

modules/platform/include/ludus/platform/
  config.h          becomes a thin shim: #include <ludus/foundation/base/config.h>
                    + windowing-backend selection (LUDUS_PLATFORM_WAYLAND/HEADLESS) only
```

Namespaces are unchanged: implementation in `ludus::foundation::core`, short
names re-exported into `ludus::foundation`. New macros keep the `LUDUS_` prefix.
`core.h`'s `.h` suffix is deliberate and consistent with the existing
macro/type-alias tier.

Why `core.h` and not `Core.h`/`Common.h`/`prelude.h`: the repo already uses a
`ludus::foundation::core` namespace and lowercase header filenames; `core.h`
reads as "the core of the foundation" and is unambiguous. `Common.h`/`Engine.h`
carry the god-header connotation the brief rejects.


---

## 18. Migration strategy

Phased, each phase independently reviewable, buildable, and revertable. **No
phase is part of this research document; this is the plan for the next agent.**

**Phase 0 — Land this design + an ADR.**
Merge this document. Add `docs/decisions/0007-foundational-core-header.md`
recording the two-tier decision, the forbidden-list, and the PCH principle.
No code change.

**Phase 1 — Consolidate Base configuration (behavior-preserving).**
- Create `config.h` (Band 0) with OS + arch + build-flavor macros (moving the
  build-flavor block out of `defines.hpp` and the OS block conceptually up from
  `platform/config.h`).
- Rename `compiler.hpp` → `compiler.h`; fold `LUDUS_INLINE` into it from
  `defines.h`; add `LUDUS_LIKELY/UNLIKELY/DEBUG_BREAK/UNREACHABLE`.
- Delete `defines.h` and `defines.hpp`; re-home `DerivedFrom`/`nullptr_t`.
- Update the *few* current includers (`profiling.hpp` uses `defines.hpp`;
  `assert.hpp`/`pointer.hpp`/`vector.hpp` use `defines.h`/`compiler.hpp`) and the
  Base `CMakeLists` `FILE_SET` (also fixes the `defines.hpp` omission bug).
- Make `platform/config.h` a shim including the Base `config.h`.
- Gate: builds warning-clean, tests pass, ASan/UBSan clean, budgets unchanged.

**Phase 2 — Introduce `core.h`.**
- Add `core.h` including `types.h` + `config.h` + `compiler.h` + `assert.hpp`,
  plus `Move`/`Forward`. Do **not** yet touch call sites.
- Add the budget entry / forbidden-include check for `core.h` (§19).
- Gate: `core.h` parses well under the default budget; no heavy include leaks.

**Phase 3 — Make the foundational contract real (public headers).**
- Add `#include <ludus/foundation/base/core.h>` first in each engine **public**
  header that currently relies on `types.h` transitively, so the contract in
  §14.2 holds. This is mechanical and low-risk.
- Gate as Phase 1.

**Phase 4 — Explicit includes above the foundation line (`.cpp` sweep).**
- Run `./scripts/check --include-cleaner` (advisory) to find `.cpp` files that
  rely on transitive `types.h`/containers/etc.; add direct includes. Adopt
  `Move`/`Forward` where `std::move`/`std::forward` are used in new/edited code
  (not a gratuitous global replace).
- Migrate `apps/smoke/main.cpp` boilerplate to `#include
  <ludus/foundation/base/core.h>` + only the specific module headers it uses.
- Gate as Phase 1; confirm the include-ordering policy via `--format`.

**Phase 5 — Optional PCH (measurement-gated).**
- Add `ludus_apply_pch()` (Band 0+1) behind a CMake option defaulting **off**;
  measure with `./scripts/profile-build`; enable only if it wins; add the
  `no-pch` CI guard (§10.4).

**Phase 6 — Optional consistency cleanup.**
- Align `version.hpp` free functions to verb-first (`GetVersion()`, …) if the
  team agrees; this is cosmetic and can be dropped.

Each phase is a separate PR. Phases 1–4 are the core; 5–6 are optional.

---

## 19. Enforcement strategy

Layered, favoring cheap/measured gates over noisy hard gates (consistent with
ADR 0005):

1. **Build-time budget (hard gate, existing).** `config/build_budget.json` +
   the `Build-time budget` CI job. Add a tight budget entry for `core.h` (it
   should sit far under the 2000 ms default). This catches any heavy include
   sneaking into the foundation — the highest-value regression to prevent.
2. **Forbidden-include check (new, narrow hard gate).** A small script
   (`scripts/check-foundational-includes`, invokable from `./scripts/check`)
   that fails only on unambiguous violations:
   - `core.h` / Band-0 headers including any header on the heavy/forbidden list
     (§9) or any container/string header.
   - Any **public** header including a heavy STL header or a `src/internal/`
     private header.
   Keep it a simple text/AST scan; keep the rule set small to avoid ADR 0005's
   "too noisy to gate" failure mode.
3. **PCH-optional CI configuration (new).** Guarantees no TU relies on a symbol
   only the PCH provided (§10.4). Only meaningful once Phase 5 lands.
4. **Include-cleaner (advisory, existing).** `./scripts/check --include-cleaner`
   remains non-gating, used during the Phase 4 sweep and thereafter for hygiene.
5. **Convention docs (existing).** Update `AGENTS.md` with a short "include
   policy" section pointing here (foundational header contract, no transitive
   reliance above the line, umbrella rules). Naming stays by-example.

Deliberately **not** adopted: a hard IWYU/include-cleaner gate (ADR 0005 already
rejected it as too noisy), and a `readability-identifier-naming` clang-tidy gate
(the repo intentionally keeps naming by-example).

---

## 20. Validation / benchmark plan

Establish a baseline **before** any change, then re-measure after each phase.
All measurements use the pinned toolchain (Clang 18) and the CI-calibrated
runner referenced by `config/build_budget.json`.

**Metrics (from `./scripts/profile-build` + ClangBuildAnalyzer, per ADR 0005):**

1. **Total frontend parse time** (currently budgeted at 120 s). Must not
   regress; expected roughly flat after Phase 1–4 (the foundation was already
   arriving transitively, so making it explicit adds little).
2. **Per-header average parse time** for `core.h`, `types.h`, `config.h`,
   `compiler.h` — must stay well under 2000 ms; `core.h` is the key new number.
3. **Slowest single TU** and **`log.hpp`/`backend.hpp` figures** — must not
   regress (guards against the foundation accidentally pulling something heavy).
4. **Incremental-build probe (new):** touch `types.h` (or `core.h`) and measure
   rebuild fan-out; touch `log.hpp` and confirm the foundation change does *not*
   widen logging's blast radius. This directly validates goal 3.
5. **Preprocessed-expansion line count** for a representative TU that includes
   `core.h` (the same technique used for the logging redesign, e.g. `clang -E`),
   to confirm the foundation stays small (~target: `core.h` self-expansion in
   the low thousands of lines, not tens of thousands).
6. **PCH A/B (Phase 5 only):** clean-build wall time and summed frontend time
   with PCH on vs off; adopt only on a measured win, and confirm the `no-pch`
   build still compiles (correctness, not speed).

**Correctness gates (every phase, from `AGENTS.md`):** warning-clean build,
unit tests pass, ASan/UBSan clean, `./scripts/check --all` (format + tidy) pass.

**Acceptance for the whole effort:**
- A single documented header (`core.h`) provides the universal vocabulary, and
  every public engine header establishes it (§14.2).
- No container/string/heavy-STL include reachable from `core.h` (enforced §19).
- Total frontend parse time within budget; no per-header regression.
- `apps/smoke/main.cpp` (and engine TUs) express real module dependencies
  explicitly, with foundational boilerplate reduced to one `core.h` include.
- The build compiles identically with any PCH disabled.

---

## Appendix A — Mapping to external practice (why these fit Ludus)

- **Unreal Engine** separates `CoreTypes.h` (primitive types + platform/build
  macros, included first by Core headers) from `CoreMinimal.h` (a *curated*
  common-vocabulary umbrella), and treats monolithic `Engine.h`/`UnrealEd.h` as
  the anti-pattern its IWYU effort exists to escape. Ludus's `core.h` plays the
  `CoreTypes.h`/`CoreMinimal.h` role at Ludus's (smaller) scale; the god-header
  ban mirrors UE's IWYU motivation. *Fit: strong — same problem, same shape.*
  (Content rephrased from Epic's IWYU documentation for compliance;
  [Epic IWYU docs](https://dev.epicgames.com/documentation/unreal-engine/include-what-you-use-iwyu-for-unreal-engine-programming).)
- **Chromium** requires including the header that declares each used symbol and
  discourages relying on transitive includes and umbrella headers, for build-
  speed reasons at massive scale. Ludus adopts this *above the foundation line*
  but not the full "forward-declare everything" stance, because Ludus values
  cohesion and is far smaller. *Fit: adopt the rule, not the extreme.*
  (Rephrased from public
  [chromium-dev discussion](https://groups.google.com/a/chromium.org/g/chromium-dev/c/qtmTA3j5784).)
- **EASTL / Abseil / Folly** ship each container/utility in its own header with
  no god-header, and Abseil explicitly forbids depending on transitive includes.
  This validates keeping containers/strings in Band 2 and the §14 policy.
  *Fit: strong for the "no implicit heavy facilities" rule.*
- General guidance that gratuitous umbrella headers *increase* build time (text
  substitution) supports restricting umbrellas to single-module boundaries
  (§11). *Content was rephrased for compliance with licensing restrictions.*

Ludus deliberately does **not** cargo-cult any of these: it does not import
`std` names (it keeps `Move`/`Forward`/verb-first vocabulary), does not adopt
Chromium's forward-declaration mandate, and does not build a `CoreMinimal.h`
umbrella of module APIs — `core.h` is vocabulary only.

## Appendix B — Concrete "before / after" call-site sketch

Before (`apps/smoke/main.cpp`, abridged — 9-line ludus block + transitive types):

```cpp
#include <ludus/diagnostics/session.hpp>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/base/version.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>
#include <ludus/platform/base/window.h>
#include <string>
#include <ludus/graphics/rhi/rhi.h>
```

After (foundation explicit once; module deps still explicit and honest):

```cpp
#include <ludus/foundation/base/core.h>          // types, macros, config, assert, Move/Forward

#include <ludus/foundation/base/pointer.hpp>     // UniquePtr (opt-in, not in core.h)
#include <ludus/foundation/base/version.hpp>
#include <ludus/foundation/logging/logging.hpp>  // curated module umbrella (if added)
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>
#include <ludus/platform/base/window.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/diagnostics/session.hpp>

#include <string_view>                           // explicit: this TU formats text
```

The boilerplate for *universal* concepts collapses to one line; every *module*
dependency remains visible. That is the boundary this design targets.


---

## Implementation status (as built)

This section records what was actually implemented, and the two places the
as-built result deviates from the original §8/§18 sketch (both deliberate).

**Delivered**

- **Band 0 / Band 1 headers.** `modules/foundation/base/include/ludus/foundation/base/`
  now contains `core.h` (Band 1), `config.h` and `compiler.h` (Band 0).
  `core.h` provides the vocabulary contract: it includes `types.h` + `config.h`
  + `compiler.h` + `assert.hpp` and defines `Move`/`Forward`/`DerivedFrom`/
  `nullptr_t` in `ludus::foundation::core`, re-exported into `ludus::foundation`.
- **OS/arch detection moved into Base.** `config.h` owns
  `LUDUS_PLATFORM_WINDOWS/MACOS/LINUX/DESKTOP` and `LUDUS_ARCH_X86_64/ARM64`
  plus the build-flavor macros. `ludus/platform/config.h` is now a thin shim
  that includes the Base header and adds only windowing-backend classification.
- **`defines.h` / `defines.hpp` removed; `compiler.hpp` renamed to
  `compiler.h`.** The duplication and the missing-from-`FILE_SET` bug are gone.
- **New codegen macros** in `compiler.h`: `LUDUS_LIKELY`/`LUDUS_UNLIKELY`,
  `LUDUS_DEBUG_BREAK`, `LUDUS_UNREACHABLE`, and compiler-identity macros, next
  to the existing `LUDUS_INLINE`/`NOINLINE`/`COLD`.
- **Migration.** Containers, profiling, and platform headers/TUs migrated to
  explicit includes; the two accidental-transitive-include sites in the
  platform window headers (`LUDUS_INLINE`/`UniquePtr`) are fixed.
- **PCH.** `cmake/EnginePch.cmake` provides opt-in `ludus_apply_pch()`
  (`LUDUS_ENABLE_PCH`, default OFF) mirroring `core.h` only; a CI job builds
  with it ON while the default build is the no-pch guarantee.
- **Enforcement.** `ludus_header_self_sufficiency` and
  `ludus_foundational_includes` CTest gates; the include-boundary check also
  runs from `./scripts/check`.

**Deliberate deviations from the initial sketch**

1. **`core.h` is opt-in, not mandatory in every file.** The design's guiding
   idea is that `core.h` is what a file *may rely on*; it is not required
   boilerplate. A file that legitimately uses only the numeric types keeps
   including `types.h` directly; `profiling.hpp` includes just `compiler.h`
   (for `LUDUS_INLINE`) to preserve its deliberately minimal footprint rather
   than pull the assertion plumbing that full `core.h` carries; the smoke app,
   already include-what-you-use clean, was not given a `core.h` it would not
   use. This keeps §15's "establish the foundation, then widen" intent without
   adding unused includes (which would violate the very IWYU discipline §14
   asks for above the foundation line).

2. **Public headers include the specific Band-0 header they need**, not always
   the full `core.h`. `core.h` remains *the* contract a consumer may lean on;
   internal foundation headers that need only one Band-0 facility (e.g. the
   private `diagnostic_platform.hpp` needing `compiler.h`) include that facility
   precisely. This is stricter than "everything includes core.h" and is what the
   self-sufficiency and boundary gates actually enforce.

These deviations were validated against the two gates and the design's own
non-goal ("minimizing `#include` lines is not the objective").
