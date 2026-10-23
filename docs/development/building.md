# Building Ludus

Build this checkout to develop the engine or produce an installed SDK. For a
game project, use [the independent-project workflow](project-sdk-workflow.md).
All commands below run from the Ludus repository root.

## Initialization and macOS SDK compatibility

Run `./init.sh` before configuring the engine with a Ludus preset. Native
preset toolchains require the current initialization record; an existing Conan
file alone does not prove setup completed. A plain configure without a toolchain
reports the init command. Advanced builds may provide a caller-owned toolchain;
they still receive the feature dependency checks. Browser setup retains its
separate Emscripten toolchain checks. Configuration never runs init for you.

On macOS, init compiles and links a small probe using the actual Clang compiler,
libc++ headers and SDK before installing managed tools or building Conan
packages. It checks `NAN` and `INFINITY`, whose header definitions HarfBuzz needs,
and that `<cfloat>` still defines `DBL_MAX_10_EXP` after `<cmath>`, which Catch2
needs. Clang 18 with the macOS 27 SDK fails the first check; libc++ 22 with that
SDK fails the second ([llvm/llvm-project#175849](https://github.com/llvm/llvm-project/pull/175849)).
The pinned LLVM 23 passes both with the macOS 26.5 and 27.0 SDKs on the tested
host, so the default Command Line Tools SDK needs no override. Without `SDKROOT`, init tests Clang's default SDK first, then installed
sibling SDKs newest first, selecting the first that passes. It does not download
an older SDK or change the system's Xcode selection.

An explicit `SDKROOT` supplied by your shell is validated and never silently
replaced. `init.sh` preserves that choice before launching Python; a default SDK
added by Apple's Python launcher does not prevent automatic SDK selection. If it fails,
install/select a compatible SDK, then rerun init, for example:

```bash
SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX27.0.sdk ./init.sh
```

If `SDKROOT` was set accidentally, use `unset SDKROOT` and rerun `./init.sh`.
The path above must exist locally. Lowering the deployment target is insufficient:
it changes the minimum supported OS, not the SDK's headers. Existing valid local
SDK/header links are preserved when validation fails.

Init records the real compiler/SDK/header destinations and metadata. Moving or
retargeting them invalidates setup and refreshes native CMake caches during
explicit repair. Conan dependencies receive the selected SDK, compiler paths and
matching libc++ include flags too; cached dependency packages cannot substitute
for the compiler/SDK probe. CMake independently checks the math-header contract
so an overridden SDK fails during configure with recovery instructions.

## Missing or stale dependencies

CMake configuration checks the dependencies required by enabled features before
creating build rules. A **Ludus setup problem** reports the affected feature,
missing tool/file or package setting, and a **How to fix** action. Run that action
explicitly, then use **CMake: Configure** in VS Code (or rerun your configure
command) before building. Configure and build do not download or repair tools.

- Missing generated native/browser toolchain: run the preset-specific `init.sh`
  command printed by the error.
- Removed compiler or Ninja executable: restore it or update the named CMake
  setting; rerun `./init.sh` for project-managed tools. If changing compilers,
  configure a fresh build directory rather than reusing an incompatible cache.
- Missing Catch2, volk, FreeType or HarfBuzz package metadata: prepare the selected preset with
  `init.sh`; tests need `--with-tests`. Custom builds can set the package's `_DIR`
  setting or `CMAKE_PREFIX_PATH` to an existing compatible installation.
- Missing shader compiler/validator: run `./scripts/shader-probe bootstrap` from
  the Ludus source repository. Browser GLSL ES also needs
  `./scripts/bootstrap-spirv-cross`. If tools moved, update
  `LUDUS_SLANG_COMPILER`, `LUDUS_SPIRV_VALIDATOR`, or `LUDUS_SPIRV_CROSS`.
  Metal raster shaders use Slang; native Metal ray queries also use the pinned
  translator. Rendered native editor setup prepares both, and **Project → Repair
  Project Setup** automatically prepares missing tools required by saved graphs.
  Installed SDK consumers use the same checks;
  bootstrap commands belong to the source repository, not the game directory.
- Unusable Python: install Python 3.10+ and set `Python3_EXECUTABLE` if needed.
- Missing editor Qt: use opted-in native editor setup or follow the
  [browser editor guide](browser-editor.md); an existing installation can be
  selected with `Qt6_DIR`.
- Missing Luau source profile: run `./scripts/luau-probe bootstrap` explicitly.
  Behavior cooking reports missing paired SDK host tools and inputs with their
  declaration names; restore the matching SDK or correct those arguments.
- Missing shader/reflection inputs: restore the named project file or fix the
  source/schema/baseline declaration. Missing installed generator files require
  restoring or reinstalling the SDK.

Optional features do not require their tools when disabled. Existing but
unrunnable Slang/validator executables report their probe failure, including
shared-library or permission problems. These checks catch common setup failures;
package-internal errors and dependencies removed after configuration can still
produce underlying CMake/build-tool diagnostics. They are not a full integrity
or tool-version audit; build-time shader checks retain that responsibility.

## One-command onboarding


```bash
git clone https://github.com/The-Ludus-Project/Ludus.git
cd Ludus
./init.sh
```

On an interactive desktop, that command opens a graphical setup window. Choose
Engine contributor (all native presets), Application developer (one native
preset), Browser developer (one Emscripten preset), or Full validation, then
review the preset and optional tools before pressing **Initialize**. Closing or
cancelling the selection runs no setup actions.

After **Initialize**, a progress window runs setup and shows:

- each planned step (for example *Conan install*, *CMake configure (editor)*,
  *Building the editor*) with a tick, a spinner or a cross, and its duration;
- *Step N of M*, a progress bar that also advances with Conan's preset count
  and Ninja's `[done/total]` build status, the newest output line and the
  elapsed time;
- **Show output** for the live log. The complete log is also written to
  `out/init/setup.log`, and the launching terminal keeps receiving it;
- **Cancel**, which asks for confirmation and stops the runner and its
  CMake/Ninja/Conan/sudo children. Finished steps are kept, so rerunning resumes;
- the outcome: *Ludus is ready* with the runner's summary, or *Setup failed
  during: step* with the failed step marked, the error, the output's last lines
  and, for a recognised cause, a plain-language fix. **Retry** reruns the same
  choices after a failure or cancellation.

When setup fails, **Open full log** opens `out/init/setup.log` in the desktop's
viewer (`open` on macOS, `xdg-open` on Linux), **Copy path** copies its
location and **Copy error report** copies the failed step, error, hint, log
path and last lines for a bug report. Recognised causes are a full disk, a
virtual environment left behind by a Python upgrade (`No module named
'encodings'`), missing Xcode Command Line Tools, a missing compiler, a sudo
password prompt without a terminal, a locked or interrupted apt, Conan remotes,
network failures, and apt or Homebrew errors; the table lives in
`scripts/python/init_failure.py`. After the window closes, the launching
terminal repeats the step, error, hint and log path, and a `--cli` run prints
the same hint below its error.

Both windows are titled *Ludus Setup* and use the Ludus Block theme and icon
(`scripts/python/init_theme.py`). They follow the desktop's dark or light
appearance; set `LUDUS_SETUP_APPEARANCE=dark` or `light` to choose one, and
`LUDUS_SETUP_SCALE` (for example `2`) when a Linux desktop's scale is not
detected. **Enter** chooses the highlighted action (Initialize, Retry or
Close) and **Esc** cancels or closes. On Linux the window class is
`ludus-setup` and the process name is `ludus-setup`; on macOS the menu bar and
Dock show *Ludus Setup* with the Ludus icon.

A sudo password prompt still appears in the launching terminal; the window
shows a notice while it waits. Setup runs in a child `engine.py init --cli`
process with every chosen option spelled out, so the window never blocks.
In the terminal, the CLI opens with a *Ludus Setup* banner (repository, preset,
scope), prints the same steps as `==> [N/M] label` headers and ends with the
outcome, the total time and the next commands; a failure names the step that
stopped. Colour is used only in an interactive terminal and is turned off by
`NO_COLOR` (any non-empty value) or `TERM=dumb`; the text is identical either way.
With `LUDUS_INIT_PROGRESS=1` it prints machine-readable lines instead:
`LUDUS_PROGRESS <index> <total> <label>`, `LUDUS_PROGRESS_DETAIL <done> <total>
<text>` and `LUDUS_PROGRESS_NOTE <text>` (see `scripts/python/init_progress.py`).

The default Engine contributor workflow prepares system prerequisites,
project-managed CMake/Ninja/Conan, and Conan dependency files for all native
presets. It does not configure or build Ludus engine targets by default. Use the
VS Code CMake extension or the command-line scripts when you want to
configure/build. Browser setup installs its separate pinned toolchain and
configures the selected browser preset.

Use `--cli` for terminal setup. CI, redirected input/output, and headless Linux
sessions automatically use the CLI. The GUI uses Python's optional Tkinter
module (`sudo apt-get install python3-tk` on Ubuntu); if Tk or a desktop display
is unavailable, default setup falls back to the CLI. `--gui` requires a window
and reports an error before setup if it cannot open one. Both interfaces share
the same setup code and flags; these options also work through `scripts/init`
and the PowerShell wrappers. Those wrappers do not establish a native Windows
port; supported native hosts and limits are described below.

```bash
./init.sh --cli                                      # existing terminal workflow
./init.sh --cli --persona application                # one native development preset
./init.sh --cli --persona browser                    # browser development tools
./init.sh --cli --persona validation                 # complete native validation
./init.sh --gui --with-rad-debugger                   # review optional debugger setup
./init.sh --cli --with-editor                        # Qt prerequisites + native editor build
./init.sh --cli --no-editor                          # omit optional editor setup (default)
```

`--persona` supplies defaults; an explicit preset and scope flags override those
defaults. Full validation prepares all native presets even when `--preset-only`
is supplied. Tests are excluded by default: `--with-tests` enables native test
targets and Catch2 dependencies for later builds, while `--run-tests` also builds
and executes them during setup. `--validate`, the Full validation workflow, and
`--ci` explicitly opt into tests as part of validation. They reject `--no-tests`.

| Workflow | Tests | Sample applications | Browser probes | Editor / RAD |
| --- | --- | --- | --- | --- |
| Engine contributor | Off | On | Off | Off |
| Application developer | Off | Off | Off | Off |
| Browser developer | Off | On | Off | Off |
| Full validation | On, executed | On | Off | Off |

Use `--with-smoke-app` / `--no-smoke-app` to select the smoke app and native input
demo, and `--with-web-probes` / `--no-web-probes` for browser feasibility probes.
`--with-shader-probe` / `--no-shader-probe` controls the isolated native shader
probe (off by default, Linux Vulkan only); it requires its separate pinned shader
tools. The GUI disables this option on macOS. Metal shader compilation,
`world_demo` and the Cornell box sample do not require it. If an older init saved
this option on macOS, rerun `./init.sh --no-shader-probe` to repair the saved
choices before configuring again. Required engine
modules and the native diagnostics helper remain available for applications and
the editor. Browser presets reject native tests, RAD, editor setup,
`--all-presets`, `--validate`, and `--ci`.

```bash
./init.sh --cli --persona application                # engine libraries, no samples/tests/editor
./init.sh --cli --with-tests --preset-only            # include tests in future builds; do not execute
./init.sh --cli --run-tests --preset-only             # build and run tests now
./init.sh --cli --persona browser --with-web-probes   # opt into browser probes
```

Selections are saved locally under `out/init/options.json` for the prepared
presets and apply to subsequent CLI, direct CMake preset, and VS Code builds.
Reinitialize to change them. `--no-editor` also disables the editor for those
presets. Custom CMake users can bypass saved choices with
`-DLUDUS_USE_INIT_OPTIONS=OFF` and set their own target options. Existing checkouts
without saved choices retain the committed developer preset settings.

The optional **Build Ludus editor** checkbox is off by default. Selecting it
installs `qt6-base-dev` and `qt6-wayland` on Ubuntu/Debian when missing, enables
`LUDUS_BUILD_EDITOR`, and builds `ludus_editor` for the selected Debug or
Development preset. `--no-system-install` uses your existing Qt 6.4+ installation
instead. Launch it afterwards with `./scripts/editor --preset <preset>`.
Qt remains an optional editor dependency outside Conan and the installed SDK.
See [the editor guide](editor-workspace.md).

Editor setup also keeps this checkout's engine installs current. Game projects
build against an installed engine below `out/install/` (selected in their
ignored `.ludus/local.json`). After building the editor, init compares each
install for the same target, flavor and ABI variant with the revision just
configured. Installs built from another revision are rebuilt and reinstalled
in place, so a project no longer builds against older engine code than the
editor runs. Installs outside this checkout are never changed. Pass
`--no-engine-refresh` to skip this step. Configure also re-runs when `git` HEAD
moves, so a plain `cmake --build` after a pull records the new revision.

Conan may still build missing third-party packages while preparing the dependency cache; Catch2 is included only when tests are enabled. That is dependency setup, not a Ludus engine target build. If an existing CMake cache points at stale tool paths, init may fresh-configure that generated build tree to repair it, but it still does not compile or link Ludus targets.

VS Code is configured to use the project-managed CMake at `out/host-tools/venv/bin/cmake`. If VS Code was already open while `./init.sh` ran, reload the window before pressing the CMake Tools Build button.

Run `./init.sh --validate` when you want the full build/test/check/sanitizer/SDK-consumer validation pass.

RAD Debugger is available as optional Linux x64 tooling. Use
`./init.sh --with-rad-debugger` or `./scripts/setup-rad-debugger` to build the
pinned debugger locally, then `./scripts/debug linux-clang-debug ludus_smoke`.
VS Code provides **Ludus: Debug target with RAD** through Tasks: Run Task;
onboarding preserves F5 and existing debugger preferences. See
[the debugging guide](debugging.md) for usage and Linux alpha
limitations, and [the tooling design](../architecture/developer-tools.md) for
future editor and scripting milestones.

## macOS support


The first macOS slice uses upstream Clang 23 and libc++ with Apple silicon
and Intel profiles, targeting macOS 14 or later. Install Xcode Command Line Tools and
Homebrew's `llvm@23` and `lld@23` (Homebrew ships LLD as a separate formula), then run:

```bash
./init.sh --cli macos-clang-development --preset-only --locked --no-system-install
./scripts/build macos-clang-development
```

The init launcher selects a Python meeting the recorded minimum before opening
the setup selector; it reuses the prepared interpreter on macOS when available.
For graphical setup with Homebrew Python 3.12, install `python-tk@3.12`.
Apple's system Tk 8.5 is unsupported. Missing or old Tk falls back to terminal
setup; use `--cli` to select terminal setup explicitly. `--gui` instead reports
the missing GUI prerequisite without starting installation.

Debug, Development, Profile, Release, and ASan/UBSan configure/build/test presets
are available under `macos-clang-*`. Setup selects the matching architecture's
Conan profile and keeps managed tools/dependencies under ignored `out/` paths.
Use `--with-tests` during setup to prepare native test dependencies.
Refreshing the Conan lock preserves pins needed by other supported hosts.

`--preset-only` prepares dependencies for just the selected preset. Before
selecting another profile in VS Code, prepare it too. For Debug with tests:

```bash
./init.sh --cli macos-clang-debug --preset-only --locked --with-tests --no-system-install
```

Omit `--preset-only` to prepare all native profiles in one setup run.
If the selected preset's generated toolchain is missing, CMake stops before
compiler detection and prints the init command needed to prepare that profile.

Debug, Development, and ASan/UBSan builds are validated on Apple silicon.
Native Cocoa integration tests require a WindowServer session and opt in with
`LUDUS_TEST_COCOA=1`; deferred feature cases explicitly skip. The pinned LLVM 23
sanitizer runtimes start on macOS 26; LLVM before 22.1.3 does not, see
[macOS sanitizer runtimes](#macos-sanitizer-runtimes). The macOS 14 CI job runs the Cocoa lifecycle and keyboard
tests, Metal lifecycle/rendering tests, native filesystem tests and Content read adapters with ASan/UBSan
as well as the regular Development build.

### macOS sanitizer runtimes

On macOS 26 (Darwin 25) and newer, LLVM sanitizer runtimes before 22.1.3 fail
during process start-up, before `main`. ASan (and therefore the
`macos-clang-asan-ubsan` preset) spins forever; TSan crashes with `SIGSEGV`.
Standalone UBSan is unaffected. Runtime options do not avoid it. The affected
code always runs during initialization:

- compiler-rt's `get_dyld_hdr()` locates dyld through
  `dyld_shared_cache_iterate_text()` while it maps shadow memory.
- On macOS 26 that function allocates (`_Block_copy`) and calls `dispatch_once`.
  Both re-enter the half-initialized runtime.
- ASan deadlocks on its own initialization lock. TSan dereferences a null
  thread state in `SlotLock`.

Andrew Haberlandt fixed this upstream in
[llvm/llvm-project PR #182943](https://github.com/llvm/llvm-project/pull/182943)
by using `_dyld_get_dyld_header()`. The fix was backported to LLVM 22.1.3.

Measured on an Apple M5 with macOS 26.6 (25G72), building an empty `main` with
each compiler:

| Compiler | `-fsanitize=address,undefined` | `-fsanitize=thread` |
| --- | --- | --- |
| Homebrew LLVM 18.1.8 (pinned before LLVM 23) | hangs | `SIGSEGV` |
| Homebrew LLVM 21.1.8 | hangs | `SIGSEGV` |
| Homebrew LLVM 22.1.8 | exits 0; detects a heap overflow | exits 0 |
| Homebrew LLVM 23.1.3 (pinned) | exits 0 (10 of 10 runs, SDK 26.5 and 27.0) | exits 0 (10 of 10 runs) |
| Apple Clang 21 (Xcode CLT) | exits 0 | exits 0 |

The pinned LLVM 23 therefore runs `macos-clang-asan-ubsan` locally on macOS 26.
On the same host its full suite passed 81 of 83 tests; neither remaining failure
was a sanitizer start-up failure. [ADR 0025](../decisions/0025-llvm-toolchain-pin.md)
records the measurements behind the pin.

Before a sanitizer configuration adds any target, CMake builds a trivial
program with the selected `-fsanitize` flags. It runs that program for at most
10 seconds, killing its process group on timeout. If the program hangs, crashes
or exits nonzero, configuration stops. The diagnostic names the known cause
when it applies. Running the tests would otherwise hang every test process. A
passing result is cached for that compiler, flag set and host OS version.

If the probe still fails, for example with a user-supplied LLVM older than 22.1.3:

- Use the pinned LLVM 23 (`brew install llvm@23 lld@23`, then rerun `./init.sh`).
- Run local onboarding validation without the sanitizer preset:
  `./init.sh --validate --skip-sanitizers`.
- `-DLUDUS_SANITIZER_RUNTIME_PROBE=OFF` disables the probe. Use it only to
  investigate a runtime, because affected tests then hang again.

Using a different LLVM for this preset is not supported. The repository pins
LLVM 23 for compilation, formatting and analysis
(`config/tool_versions.json`), so another toolchain would need to be adopted
deliberately across all tools. Repository scripts and CMake compiler gates read
the major from that file's `minimum.clang`. The ludus-tools wheel constant, Conan
profiles and example presets cannot read it; `test_ci.ToolchainPinTests` fails
when they disagree. CI workflows, the devcontainer and Behavior profiles still
name the version; `test_ci.ToolchainPinTests` also checks the workflow and
devcontainer package names.

Until Homebrew publishes versioned formulae for LLVM 23, `llvm@23` and `lld@23`
are aliases of the unversioned `llvm` and `lld`, and `brew upgrade` can move them
to the next major. Init then stops and names the formula; `brew pin llvm lld`
prevents the move while those formulae still provide LLVM 23.

Platform now provides [native Cocoa windows and keyboard input](../architecture/macos-platform.md).
It publishes borrowed native handles and backing-pixel dimensions for the Metal backend. RHI supports Cocoa presentation and headless rendering,
including frame clearing, fullscreen pipelines, MSL shaders and uniform uploads.
See the [Metal rendering guide](fullscreen-rendering.md#macos-metal). FoundationFilesystem provides [native regular-file reads](../architecture/filesystem.md),
including pinned roots, revision clones and independent offset reads. Content
read adapters use this backend. Vulkan/Volk is excluded on macOS. Content supports [atomic native saves](../architecture/content-resources.md#native-saves).
Audio stream/device output, interactive diagnostic helpers, the
[native Qt Editor](editor-workspace.md), and the
[world demo](../examples/world-demo.md) support macOS. Game
[release packaging](game-packaging-publishing.md#macos-app-packages) creates
ad-hoc signed app bundles. Live Play/RAD debugging and Developer ID signing/
notarization remain deferred. The smoke app can present through Metal. Linux and browser
backends retain their existing implementations.


## Headless and sandboxed builds

Prepare only the preset you need, with tests explicitly enabled:

```bash
./init.sh --cli linux-clang-development --preset-only --locked --with-tests
./scripts/build linux-clang-development
./scripts/test linux-clang-development
```

Use `--no-system-install` when system prerequisites are already available.
User-owned LLVM 23 installations work when their `bin` directory is on `PATH`
and their shared libraries are discoverable by the host loader. Setup resolves
versioned executables such as `clang++-23` and creates project-local tool shims.
Ubuntu 24.04 does not package LLVM 23. On Ubuntu, init registers the pinned
`llvm-toolchain-<codename>-23` suite from [apt.llvm.org](https://apt.llvm.org/)
when the configured sources lack it, and prints that it adds this third-party
source. Its archive key is committed as `config/apt/apt.llvm.org.asc` and checked
against the published fingerprint first; `python3 scripts/python/llvm_apt_source.py`
performs the same registration manually.
It validates the host toolchain before creating the build-tool venv or downloading
Python packages. `--no-system-install` still downloads the pinned managed tools
and Conan dependencies; `--locked` preserves the committed dependency lock.
Conan resolution, downloads and builds stream their output to the terminal.

Outbound HTTPS access is needed during dependency setup. Builds and tests use
the populated local cache. A headless build needs no Wayland packages; CMake
selects the headless backend automatically when the client headers, protocols
and scanner are unavailable. Inspect the configure output for the selected
backend. Live compositor/GPU tests remain opt-in and skip when their prerequisites
are absent; passing the portable tests does not validate hardware rendering.

Some diagnostic and GameHost tests, and the installed SDK consumer, exercise
Unix domain socket pairs (`SOCK_DGRAM` and `SOCK_SEQPACKET`), socket options and
child processes. Sandboxes may restrict these even though no external service
is contacted. `PermissionError: Operation not permitted` from socket operations,
unavailable diagnostic transports or a host exiting before its protocol handshake
can indicate a sandbox restriction. Allow local IPC in the execution environment,
then rerun the affected tests:

```bash
out/host-tools/venv/bin/ctest --preset linux-clang-development --rerun-failed --output-on-failure
```

Verify the actual failure before attributing it to sandbox policy. Keep these
tests enabled: they validate engine diagnostics and process supervision. To
force diagnostic helpers into report-only mode during local headless work, set
`LUDUS_DIAGNOSTIC_INTERACTIVE=0`.

## Development container

The checked-in `.devcontainer/` configuration supplies the Ubuntu 24.04 reference
host, LLVM 23 from its pinned apt.llvm.org suite (including formatting, analysis and sanitizer tools), Python venv
support and native build prerequisites. It omits Wayland and Qt development
packages so the default configuration uses the headless backend.

Install Docker and the VS Code Dev Containers extension on the host, clone Ludus,
and choose **Dev Containers: Reopen in Container**. Container creation prepares
`linux-clang-development` with tests and the committed Conan lock; it downloads
dependencies but does not build Ludus. Then run in the container terminal:

```bash
./scripts/doctor
./scripts/build linux-clang-development
./scripts/test linux-clang-development
./scripts/install-sdk linux-clang-development
```

Setup runs as the unprivileged `vscode` user with `--no-system-install`. System
packages are installed while building the image, so workspace initialization
does not need sudo. The host still needs access to its Docker daemon; this
configuration does not install Docker or grant daemon access. Container creation
requires network access for the base image, Ubuntu packages and pinned dependencies.
The Dockerfile accepts an optional BuildKit `proxy_ca` secret containing a trusted
CA bundle for HTTPS package mirrors behind a proxy.

Start with a fresh checkout or move existing generated `out/` state aside before
switching between host and container builds. Tool shims, venvs, Conan files and
CMake caches contain machine-specific paths. The container limits builds to two
workers by default; adjust `CMAKE_BUILD_PARALLEL_LEVEL` for the available resources.
Desktop/editor and GPU rendering validation require a separate capable host.

## Trees

The source tree contains only hand-written project files. Generated state belongs under `out/`:

```text
out/host-tools/venv/        pinned CMake, Ninja, and Conan Python packages
out/host-tools/raddebugger/  optional pinned RAD source builds
out/conan/home/             project-local Conan cache (unless LUDUS_CONAN_HOME is set)
out/conan/profiles/         installed Conan profile for this checkout
out/conan/<preset>/         Conan CMakeToolchain and CMakeDeps files
out/build/<preset>/         CMake build trees and compile_commands.json
out/install/<preset>/       installed SDK prefixes
out/test-results/           reserved for local test output
out/logs/                   reserved for local logs
out/debug/rad/              local RAD launch descriptions and debugger sessions
```

The install tree is treated as the SDK boundary. The external consumer test uses `find_package()` against `out/install/<preset>/` rather than `add_subdirectory()` on the engine.

## Build flavor and assertion policy

Presets set `LUDUS_BUILD_FLAVOR` explicitly. SDK variants keep their generated
assertion/profiling policy with their headers and libraries; use separate install
prefixes for each variant. See [diagnostic helpers and assertion policies](diagnostics.md)
for launch/report commands and [the assertion contract](../architecture/assertions.md)
for caller behavior.

## Bootstrap

Run:

```bash
./scripts/bootstrap
```

Bootstrap is the exhaustive dependency-preparation step. It uses the system Python to create `out/host-tools/venv/`, then installs the pinned CMake, Ninja, and Conan versions from `config/tool_versions.json`. This is intentional network access.

`out/host-tools/venv/` is reused only while it still belongs to the Python running init. Init compares the `version` (or
`version_info`) and `home` recorded in its `pyvenv.cfg` with the current interpreter, then runs the environment's own
`python` to import `encodings` and find pip. When any check fails, for example after Ubuntu or Homebrew replaces Python
3.12 with 3.14, init prints `Recreating stale virtual environment (<reason>)`, shows the same notice in the setup
window, deletes only that directory and rebuilds it before reinstalling the pinned packages. Rebuilding in place is not
enough: `venv` keeps an existing `bin/python` copied from the removed interpreter. `./scripts/doctor` lists the reason
on the "Project-managed virtual environment" row, and the `scripts/clangd`, `editor-task`, `setup-editor` and
`setup-quality-tools` wrappers fall back to the system `python3` while the environment cannot start.

After tool validation, bootstrap installs the committed Conan profile into `out/conan/profiles/`, creates `conan.lock`, populates the Conan cache, writes Conan generator files under `out/conan/<preset>/`, and runs a minimal CMake configure smoke test.

## Conan

Conan 2 is used only before CMake configure. CMake does not invoke Conan, `FetchContent`, or any network fetch. The current native dependency set is declared in `conanfile.py` and pinned by
`conan.lock`; Catch2 is included only when tests are enabled. Some engine backends
use separately pinned sources under `third_party/`. Browser builds use the
separate pins in `config/web_toolchain.json`. The early Milestone 0 dependency
inventory is historical.

Normal configure, build, test, check, and SDK install commands use the Conan files already generated by bootstrap. They should not require network access unless generated state is removed.

## Rerunning `./init.sh`

A rerun of `./init.sh` skips steps whose inputs haven't changed since they last
succeeded. The step then prints `Up to date: ...` in the terminal, and the
setup window shows `up to date` in place of its time:

- The pinned CMake, Ninja and Conan aren't reinstalled while the managed
  environment already has the pinned versions.
- The Conan step is skipped while every selected preset's generated toolchain
  exists and its initialization record, the installed profile, the local
  `third_party/harfbuzz` and `third_party/catch2` recipes, the `--locked` choice
  and the Conan home all match the last install. The record lives in
  `out/init/stamps/conan.json`.
- The editor's CMake configure is skipped while its arguments,
  `CMakePresets.json`, `CMakeUserPresets.json`, the saved init options, the
  Conan toolchain and the compiler environment (`CC`, `CXX`, the flag
  variables, `SDKROOT` and the ccache variables) match the last configure. The
  build still reruns CMake by itself when a CMake file or the Git `HEAD`
  changes.
- On macOS, `brew install` isn't run while Homebrew Qt is installed, because it
  refreshes Homebrew's package index every time. Run `brew upgrade qtbase
  qtdeclarative` to update Qt.

The pinned shader tools are still unpacked and checked against their pins on
every run, but in the background while Conan and the next steps run.

Set `LUDUS_INIT_FORCE=1` to ignore every record and redo each step, for example
to resolve newer upstream Conan revisions in an unlocked initialization:

```bash
LUDUS_INIT_FORCE=1 ./init.sh --with-editor
```

When ccache is installed and `LUDUS_ENABLE_CCACHE` isn't set, the editor setup
turns ccache on for a new build tree, so removing `out/` or switching branches
reuses earlier compiles. An existing build tree keeps its choice, since
changing the compiler launcher recompiles everything once. Set
`LUDUS_ENABLE_CCACHE=1` or `LUDUS_ENABLE_CCACHE=0` to choose explicitly.

## Sharing caches between checkouts

By default every checkout has its own Conan cache under `out/conan/home/` and
compiles every translation unit itself. On a host with many checkouts at once,
such as concurrent agent worktrees, a fresh checkout repeats the same
HarfBuzz, FreeType and Catch2 builds and the same engine compiles. Three opt-in
environment variables let checkouts reuse each other's results:

```bash
export LUDUS_CONAN_HOME="$HOME/Library/Caches/ludus/conan"   # Linux: $HOME/.cache/ludus/conan
export LUDUS_ENABLE_CCACHE=1
export LUDUS_CCACHE_SHARE_CHECKOUTS=1
```

Export them before `./init.sh` and before the first configure of a build tree.
CI sets none of them. [Fresh worktree setup cost](build-profiling.md#fresh-worktree-setup-cost)
records what they saved on a shared development host.

### Shared Conan home

`LUDUS_CONAN_HOME` must be an absolute path outside any checkout. The Conan home
is `<path>/conan-<pinned Conan version>`, so checkouts pinned to different
Conan versions never migrate the same cache. The first checkout builds the
third-party packages and Conan's own build tools into it. Later checkouts with
the same recipe revisions, settings and options reuse those packages instead of
building them. Each checkout keeps its own installed profile in
`out/conan/profiles/`, because the profile names that checkout's compiler, SDK
and libc++ links. Its generated toolchain in `out/conan/<preset>/` and its
initialization record stay local too.

Conan 2 does not lock its package cache between processes. `./init.sh`
therefore holds `<home>/.ludus-cache.lock` from the remote update through the
last `conan install`. A concurrent initialization prints `Waiting for another
checkout to finish with the shared Conan cache` and continues afterwards,
usually finding every package already built. Configure and build only read
installed packages, so they don't take the lock.

`--locked` still installs exactly the revisions that `conan.lock` pins. A
shared cache may already hold a pinned revision that another checkout
exported, so a locked initialization fails when this checkout's
`third_party/harfbuzz` or `third_party/catch2` recipe exports a revision that
`conan.lock` does not pin. It no longer quietly uses the older recipe. An
unlocked initialization exports and resolves `conan.lock` under the same lock,
so the lock records this checkout's recipes.

Generated toolchain files point into the shared cache. Removing packages from
it with `conan remove`, or deleting the directory, breaks every checkout that
uses them until it reruns `./init.sh`. Conan leaves each source build's build
folder in the cache. To reclaim that space and keep the packages, run this with
the pinned Conan:

```bash
CONAN_HOME="$LUDUS_CONAN_HOME/conan-2.8.1" out/host-tools/venv/bin/conan cache clean "*" --build --temp
```

Package reuse follows Conan's package IDs. The SDK, compiler and libc++ paths
in the profile are configuration, not package-ID inputs, so a checkout-local
cache already reuses a package after the macOS SDK or Homebrew LLVM changes.
After such a host toolchain change, point `LUDUS_CONAN_HOME` at a new directory,
or remove the packages from the shared cache, to rebuild them.

### Compiler cache across checkouts

`LUDUS_ENABLE_CCACHE=1` compiles C and C++ through
[ccache](https://ccache.dev), which must be installed (`brew install ccache` or
`sudo apt install ccache`). On its own it reuses results only inside one
checkout. ccache hashes the compiler command line, which names absolute
checkout paths, and for `-g` builds it also hashes the build directory.
`LUDUS_CCACHE_SHARE_CHECKOUTS=1` needs ccache 4.8 or newer. It adds these
per-compilation settings to the launcher:

- `base_dir=<checkout root>`: ccache passes paths inside the checkout to the
  compiler relative to the build directory, so every checkout hashes the same
  command line. Paths outside the checkout stay absolute: Homebrew LLVM, the
  SDK and a shared Conan home. CMake always spells a checkout under macOS
  `/private/tmp` as `/tmp`, while the Conan toolchain uses the resolved path.
  For such a checkout, or any checkout reached through a symlink, the launcher
  lists both spellings in `base_dir`, which needs ccache 4.12 or newer. It
  also runs ccache through `env PWD=<build directory>`, because ccache
  computes relative paths from `$PWD` only when `$PWD` names the working
  directory. Otherwise every `/tmp` path would be relativized through `/` and
  the checkout's own name.
- `hash_dir=false`: Development, Profile and Debug builds compile with `-g`.
  Without this setting, the build directory would be part of every key.
- `absolute_paths_in_stderr=true`: warnings and errors keep absolute paths.

Use it together with `LUDUS_CONAN_HOME`. A checkout-local Conan cache installs
source-built packages under randomly named folders such as `p/b/catch<random>`.
Translation units that include Catch2, HarfBuzz or FreeType headers then never
match another checkout's.

Sharing changes these build outputs:

- `__FILE__` and `std::source_location` name sources relative to the build
  directory (`../../../modules/...`), not by absolute path. Linux's
  `-fmacro-prefix-map` no longer shortens them.
- A reused object's debug information names the build directory that compiled
  it first. LLDB opens that checkout's copy of the source. The copy is
  identical when the object was reused, until that checkout changes the file or
  is deleted. Build with `-DLUDUS_CCACHE_SHARE_CHECKOUTS=OFF` when you need
  exact source paths in the debugger.
- Translation units with checkout-specific definitions, such as the text tests'
  absolute fixture directory, still compile in each checkout.
- Generated headers must not embed a checkout path. ccache hashes preprocessed
  output by relativizing every line-start `#line N "path"` under `base_dir`,
  and cannot tell a line marker from the same text inside a raw string literal.
  A header whose embedded text differs only by such a path hashes equal in
  every checkout, so one checkout's object, with its own folded literal length,
  is reused next to another checkout's copy of the same `inline` literal. The
  shader helpers therefore name Slang's emitted `#line` files relative to the
  captured source and include directories
  ([fullscreen rendering](fullscreen-rendering.md#macos-metal)).
- `-ftime-trace` compilations bypass ccache, because ccache does not cache that
  option. `./scripts/profile-build` and `./scripts/check-build-budget` also
  turn ccache off, so build-time evidence is unaffected.

Both settings are CMake cache options seeded from the environment when a build
tree is first configured. To change an existing tree, pass
`-DLUDUS_ENABLE_CCACHE=ON|OFF` or `-DLUDUS_CCACHE_SHARE_CHECKOUTS=ON|OFF`.
Turning `LUDUS_ENABLE_CCACHE` off also removes the ccache launcher from the
tree. All checkouts of one user already share ccache's default directory
(`~/Library/Caches/ccache` on macOS, `~/.cache/ccache` on Linux). One
development build stores about 52 MB there, well within ccache's default
5 GiB limit.

## Presets

Development is the default local flavor. Prepare each preset before selecting
it; `--preset-only` does not prepare another profile's dependencies.

| Flavor | Linux | macOS | Purpose |
| --- | --- | --- | --- |
| Debug | `linux-clang-debug` | `macos-clang-debug` | Debug assertions and source inspection |
| Development | `linux-clang-development` | `macos-clang-development` | Optimized local development |
| Profile | `linux-clang-profile` | `macos-clang-profile` | Profiling flavor with Profile assertion policy |
| Release | `linux-clang-release` | `macos-clang-release` | Release policy; tests off by default |
| ASan/UBSan | `linux-clang-asan-ubsan` | `macos-clang-asan-ubsan` | Sanitizer validation |

Browser presets are `web-emscripten-development` and `web-emscripten-release`.
See [browser packaging](web-packaging.md) for that separate toolchain.
To inspect selectable presets with the project's prepared CMake:

```bash
out/host-tools/venv/bin/cmake --list-presets=configure
out/host-tools/venv/bin/cmake --list-presets=build
out/host-tools/venv/bin/cmake --list-presets=test
```

Development/Profile and sanitizer presets use `RelWithDebInfo`; the explicit
engine flavor determines assertion policy. Saved init selections can disable
sample/test/editor targets within a preset. See [diagnostics](diagnostics.md).

## VS Code

The shared workspace settings make CMake Tools use presets and point it at Ludus' project-managed CMake:

```text
out/host-tools/venv/bin/cmake
```

Run `./init.sh` before pressing the CMake Tools Build button. If the extension still reports `/usr/bin/cmake` or an older CMake version, reload the VS Code window so it picks up `.vscode/settings.json` and the freshly installed managed tools.

After building the native preset you want to edit, select it explicitly:

```bash
./scripts/setup-editor macos-clang-development # use linux-clang-development on Linux
```

Install reference `clangd-23` on Linux (`sudo apt-get install clangd-23` from the pinned apt.llvm.org suite); Homebrew
`llvm@23` supplies it on macOS. Initialization creates its managed shim when
available. Existing prepared checkouts can rerun initialization to add the shim.
Then run **clangd: Restart language server**. The tracked clangd launcher uses
that selection's database with reference LLVM 23 builtin headers and the preset's
explicit SDK/libc++ paths. It executes no external query driver. It rejects
missing/stale setup, moved checkouts, wrong-host presets and incomplete source
inventories. Opening the editor does not download, configure or repair anything.

**Ludus: Select prepared editor preset** performs the same operation. Build,
test, format, tidy, analyzer, tooling and build-cost tasks use that selection.
Compiler/tidy diagnostics enter VS Code's Problems panel through `$gcc`.
CMake Tools maintains its own interactive preset selector: select the same
preset there, or use the Ludus tasks. Changing that selector alone does not
change clangd. Browser editing is not admitted by this native launcher.

Selection lives in ignored `out/editor/selection.json`; repair changes only that
file, preserving custom settings, tasks, launch configurations and user presets.
After moving tools or the checkout, prepare/build again and rerun setup. Failed
selection leaves the previous record intact, but the launcher revalidates it
before use. See the [CI guide](continuous-integration.md#q1-analysis-and-host-tooling)
for the advisory check rollout and pinned quality-tool preparation.

## Checks

Run both formatting and targeted clang-tidy:

```bash
./scripts/check linux-clang-development --all
```

Run only formatting:

```bash
./scripts/check linux-clang-development --format
```

Apply formatting:

```bash
./scripts/check linux-clang-development --format --fix
```

The formatter uses clang-format 23 and preserves compact designated initializers
on one line (`value = { .A = 0, .B = 1 };`). Multiline initializers place the
opening brace on its own line, with one member per line and a trailing comma.
Clang-format 23 alone cannot express the opening-brace rule, so run the project
command for both applying and checking formatting. The brace normalization
changes only whitespace: where clang-format keeps a closing brace on the last
member's line, such as an initializer in a ternary operand, that list keeps
clang-format's layout. If normalization would change any other token, the
formatter reports an error and leaves the file unchanged.

Local `./init.sh` enables the pre-commit formatter. To enable it in an existing
checkout, run `./scripts/install-hooks`. It formats only staged C/C++ in
`modules/`, `apps/`, and `tests/`; partially staged files keep their unstaged
contents, and fully staged files also receive the formatting in the working
tree. Custom hook paths and existing hooks are preserved. CI checks the same
formatter in a separate job before starting builds.

Run only clang-tidy:

```bash
./scripts/check linux-clang-development --tidy
```

clang-tidy uses the selected preset's `compile_commands.json` and analyzes project sources under `modules/` and `apps/`, not generated, installed, or third-party code.

`LUDUS_TIDY_JOBS` bounds concurrent analyzer processes (default 1). CI selects
worker and shard limits per job; the explicit Editor source lists use four
workers on Linux and three on macOS. To reproduce one shard:

```bash
LUDUS_TIDY_JOBS=2 LUDUS_TIDY_SHARD_COUNT=2 LUDUS_TIDY_SHARD_INDEX=0 \
  ./scripts/check linux-clang-development --tidy
```

Shard indices start at zero; every shard must pass. The default count of one
analyzes the full database. Invalid indices and empty shards fail explicitly.

To analyze a few compiled sources concurrently, for example after editing the
Editor, list them explicitly; the result equals running `clang-tidy -p` on each:

```bash
LUDUS_TIDY_JOBS=4 ./scripts/check linux-clang-development --tidy-sources \
  apps/editor/src/controller.cpp apps/editor/tests/controller_tests.cpp
```

This runs the full project tidy policy for the listed sources, without the
changed-line filtering of `--tidy-changed`. Build the selected preset first.
A missing compilation database, duplicate source or source absent from that
database fails before analysis starts. Analyzer failures retain diagnostics
for every selected source and fail the check.

Browser analysis uses the same worker limit and includes owned engine,
application, probe and consumer-fixture sources while excluding pinned vendor implementations.
Both browser build configurations remain checked.

The native Development CI job also installs and relocates the SDK and links an
external consumer with Clang 23, without producer dependency paths. This reuses
the existing SDK build and makes relocation a required gate. Debug runtime
validation covers Debug assertion policies; the separate assertion matrix
continues to exercise Profile and Release.

`--all` (and `--format`) also runs the foundational include-boundary check
(`tools/check_foundational_includes.py`): a fast, text-only gate that fails if a
foundational header (`core.h`/`config.h`/`compiler.h`/`types.h`) pulls a
string/container/heavy STL header, or if any public header pulls a heavy STL or
private `internal/` header. Header self-sufficiency (every public header
compiles standalone) is checked by the `ludus_header_self_sufficiency` CTest.
Its probes run concurrently; `LUDUS_HEADER_CHECK_JOBS` bounds the compiler
processes (default: the CPUs available to the test). Every header keeps the
flags reconstructed from `compile_commands.json` and its own failure report.
The test's 600 s CTest timeout only detects a hung compiler: probe wall time
follows host load, so heavily shared hosts need the headroom. Header parse cost
is budgeted by `config/build_budget.json`, not by this timeout.
See `docs/architecture/foundational-headers.md` and ADR 0007.

## Foundational header

Engine files get the universal Ludus vocabulary — fixed-width types, build/OS/
arch/compiler macros, codegen attributes, `Move`/`Forward`, and the assertion
macros — from a single header:

```cpp
#include <ludus/foundation/base/core.h>
```

Everything heavier (containers, `UniquePtr`, strings, logging, profiling,
platform, graphics) is included explicitly by the files that use it. `core.h` is
what a file *may* rely on, not mandatory boilerplate; a file that needs only the
numeric types may include `<ludus/foundation/base/types.h>` directly. See
`AGENTS.md` ("Foundational headers and the include boundary") for the
Allowed / Required-explicitly / Forbidden rules.

## Precompiled header

`core.h` can be precompiled as a pure build accelerator. It is opt-in and off by
default:

```bash
cmake --preset linux-clang-development -DLUDUS_ENABLE_PCH=ON
```

The PCH only ever mirrors `core.h` (never containers/strings/logging), and the
default build (PCH off) is the guarantee that no file depends on a PCH-only
symbol. See `cmake/EnginePch.cmake` and ADR 0007.

## Cleaning Generated State

It is safe to remove generated state under `out/`:

```bash
rm -rf out/build
rm -rf out/install
rm -rf out/conan/linux-clang-development
```

If you remove `out/conan/` or `out/host-tools/`, rerun:

```bash
./init.sh
```

Do not remove source files to clean builds.

## Diagnosing Toolchains

Run:

```bash
./scripts/doctor
```

Doctor reports required and optional tools, versions, paths, and whether each satisfies the project requirements. It exits nonzero only when a required item is missing or invalid.

## Review-derived source checks

Before claiming PR readiness, read the current human review and every inline
thread, reconcile feedback with implementation/evidence, and repeat after the
final change. Passing build and test jobs does not satisfy unresolved review
requirements. Keep generalizable policy in the contributor rules and add
regressions to the owning check rather than relying on an isolated PR comment.

Run the changed-source gate with an explicit comparison:

```sh
python3 scripts/python/source_policy.py --base origin/develop --clang-format clang-format-23
PYTHONPATH=scripts/python python3 -m unittest test_source_policy test_check_naming test_changed_tidy -v
./scripts/check linux-clang-development --tidy-changed --base origin/develop
```

CI uses the PR's actual base commit and includes standalone examples, tests and
shader includes. It reads the exact PR head tree rather than attributing newer
main changes in GitHub's synthetic merge to the PR (`--head <commit>`). Local
checks without that option include uncommitted and untracked source changes.
The gate rejects newly introduced inferred variable types
except unspellable lambdas or explained iterator/dependent types, checks Slang
function/member/out names and coordinate member labels, and keeps target
annotations at the RHI shader interface boundary. Existing violations are
migration debt and cannot excuse newly added violations; no debt manifest is
regenerated. This is a lexical check: it does not resolve C++ types, prove a
coordinate transform, judge an exception's reason, or validate a native binding.
The gate also rejects recognizable copies of the 500,000-vertex ceiling and
1/4/8/16/32/64 MiB capacities in scene-cooker implementation files, Content's
native file adapter and the Editor's scene composer. It recognizes decimal byte
counts, ordinary `1024 * 1024` expressions (including `usize{...}`) and `<< 20`.
Comments, strings, directives, unchanged lines, owner headers and test fixtures
are excluded. It does not evaluate arbitrary C++ expressions or establish that
the selected constant represents the right policy; review those semantics using
[limit ownership](../architecture/content-resources.md#limit-ownership).

On changed C++ lines the gate also applies the naming rules: `out`/`inout`
reference parameter prefixes (N2), boolean names that read as questions (N3),
`OrNull` on explicitly null-initialized pointers (N4), and a list of rejected
abbreviations in declarations (N5). Outside test sources, `tests/`,
`benchmarks/` and `tools/`, numeric literals other than 0, 1, 2 and 0.5 need a
named constant unless they initialize a `constexpr`/`const` declaration, an
enumerator or a `static_assert`; append `// ludus-style: magic-number <reason>`
to keep a justified literal. `tools/check_naming.py` (run by
`./scripts/check --format`) rejects `!`/`not` negation in every first-party
C++ file. `--tidy-changed` runs clang-tidy `readability-identifier-naming`,
`readability-magic-numbers` and `readability-implicit-bool-conversion` on the
changed lines of each changed translation unit (and the headers it includes);
batch validation runs it in the static-analysis job. A header changed without
a changed includer is only covered by the lexical gate.
Formatting, compilation, independent shader reflection and semantic review
remain necessary. Shader entry-point renames must update every source and
installed-SDK cook call; shared includes must be installed and appear in compiler
depfiles so edits rebuild consumers.
