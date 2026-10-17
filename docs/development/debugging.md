# Native debugging with RAD on Linux

RAD is an optional external debugger. VS Code remains the source editor, and
existing VS Code debugger configurations and F5 bindings remain yours.
The architecture and future editor/scripting milestones are in
[developer-tools.md](../architecture/developer-tools.md).

## Install

```bash
# During onboarding:
./init.sh --with-rad-debugger

# Or on an already initialized checkout:
./scripts/setup-rad-debugger

# Require all build dependencies to be present; never run apt:
./scripts/setup-rad-debugger --no-system-install
```

The installer fetches the exact official RAD commit pinned in
`config/rad_debugger.json` and builds its release configuration with Clang 23.
Source and executable stay under `out/host-tools/raddebugger/<revision>/`.
Rerunning setup reuses a completed installation. Failures can be retried; setup
refuses to overwrite unexpected source revisions or edits. Ordinary `init.sh`,
builds, tests, and CI do not install or require RAD.

On Ubuntu/Debian, explicit RAD setup can install Git, Clang/LLVM 23, pkg-config,
and the FreeType, X11, Xext, Xfixes, GL, and EGL development packages if missing.
On other Linux x64 distributions, install equivalent packages yourself and use
`--no-system-install`. RAD currently uses X11; a Wayland desktop needs XWayland
and a valid `DISPLAY`. This is independent of Ludus's Wayland game windows.

For an existing RAD installation, set `LUDUS_RAD_DEBUGGER` to its executable
path, or use `--debugger /path/to/raddbg` on a debug command. The launcher chooses
an explicit option, then that environment variable, then the completed managed
installation, then `raddbg` on PATH. Invalid explicit preferences fail rather
than silently falling back. `scripts/doctor` reports RAD as optional and labels
external executables as unverified; it never opens RAD to detect a version.

## Update or roll back

Ludus pins a reviewed version and commit in `config/rad_debugger.json`. An
upstream release does not automatically update your debugger. After pulling a
Ludus change that updates the pin, close RAD and run:

```bash
./scripts/setup-rad-debugger
./scripts/doctor
./scripts/debug --dry-run linux-clang-debug ludus_smoke
```

Use `--no-system-install` on setup if you manage dependencies yourself. Launch,
doctor, and ordinary init never download RAD. Explicit executable preferences
take priority over the managed pin, so check the reported path if you expected
the new version.

Each commit installs into a separate directory. Setup retains previous builds;
a failed download or build does not replace them. Retry setup after fixing the
failure. If needed, choose a previous build explicitly (replace the revision
placeholder with its full commit):

```bash
./scripts/debug --debugger "$PWD/out/host-tools/raddebugger/<previous-revision>/source/build/raddbg" linux-clang-debug ludus_smoke
```

Alternatively, restoring the previous repository pin makes its completed build
discoverable again. An unavailable new pin does not silently select an older
managed build; an explicitly configured or PATH executable can still be used.
Previous installs and sessions are not automatically pruned.

An upgrade starts with fresh debugger settings. Existing breakpoints and UI
state remain saved under the previous debugger identity and are reused when
you roll back. To import settings deliberately, prepare the new session with
`--dry-run`, close both debugger versions, back up the new session files, and
copy the old `session.raddbg_user` and `session.raddbg_project` into the new
directory printed by the launcher. Keep the originals, test the copied settings
in the new RAD, and restore the new-session backup if they fail to load. Do not
point the new RAD directly at the old session: RAD autosaves its files.

Maintainers must update the version/commit together, check changed dependencies
and adapter assumptions, run tooling tests, and repeat the interactive
compatibility acceptance below for each new pin. A future editor will expose
explicit update/rollback and settings-import actions through this same policy.

## Launch a target

```bash
# Configure and build only ludus_smoke, then open RAD:
./scripts/debug linux-clang-debug ludus_smoke

# Optimized development build:
./scripts/debug linux-clang-development ludus_smoke

# Exercise an individual test executable:
./scripts/debug linux-clang-debug ludus_foundation_base_tests

# Options precede the preset; target arguments follow --:
./scripts/debug --cwd /path/to/game/content linux-clang-debug my_game -- --level forest

# Reuse an existing configuration and binary:
./scripts/debug --no-build linux-clang-debug ludus_smoke

# Inspect the resolved launch without opening a GUI (still builds by default):
./scripts/debug --dry-run linux-clang-debug ludus_smoke
```

The target must be an executable in the selected CMake preset. The launcher uses
the CMake File API, including custom executable output paths. `--no-build` needs
a configuration created by a prior debug command; it does not configure or
download tools. The default working directory is the repository root.

RAD opens with a temporary command-line target. Set your breakpoints and select
Run or Step Into in RAD. The launcher does not auto-run the application. Do not
use **Save To Project** for the supplied target: it is recreated on each launch,
and saving it would add another persistent target. If you have saved additional
targets manually, select/disable them in RAD before running.

Session files live in
`out/debug/rad/<preset>/<target>/versions/<debugger-identity>/`. Managed debugger
identities are `revision-<commit>`; external executables use `external-<sha256>`
of their contents, so replacing an external binary at the same path also gets
separate state:

- `launch.json` is regenerated and records the executable, argv, cwd, debugger
  identity, and managed pin when available.
- `session.raddbg_project` retains breakpoints and project debugger settings.
- `session.raddbg_user` retains personal UI and watch settings.

Ludus creates the RAD files only when absent and never rewrites their contents.
The launcher waits until RAD exits and rejects a second launch for the same
target, even with another debugger version. Close RAD before rerunning that
target. Earlier unversioned files directly under the target directory are kept;
you can import them by copying as described above. All files are local ignored
state; removing `out/debug/` removes your saved debugger sessions.

## From VS Code

Run **Tasks: Run Task** from the Command Palette and select:

- **Ludus: Set up optional RAD Debugger** to install it.
- **Ludus: Debug target with RAD** to choose Debug/Development and an executable
  target (defaults to `ludus_smoke`).

No extension or keyboard remapping is required. You may assign your own shortcut
to the task through VS Code user settings. Breakpoints, watches, and stepping
belong to RAD; VS Code's breakpoint dots and debug toolbar do not control it.
Use the CLI for game arguments and a custom working directory.

## Compatibility and acceptance

The pinned v0.9.29-alpha release has preliminary Linux x64 support and no
prebuilt Linux binary. Its known limitations include fork/vfork, some TLS and
DWARF types, and unwinding without `.eh_frame_hdr`.
[Upstream release notes](https://github.com/EpicGames/raddebugger/releases/tag/v0.9.29-alpha)

The pinned Linux argument parser also passes quote characters through to the
debuggee and loses empty arguments. Consequently, this launcher rejects empty,
double-quoted, control-character, or whitespace-containing argument values.
Simple flags and values work; shell-looking text is passed as data. Paths to
the executable and working directory may contain spaces. Use LLDB/GDB for an
argv configuration RAD cannot represent. Recheck this restriction on pin upgrades.

The integration accepts Debug and Development presets. Release/Profile stepping,
sanitizer debugging and browser debugging are separate workflows. Independent
installed-SDK games can use the Editor flow below; scripts/debug still targets
the engine checkout. There is no debugger attach command yet.

Debugging launches the engine executable directly, using the existing attached
debugger assertion path. Continue past an enabled resumable assertion only after
inspection; REQUIRE/FATAL remain terminal. `scripts/run` continues to provide
diagnostic-helper report capture. The RAD launcher does not wire its descriptors.

Before recording a pin as validated for Ludus, perform D2 acceptance on a real
desktop: source breakpoints, locals and watches, stack/thread views, assertion
break/continue and terminal failures, argv/cwd, restart, and session persistence
in both Debug and Development. Build success and automated launcher tests do not
prove interactive stepping or correct DWARF evaluation.

## From the Ludus Editor

Use **Build & Debug** in the Game toolbar or Build menu (F5 inside the Editor).
The saved native Debug or Development profile is configured and built before
launching RAD. The executable must contain DWARF symbols and an .eh_frame_hdr
section. Both engine targets and independent SDK game projects are supported.

RAD remains optional. Opening a project never installs or starts it. When Debug
cannot find the selected executable, the Editor offers **Set Up RAD**, **Choose
Existing**, and **Cancel**. Setup explicitly downloads/builds the pinned revision
without running a privileged system installer. Missing system prerequisites are
reported in Output. A chosen external executable is identified as unverified.
The choice is saved in the ignored local .ludus/debugger.json file.

**RAD session open** means the debugger started; the game may be paused or running.
Use RAD to run, step, inspect variables, and manage breakpoints. The Editor does
not infer a successful game run from RAD exiting. Stop closes the owned debugger
session and its game, including a paused child, before enabling another build.
A failed build or symbol check never launches the previous executable.

Session files stay under the game's out/debug/rad directory, separated by profile,
target and debugger identity. Existing RAD project/user files and breakpoints are
preserved. Editor and CLI launches share the same exclusive session locks. The
pinned Linux RAD argument restrictions described above apply to Editor launches.
Attach, embedded debugger controls, and browser debugging are future work. The
interactive compatibility acceptance checklist above remains pending.
