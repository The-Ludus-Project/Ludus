# S6: Behavior safety and platform qualification

**S6 is in progress.** This slice implements shared target admission, a production
provider safety suite, and recorded native/Node/Chromium evidence. It does not
complete the six-platform milestone. Windows, Android and iOS need engine
backends; physical-device qualification and broader browser coverage are pending.
[S4](behavior-s4.md) defines the installed contract/cooker, and [S5](behavior-s5.md)
defines the Editor and GameHost workflow. S7 remains representative project,
performance, designer and release acceptance.

## Admission and evidence

`config/behavior_profiles.json` is the shared interpreter admission policy. The
producer and installed `LudusConfig.cmake` evaluate the same helper before engine
or SDK dependencies are loaded. Installed consumers must match the producer's
system and architecture. The exported target also asserts the compiler's actual
architecture in the public header. `LUDUS_SKIP_TOOLCHAIN_CHECK` cannot bypass this
Behavior gate. Qualification admission permits a build; it does not establish
that it has run on a device.

| Target | Admission | Required execution evidence |
| --- | --- | --- |
| Linux x86_64 / arm64, Clang 23 | Qualification builds | Native production-provider suite, ASan/UBSan, relocated SDK and extracted player; CI currently exercises x86_64 |
| macOS x86_64 / arm64, upstream Clang 23 | Qualification builds, one architecture per SDK | Native production-provider suite, ASan/UBSan, relocated SDK and extracted Mach-O player; macOS CI records its actual CPU |
| Web wasm32, pinned Emscripten LLVM 22 | Qualification builds | Development/Release Node and headless Chromium execution of the same hashed Wasm; physical browsers/devices and additional browser engines pending |
| Windows x86_64 / arm64 | Rejected: engine backend pending | Native Foundation/backend build and execution, SDK/player, sanitizer and debugger acceptance pending |
| Android arm64 | Rejected: engine backend pending | NDK/native engine profile, physical device execution and packaging pending |
| iOS arm64 | Rejected: engine backend pending | Apple mobile engine profile, signed physical-device execution and packaging pending |

Other systems, compilers, compiler majors, architectures, 32-bit native targets,
wasm64, and macOS universal SDKs are rejected. The pinned Web SDK remains separate
from reference Clang 23. A policy update requires both implementation review and
new execution evidence; adding a name to this table is insufficient.

## Execute the safety suite

Use a prepared pinned toolchain and opt-in paired Luau tools:

```bash
./scripts/script-provider bootstrap
./scripts/script-provider run --preset linux-clang-development
./scripts/script-provider check --preset linux-clang-development
./scripts/script-provider run --preset linux-clang-asan-ubsan
./scripts/script-provider run --preset web-emscripten-development
./scripts/script-provider run --preset web-emscripten-release
node tools/script-provider/browser.mjs out/build/web-emscripten-release/tools/script-provider
```

Browser execution requires the existing pinned Playwright installation under
`tools/web-browser-tests`; it does not download tools implicitly. macOS CI uses
`macos-clang-development` and `macos-clang-asan-ubsan`, with init's upstream Clang
shims and paired macOS SDK/libc++ headers. Host compiler/analyzer executables remain
separate processes that can use upstream exceptions; they never enter the player
link closure. Explicit bootstrap refreshes their CMake cache when paths change.

CTest's `ludus_behavior_safety` executes the public `LuauProvider`, not a mock or
private test bridge. The twelve named scenarios cover:

- Exact frozen global inventory; read-only configuration, events and engine API.
- Fractional, NaN, infinite, negative, overflow and contract-range integers;
  forged entity values; boolean conversion; every entity identity dimension.
- Interrupt and stack-fault retirement, expired retained state facades, and a
  shared safepoint budget across debugger resumes.
- Bounded package-metadata truncation with active-package retention.
- Native service resource completion and recursive provider-call rejection.
- Every allocation point observed in representative load/invoke and debugger
  runs, and every candidate-load point while an active provider exists.

A separate private allocator contract test forces shrink/equal-size/capacity reuse
under persistent failure, checks copied growth, exact physical freeing, header
charges and overflow rejection. Its private header is not installed.

The acceptance-only executable replaces FoundationMemory's fallible aligned
allocation boundary. Each sweep rejects the selected allocation and every later
allocation, then checks the explicit status, unchanged caller state/effects,
VM retirement or active-package retention, and successful closure. Counting and
injection are enabled only around serial provider operations. There are no public
SDK injection hooks. Provider metadata/native allocations outside this boundary
are not covered by these VM sweeps; this is not an exhaustive heap fault campaign.

`verify_safety.py` requires all scenarios, actual compiled target identity,
nonzero sweep counts, a successful exit, and exception-free compile commands for
Behavior, Scripting, Luau VM and Common. It writes `safety-evidence.json` beside the
executable with input/artifact/compile-command hashes and measured counts. Missing,
duplicated, unknown or contradictory evidence fails the gate. Counts are observed
per artifact rather than promised across compiler versions.

The browser runner verifies that downloaded JS/Wasm hashes match Node's evidence,
executes the same scenarios and counts in Chromium, and writes
`out/browser-qa/results/behavior-s6.json` with browser version and logs. CI publishes
native, sanitized and Wasm evidence. Synthetic CMake policy tests prove rejection
logic; they are never recorded as physical-device execution.

## Nonlocal-jump and resource audit

The interpreter uses Luau's longjmp recovery, with production translation units
compiled without C++ exceptions and no native code generation. Thanks to Roblox /
Luau for the exact pinned source reviewed here:
[revision 1eca9fda](https://github.com/luau-lang/luau/tree/1eca9fda3e4753a1592000f6cfdf659aaa778b7d).
The [S0 loader review](luau-s0.md) remains the source of the exact, fail-closed
`TempBuffer` lifetime patch. Its scratch owners, including `nilKeys`, live outside
`luaD_rawrunprotected`'s jump region; their destructors run after recovery. This
slice does not change that patch or admit other upstream revisions.

The production boundary was reviewed with these rules:

| Boundary | Ownership and recovery rule |
| --- | --- |
| Runtime protected dispatch and continuations | Context/identity/value locals across VM operations are trivially destructible; owning strings, records and provider objects live in the caller or runtime outside the protected region |
| Behavior service trampoline | Copied EntityRef/Value/Record/Services/CommandResult/Transaction representations have static assertions for trivial destruction; the service returns and destroys its own resources before any subsequent VM allocation/error |
| VM allocator | FoundationMemory domain accounting uses explicit failure; retained-capacity headers make shrink/equal-size/reuse infallible; physical accounting includes headers, retained capacity and old+new growth overlap; actual upstream allocation denial and configured quota denial both increment the denial counter |
| Bytecode load | Upstream `luau_load` can recover OOM internally and return a loader error; the runtime preserves its allocation-denial cause before `lua_error` translates the outer status, so it reports AllocationFailure rather than ScriptFault |
| Debug frames, locals and breakpoints | Borrowed VM information is copied into bounded native snapshots; no language callback, getter or formatter executes during inspection; native snapshot owners stay outside protected dispatch |
| Entity and state userdata | Payloads are copied POD identities/facades with no native owning destructor; generations and live facade validity are checked at use |
| Host service calls | Synchronous, bounded, noexcept; recursive Load/Invoke/Close/debug operations reject entry while the runtime is active; a service must not invoke raw VM APIs or escape its own native resources |

ASan/UBSan and allocation sweeps supplement this lifetime audit. They cannot prove
the C++ rules for nonlocal jumps. Any new C callback, upstream update or userdata
owner requires another frame/resource review and meaningful failure tests.

The global environment is now an explicit copied allowlist instead of upstream
base globals followed by a denylist. It contains `assert`, `error`,
`getmetatable`, `ipairs`, `next`, `pairs`, `rawequal`, `rawget`, `rawlen`, `select`,
`tonumber`, `tostring`, `type`, `typeof`, `_VERSION`, `_G`, the frozen generated
engine API, and literal-catalog `require`. The global table and exposed values
are sandboxed/frozen. Upstream additions cannot silently grant another facility.
Protected calls, coroutines, raw writes, environment mutation, native-pointer
exposure, filesystem/network/process access and dynamic source compilation are
absent. A future library expansion is a capability and failure-path review.

## Remaining qualification limits

Packages are admitted only from the paired, owner-controlled source cooker.
Digest checks establish integrity and contract identity, not authenticity or a
safe arbitrary-bytecode verifier. Metadata rejection tests do not execute mutated
bytecode. This provider is not an untrusted plug-in or process-isolation boundary.

The heap cap covers each VM; candidate replacement can temporarily hold both old
and candidate heaps plus native metadata. Safepoints count interpreter interrupt
checks, not instructions or elapsed time. Native services are not preemptible and
must provide their own bounded work. Debug stops retain the same invocation budget.
These constraints must inform project budgets and S7 measurements.

To finish S6, implement the rejected engine target backends and SDK/toolchain
profiles; execute and retain equivalent native/player/debugger evidence for each
supported architecture; add physical Android/iOS and desktop hardware runs; and
qualify the declared browser engines on physical devices. Review allocator,
resource and platform-specific ABI/toolchain paths on each new target before
changing admission. No six-platform completion claim is made by this slice.
