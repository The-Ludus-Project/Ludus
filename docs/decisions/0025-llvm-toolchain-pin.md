# ADR 0025: Pin the native reference toolchain to LLVM 23

## Status

Proposed. It takes effect when the maintainer accepts the toolchain bump into
`develop`. The first full batch run on that commit is the Linux qualification.

## Context

Ludus pins one LLVM major for native compilation, formatting, clang-tidy, the
static analyzer and sanitizers (`config/tool_versions.json`). Scripts and CMake
gates read the major from `minimum.clang`. Copies that cannot read that file
must agree with it, and `test_ci.ToolchainPinTests` checks them.

LLVM 18 stopped working on current macOS hosts:

- On macOS 26 and newer, compiler-rt before 22.1.3 hangs (ASan) or crashes
  (TSan) before `main`. The [macOS sanitizer runtimes](../development/building.md#macos-sanitizer-runtimes)
  section describes the cause. Thanks to Andrew Haberlandt for the upstream fix,
  [llvm/llvm-project#182943](https://github.com/llvm/llvm-project/pull/182943),
  which uses `_dyld_get_dyld_header()` and was backported to LLVM 22.1.3.
- Clang 18 cannot compile against the macOS 27 SDK, which current Command Line
  Tools select by default: the SDK's math headers need a `NAN` definition that
  Clang 18 lacks.

We measured LLVM 22.1.8 and 23.1.3 on an Apple M5 running macOS 26.6 with
SDKs 26.5 and 27.0:

| | LLVM 22.1.8 | LLVM 23.1.3 |
| --- | --- | --- |
| ASan+UBSan and TSan start-up, macOS 26.6 | passes (SDK 26.5) | passes 10 of 10 runs (SDK 26.5 and 27.0) |
| macOS 27 SDK | libc++ `<cfloat>` is empty after `<cmath>` (C++23), so Catch2 fails | init and full build succeed |
| New compiler warnings | `-Wc2y-extensions` at every Catch2 `TEST_CASE` | one `-Wunused-but-set-global` |
| clang-format changes | 14 files | 15 files |
| clang-tidy configuration | valid | `hicpp-exception-baseclass` removed |
| `macos-clang-asan-ubsan` tests | 81 of 83 | 81 of 83 (same two, neither caused by the sanitizers) |

The libc++ 22 failure comes from libc++'s `float.h` wrapper. It sets its include
guard on the SDK's partial `__need_infinity_nan` include. LLVM 23 removes that
wrapper; thanks to Ian Anderson, [llvm/llvm-project#175849](https://github.com/llvm/llvm-project/pull/175849).
The init SDK probe now also checks `<cfloat>` after `<cmath>`, whichever LLVM is pinned.

## Decision

Pin LLVM 23.1.x, at least 23.1.3, for every native tool. The pin covers
`tool_versions.json` minimums, the ludus-tools mirror, Conan profiles, example
presets, the Behavior native compiler major, CI, the devcontainer and setup.
Managed Conan moves to 2.33.0, the first release whose `settings.yml` lists clang 23.

- **Ubuntu 24.04.** Its archive stops before LLVM 23, so CI, the devcontainer and
  init register the pinned `llvm-toolchain-<codename>-23` suite from apt.llvm.org.
  The archive key is committed as `config/apt/apt.llvm.org.asc`, and
  `scripts/python/llvm_apt_source.py` checks its fingerprint
  (`6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`) before apt trusts it. Setup
  never downloads a key. Thanks to the LLVM project for the
  [apt.llvm.org packages](https://apt.llvm.org/).
- **macOS.** Setup and CI install `llvm@23` and `lld@23`; Homebrew packages LLD
  separately. Until Homebrew versions LLVM 23, both are aliases of the
  unversioned formulae. Init therefore rejects an alias that `brew upgrade`
  moved to another major.
- **clang-tidy.** `hicpp-exception-baseclass` is gone; its successor,
  `bugprone-std-exception-baseclass`, comes in through `bugprone-*`.
  `portability-avoid-pragma-once` contradicts the `#pragma once` convention and
  is disabled. `bugprone-signed-bitwise` is disabled: on macOS it reported 348
  sites, and the 49 left after ignoring positive literals were all integer
  promotion of unsigned bytes or C API flag macros defined as signed `int`.
  Other new findings are fixed or suppressed at the site with a reason.
- **Formatting.** clang-format 23 differs from 18 on a few constructs, so the
  repository is reformatted in a single commit alongside the pin.

## Consequences

- Local macOS 26 developers can run the sanitizer presets and use the default SDK.
- Ubuntu hosts depend on a third-party apt source. apt.llvm.org serves only the
  newest 23.1.x build, so the exact patch version follows its releases.
- Installed SDKs and game projects record the exact compiler. They must be rebuilt
  or repaired after the bump. CI compiler and Conan caches start cold.
- Behavior native profiles are admitted per compiler major, so their qualification
  evidence must be re-run with Clang 23.
- `config/build_budget.json` keeps its LLVM 18 measurements until an idle host
  re-measures them.
- Not measured before this decision: Linux builds, tests and tidy with Clang 23;
  Vulkan and Wayland translation units; the Qt editor; the SDK consumer; and the
  devcontainer image build. The first CI run of the bump qualifies them.
