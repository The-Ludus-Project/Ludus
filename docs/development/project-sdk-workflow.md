# Independent Ludus game projects and shared SDKs — user guide

This guide covers installing the host tools, installing and overriding/refreshing
SDKs, project creation and migration, building with the CLI (and the Editor),
direct CMake use, recovery and the current compatibility limits. It documents the
workflow implemented by `.kiro/specs/project-sdk-workflow`.

On macOS, **New Project** and **Repair Project Setup** reuse the Apple SDK
prepared in that engine checkout whenever they prepare the editor's engine,
including automatic preparation of a missing or stale engine installation. An
unrelated `SDKROOT` inherited by the GUI does not replace it. If no prepared SDK
exists, init tests installed SDKs automatically. Running `./init.sh` yourself
continues to honor an explicit shell `SDKROOT` override.

> Scope. Native project tooling supports **Linux x64** (Ubuntu 24.04) and
> **macOS** with pinned upstream Clang 23, single-config Ninja, and
> Debug/Development/Release SDK identities. The installed macOS CLI has native
> Apple silicon acceptance; Intel identity/architecture selection is covered by
> unit tests and still needs native Intel acceptance. Windows tooling and general
> scene authoring require separate acceptance. [Browser releases](editor-game-releases.md) and
> [native live editing](project-live-reload.md) have their own implemented scope;
> an embedded viewport remains future work.
> Do not assume unsupported combinations work.

## Prerequisites

**Host tools** (the `ludus` CLI): Python ≥ 3.10. No Qt, no engine checkout.

**Building a generated game** additionally needs the documented toolchain for a
Ludus SDK: CMake 3.29.6, Ninja 1.11.1, and upstream Clang 23. Linux uses LLD 23;
macOS uses the prepared Apple SDK and Clang 23 libc++ headers with the deployment
baseline recorded in the SDK manifest (currently macOS 14.0). For shaders, use
the pinned Slang compiler (`config/shader_toolchain.json`): macOS emits Metal
source; Linux and browser SPIR-V paths also require `spirv-val`. A
windowed game also needs its runtime display/GPU environment (Metal on macOS,
a Vulkan loader and driver on Linux); headless *tooling* does not imply a windowed game runs without a
compositor/GPU.

## Install the host tools

```bash
python3 -m venv out/ludus-cli-venv
out/ludus-cli-venv/bin/python -m pip install ./scripts/python
out/ludus-cli-venv/bin/ludus --help
```

Activate that venv, or use its `bin/ludus` path explicitly for the commands below.
The installed `ludus` launcher locates its own packaged modules; it does not need
a Ludus source checkout.

## Install an SDK

```bash
# From a local archive (checksum optional but recommended):
ludus sdk install --archive ludus-sdk-development.tar.gz --digest <sha256>

# From a publisher release catalog:
ludus sdk install --version 0.1.0 --target linux-x64 --flavor development \
    --catalog https-or-local-catalog.json

ludus sdk list
```

SDKs install into a shared user store (Linux default `~/.local/share/ludus`;
macOS default `~/Library/Application Support/Ludus`). `--store`,
`LUDUS_SDK_STORE`, and `XDG_DATA_HOME` retain explicit location precedence. Each SDK directory is immutable
and content-addressed, so several projects share one copy. Installation validates
archive bounds/containment, the payload digest and the manifest identity, stages
to a sibling directory and atomically publishes — a concurrent or cancelled
install never exposes a partial SDK, and an existing good SDK is never replaced by
a partial one. Repair a corrupt store entry explicitly with `--repair`.

## Create a project

```bash
# Against a release (writes an exact engine requirement; the lock stays
# "unresolved" until a published release exists):
ludus project create ./MyGame --name "My Game" --template minimal --engine 0.1.0 \
    --components FoundationBase,GraphicsRhi

# Against a local SDK you built yourself (engine-developer flow). The local
# prefix is recorded ONLY in ignored .ludus/local.json; the committed lock stays
# release-oriented:
ludus project create ./MyGame --name "My Game" --template minimal --sdk /path/to/sdk-prefix
```

Creation is atomic and no-replace: it refuses an existing/symlink/nonempty
destination, stages the complete project, and publishes only to an absent
destination (a destination appearing mid-create is never overwritten). Generated
files are yours after creation; later template versions never overwrite your game
code. The generated `CMakeLists.txt`/`CMakePresets.json` are ordinary, editable
CMake — project metadata is not a second build language.

## macOS creation and setup

Prepare the trusted tooling checkout and install a complete SDK first:

```bash
./scripts/init macos-clang-development --preset-only --cli --no-system-install
./scripts/shader-probe bootstrap
./scripts/install-sdk macos-clang-development
out/host-tools/venv/bin/python -m pip install ./scripts/python
```

Create and verify a project against that SDK:

```bash
out/host-tools/venv/bin/ludus project create /path/to/MyGame --name "My Game" \
    --sdk "$PWD/out/install/macos-clang-development" --tools "$PWD"
out/host-tools/venv/bin/ludus project run /path/to/MyGame
```

`--tools` explicitly authorizes preparation/verification using that trusted
checkout. Without `--sdk`, creation selects the host's Development profile,
reuses only a current matching SDK, or explicitly prepares a separate ABI-keyed
install. Preparation bundles the dependency closure before staged game validation.
Bundled link interfaces select Apple system frameworks by name from the
consumer's SDK, preserving relocation without embedding the producer sysroot.
With `--sdk`, the project profile follows that SDK's native flavor; when combining
`--tools` with another flavor, also pass its full `--profile` name.

Minimal template version 4 includes macOS configure/build/test presets for
`macos-clang-debug`, `macos-clang-development`, and `macos-clang-release`.
macOS presets are selectable on Darwin. Each flavor requires its matching SDK;
a Development SDK is never used silently for Debug or Release. Repair verifies
the selected native flavor with real configure/build/test commands and writes
its selectable `ludus-local-<profile>` presets. Switching flavor requires its matching SDK and explicit repair:

```bash
ludus project repair /path/to/MyGame --tools /path/to/Ludus \
    --profile macos-clang-debug --sdk /path/to/debug-sdk
ludus project check /path/to/MyGame --tools /path/to/Ludus --profile macos-clang-debug
ludus project run /path/to/MyGame --profile macos-clang-debug
```

Each verified native profile retains its own ignored setup record and owned
presets. Repairing another flavor preserves those entries and the descriptor
keeps its original default profile.

Mac setup uses the matching architecture (`arm64` or `x86_64`), manifest deployment
baseline, prepared SDK sysroot, and Clang 23 libc++ headers. Machine paths live in
ignored user presets. Retargeted tool/SDK symlinks and stale macOS cache flags
require explicit repair. Native flags are excluded from browser presets. The
selected compiler's identity is checked before repair writes local settings and
before ordinary CLI operations; Apple Clang is not the reference compiler.

Existing version-2 projects keep their source, descriptors, and tracked presets.
To port a Linux project deliberately, select its macOS descriptor preset and
add the matching project-owned CMake preset (or hidden base), then run repair
with the macOS SDK. Repair does not rewrite tracked project intent. Legacy
version-1 projects still use the original Linux schema until explicit migration.

The [native Qt Editor](editor-workspace.md) supports macOS project workflows.
[macOS release packaging](game-packaging-publishing.md#macos-app-packages) creates
and verifies local ad-hoc signed game apps. Universal binaries, Developer ID
signing/notarization and native Intel acceptance remain separate follow-ups.

## Configure, build, run

```bash
ludus project configure ./MyGame --profile linux-clang-development
ludus project build     ./MyGame --profile linux-clang-development
ludus project run       ./MyGame --profile linux-clang-development   # ensures a fresh build first
```

Configure/build/run never download an SDK, refresh the lock or compile engine
sources. SDK resolution precedence is: `--sdk <prefix>` for this command → the
saved `.ludus/local.json` override → the exact locked release in the store. The
resolved identity, path and override status are printed. A failed or cancelled
build never launches an older binary. The CLI and Editor share a per-build-tree
cooperative lock, so a concurrent build of the same tree reports `Busy` rather
than corrupting it. Raw `cmake --preset …` works too (set `LUDUS_SDK_PREFIX`
yourself) but is outside the cooperative-lock guarantee.

## Check and repair project setup

In the Editor choose **Project → Check Setup**. Eligible project opening also
performs this read-only check: missing/stale presets, tools and SDK inputs are
reported without downloading, configuring, building or rewriting the game.

Save settings, then choose **Project → Repair Project Setup** and keep
**Use this project's selected engine** unless changing the engine deliberately.
Repair preserves custom presets and unrelated IDE settings, refreshes stale
owned CMake inputs, and verifies selectable configure/build/test presets plus an
actual configure/build/native test run. Browser repair requires a matching SDK.

The installed CLI exposes the same operations:

```bash
ludus project check /path/to/MyGame --tools /path/to/Ludus
ludus project repair /path/to/MyGame --tools /path/to/Ludus --sdk /path/to/sdk
```

When tools or an SDK move, check and repair explicitly. Keep machine paths in
ignored `CMakeUserPresets.json` and `.ludus/local.json`; the committed engine
requirement/release lock describes portable intent. The IDE should use the
selected project-managed CMake in preset mode. A hidden base preset alone does
not prove a usable setup. See [platform compatibility](../wiki/guides/platform-targets.md)
and [Editor operations](editor-workspace.md#create-initialize-repair-and-update-projects).

## Engine override and refresh (engine developers)

```bash
# Point a project's build at a locally built SDK, without touching the lock:
ludus project engine ./MyGame --sdk /path/to/local/sdk --profile linux-clang-development

# After rebuilding/reinstalling that local SDK, just rebuild the game — the
# tooling detects the changed SDK inputs (not just the path) and reconfigures/
# relinks. Local engine install and game build must not run at the same time.
ludus project build ./MyGame --profile linux-clang-development

# Return to the locked release without editing committed files:
ludus project engine ./MyGame --clear-override --profile linux-clang-development
```

The override is visible in build output and never rewrites the committed
descriptor/lock. The `engine` command changes the local SDK selection; it has no `--version`
option. Committed engine requirements and release locks remain explicit project
intent. The migration command below handles legacy version-1 project metadata.

## Migrate a version-1 project

```bash
ludus project migrate ./LegacyGame --engine 0.1.0
```

Migration is explicit (Open never migrates silently), preserves relative paths and
launch settings, requires an engine selection, and commits the new descriptor and
lock together. If a migration is interrupted, leftover `*.migrating` files mark an
incomplete commit; rerun migration or discard them to recover.

## Direct CMake (no Ludus CLI)

```bash
export LUDUS_SDK_PREFIX=/path/to/installed/sdk-prefix
cmake --preset linux-clang-development
cmake --build out/build/linux-clang-development --target my_game

# macOS, after explicit project repair, select its owned toolchain preset:
/path/to/Ludus/out/host-tools/venv/bin/cmake --preset ludus-local-macos-clang-development
/path/to/Ludus/out/host-tools/venv/bin/cmake --build --preset ludus-local-macos-clang-development
/path/to/Ludus/out/host-tools/venv/bin/ctest --preset ludus-local-macos-clang-development
```

The generated presets read `$env{LUDUS_SDK_PREFIX}`. `find_package(Ludus CONFIG
REQUIRED)` resolves the bundled dependency metadata relative to the SDK prefix and
performs the usual CMake version/variant compatibility checks.

## Recovery and failure modes

- Unresolved lock + no override → build stops with an actionable message (install
  the release or set a local override); it never falls back to building the engine.
- Flavor/toolchain/policy mismatch → rejected before configure with expected-vs-
  actual detail.
- Corrupt SDK in the store → explicit `ludus sdk install --repair`.
- Interrupted migration → discard `*.migrating` and retry.

## Compatibility limits

SDK identity is derived from the real build inputs (triple, compiler id/version,
C++ runtime ABI, flavor, variant, policy, features) — never inferred from a
version string. An SDK is used only when that identity is compatible with the
request. See [the architecture](../architecture/project-sdk-workflow.md) for the rationale
and [implementation evidence](project-sdk-workflow-evidence.md) for the recorded
validation scope.

## Ludus-Sandbox conversion (follow-up)

Ludus-Sandbox should become the reference independent application using this
interface (remove its mandatory nested engine build; pin an engine release). That
repository is **not present in this checkout** and has not been inspected, so its
conversion is a **separate** change requiring access to that repository and its
own evidence. The reference consumer validated here is the standalone project in
`tests/sdk_consumer` plus the generated minimal template; converting Sandbox is
tracked as external follow-up and is not claimed complete by this work.
