# Mobile support implementation plan

Status: proposed implementation sequence, 2026-10-12. No mobile deployment is
qualified by this plan. The baseline audit is `850e48e`; recheck it before each
implementation slice. Maintain this page as the mobile milestone owner and use
the [platform porting checklist](platform-porting.md) for subsystem coverage and
evidence. Existing API comments and module designs remain contract owners.

## Goal and initial scope

Run one shared Ludus game on an iPhone 16 Pro, iPad Pro 11-inch (M4), and the
owner's Galaxy Flip. The reported name “Galaxy Flip V” has not been resolved to
an exact model: record its model number, OS build and GPU during M0 instead of
assuming a generation or Vulkan feature set. All three devices are available
for acceptance; none has been tested for this plan.

Start Apple and Android implementation together after the shared M0 contracts
are agreed. Apple uses an iOS/iPadOS UIKit host with Metal; Android starts with
an ARM64 GameActivity host and Vulkan. These are proposed integration choices,
not claims that the desktop backends already work on phones. Qualify real
devices first; Apple Simulator and Android emulator builds are separate profiles.
Mac Catalyst, other Android ABIs, the Flip cover display, stylus features, mobile
editor hosting and store distribution are outside the first deployment gate.

The desktop editor remains the authoring host. Mobile games statically link
game code initially; desktop shared-module live reload is not a mobile runtime
requirement. Include interpreted behavior and cooked content in later gates.

## Baseline blockers and reuse candidates

| Area | Observed baseline | Port action |
| --- | --- | --- |
| Target identity | Foundation recognizes Android/iOS; `config/behavior_profiles.json` marks both pending | Preserve detection; admit profiles only with build and runtime evidence |
| FoundationBase | Its CMake guard accepts Linux/Darwin or Emscripten with Clang | Add explicit mobile assertion/emergency/debugger boundaries and compiler qualification |
| Platform | Cocoa is selected only for Darwin; other native targets can reach the null path | Add explicit UIKit and Android selections; mobile shipping configuration must reject an accidental null backend |
| Metal | RHI's broad `APPLE` branch links AppKit and calls the Cocoa configuration helper | Separate macOS from iOS build selection and native view ownership; reuse portable Metal resource code only after audit |
| Vulkan | Presentation accepts Wayland descriptors; current retirement uses maintenance1 features | Add Android surface ownership and device feature negotiation; establish safe retirement before presenting |
| Filesystem | Native storage/persistence selection accepts Linux/Darwin; mobile selects unsupported implementations | Add bundle/APK asset sources and writable sandbox mounts, including background reads and persistence |
| Runtime/tooling | Packaging accepts Linux, macOS and web; native game host contains shared-module loading | Add static mobile host and separate mobile packaging/SDK profiles |
| Input/audio | Existing keyboard reduction and private audio integration offer reuse candidates | Add bounded touch vocabulary and verify mobile audio sessions, interruptions and routes |

Audit FoundationTime, memory, threading, logging/profiling, resources, scripting,
networking, renderer, text/localization and UI explicitly using the checklist.
A POSIX-looking API or an existing third-party backend is not acceptance evidence.

## Parallel implementation and merge order

Use one shared contract branch and separate Apple and Android branches/worktrees.
Merge shared contracts through reviewed PRs before both tracks consume them.
Assign each shared change one owner; coordinate edits to root CMake, presets,
Foundation headers, native descriptors and Input rather than making divergent
platform versions. Neither track waits for the other's backend completion.

| Phase | Shared work | Apple track | Android track | Exit gate |
| --- | --- | --- | --- | --- |
| M0: contracts and tools | Scope/profile matrix, dependency inventory, lifecycle and touch contracts | SDK/compiler probe, signing/device inventory | SDK/NDK/compiler probe, Galaxy capability inventory | Explicit profiles and blockers recorded; both minimal toolchain probes work |
| M1: runtime boot | Static game-host path, target-safe Foundation, diagnostics | UIKit scene host, tick scheduling, Foundation consumer | GameActivity/native loop, JNI boundary, Foundation consumer | Installed app boots and reports target facts on each device |
| M2: surface and rendering | Surface generation/retirement contract, shared clear/triangle fixture | UIKit view and Metal drawable integration | Android native window and Vulkan surface/swapchain | Clear/triangle survives surface and lifecycle transitions on all devices |
| M3: playable content | Touch reducer, mounts/save contract, small cooked scene | Bundle/sandbox adapters, touch/insets | APK assets/private storage adapters, touch/insets | Same packaged interactive scene and durable save work on all devices |
| M4: complete runtime | Audio, behavior/resources, text/UI, jobs and memory pressure | Audio session/route lifecycle and iPad resizing | Audio route lifecycle and fold/recreation tests | Required runtime subsystems pass interruption/failure journeys |
| M5: performance and qualification | Budgets, conformance, SDK/install/CI and setup repair | Device captures, sanitizer availability, Release build | Device captures, page-size compatibility, Release build | Reproducible supported profiles and reviewed acceptance evidence |
| M6: distribution | Release metadata, symbol retention, upgrade/uninstall policy | Signed archive/TestFlight/store workflow | APK/AAB signing and Play workflow | Separate distribution acceptance; publish only when explicitly authorized |

These phases describe dependency order rather than calendar estimates. A track
may reach M3 while the other fixes M2; shared readiness is the minimum completed
gate across required devices. Avoid treating a triangle as full engine support.

## M0: establish contracts before platform branches diverge

- [ ] Record real device/OS identities and available graphics capabilities;
  select minimum deployment OS/API levels after probes, not from device names.
- [ ] Pin mobile toolchain/dependency versions and qualify C++23 support.
  The upstream Clang 18 rule must not silently become AppleClang/NDK support.
  Propose a reviewed toolchain-policy update with SDK/compiler/linker evidence;
  retain reference Clang 18 format/tidy checks and existing native/browser gates.
- [ ] Separate macOS host cookers/shader tools from target libraries; never run
  an ARM64 mobile executable as a build-time tool. Audit Conan profiles, imported
  libraries, Objective-C++ flags, third-party licenses and SDK export closure.
- [ ] Define bounded events for activation, suspension, memory pressure,
  drawable availability, resize/insets and surface generation. Specify callback
  thread, ordering, ownership, overflow and explicit failure behavior.
- [ ] Specify touch IDs, begin/move/end/cancel, coordinate units/conversion,
  multitouch capacity and focus cancellation in Input's owning contract. Keep
  UI safe areas and physical drawable pixels distinct.
- [ ] Agree frame scheduling, monotonic clock behavior and resume delta policy.
  Suspension must not produce a giant simulation step or busy render loop.
- [ ] Create independently selectable configure/build/test profiles for Apple
  devices and Android ARM64. Test execution needs a device runner and must never
  accidentally execute target binaries on the Mac. Simulator/emulator profiles
  are optional and independently named/qualified.

Exit: both SDK probes compile/link on their real target toolchains; proposed
toolchain policy, contracts and checklist rows have owners and concrete blockers.

## M1 and M2: boot and present independently

Apple: implement scene-aware UIKit lifecycle and a small Objective-C++ boundary.
Specify the active scene's view ownership and main-thread obligations, and use
an OS-driven frame callback. Audit Metal code for AppKit, desktop-only feature
assumptions, drawable availability, scale and shader library packaging. Keep GPU
device/resource ownership in RHI rather than Platform. Initially support one
game scene; document explicit handling of additional scene requests.

Android: integrate a pinned GameActivity dependency with a small Java/Kotlin
shell and private JNI/native adapter. Distinguish UI and native-loop threads;
retain/release native windows explicitly. Add `VK_KHR_android_surface` selection
and negotiate the actual Galaxy's features. If current presentation-retirement
extensions are unavailable, implement and validate an alternative retirement
contract or report a blocker; do not remove safety checks to get a frame.

Both: supply static game-host startup/shutdown, bounded ordinary diagnostics and
an independent assertion emergency path. Surface destruction does not imply
the game world is destroyed. Stop presentation when no usable drawable exists;
never access an old surface after its ownership ends. Application termination
may omit orderly teardown, so persistence must not depend on a final callback.

Exit: install from a clean target build and run boot plus clear/triangle on all
three devices. Exercise rotation, lock/unlock, Home/app switching, repeated
resume and surface replacement. Capture logs, device identities, shader package
identity and commit. Verify repeated teardown/recreation without stale handles.

## M3: make the same game playable

- [ ] Extend Input with touch and cancellation; map touch to shared game actions
  without synthetic keyboard events. Test simultaneous contacts, ID reuse,
  overflow, orientation and cancellation during background transitions.
- [ ] Mount immutable cooked content from the Apple bundle and Android package;
  avoid assuming APK entries are ordinary paths or uncompressed mapped files.
  Define writable save/cache roots, quotas/errors and output preservation.
- [ ] Validate worker reads, cancellation during shutdown, corrupt/missing packs,
  bounded allocations and interruption-safe save replacement.
- [ ] Package a small shared scene with mesh, texture, camera, basic UI and a
  touch action. Cook Metal/SPIR-V variants on the host; verify shader reflection,
  packing and capability admission against existing RHI/renderer contracts.
- [ ] Save one game-state value, relaunch after process termination and verify
  recovery. Cover schema mismatch and failed writes explicitly.

Exit: the same content revision is interactive and reloads a durable save on all
devices. iPad resizing and Galaxy fold/unfold do not stretch input coordinates,
retain stale contacts or require a desktop keyboard.

## M4: qualify the required runtime surface

Audit each checklist row; classify unavailable features as unsupported with
explicit behavior rather than omitting them. Verify audio start/stop, silence on
interruption, route changes and resume with the existing private audio dependency
before choosing a replacement. Establish audio-session ownership on Apple.
Test script/behavior profile admission, resource lifetime, jobs, timers,
localization/text/UI and the sample's required networking path. Confirm memory
pressure and low-storage failure degrade gracefully. Virtual keyboard/IME is a
required feature only if the sample accepts text; otherwise mark it deferred.

Device journeys include iPhone safe areas/rotation, iPad window resizing under
the installed OS, and Galaxy fold/unfold, configuration changes and Activity
recreation. Distinguish live surface recovery from cold process restart and save
recovery. Record cover-display, Pencil, controller and multi-scene limitations
explicitly; none is implied by core touch support.

Exit: required module contracts, platform-independent reducer tests, and actual
device interruption/failure journeys pass with evidence attached to their rows.

## M5 and M6: qualification and distribution

Choose measurable frame-time, peak-memory, startup and package-size budgets at
M0/M3; record resolution, scene, refresh setting and build flavor. At M5 run
repeatable sustained workloads (initial protocol: at least 20 minutes per
device), reporting p50/p95/p99 frame times, memory growth, thermal conditions and
audio/input interruptions. The protocol is an acceptance proposal, not a claim
these powerful devices represent low-end phones. Do not mandate desktop compute,
ray or optional RHI features on the mobile minimum profile.

Build warning-clean target libraries and an installed-SDK consumer in Debug,
Development and Release. Run reference format/tidy, portable unit tests and
available target sanitizer tests. Document exact sanitizer/tool limitations;
simulator or desktop tests do not replace physical-device acceptance. CI should
build target artifacts and portable tests; an explicit device-run result remains
required where unattended device testing is unavailable.

Use the selected CMake to list configure/build/test presets. Verify SDK/compiler,
Ninja, host shader tools and IDE preset mode through the shared CLI/Editor setup
workflow. Loading/checking is read-only; explicit repair preserves custom
presets/settings, regenerates owned ignored settings and refreshes stale caches.
Test fresh clone, missing presets, moved SDK/toolchain and repeated repair, then
perform a real configure/build/device test before reporting repaired setup.

Android qualification includes native library and APK alignment, dependencies,
and 16 KB page-size runtime evidence in an appropriate emulator or device;
the Galaxy alone may not offer that configuration. Pin compatible NDK/build
tools after testing. Store-policy requirements are rechecked when M6 begins.
Apple device signing and distribution use separate configurations; keep private
keys, provisioning material and developer identities out of public docs.

M5 admits only named, evidenced runtime profiles and updates the existing
capability/status pages, behavior admission and package validation in the same
implementation change. M6 adds signed distribution, clean install/upgrade,
symbolication and store compliance; development installation is not distribution
acceptance. Deferred features remain visible after a platform is admitted.

## Running the M0 probes

After the maintainer installs Xcode and the Android SDK and NDK, one command per
platform produces the sanitized records that the M0 acceptance needs:

```bash
./scripts/mobile-probe all
```

Run one probe with `apple-toolchain`, `apple-signing`, `android-toolchain`,
`android-signing` or `devices` instead of `all`. Each writes
`out/mobile-probes/<probe>.json` (ignored by Git). The record keeps versions,
statuses and yes/no answers only. Serial numbers, signing identity names, team IDs,
device identifiers and machine paths never reach it: values are collected from a
fixed list of fields, rewritten, and the probe refuses to write a record that
still holds a private token. Review a record before linking it from the tracker.

- **Exit status.** `0` passed, `1` failed (a compile, link or alignment check
  failed; the record names the step), `3` not-installed (Xcode, an SDK, the NDK,
  `adb` or the pinned Clang is missing, with what to install), `4` blocked (the
  tools work, but signing material or a connected device is missing).
- **Compiler.** The probes use the pinned upstream Clang (`out/host-tools`, then
  Homebrew `llvm@23`, or `--clang` / `LUDUS_MOBILE_CLANG`) and reject another
  major. They compile `tools/mobile-probe/probe_cxx23.cpp` (it includes the real
  Band 0 headers), `probe_objcxx_arc.mm` (ARC, UIKit required) and
  `probe_jni.cpp` with the engine warning set, `-Werror` and `-fno-exceptions`.
  Apple builds use `-target` with the device and simulator SDKs and link with
  Xcode's `ld`. Android builds use the NDK sysroot and its libc++ headers with the
  pinned Clang (option A) and again with the NDK's own Clang for comparison,
  linking with 16 KB `max-page-size` and reading the `LOAD` alignment with
  `llvm-readelf -l`.
- **Provisional levels.** `--ios-minimum` (default 17.0) and `--android-api`
  (default 29) only select what to compile for. Each record says whether the value
  was a default. Record the chosen minimums under Target profiles after the probes.
- **Devices.** Connect each device by cable and trust the Mac. The probe reads
  Apple devices with `devicectl` and Android devices with `adb`, and labels them
  `apple-1`, `android-1` and so on.

The mobile presets build the probes while FoundationBase still rejects iOS and
Android. List them with the selected CMake:

```bash
cmake --list-presets=configure
cmake --list-presets=build
cmake --list-presets=test
```

Configure with `cmake --preset ios-clang-development`, or one of the other names
in Target profiles. Toolchain files live in
`config/cmake/ios.cmake` and `config/cmake/android.cmake`. Without the SDK or NDK,
configure stops with a message that names what to install. Target test presets run
binaries only through `scripts/mobile-device-runner`
(`CMAKE_CROSSCOMPILING_EMULATOR`), which exits with an error until
`LUDUS_MOBILE_DEVICE` selects a device: an `adb` serial for Android devices or
emulators, a booted simulator UDID for `ios-simulator-clang-development`. Android
device presets refuse an emulator and the reverse. A bare executable cannot run on
an iPhone or iPad, so `ios-clang-*` tests refuse to run until the M1 app bundle
exists. Both toolchain files and the Android runner path have not run against a real
SDK, NDK or device yet; the first run on the maintainer Mac is the evidence.

## Milestone tracker

Update a cell when its gate is met; link the implementation PR and evidence.
Use `not started`, `in progress`, `blocked: reason`, or `passed: evidence`.
Device/tool versions and logs belong in evidence, not speculative completion dates.

| Gate | Shared | iPhone 16 Pro | iPad Pro M4 | Galaxy Flip |
| --- | --- | --- | --- | --- |
| M0 | not started | not started | not started | not started |
| M1 | not started | not started | not started | not started |
| M2 | not started | not started | not started | not started |
| M3 | not started | not started | not started | not started |
| M4 | not started | not started | not started | not started |
| M5 | not started | not started | not started | not started |
| M6 | not started | not started | not started | not started |

## Subsystem port ledger

Initial coverage from the baseline audit. Track ownership by workstream until a
contributor takes a row; implementation PRs and runtime evidence are pending for
every row. Status applies to the proposed mobile profiles only. Expand a cell
with a blocker, PR and evidence link as work progresses, using the checklist's
record format. `reuse candidate` still requires target compilation and testing.

| Subsystem | Workstream owner | Apple status | Android status | Next action / gate |
| --- | --- | --- | --- | --- |
| Target identity | Shared | reuse candidate | reuse candidate | Real SDK/ABI probes, M0 |
| Toolchain/dependencies | Shared with both tracks | needs port | needs port | Pin/qualify compilers, libraries and host cookers, M0 |
| FoundationBase runtime | Both platform tracks | needs port | needs port | Replace rejection with explicit target runtime, M1 |
| Memory/containers | Shared | unreviewed | unreviewed | Alignment/allocation/atomics tests, M1/M4 |
| Time/threading | Shared | unreviewed | unreviewed | Target clock/jobs and suspend policy, M1/M4 |
| Logging/profiling | Both platform tracks | unreviewed | unreviewed | Device sinks and emergency diagnostics, M1 |
| Filesystem | Shared contract, both adapters | needs port | needs port | Bundle/package mounts and durable saves, M3 |
| Platform/host | Both platform tracks | needs port | needs port | UIKit/GameActivity and lifecycle, M1/M2 |
| Input | Shared reducer, both adapters | needs port | needs port | Bounded touch and cancellation, M3 |
| RHI | Both platform tracks | needs port | needs port | Remove AppKit coupling/add Android surface, M2 |
| Renderer/graph | Shared | reuse candidate | reuse candidate | Mobile variants and same scene, M3/M5 |
| Text/localization/UI | Shared with both tracks | unreviewed | unreviewed | Scale/insets and target dependencies, M3/M4 |
| Content/resources | Shared | unreviewed | unreviewed | Cook/load closure and pressure handling, M3/M4 |
| Runtime/game host | Shared with both tracks | needs port | needs port | Static host/deployment path, M1/M3 |
| Behavior/scripting | Shared | needs port | needs port | Interpreter conformance/profile admission, M4/M5 |
| Gameplay/physics | Shared | unreviewed | unreviewed | Scoped fixtures and resume ticks, M4 |
| Audio | Both platform tracks | unreviewed | unreviewed | Existing dependency/session/route probes, M4 |
| Networking | Shared with both tracks | unreviewed | unreviewed | Define required sample scope, reconnect tests, M4 |
| SDK/projects/editor tools | Shared | needs port | needs port | Target presets, export and setup repair, M5 |
| Packaging/distribution | Both platform tracks | needs port | needs port | Development bundle/APK, M1; distribution, M6 |
| CI/documentation | Shared | needs port | needs port | Cross builds, device evidence and support matrix, M5 |

First implementation slices: M0 contracts/toolchain qualification, M1 Apple
boot and M1 Android boot. Each platform's M2 rendering slice depends on its own
M1 plus the shared surface contract. M3 depends on its own M2 and shared input,
storage and content work. This permits simultaneous progress with small PRs.

## Sources and related contracts

Thanks to Apple, [Managing your app's life cycle](https://developer.apple.com/documentation/uikit/managing-your-app-s-life-cycle),
for the scene/application lifecycle distinction informing the proposed UIKit
host. Thanks to Google, [GameActivity](https://developer.android.com/games/agdk/game-activity),
for the native loop, input and framework integration model, and
[Support 16 KB page sizes](https://developer.android.com/guide/practices/page-sizes),
for native binary/package and page-size validation requirements. These sources
inform the plan; no upstream implementation code was copied. Implementation
slices must preserve attribution beside code informed by these sources.

Existing owners: [target detection](../architecture/platform-detection.md),
[macOS Platform](../architecture/macos-platform.md),
[RHI contracts](../architecture/rhi-gdi.md),
[filesystem](../architecture/filesystem.md),
[audio](../architecture/audio-authoring.md),
[project setup](project-sdk-workflow.md), and
[packaging](game-packaging-publishing.md).
