# Mobile support implementation plan

Status: proposed implementation sequence, 2026-10-12. No mobile deployment is
qualified by this plan. The original baseline audit is `850e48e`; the M0 record
below rechecked it against `develop` at `b6c7a6d8c` on 2026-11-19. Recheck it
before each implementation slice. Maintain this page as the mobile milestone owner and use
the [platform porting checklist](platform-porting.md) for subsystem coverage and
evidence. Existing API comments and module designs remain contract owners.

## Goal and initial scope

Run one shared Ludus game on an iPhone 16 Pro, iPad Pro 11-inch (M4), and the
owner's Galaxy Z Flip5. The Android M0 inventory identified model SM-F731N.
See the [milestone tracker](#milestone-tracker) for the device evidence.

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
| Target identity | Foundation recognizes Android/iOS; `config/behavior_profiles.json` marks both `pending-engine-backend` with compiler major 23 | Preserve detection; admit profiles only with build and runtime evidence |
| FoundationBase | Its CMake guard accepts Linux, Darwin, Windows or Emscripten with Clang (or MSVC on Windows) and rejects `CMAKE_SYSTEM_NAME` `iOS` and `Android` | Add explicit mobile assertion/emergency/debugger boundaries and compiler qualification |
| Platform | Cocoa is selected only for `Darwin`; `iOS` and `Android` fall through to the headless backend | Add explicit UIKit and Android selections; mobile shipping configuration must reject an accidental null backend |
| Metal | RHI's broad `APPLE` branch links AppKit and calls the Cocoa configuration helper | Separate macOS from iOS build selection and native view ownership; reuse portable Metal resource code only after audit |
| Vulkan | Presentation creates only Wayland surfaces and fails without `VK_EXT_swapchain_maintenance1`, which it needs for safe retirement | Add Android surface ownership and device feature negotiation; establish safe retirement before presenting |
| Dependencies | Conan requires volk on every OS except `Macos` (so an iOS graph would pull it) and has profiles only for Linux, macOS and Windows | Add iOS/Android Conan profiles and target-correct requirements; see the [dependency inventory](#dependency-inventory) |
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
  The [device inventory probe](#acceptance-probes) defines what to record.
- [ ] Pin mobile toolchain/dependency versions and qualify C++23 support.
  The upstream LLVM 23 rule ([ADR 0025](../decisions/0025-llvm-toolchain-pin.md))
  must not silently become AppleClang/NDK support. The
  [toolchain policy proposal](#toolchain-policy-proposal) awaits probe evidence;
  reference LLVM 23 format/tidy checks and existing native/browser gates remain.
- [x] Separate macOS host cookers/shader tools from target libraries; never run
  an ARM64 mobile executable as a build-time tool. Conan profiles, imported
  libraries, Objective-C++ flags and licenses are audited in the
  [dependency inventory](#dependency-inventory); SDK export closure is an M5 check.
- [x] Define bounded events for activation, suspension, memory pressure,
  drawable availability, resize/insets and surface generation. Specify callback
  thread, ordering, ownership, overflow and explicit failure behavior:
  [mobile application lifecycle and surfaces](../architecture/mobile-application-lifecycle.md).
- [x] Specify touch IDs, begin/move/end/cancel, coordinate units/conversion,
  multitouch capacity and focus cancellation in Input's owning contract. Keep
  UI safe areas and physical drawable pixels distinct:
  [touch input contract](../architecture/touch-input.md).
- [x] Agree frame scheduling, monotonic clock behavior and resume delta policy.
  Suspension must not produce a giant simulation step or busy render loop:
  [frame scheduling and time](../architecture/mobile-application-lifecycle.md#frame-scheduling-and-time).
- [ ] Create independently selectable configure/build/test profiles for Apple
  devices and Android ARM64. Test execution needs a device runner and must never
  accidentally execute target binaries on the Mac. Simulator/emulator profiles
  are optional and independently named/qualified. The
  [target profiles](#target-profiles) fix names and rules; presets come with the probes.

Exit: both SDK probes compile/link on their real target toolchains; proposed
toolchain policy, contracts and checklist rows have owners and concrete blockers.

## M0 record

Audited `develop` at `b6c7a6d8c` on 2026-11-19 for
[#1229](https://github.com/The-Ludus-Project/Ludus/issues/1229). The contracts
are frozen in their owners; the probes, presets and the toolchain decision are
still open, and so is the M0 exit gate.

### Target profiles

| Profile | Target triple | SDK / sysroot | Compiler and linker | Test runner | Status |
| --- | --- | --- | --- | --- | --- |
| iOS device (required) | `arm64-apple-ios<minimum>` | Xcode `iphoneos` SDK | Pinned Clang 23 and Xcode `ld` (proposed) | Physical device through `devicectl` | proposed |
| iOS Simulator (optional) | `arm64-apple-ios<minimum>-simulator` | Xcode `iphonesimulator` SDK | Same as device | Simulator | proposed; never replaces device evidence |
| Android device (required) | `aarch64-linux-android<api>` | NDK sysroot | Pinned Clang/LLD 23 (proposed) | Physical device through `adb` | proposed |
| Android emulator (optional) | `aarch64-linux-android<api>` | NDK sysroot | Same as device | ARM64 emulator image, also the 16 KB page-size check | proposed |

- **Preset names** follow `<platform>-clang-<flavor>`: `ios-clang-debug`,
  `ios-clang-development`, `ios-clang-release`, `ios-simulator-clang-development`,
  `android-clang-debug`, `android-clang-development`, `android-clang-release`
  and `android-emulator-clang-development`. Each has its own configure, build
  and test preset and its own build tree.
- **Minimum OS/API.** Choose the lowest version that the pinned SDK supports,
  every required device runs, and that provides each API the contracts use,
  with probe evidence. Device names do not decide it. Record the choice here.
- **Host tools.** Shader compilers, cookers and packaging tools are built and
  run from a separate macOS host tree and reached through an explicit host-tools
  prefix. Target trees never build or run them.
- **Test execution.** Target test presets set `CMAKE_CROSSCOMPILING_EMULATOR`
  to a device runner. With no device selected, the runner exits with an explicit
  error. CTest never runs a target binary on the Mac. Portable reducer tests
  also run in the existing host presets.
- **Packaging.** Native code builds with Ninja through the Ludus presets. On
  Apple, the `.app` bundle is assembled and code-signed outside the Xcode
  generator. On Android, Gradle packages the prebuilt `.so` files and the Java
  shell as `jniLibs`, so AGP's NDK toolchain never compiles engine code.

### Toolchain policy proposal

**Recommended (option A): one LLVM major.** Compile every target C, C++ and
Objective-C++ source with the pinned upstream Clang 23 against the vendor
sysroot: `-target` with `-isysroot` from Xcode for Apple, and `--target` with
`--sysroot` from the NDK for Android. Link Android with LLD 23 and Apple with
Xcode's `ld`, as macOS builds already do. Reasons:

- ADR 0025 pins one major for compilation, format, tidy and sanitizers.
  `config/behavior_profiles.json` already declares compiler major 23 for both
  `Android` and `iOS`.
- The Cocoa backend already requires the pinned upstream Clang for
  Objective-C++. A UIKit host would use the same flags (`-fobjc-arc`,
  `-fno-objc-exceptions`).
- Warning, exception (`-fno-exceptions`) and tidy behavior stay identical to
  the qualified desktop builds.

Risks the probes must settle:

- The NDK's libc++ headers match the NDK's own Clang, not Clang 23. Building
  Ludus TUs that use C++23 language features and the ADR 0003 allowlist
  against those headers must succeed warning-clean.
- Upstream Clang must accept the current iOS SDK headers, including Objective-C
  module maps, and Xcode's linker must accept its objects.
- Sanitizer runtimes for the mobile targets come from the vendor toolchains,
  not from Homebrew LLVM. Their availability is an M5 finding.

**Fallback (option B), per platform:** use the vendor compiler (AppleClang from
Xcode, or the NDK's Clang) for that platform only. This requires an ADR 0025
amendment that names the exact compiler version, a separate Behavior profile
compiler major, and its own C++23 qualification. Format and tidy stay on the
pinned LLVM 23 in either case. Choose option B only when option A fails a probe,
and record that failure.

The provisional Android setup pins for the M0 probes live in
`config/android_toolchain.json`. The probe compile API remains separate from
the installed SDK platform. Add AGP, Gradle and GameActivity pins when M1
introduces an app. The Apple pins (Xcode version and build, iOS SDK, minimum OS)
remain pending. Record the toolchain decision after collecting probe evidence.

### Dependency inventory

| Dependency | Pin owner | License | Role | iOS action | Android action |
| --- | --- | --- | --- | --- | --- |
| FreeType 2.14.3 | Conan (`conanfile.py`) | FreeType License (FTL) or GPLv2 | Target library (text) | Conan iOS profile, static | Conan Android profile, static |
| HarfBuzz 14.5.1 | Local recipe `third_party/harfbuzz` | MIT | Target library (text) | Same | Same |
| volk 1.4.357.0 | Conan | MIT | Vulkan loader for the target | **Exclude**: today only `Macos` is excluded | Keep; load `libvulkan.so` |
| Vulkan Memory Allocator 3.5.0 | `config/vulkan_memory_allocator.json` | MIT | Target library (Vulkan RHI) | Not used | Keep |
| miniaudio 0.11.23 | `third_party/miniaudio/SOURCE-LOCK.json` | MIT-0 | Target library (audio) | Core Audio backend; audio-session ownership in M4 | AAudio backend; route lifecycle in M4 |
| yyjson 0.10.0, stb_image 2.30, MikkTSpace, xxHash | `third_party/*` | MIT; MIT or public domain; zlib; BSD-2-Clause | Portable target code | Compile with target flags | Compile with target flags |
| Luau | `config/luau_toolchain.json` | MIT | Optional Behavior interpreter | Interpreter only; iOS allows no JIT, and none is used | Interpreter |
| ICU 78.3 | `config/icu_toolchain.json` | Unicode License v3 | Localization probe | Outside the mobile closure until localization selects it | Same |
| Catch2 3.4.0 | Conan (test only) | BSL-1.0 | Tests | Host tests; device test binaries in M5 | Same |
| Slang, DXC, SPIRV-Tools, SPIRV-Cross, WGSL validator | `config/shader_toolchain.json`, `config/spirv_cross_toolchain.json` | Various permissive | **Host tools only** | Cook Metal variants on the host | Cook SPIR-V variants on the host |
| Qt, tree-sitter, Crashpad, OIDN, D3D12 Agility | Editor/host configuration | Various | Editor or Windows only | Excluded | Excluded |
| GameActivity (Android Games SDK) | **New**: `config/android_toolchain.json` | Apache-2.0 | Target host shell | n/a | Pin the AAR and prefab version; M1 |
| Android Frame Pacing (Swappy) | Candidate only | Apache-2.0 | Optional pacing | n/a | Decide in M2; not adopted |

Licenses come from each pin owner's record or vendored license file. Recheck
them before an SDK export (M5) and a distribution (M6).

### Acceptance probes

The [#1229](https://github.com/The-Ludus-Project/Ludus/issues/1229) acceptance
needs these probes. Each one writes a sanitized record with no serial numbers,
signing identities, team IDs or machine paths. The record is linked from the
tracker below; raw output stays private.

- **Apple toolchain:** Xcode version and build, `iphoneos` and `iphonesimulator`
  SDK versions, and the pinned Clang version. Compile and link, warning-clean
  with `-fno-exceptions`, a C++23 FoundationBase-style TU and an ARC
  Objective-C++ TU for both device and simulator. Record the linker version and
  which libc++ headers and runtime were used.
- **Apple signing:** whether a development signing identity and team exist.
  Record only yes or no, never a name or ID. Whether a development
  provisioning profile can sign the probe app.
- **Android toolchain:** versions of command-line tools, platform, build-tools,
  NDK, AGP, Gradle and JDK. Compile and link the same C++23 TU plus a JNI TU with
  option A, and also with the NDK's Clang for comparison. Confirm 16 KB `LOAD`
  segment alignment with `llvm-readelf -l`.
- **Android signing:** whether a debug keystore exists. Release signing belongs
  to M6.
- **Devices:** marketing name, model identifier, OS version and build, and page
  size. For Android, the Vulkan API version and whether
  `VK_KHR_android_surface` and `VK_EXT_swapchain_maintenance1` are present, read
  from `adb shell cmd gpu vkjson` where the OS provides it, otherwise from the
  M1 probe app. On-device probes in M1 and M3 record Metal GPU family, the
  simultaneous-touch limit and whether
  `steady_clock` advances while the device sleeps.

### Open blockers

| Blocker | Owner | Next action |
| --- | --- | --- |
| Xcode is installed, but needs license acceptance and selection for the Apple probes | Maintainer | Review the Xcode license, select Xcode and run the Apple probes |
| Apple signing material | Maintainer | Create an Apple development team identity locally and run the Apple signing probe |
| FoundationBase rejects `iOS` and `Android` | Both tracks | Add explicit target runtime boundaries in M1 |
| Platform falls back to headless on `iOS` and `Android` | Both tracks | Add UIKit and Android selections that reject a silent null backend in M1 |
| RHI's `APPLE` branch links AppKit | Apple track | Separate macOS and iOS selection in M2 |
| Conan pulls volk on iOS and lacks mobile profiles | Shared | Add profiles and target-correct requirements with the probes |
| Vulkan retirement needs `VK_EXT_swapchain_maintenance1` | Android track | The Galaxy advertises the extension. Verify its feature bit and present-fence behavior in M2 |

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

Use Vulkan FIFO as the first Android presentation baseline. Request three
swapchain images when the surface limits and RHI capacity allow it. Record the
surface limits, requested and returned image counts, and selected present mode
when creating or recreating the swapchain. Run the lifecycle fixture with the
baseline and any supported fallback image count. Thanks to Samsung Galaxy
GameDev's [Vulkan Usage Recommendations](https://developer.samsung.com/galaxy-gamedev/resources/articles/usage.html),
“Swapchains,” for the FIFO baseline and image-count tradeoff.

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

For the Galaxy run, measure actual display intervals from the game's surface
using [SurfaceFlinger timing](https://developer.android.com/games/optimize/vitals/slow-session)
or supported [Vulkan presentation timing](https://developer.android.com/games/develop/vulkan/frame-pacing-extensions).
Record CPU/GPU work timings alongside the display intervals. On the same package,
scene replay and refresh setting, compare two- and three-image FIFO requests
when both are supported and yield different actual image counts. Report
frame-interval tails, missed target intervals and memory cost. If the comparison
is unavailable, record why. Where available, use Samsung's
[GPUWatch](https://developer.samsung.com/galaxy-gamedev/gpuwatch.html) in a
separate diagnostic run so the overlay is absent from the budget measurements.

In the sustained Galaxy Release run, record emitted draw commands and CPU
times for renderer preparation, Vulkan command recording, and queue submission.
Report acquire and wait time separately, alongside GPU elapsed time and actual
display intervals. If command recording contributes to a repeatable budget
miss, compare the serial baseline with an equivalent indexed-instanced variant
where the scene has compatible repeated opaque geometry. Hold the camera
replay, resolution, visible geometry, and material settings fixed. Verify
matching output and compare draw counts, CPU and GPU timing tails, and missed
display intervals. Record missing timing coverage and any instrumentation cost.
If recording remains a bottleneck, use the trace to scope the
[serial versus worker recording comparison](../architecture/rhi-gdi.md#schedule-comparison-for-r15).
Thanks to Max Röhrbein-Kling and Johannes Kuhlmann for isolating CPU draw
emission in [*Bringing Galaxy on Fire 3 to Vulkan: Stats & Summary*, “Some Numbers”](https://www.gamedeveloper.com/programming/bringing-galaxy-on-fire-3-to-vulkan-stats-summary).

Record [Android thermal status or headroom](https://developer.android.com/games/optimize/adpf/thermal)
where available, including the initial state and changes during the run. Mark
unavailable telemetry explicitly and retain a fixed-quality run. Revisit
[Android Frame Pacing](https://developer.android.com/games/sdk/frame-pacing)
if display intervals miss the budget while frame work fits it. Consider app-owned
quality or frame-rate adaptation if repeatable thermal throttling causes the
packaged scene to miss the sustained budget. Compare any candidate against the
baseline in the standalone Release app.

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

### Prepare the Android tools

On macOS or Linux x64, run `./init.sh`, select **Install Android SDK and NDK**
under **Targets**, then choose **Initialize**. The setup window displays Google's
SDK agreement for review before installation. From a terminal, use
`./init.sh --cli --with-android` alongside native setup, or install just the
Android tools on an initialized checkout:

```bash
./init.sh --cli --android-only
```

For unattended setup after reviewing the pinned agreement, pass
`--accept-android-sdk-license`. Setup installs the pinned command-line tools,
SDK platform, build-tools, NDK and Java under `out/host-tools`. It also installs
stable platform-tools (including ADB) at or above the minimum in
`config/android_toolchain.json`. Allow several gigabytes for downloads and the
extracted NDK. Setup reuses compatible packages in that checkout and repairs
missing or incompatible ones. Android Studio, system-wide packages, and other
checkouts keep their own SDKs and Java installations.

The mobile presets and probes prefer the checkout-local tools. If no managed
tools are available, they can use the SDK from `ANDROID_HOME` or
`ANDROID_SDK_ROOT`. A preset requires the pinned NDK revision, even with a
`LUDUS_ANDROID_NDK` CMake override. To compare an alternate NDK explicitly, run
`./scripts/mobile-probe android-toolchain --ndk <path>`.

On the phone, enable USB debugging in Developer options, connect it by USB
while unlocked and approve the Mac's debugging key. If ADB still shows no
device after reconnecting, check whether Samsung Auto Blocker is preventing USB
debugging. See [Android device setup](https://developer.android.com/studio/run/device)
and [Samsung Auto Blocker](https://www.samsung.com/ca/support/mobile-devices/protect-your-samsung-galaxy-with-auto-blocker/).

### Collect the evidence

With Xcode installed for the Apple probes and the Android tools ready, run the
probes to produce the sanitized M0 acceptance records:

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
exists. See the milestone tracker for toolchain and device evidence.

## Milestone tracker

Update a cell when its gate is met; link the implementation PR and evidence.
Use `not started`, `in progress`, `blocked: reason`, or `passed: evidence`.
Device/tool versions and logs belong in evidence, not speculative completion dates.

| Gate | Shared | iPhone 16 Pro | iPad Pro M4 | Galaxy Flip |
| --- | --- | --- | --- | --- |
| M0 | in validation: shared compiler decision awaits Apple probes ([#1229](https://github.com/The-Ludus-Project/Ludus/issues/1229)) | blocked: Xcode license, signing and device probes pending | blocked: Xcode license, signing and device probes pending | passed: [Android M0 evidence](#android-m0-evidence) |
| M1 | not started | not started | not started | not started |
| M2 | not started | not started | not started | not started |
| M3 | not started | not started | not started | not started |
| M4 | not started | not started | not started | not started |
| M5 | not started | not started | not started | not started |
| M6 | not started | not started | not started | not started |

### Android M0 evidence

On 2026-11-22, the Apple silicon host used SDK command-line tools 21.0,
platform-tools 37.0.3, SDK Platform 36, Build Tools 36.0.0, NDK 29.0.14206865 and
Temurin 21.0.13.1. `./scripts/mobile-probe android-toolchain` passed at the
provisional minimum API 29. Upstream Clang and LLD 23.1.3 compiled and linked
the C++23 and JNI probes against the NDK sysroot. The NDK's Clang passed the
comparison too. Both builds produced ELF libraries whose `LOAD` segments were
aligned to 16 KiB. The Android signing probe passed using a local debug keystore.

The physical Galaxy Z Flip5 (SM-F731N) reported Android 17 (API 37), arm64-v8a,
4 KiB memory pages and Adreno 740 with Vulkan 1.3.128. Its inventory advertised
`VK_KHR_android_surface` and `VK_EXT_swapchain_maintenance1`. The
`android-clang-development` preset built with upstream Clang 23, and CTest
passed `ludus_mobile_probe_runner` on the phone over USB. The 16 KiB runtime
check and the GameActivity/Vulkan presentation path remain later acceptance
gates.

## Subsystem port ledger

Initial coverage from the baseline audit. Track ownership by workstream until a
contributor takes a row; implementation PRs and runtime evidence are pending for
every row. Status applies to the proposed mobile profiles only. Expand a cell
with a blocker, PR and evidence link as work progresses, using the checklist's
record format. `reuse candidate` still requires target compilation and testing.

| Subsystem | Workstream owner | Apple status | Android status | Next action / gate |
| --- | --- | --- | --- | --- |
| Target identity | Shared | reuse candidate | reuse candidate | Real SDK/ABI probes, M0 |
| Toolchain/dependencies | Shared with both tracks | needs port | needs port | Run the [acceptance probes](#acceptance-probes) and settle the [toolchain proposal](#toolchain-policy-proposal), M0 |
| FoundationBase runtime | Both platform tracks | needs port | needs port | Replace rejection with explicit target runtime, M1 |
| Memory/containers | Shared | unreviewed | unreviewed | Alignment/allocation/atomics tests, M1/M4 |
| Time/threading | Shared | unreviewed | unreviewed | Suspend/resume policy in the [lifecycle contract](../architecture/mobile-application-lifecycle.md#frame-scheduling-and-time); target clock/jobs, M1/M4 |
| Logging/profiling | Both platform tracks | unreviewed | unreviewed | Device sinks and emergency diagnostics, M1 |
| Filesystem | Shared contract, both adapters | needs port | needs port | Bundle/package mounts and durable saves, M3 |
| Platform/host | Both platform tracks | needs port | needs port | Implement the [lifecycle contract](../architecture/mobile-application-lifecycle.md) in UIKit/GameActivity, M1/M2 |
| Input | Shared reducer, both adapters | needs port | needs port | Implement the [touch contract](../architecture/touch-input.md), M3 |
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
