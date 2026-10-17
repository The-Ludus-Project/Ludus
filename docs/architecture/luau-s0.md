# S0: private Luau interpreter feasibility

Status: first S0 implementation, based on Ludus `d13b4f8`. Production adoption
remains gated. This experiment implements no public scripting SDK, behavior
provider, language-neutral manifest, graph editor, C# runtime, or live reload.
See [the architecture](scripting.md) for those subsequent contracts.

## Scope and reproducibility

`LUDUS_BUILD_LUAU_PROBE` defaults to OFF. Ordinary configure, build, install and
SDK consumers neither download nor require Luau. Enabling the option explicitly
requires prepared sources and cooked fixtures; missing, edited or stale inputs
fail configuration and the probe's build dependency. Luau targets have no Ludus
install/export rules.

The [toolchain record](../../config/luau_toolchain.json) pins compiler and VM to
`1eca9fda3e4753a1592000f6cfdf659aaa778b7d`, archive SHA-256
`5b93ecef638a485065f042be1f48436e2d835deb9cdea7ccdccbfaa467e74a48`.
The experimental profile is `s0-interpreter-longjmp-loader-lifetime-v2`.
The compiler uses optimization level 1 and debug level 2. Fixture `--!strict`
annotations document intent; this spike does not yet implement generated API
definitions or a type-checking acceptance gate.

```sh
./init.sh --cli linux-clang-development --preset-only --locked --with-tests --no-system-install
./scripts/luau-probe bootstrap
./scripts/luau-probe cook
./scripts/luau-probe run --preset linux-clang-development
./scripts/luau-probe check --preset linux-clang-development

./init.sh --cli linux-clang-asan-ubsan --preset-only --locked --with-tests --no-system-install
./scripts/luau-probe run --preset linux-clang-asan-ubsan
python3 -m unittest discover -s tools/luau-probe -p 'test_*.py' -v
```

Bootstrap is the explicit network action. It verifies the archive, reconstructs
the pinned source plus the checked loader, assertion-helper and analyzer-include
edits, and builds upstream `luau-compile`
in a separate host build. Upstream compiler/CLI exception handling stays in that
process; no Ludus code is compiled there. The engine probe links VM and Common
only, both using the Ludus no-exception and sanitizer policy. No compiler,
checker, require implementation or native code generator is linked into it.

Cook generates private bytecode arrays and a manifest under `out/luau-probe`.
Source, pin and generated header fingerprints invalidate stale inputs. Verify
compares every extracted upstream file with the verified archive plus the exact
profile transformations. This detects accidental edits; these local fingerprints
are not an authenticity boundary against a developer controlling the checkout.
Only trusted bytecode from the paired compiler enters the VM.

The host analyzer's pinned `Analysis/src/TableLiteralInference.cpp` uses integer
`std::abs` without including its declaring header. Bootstrap adds `<cstdlib>`
at one checked include anchor so the analyzer builds with libc++ 23. Thanks to
[Roblox/Luau's source at the pinned revision](https://github.com/luau-lang/luau/blob/1eca9fda3e4753a1592000f6cfdf659aaa778b7d/Analysis/src/TableLiteralInference.cpp).
The source body and upstream license are preserved. Changed anchors, an already
patched upstream file, and extracted bytes differing from the checked transform
are rejected. This host-tool include correction keeps the v2 VM profile.

For Web, prepare the SDK pinned in `config/web_toolchain.json` using the existing
browser bootstrap workflow, then run:

```sh
./scripts/luau-probe run --preset web-emscripten-development
./scripts/luau-probe run --preset web-emscripten-release
# After npm ci and Playwright Chromium installation in tools/web-browser-tests:
node tools/luau-probe/browser.mjs out/build/web-emscripten-release/tools/luau-probe
```

The run command disables unrelated Web smoke/probe targets in that build cache.
Reconfigure those targets with their ordinary scripts when returning to them.
It enables Info logging in Release/Profile probe configurations so verification
can observe each case; this is a diagnostic probe configuration, not the engine's
ordinary Release logging profile.
The wasm profile uses `SUPPORT_LONGJMP=wasm`; its implementation requires wasm
exception-handling support, while every VM/host C++ translation unit still uses
`-fno-exceptions`. The VM does not use Luau's stock exception-enabled Web demo.
The SDK's Node execution and actual Chromium execution are separate checks.
Windows, macOS, Android and iOS have no acceptance evidence from this slice.

## Lifetime audit and loader profile

Thanks to Roblox Corporation and Lua.org/PUC-Rio for Luau's MIT-licensed VM,
particularly [ldo.cpp](https://github.com/luau-lang/luau/blob/1eca9fda3e4753a1592000f6cfdf659aaa778b7d/VM/src/ldo.cpp),
[lapi.cpp](https://github.com/luau-lang/luau/blob/1eca9fda3e4753a1592000f6cfdf659aaa778b7d/VM/src/lapi.cpp),
[lstate.cpp](https://github.com/luau-lang/luau/blob/1eca9fda3e4753a1592000f6cfdf659aaa778b7d/VM/src/lstate.cpp),
[lvmload.cpp](https://github.com/luau-lang/luau/blob/1eca9fda3e4753a1592000f6cfdf659aaa778b7d/VM/src/lvmload.cpp),
and the debugger examples in `tests/Conformance.test.cpp` at that revision.
The host probe adapts the documented protection/debugger contracts; it does not
copy an engine-object binding implementation. Both inherited MIT notices are
preserved in acquired sources and copied beside probe artifacts.

The [C++ nonlocal-jump rule](https://eel.is/c++draft/csetjmp.syn) forbids crossing
automatic objects whose nontrivial destruction would be required by equivalent
exception unwinding. The audit is therefore static as well as dynamic:
sanitizers and an allocation counter cannot alone prove this language rule.

| Region | Ownership and recovery decision |
| --- | --- |
| `luaD_rawrunprotected` | Explicit longjmp mode uses a jump buffer and volatile status; never compile the exception branch. |
| `lua_newstate` | Upstream protects its allocating initialization and closes partially created state. Sweep rejection during this path. |
| `lua_cpcall` | Protects stack growth and C-closure creation before entering the host callback; use it for setup and ordinary execution. |
| `Initialize`, `Execute`, `HostAdd`, `Interrupt` | Only scalars, pointers and trivially destructible contexts. No locks, containers, scope guards, logging, native resource owners or reentrant engine calls. |
| `luau_load` outer scope | `ScopedSetGCThreshold` and the original string/prototype `TempBuffer` owners sit outside its internal protected call, so normal return destroys them. Pre-load GC is covered by the outer host protection. |
| Constant-table deserialization | Original `TempBuffer<int32_t> nilKeys` sits inside `loadsafe`, below the protected boundary. An allocation failure can exit its lifetime by longjmp. Move it into `LoadContext`, pass a reference into `loadsafe`, and release/reset it before another table. Its destructor then runs after protected recovery. |
| Debugger callbacks and resumes | POD-only callbacks, a registry-rooted coroutine, bounded local inspection, and resume status handling. No callback-owned native resources. |
| Allocator and destruction | Fallible C realloc/free, explicit byte accounting and an 8 MiB cap. Rejected realloc preserves the original block. No Lua calls in the allocator; no custom userdata finalizers in this profile. Always close a failed VM. |
| Common and VM helpers | Reviewed VM nontrivial owner declarations and STL usage; the loader owners above are the relevant crossed-frame finding. Common time-trace RAII is disabled. Future libraries, userdata callbacks and native execution require a new audit. |

`scripts/luau-probe` applies seven exact, checked substitutions to the loader;
an upstream pin change fails closed rather than applying an approximate patch.
It preserves the upstream notices and constructor/destructor ownership model.
This is a source-audit finding, not a claim that ASan diagnosed longjmp UB or that
every unmodified allocation fail point necessarily leaks. The unmodified pin is
not approved for production. Before S1 adoption, decide whether to obtain an
upstream fix, change the reviewed pin, or explicitly maintain this small profile.

### Sanitizer function-prefix profile

Q0 fatal UBSan exposed a Clang 18 Mach-O prefix/coalescing problem at VM close.
The callback in `lgc.cpp` and indirect call in `lmem.cpp` have matching signatures
and object-file type hashes. The preceding shared noinline `assertCallHandler`
is emitted as a coalesced inline function; its atom also contains the following
local callback's sanitizer prefix. Selecting another translation unit's helper
can therefore give the callback a different linked prefix and a false
function-type-mismatch finding. Both Apple ld and LLVM 18 ld64.lld reproduced it.

Thanks to Roblox/Luau's
[Common.h at the pinned revision](https://github.com/luau-lang/luau/blob/1eca9fda3e4753a1592000f6cfdf659aaa778b7d/Common/include/Luau/Common.h)
for the existing assertion contract. The v2 profile gives only
`assertCallHandler` internal linkage with `static inline`; its body, noinline
attribute, shared handler storage, licenses and diagnostics are preserved.
`scripts/luau-probe` applies one exact checked substitution and source verification
includes the transformed header. The compiler/VM revision and archive checksum
remain pinned; the profile fingerprint invalidates old cooked evidence.

A Clang 18 UBSan-only VM create/close reproducer aborted before this edit and
exited zero afterward. Required macOS Behavior and safety acceptance exercises
VM close with ASan/UBSan; no sanitizer check or recovery policy is disabled.

## Executed contracts

The harness runs the same cooked fixtures in native and wasm interpreters:

- Checked scalar binding, result 42, and absent os/io/debug/require/print access.
- Protected script error and invalid binding argument; neither escapes the VM.
- Recursive script stack overflow and an infinite loop stopped at 64 interpreter
  safepoints. This is a cooperative safepoint count, not an instruction count or
  a hard wall-clock limit; a long native call is not preempted.
- Allocation rejection starting at every allocator call in a successful baseline,
  spanning state creation, library setup, bytecode loading and table allocation.
  Each rejection must report failure and leave zero bytes after close.
- Two constant-table templates exercising the loader scratch-owner path.
- A source breakpoint at line 3, local value 20, static generated-node mapping to
  node 1001, single-step to line 4/value 42, and successful continuation to 42.
  This proves debugger primitives, not a DAP server or general graph source map.
- A 10,000-call scalar dispatch batch and a successful fresh VM after failures.

The error policy for this first experiment destroys each failed VM. Reusing a
failed VM, preserving game state, staging commands, generation-checked entity
handles and provider fault isolation belong to subsequent integration slices.
No hard sandbox claim follows from exposing only a subset of base functions.
In particular, production cancellation must address script-protected calls and
native callback costs, and numeric bindings need their declared finite/range rules.

Every semantics run writes `evidence.json` beside the executable, including pin,
host, output, executable/wasm byte counts and hashes. Output includes allocator
fail-point counts, heap peaks and dispatch-batch time. Native file size includes
debug symbols and FoundationLogging; Web file size includes its runtime glue and
Foundation dependencies. Neither is an incremental production VM-size result.
Batch time includes VM creation, setup, loading and teardown; it is not a pure
call benchmark or evidence that a game meets the proposed 0.83 ms tick budget.

CI adds these checks to native Development, ASan/UBSan and the existing browser
workflow. Browser artifacts retain licenses and evidence; the Chromium job
executes the exact Release probe output. Tooling tests cover corrupt archives,
path traversal/symlinks, source edits and stale fixture/pin invalidation.

## Remaining S0 acceptance work

The first implementation makes the feasibility question reproducible. S0 remains
in progress until the pin/profile decision is accepted, useful workload and
incremental-size budgets are measured on reference hardware, target device builds
are recorded, and the production library/callback surface receives its own audit.
S1 then introduces the smallest real Luau/native-equivalent interaction using
the shared gameplay manifest. C# and a visual editor remain separate deliveries.

The scalar argument checks also adopt the boundary-validation idea from Waldemar
Celes, Luiz Henrique de Figueiredo and Roberto Ierusalimschy, **“Binding C/C++
Objects to Lua”**, *Game Programming Gems 6*, §4.2, pp. 341–355, as recorded in
the [source review](scripting-gems-review.md). Unlike that chapter's ownership
examples, this probe exposes no raw engine-object pointer or GC-owned entity.
