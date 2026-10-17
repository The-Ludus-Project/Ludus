# Platform targets and compatibility

Use this guide when selecting an SDK, diagnosing a cross build, or writing a
platform-specific implementation. Ludus detects the target of each C++
translation unit from the compiler and target SDK. It does not detect the build
machine or promise that a device, compositor or browser feature is available.

## Keep the four sources of information separate

| Question | Authority | How to use it |
| --- | --- | --- |
| Which OS, CPU, pointer width and byte order is this code compiled for? | Compiler target and target SDK | FoundationBase `config.h` and the opt-in `target.hpp` descriptor |
| Which compiler frontend and extensions compile this code? | Compiler predefined macros and feature queries | `compiler.h`; use feature queries for builtins and attributes |
| Which build flavor and backend are included? | Build configuration and owning module | Selected preset/SDK and module configuration |
| Which devices and features can this running game use? | Successful subsystem initialization and queries | Live RHI, windowing, audio and other subsystem results |

For example, a Linux x64 Editor can build an Emscripten wasm32 game. The Editor's
compiled target describes the Editor; the game's compiled target describes the
game. `LUDUS_OS_WEB` identifies the Emscripten environment, including Node runs;
it does not identify the player's browser or establish DOM/WebGPU availability.

The Editor's **Executable target** field is a CMake executable target name,
not an OS or CPU selection.

## Read target facts in C++

Include the descriptor explicitly; `core.h` does not include it:

```cpp
#include <ludus/foundation/base/target.hpp>

static_assert(ludus::foundation::kTarget.PointerBits == sizeof(void*) * 8);

constexpr const char* kOsName =
    ludus::foundation::TargetOsName(ludus::foundation::kTarget.Os);
constexpr const char* kArchName =
    ludus::foundation::TargetArchName(ludus::foundation::kTarget.Arch);
```

`kTarget` contains `Os`, `Arch`, `Compiler`, `Endian` and `PointerBits`.
It is immutable, requires no runtime probe or allocation, and describes the
current translation unit. `TargetOsName` and `TargetArchName` return static
strings suitable for diagnostics. Its classification values are not a serialized
format or an SDK ABI compatibility certificate.

Pointer width is a compiler data-model fact; do not infer it from the CPU name.
Native byte order likewise does not define a file/network format. Specify fixed
widths and byte order explicitly at serialization boundaries; FoundationBase's
opt-in `byte_order.hpp` provides bounded codecs.

## Select an implementation boundary

Use centralized numeric selectors for new preprocessor decisions:

```cpp
#include <ludus/foundation/base/config.h>

#if LUDUS_TARGET_OS == LUDUS_OS_LINUX
// Desktop Linux implementation boundary.
#endif
```

Other selectors are `LUDUS_TARGET_ARCH`, `LUDUS_TARGET_COMPILER`,
`LUDUS_POINTER_BITS` and `LUDUS_TARGET_ENDIAN`. Compiler identity comes from
`compiler.h` (also included by `target.hpp` and `core.h`). Keep OS APIs private to
their owning implementation rather than spreading platform branches across
callers. Use `-Wundef` to catch misspelled numeric selectors.

Existing `LUDUS_PLATFORM_*`, `LUDUS_ARCH_*` and `LUDUS_COMPILER_*` identity flags
are defined to `1` only when active; inactive flags remain undefined. Existing
`#ifdef` callers retain their meaning. Clang-cl is a Clang frontend with separate
Microsoft ABI compatibility (`LUDUS_COMPILER_MSVC_ABI`). Query extension support
through `LUDUS_HAS_BUILTIN`, `LUDUS_HAS_ATTRIBUTE` or `LUDUS_HAS_CPP_ATTRIBUTE`
rather than inferring it from a compiler name.

Do not define detection outputs or compiler-owned OS/CPU macros yourself.
Select a real compiler target, toolchain and matching SDK instead. Build flavor
and backend inputs belong to the build system, separately from detected facts.

## Recognition is not engine support

| Target or toolchain | Current scope |
| --- | --- |
| Ubuntu 24.04, Linux x64, Clang/LLVM 23 | Native engine reference environment |
| Pinned Emscripten, wasm32 | Browser Foundation, Platform and RHI paths plus smoke samples; browser/device acceptance is separate |
| Windows, macOS, Android and iOS; ARM64; Emscripten wasm64 | Recognized classifications, not validated engine deployments |
| GCC and MSVC frontend dispatch | Synthetic dispatch tests, not native engine toolchain support |

Not every combination of a recognized OS and architecture is a supported target.
The compiler matrix includes real Clang target triples, but Apple cases use a
minimal target-conditionals fixture rather than a real Apple SDK. ARM64 big
endian and x86_64's x32 pointer model are detection cases, not shipped engine
profiles. ARM64EC, 32-bit native CPU architectures, WASI and unrecognized OS families fail
explicitly. Emscripten wasm64 recognition does not enable the shipped wasm32
presets for memory64.

Platform backend checks remain separate: Wayland requires desktop Linux and the
canvas backend requires Emscripten. The X11 flag is reserved, not an implemented
backend. A compiled Wayland backend does not prove a compositor is reachable;
a recognized CPU does not authorize AVX instructions. Read live subsystem
results for actual availability. RHI capabilities describe the initialized
session and are available only while it is Ready.

See [current capabilities](../getting-started/status.md) and the
[integrated browser acceptance evidence](../../development/webgpu-w6-evidence.md)
for the scope of implemented workflows.

## Recover from target and SDK errors

| Diagnostic or symptom | Next step |
| --- | --- |
| `compiler target disagrees with the configured SDK target` | Check the selected compiler/toolchain, CMake preset and SDK. Select an SDK built for the intended target OS and repair stale setup. |
| `target detection outputs must not be overridden` | Remove manual `LUDUS_TARGET_*`, pointer/endian or identity-flag definitions. Select the intended compiler target instead. |
| Unsupported OS, CPU, Apple target, pointer width or byte order | Check for an accidental target selection. If intentional, this needs a reviewed port and tests; redefining macros does not supply a backend. |
| Invalid build flavor or incompatible backend selection | Correct the preset/module build inputs; keep exactly one valid flavor and a backend supported by that target. |
| Target compiles but graphics/window/audio startup fails | Inspect the owning subsystem's startup result and confirmed runtime environment; the compiled OS/CPU is not an availability test. |

The exported FoundationBase usage requirements currently check expected **OS**
against compiler detection. That guard does not by itself certify matching CPU,
pointer width, byte order or every ABI property. Keep SDK/toolchain compatibility
checks and a real consumer build as part of setup verification.

Use **Project → Check Setup**, then explicit **Repair Project Setup** with the
intended engine if inputs are missing or stale. The
[project setup guide](../../development/project-sdk-workflow.md) covers the corresponding CLI operations.
Do not reuse a CMake cache from another target. Record host OS and intended game
target separately in a [troubleshooting report](troubleshooting.md#gather-a-useful-report),
including SDK identity, compiler/toolchain, profile and the first useful error.

## Contribute a new target

Use the [platform porting checklist](../../development/platform-porting.md) to
track subsystem owners, blockers and acceptance evidence. The
[mobile support plan](../../development/mobile-support-plan.md) sequences the
shared contracts and parallel Apple/Android implementation tracks.

Change detection and its positive/negative compiler matrix together. Then add
backend source selection, real target SDK/consumer builds and runtime acceptance
as separately verified work. A synthetic fixture or successful header compile
alone does not establish engine support. Follow the
[platform design and consulted-source review](../../architecture/platform-detection.md)
and [ADR 0016](../../decisions/0016-platform-detection.md).

## References and adopted ideas

Thanks to **David Etherton**, “Designing and Maintaining Large Cross-Platform
Libraries,” _Game Programming Gems 4_, chapter 1.4, pp. 35–41: sparse platform
conditionals and explicit size/byte-order assumptions informed the centralized
selectors. Ludus preserves active-only compatibility flags and does not adopt a
forced universal include.

Thanks to **Jason Hughes**, “What to Look for When Evaluating Middleware for
Integration,” _Game Engine Gems 1_, chapter 1, sections 1.10–1.13, pp. 10–12:
small integration boundaries and visible portability assumptions informed the
opt-in descriptor. Target identity remains separate from serialization and
runtime capabilities.

The [detailed source review](../../architecture/platform-detection.md#reference-review-and-revisions)
records the consulted chapters and departures. See the
[platform reading path](../learn/references.md#6-understand-platform-boundaries)
for the primary compiler/toolchain references. These are design influences;
Ludus's implementation and tests define the shipped behavior.
