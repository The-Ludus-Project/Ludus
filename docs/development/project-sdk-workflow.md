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

> Native project tooling supports **Linux x64** and **macOS** with pinned
> upstream Clang 23, single-config Ninja, and Debug/Development/Release SDK
> identities. Ubuntu 24.04 is the Linux CI baseline. The installed macOS CLI
> has native Apple silicon acceptance. Intel identity and architecture selection
> have unit-test coverage, with native Intel acceptance pending.
> [Windows MSVC project setup](#windows-msvc-projects) covers four SDK flavors,
> the [runtime SDK](#windows-runtime-sdk), and
> [VS Code and Visual Studio](#vs-code-and-visual-studio-on-windows) workflows.
> See the guides for [World authoring](editor-workspace.md#world-renderer-and-camera-preview),
> [native live editing](project-live-reload.md) and [browser releases](editor-game-releases.md)
> for their setup and validation coverage.

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

## Continuous integration (GitHub Actions)

A project can carry a GitHub Actions workflow that builds and tests every push
to `main`, every pull request and every manual run. Add it while creating the
project, or later to an existing one:

```bash
# New project (in the Editor: tick "Add a GitHub Actions build and test workflow"):
ludus project create ./MyGame --name "My Game" --sdk /path/to/sdk-prefix \
    --ci github --tools-ref <40-character Ludus commit>

# Existing project; --platform defaults to the project's preset family
# (macOS uses this host's architecture):
ludus project ci init ./MyGame --tools-ref <40-character Ludus commit>

# Existing project, one build job per listed platform:
ludus project ci init ./MyGame --tools-ref <40-character Ludus commit> \
    --platform linux-x64 --platform macos-arm64
```

Setup writes `.github/workflows/ci.yml` and `config/ludus-tools-revision.txt`.
These files are yours: edit the triggers, branches and steps as the project grows.
Setup never overwrites a file. An existing tools pin naming the same commit, for
example one written by `ludus project release init`, is reused. A pin naming a
different commit is a `Conflict`. The Editor pins the commit of its trusted
tooling checkout. CI checks out that exact commit from
`github.com/The-Ludus-Project/Ludus`, so the commit must be available there.

The workflow uses the project's selected native profile. For example, a project
whose descriptor selects `macos-clang-development` builds and runs CTest with a
Development SDK. If you request multiple platforms, each gets the corresponding
preset for the same build flavor. Include that selectable preset in the project
for each requested platform.

When no SDK archive is configured, CI prepares the pinned Ludus checkout, builds
and installs its SDK for the job's profile, and uses the normal project setup
command to configure, build, and test the game against the installed SDK. For a
project that requires an exact SDK source revision, use that commit as the tools
pin. The SDK and game are built with the same prepared compiler.

To reuse a prebuilt SDK, set both repository variables for the selected flavor.
The URL must be HTTPS and the SHA-256 must be lowercase hexadecimal.

| Build flavor | Archive variables |
| --- | --- |
| Debug | `LUDUS_DEBUG_SDK_URL`, `LUDUS_DEBUG_SDK_SHA256` |
| Development | `LUDUS_DEVELOPMENT_SDK_URL`, `LUDUS_DEVELOPMENT_SDK_SHA256` |
| Release | `LUDUS_RELEASE_SDK_URL`, `LUDUS_RELEASE_SDK_SHA256` |

The Release names are shared with the [release workflow](game-packaging-publishing.md).
If either variable is set, CI validates the pair and fails on incomplete or
invalid input. The archive must match the game's SDK requirements, runner and
profile, including the compiler version. For multi-platform CI, append the
platform suffix to both variables, for example
`LUDUS_DEVELOPMENT_SDK_URL_MACOS_ARM64` and
`LUDUS_DEVELOPMENT_SDK_SHA256_MACOS_ARM64`.

The workflow uses read-only repository permissions and no configured secrets.
It can download a release asset from the game repository itself using the
job's read-only token, including for private game repositories. Other archive
URLs must be publicly readable. Every download is limited to 4 GiB and verified
against its SHA-256 before installation.

Linux jobs run on `ubuntu-24.04`. Apple silicon jobs run on `macos-15`, and
the SDK keeps its macOS 14.0 deployment floor. Setup refuses `macos-x64`
because hosted Intel macOS runners lack the required LLVM binaries.

GitHub runs workflows only from the repository root's `.github/workflows`.
Setup therefore refuses a project inside another Git work tree; add a workflow at
that repository's root yourself. A new project is never the root of an existing
work tree, so the Editor disables the option for such a location. A project
outside any repository is accepted; initialize Git there and push it to GitHub.
`ludus project ci init` reports whether `origin` is on github.com. It reads
`.git/config` and never runs Git or contacts the remote. GitHub Actions is the
only supported provider.

## Windows MSVC projects

Use the x64 Visual Studio 2022 developer shell initialized with MSVC 14.44
and Windows SDK 10.0.26100.0. Install Python 3.10 or later and prepare the host
tools in the Ludus checkout. Run these PowerShell commands from that checkout.

```powershell
$tools = $PWD.Path
$env:PYTHONPATH = "$tools/scripts/python"
python -m venv out/host-tools/venv
$python = "$tools/out/host-tools/venv/Scripts/python.exe"
$pins = Get-Content config/tool_versions.json | ConvertFrom-Json
& $python -m pip install "cmake==$($pins.windows_msvc_probe.cmake)" "ninja==$($pins.managed.ninja)"

$project = 'C:/Projects/MyGame'
& $python -m ludus_tools project create $project --name MyGame --tools $tools --sdk 'C:/SDKs/Ludus/Debug'
& $python -m ludus_tools project check $project --tools $tools
& $python -m ludus_tools project repair $project --tools $tools --profile windows-msvc-profile --sdk 'C:/SDKs/Ludus/Profile'
& $python -m ludus_tools project check $project --tools $tools --profile windows-msvc-profile
```

The native editor accepts the same version-2 Windows profiles as the project
tooling. The Project profile control lists both Ninja and Visual Studio
configurations. A new Windows editor project starts with the Ninja Development
preset; hosted Play uses the Ninja Debug and Development presets.

The minimal template provides `windows-msvc-debug`,
`windows-msvc-development`, `windows-msvc-profile`, and
`windows-msvc-release` as configure, build, and test presets. Select a matching
[installed SDK](../architecture/windows-direct3d12.md) for each flavor.
Development and Profile both use CMake `RelWithDebInfo` and have separate
SDK selections and build trees. Create with `--tools` verifies the new
project before publication. Repair configures, builds and runs CTest with
the local preset. Use the resulting `ludus-local-windows-msvc-*` presets in
CMake Tools launched from the same developer environment.

For Visual Studio/MSBuild, use the corresponding
`windows-msvc-visual-studio-debug`, `windows-msvc-visual-studio-development`,
`windows-msvc-visual-studio-profile`, or `windows-msvc-visual-studio-release`
profile. Each profile generates a solution with the configuration accepted by
its installed SDK. Development and Profile use `RelWithDebInfo` in separate
solutions, each linked against its own SDK flavor. The CLI builds that
configuration and uses the CMake File API to launch its executable. For example:

```powershell
$profile = 'windows-msvc-visual-studio-profile'
& $python -m ludus_tools project repair $project --tools $tools --profile $profile --sdk 'C:/SDKs/Ludus/Profile'
& $python -m ludus_tools project run $project --profile $profile
```

Check reports missing or stale setup without writing files. After moving an SDK
or replacing the tooling, run repair with the new paths. Repair preserves
other flavors, custom presets, and unrelated IDE settings. For Win32 and
D3D12 applications, select the installed `Platform` and `GraphicsRhi`
components. Repair prepares Slang and DXC for projects using
`ludus_compile_shader`. The Windows workflow uses a local SDK prefix. The
Visual Studio solution installs the Foundation, Input, Platform, and RHI closure
in all four flavors. The runtime SDK below adds the renderer and content closure
with a Ninja producer and consumers for both generators. Editor hosting, dynamic GameHost loading, browser setup, and
release automation have separate acceptance requirements.

### Windows runtime SDK

The `windows-msvc-runtime-sdk` preset builds and installs the runtime closure
that the [Windows World player](../examples/world-demo.md#windows-direct3d-12)
uses: Renderer, GraphicsText and Text, Content and Resources, World, camera and
sequencer, UI, localization, audio processing, fluid, networking wire formats
and the static GameHost. Install it from the same developer shell after
preparing the RelWithDebInfo FreeType and HarfBuzz packages:

```powershell
$env:LUDUS_WINDOWS_DEPENDENCIES = "$tools/out/windows-msvc/runtime-sdk-dependencies"
conan export third_party/harfbuzz
conan install --requires=harfbuzz/14.5.1 --lockfile conan.lock --lockfile-partial `
  --profile:host config/conan/profiles/windows-msvc-probe-x86_64 `
  --profile:build config/conan/profiles/windows-msvc-probe-x86_64 `
  --settings:host build_type=RelWithDebInfo --settings:build build_type=Release --build=missing `
  -o 'freetype/*:shared=False' -o 'freetype/*:with_png=False' -o 'freetype/*:with_zlib=False' `
  -o 'freetype/*:with_brotli=False' -o 'freetype/*:with_bzip2=False' `
  -g CMakeDeps --output-folder $env:LUDUS_WINDOWS_DEPENDENCIES
python scripts/shader-probe bootstrap
./scripts/install-sdk.ps1 windows-msvc-runtime-sdk
```

The result in `out/install/windows-msvc-runtime-sdk` is a relocatable
Development SDK built as `RelWithDebInfo`. FreeType and HarfBuzz are bundled
under `lib/cmake/Ludus/dependencies`, and install rejects metadata that still
names the checkout or the Conan cache. `ludus_compile_renderer_shaders` compiles
the built-in renderer recipes to DXIL with the pinned Slang and DXC. Select the
prefix with `--sdk` for the `windows-msvc-development` or
`windows-msvc-visual-studio-development` profile, and list the modules you link
in `--components`.

Use a separate CMakeDeps folder for each dependency configuration and install
each flavor to its own prefix. Switch the Conan host `build_type` in the command
above to match the selected SDK preset. For Debug, pass
`config/conan/profiles/windows-msvc-debug-x86_64` as the host profile.
It supplies the pinned FreeType feature results to HarfBuzz because CMake's
Debug feature probe cannot resolve the nested Conan import.

| Flavor | SDK preset and install directory name | Conan host build type |
| --- | --- | --- |
| Debug | `windows-msvc-runtime-sdk-debug` | `Debug` |
| Development | `windows-msvc-runtime-sdk` | `RelWithDebInfo` |
| Profile | `windows-msvc-runtime-sdk-profile` | `RelWithDebInfo` |
| Release | `windows-msvc-runtime-sdk-release` | `Release` |

The Debug, Development and Profile SDKs embed MSVC debug information in the
engine objects, so linking a relocated archive does not require the producer's
compiler PDB. Use the matching `windows-msvc-<flavor>` or
`windows-msvc-visual-studio-<flavor>` project preset with the corresponding
prefix under `out/install`. The runtime SDK includes the scene cooker (`SceneCook`) and shader file delivery
(`ShaderDevelopment`). It omits the development diagnostic session, Unicode and
the OIDN denoiser. Behavior is an opt-in for the Development SDK journey.

To include the optional Luau Behavior provider and its paired cooking tools,
run these commands from the same x64 developer shell:

```powershell
python scripts/script-provider bootstrap
./scripts/install-sdk.ps1 windows-msvc-runtime-sdk --behavior
```

Configure the scripted game sample against this prefix to cook its assets and
build the static player, host and reloadable DLL:

```powershell
cmake -S examples/scripted-game -B out/scripted-game -G Ninja `
  -DCMAKE_CXX_COMPILER=cl.exe -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  "-DCMAKE_PREFIX_PATH=$PWD/out/install/windows-msvc-runtime-sdk"
cmake --build out/scripted-game
ctest --test-dir out/scripted-game --output-on-failure
```

See [S5](../architecture/behavior-s5.md) for the debugger contract. The Windows SDK lane relocates the prefix before building the
sample, launches the static player, and exercises the module's script debugger
through the host's inherited socket. The Windows game helper places the static
player and its PDB under `shipping/` in the build directory. The development
DLL and its matching PDB stay beside the host executable.

### VS Code and Visual Studio on Windows

Start both IDEs from the x64 developer shell so they inherit MSVC 14.44 and
Windows SDK 10.0.26100.0. Repair the profiles you use first; it writes the
`ludus-local-windows-msvc-*` presets and generates each solution.

**VS Code.** Open the project folder and install the recommended CMake Tools
and C/C++ extensions. Repair sets `cmake.useCMakePresets` to `always` and,
unless you chose another provider, routes C/C++ navigation through CMake Tools,
so the language service uses each preset's code model and SDK headers. Select a
`ludus-local-windows-msvc-*` configure preset, then build and test from CMake
Tools. MSVC reports `file(line): error Cnnnn`, which CMake Tools shows in the
Problems panel. The tracked `.vscode/launch.json` configuration
**Ludus: Debug launch target (Windows)** debugs the selected launch target with
`cppvsdbg` in the project directory, so breakpoints, stepping and locals work
with F5. Its program comes from CMake Tools, so the file holds no machine paths.

**Visual Studio.** Open
`out/build/windows-msvc-visual-studio-<flavor>/<Project>.sln`. The game is the
startup project and starts in the project directory, so F5 builds and debugs it.
Each solution has the single configuration its SDK accepts.

**Install.** Run **CMake: Install**, build the solution's `INSTALL` project, or
use CMake directly. The game goes to `bin`, with the Agility runtime it loads in
`bin/D3D12`:

```powershell
cmake --install "$project/out/build/windows-msvc-debug" --prefix "$project/out/install/debug"
cmake --install "$project/out/build/windows-msvc-visual-studio-debug" --config Debug --prefix "$project/out/install/vs-debug"
```

To switch configurations, repair each profile once, then change the CMake Tools
preset or open the other solution. Each profile has its own build tree, so
switching does not invalidate the others.

Projects created from template version 7 contain the launch configuration,
startup project and install rules. Repair does not add them to older projects;
copy them from a newly created project if you want them.

The Windows workflow exercises these journeys with the relocated SDKs. It
creates a project with the shipped CLI and repairs the Debug and Development
profiles with Ninja and MSBuild. It then checks the code model's SDK include
directory and the solution's startup project. It sets a source-line breakpoint,
steps out and over, and reads locals with `cdb`, which uses the same debugger
engine as Visual Studio and `cppvsdbg`. It checks the reported location of a
compiler error, then installs and launches the game. Interactive IDE sessions
on a developer machine are not automated.

## Check and repair project setup

In the Editor choose **Project → Check Setup**. Eligible project opening also
performs this read-only check: missing/stale presets, tools and SDK inputs are
reported without downloading, configuring, building or rewriting the game.
A failed check shows a one-line **Setup needs attention** notice above all
workspace tabs, including World; its tooltip holds the full problem. Choose
**Repair setup…** to review and confirm repair, or keep editing and repair later.
**Check again** reruns only the read-only check. Save changed project settings
before either action. The notice stays through retries and failed/cancelled
repairs until setup succeeds; closing or switching projects retires the previous
project's notice. **✕** dismisses the notice for that problem only: a different
problem shows it again, and the status bar line and **Project → Repair Project
Setup…** remain available.

Save settings, then choose **Project → Repair Project Setup** and keep
**Automatic (recommended)** unless changing the engine deliberately.
Repair preserves custom presets and unrelated IDE settings, refreshes stale
owned CMake inputs, and verifies selectable configure/build/test presets plus an
actual configure/build/native test run. Browser repair requires a matching SDK.

Repair also prepares the rendering tools required by the project's saved render
graphs, including native ray tracing on macOS. It selects and verifies these
dependencies automatically; no executable paths or separate bootstrap commands
are needed. The same dependency rules drive graph builds and setup checks, so
**Setup ready** cannot overlook a missing ray-tracing translator. Missing tools
discovered by a graph build bring back the setup notice with **Repair setup…**.
Existing drawable viewport output is retained while setup needs attention.
Tool preparation runs through the background setup job and supports cancellation;
failure or cancellation before project verification keeps existing project settings.
Opening a project and **Check Setup** remain read-only. Native editor installation
prepares its Metal rendering dependencies up front; projects with only raster
graphs do not acquire unnecessary dependencies during repair.

The installed CLI exposes the same operations:

```bash
ludus project check /path/to/MyGame --tools /path/to/Ludus
ludus project repair /path/to/MyGame --tools /path/to/Ludus --sdk /path/to/sdk
```

### When the project's engine is older than the Editor

The Editor runs from its tooling checkout, but a game builds against the engine
install recorded in `.ludus/local.json`. Each install's
`share/Ludus/LudusSdkManifest.json` records the revision it was built from.
**Check Setup** compares that revision with the checkout's `HEAD`, even when
the rest of the check fails. When they differ, or when the selection is not an
install at all (for example a hand-edited path to `out/build/<profile>`), the
project panel names both revisions and shows **Use this editor's engine**. That
button builds and installs this checkout's engine, selects it, keeps saved
browser setup and verifies the project, all in one step.

**Automatic** repair does the same for installs this Editor owns, those below
`<checkout>/out/install` (or a broken selection inside `<checkout>/out`). An
install you chose elsewhere is never replaced silently: Check Setup reports it
and **Use this editor's engine** is the explicit switch. In **Choose a different
installed engine…**, the dialog lists this checkout's matching installs and
rejects build folders, folders without a manifest and other build flavors
before repair runs.

Use the CLI instead of editing `.ludus/local.json` by hand:

```bash
ludus project engine ./MyGame --status --tools /path/to/Ludus   # current, stale, invalid or unknown
ludus project engine ./MyGame --list   --tools /path/to/Ludus   # this checkout's installs and revisions
ludus project engine ./MyGame --sdk /path/to/Ludus/out/install/<profile>-<id>
```

`--sdk` validates the folder (an install, not a build tree, with the project's
target, flavor and engine version) before saving it. Existing `local.json` files
keep working; no migration is needed.

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

## Qualify a project BRDF provider

A game can own a reflection model as material data: a
[closure program](../architecture/scene-source-format.md#closure-program) in a
scene source of its own, the **provider**. The engine's fixture is
`tests/scenes/materials/oren-nayar-provider.luscene.json`, whose
`project/oren-nayar` material is one qualitative
[Oren–Nayar lobe](../architecture/renderer-systems.md#project-oren-nayar-lobe-under-renderer-owned-lights)
with σ = 0.5 radians. Registration and cooking use the ordinary World path:

1. Add the provider scene to the project and import it into the World. The
   import registers its material in the project catalog
   (`assets/catalog.json`).
2. Select the slot to draw with it and choose the material by its project name
   under **Project BRDF** in the **Material** group, then **Assign Project
   BRDF**. The level stores a `project_brdf` binding with the material's
   resource ID; other slots keep their own materials.
3. Save the World. Play and the cooked-scene export cook the binding into the
   same LUSCN record as the provider's material. A standalone app loads that
   record and draws it through an installed graph template with unchanged
   source, beside the engine's built-in materials.

If the provider's catalog record is gone, for example because its cooked assets
were not checked in, the World names the project material, by its project name
and resource ID, and the slot. It tells you to import the provider scene again,
which restores the material under the same resource ID, or to choose **Use
Inherited Material** on the slot. The World keeps the last drawable view
visible until a repaired World prepares. See
[assign and edit a project BRDF](editor-workspace.md#assign-and-edit-a-project-brdf).

### Run the standalone qualification

The fixture is
[`tests/sdk_consumer/project_brdf_world`](https://github.com/The-Ludus-Project/Ludus/tree/develop/tests/sdk_consumer/project_brdf_world).
Its `verify.py` is the reproducible driver on macOS arm64 with Metal and a
Development SDK:

```bash
out/host-tools/venv/bin/python tests/sdk_consumer/project_brdf_world/verify.py \
    --tools "$PWD" \
    --sdk out/install/macos-clang-development \
    --editor out/build/macos-clang-development/apps/editor/ludus_editor_tests \
    --evidence out/project-brdf-world
```

Add `--plan` to print the ordered commands without running them. The driver
does the following:

1. It runs the hidden Editor test `[.sdk-project-brdf-export]`. The test
   imports `oren-nayar-two-light-boards.luscene.json`, two boards of the
   built-in Diffuse control, and the provider. It assigns `project/oren-nayar`
   to the **Project board** and adds two World directional lights, because
   scene sources cannot author lights. It saves the World, reopens it and
   exports its cooked `scene.luscn` with `ExportCookedWorldScene`. It also
   writes the capture view (`view.bin`) and the board pixels (`labels.bin`).
   For the first light, the second light and both together, it captures the
   reopened World three ways at 64 × 32 and 128 samples: the CPU oracle and
   the GPU reference, as raw linear `.luhdr` files with one scattering event
   and centred rays, and the World graph capture through the compute
   ray-tracing template, as `.lurgcap`. `expectations.json` holds each board's
   mean in every capture and in the analytic direct response.
2. It relocates the SDK with its bundled dependencies. It then builds the
   Qt-free `project_brdf_capture` app against it, which cooks the installed
   `compute_ray_tracing` template unchanged, and installs the app to a clean
   package.
3. It stages `scene.luscn` and `view.bin` beside the app. It checks that the
   app depends on no Qt, Homebrew or checkout path and that
   `--check-materials` finds the project material and the built-in control.
4. It launches `./bin/project_brdf_capture --lights <mask> sdk-<set>.lurgcap`
   from the package once per light set. Each board's mean must be within
   3% of the analytic response and of the Editor's CPU, GPU and graph
   captures, and both lights together must be the sum of each alone.
5. It records the commit, SDK identity, World, scene and graph SHA-256 in
   `provenance.json` and the comparisons in `comparison.json`. It also checks
   that a missing `view.bin` exits with code 4 and names the file.

The app links no Qt. It reads the cooked scene through `Ludus::Resources` and
renders through `Ludus::GraphicsRenderer`'s `RenderGraphCapture`, as the
Editor's World graph capture does. A cooked scene without the project
material makes `--check-materials` exit with code 5 and say how to assign and
export it again.

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
