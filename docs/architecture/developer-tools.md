# Developer tools: RAD Debugger, code editors, and future scripting

Design baseline: 2026-10-01. D0/D1 and an external-debugging subset of D3/D4 are implemented, as recorded in
[implementation status](#implementation-status). D2 interactive acceptance and
the remaining integration milestones are still pending.

## Goals and boundaries

External GPU tooling has a separate
[graphics debugging architecture and phased plan](graphics-debugging.md), starting
with shared GPU names/scopes and Metal/RenderDoc capture. It reuses the process
launch and local tool preference ownership described here.

Use VS Code for source editing and the existing CMake/Conan scripts for builds.
Offer RAD Debugger as the preferred external native debugger on Linux x64.
Contributors retain their own debugger and keybindings: no onboarding command
rewrites F5, user settings, or existing launch configurations. RAD is optional
and never becomes a required engine, SDK, or CI dependency.

The CLI establishes the workflow before Ludus has an editor. Later, the editor
should consume the same launch description and tool discovery rules. Avoid a
RAD dependency in FoundationBase, automatic debugger launch on assertions,
an embedded code editor, or a custom Debug Adapter Protocol implementation now.
All eventual engine-side process APIs must follow AGENTS.md: explicit failure,
no exceptions, private OS implementation, and no heavy public-header includes.

## Current upstream support

Pin RAD v0.9.29-alpha, commit
`cd41ba199bbe091d348a9b2be5a3528cb8acbff0`. Its Linux x64 support is preliminary;
upstream supplies source rather than Linux release binaries. Build it on the
developer's host with the reference Clang 23, using upstream's build script.
Keep its source, artifacts, and build stamp under ignored `out/host-tools/`.
Changes to the pin require another compatibility acceptance pass.

The release lists unsupported fork/vfork debugging, TLS assumptions about libc,
unwinding problems without `.eh_frame_hdr`, and incomplete bitfield/DWARF type
handling. Do not change Ludus's linker or debug-info format just to use RAD.
Validate existing Debug and Development ELF/DWARF builds first. LLDB/GDB remain
available fallbacks; browser builds use browser-specific tools.

Sources:

- [RAD release and Linux limitations](https://github.com/EpicGames/raddebugger/releases/tag/v0.9.29-alpha)
- [Pinned Linux build instructions](https://github.com/EpicGames/raddebugger/blob/cd41ba199bbe091d348a9b2be5a3528cb8acbff0/README.md#linux-x64)
- [Pinned build script](https://github.com/EpicGames/raddebugger/blob/cd41ba199bbe091d348a9b2be5a3528cb8acbff0/build.sh)
- [VS Code tasks](https://code.visualstudio.com/docs/debugtest/tasks)
- [VS Code debugger architecture](https://code.visualstudio.com/api/extension-guides/debugger-extension)

## Developer workflows and process ownership

| Role | Today | With an editor |
| --- | --- | --- |
| Engine developer | Edit in VS Code; RAD launches a smoke app or test that exercises the engine. | RAD launches the editor for editor bugs, or the game for runtime bugs. |
| Game developer | Edit in VS Code; RAD launches a native game target. | Run the editor normally; Open Code opens the selected code app, Debug Game launches the game through RAD. |

RAD must own the process executing the code of interest. Attaching to a parent
does not establish control of every child. Prefer debugger-launched processes
for startup breakpoints; offer attach separately when live-process investigation
is required. Do not weaken Linux ptrace policy globally during onboarding.

The proposed default editor play mode uses a separate game process. Pausing or
crashing gameplay then leaves the editor responsive. This requires explicit
editor/game IPC and project synchronization, which are later design work. An
optional in-process play mode would pause the editor on gameplay breakpoints.
Do not assume RAD's current Linux subprocess support can provide either mode.

## Tool discovery and installation

Resolution order for RAD is an explicit `--debugger` executable, then
`LUDUS_RAD_DEBUGGER`, then a valid managed installation, then `raddbg` on PATH.
An invalid explicit preference fails with a useful message instead of silently
selecting another debugger. Never invoke a graphical debugger to detect its
version. `scripts/doctor` reports availability as optional, not compatibility.

`init.sh --with-rad-debugger` opts into RAD installation. Also provide
`scripts/setup-rad-debugger` for existing checkouts. Only those commands may
download RAD or install its build prerequisites. `--no-system-install` applies
to RAD too: check existing packages and print the missing dependency remedy.
The optional build requires Git, Clang/LLVM 23, pkg-config, FreeType, X11/Xext/
Xfixes, GL, and EGL development libraries. On Wayland, the initial RAD UI needs
XWayland; Ludus's game window backend remains independent.

Fetch the exact commit into a dedicated checkout, verify it, and build release
RAD without applying Ludus's C++ warnings/exception flags to third-party C code.
Record the pin and compiler in a stamp only after a successful executable build.
Reuse completed installations; interrupted builds can be retried. Refuse to
overwrite unexpected source edits. A requested RAD setup failure is an explicit
command failure; omitting RAD never prevents ordinary onboarding.

## Updates, compatibility, and rollback

RAD follows a reviewed repository pin, not upstream's latest release or moving
branch. Neither launch, doctor, nor ordinary init checks for new releases,
downloads an upgrade, or replaces a running debugger. Rerunning explicit setup
installs the revision currently in `config/rad_debugger.json`. A contributor who
pulls a changed pin opts into that build by running setup again. External RAD
installations remain the contributor's responsibility and are unverified.

Maintain upgrades as focused changes to the version and exact commit together.
Read the upstream release notes and inspect Linux build dependencies, CLI/IPC
syntax, argument handling, configuration formats, and ELF/DWARF support. Update
the adapter and dependency list when necessary. Run the automated tooling tests,
build the new pin, and repeat D2 interactive acceptance for both supported
presets. Record the tested version, host, limitations, and configuration import
results in the change. Linux release binaries can replace source builds later,
after a separate packaging/checksum decision. Release monitoring may propose a
pin change; it must not install one automatically.

Installations are keyed by commit, so building a new pin leaves older revisions
available. A failed new build has no success stamp and is not selected; the
launcher does not silently choose an older managed revision. Existing explicit
preferences and PATH discovery still apply. Retry setup, explicitly select a
previous executable, or revert the pin. Setup/launch locking also applies to
older managed builds selected explicitly, so rollback cannot rebuild an active
debugger. The installer does not prune previous installations or sessions.

RAD autosaves its configuration. Store project/user files separately for each
managed revision; external binaries use a SHA-256 content identity so replacing
the executable at the same path gets a new session. New identities start with
fresh state. Never give a newer debugger the old version's original files or
automatically assume backward-compatible formats. A contributor can deliberately
copy settings into the new session after compatibility testing, keeping the old
files for rollback. Rolling back selects the old identity and restores its
existing state. Portable game launch settings will stay separate from these
RAD-owned files in D3.

## Launch description and session state

D1 provides `scripts/debug [options] <preset> <target> -- [game arguments]`.
It configures CMake, resolves an EXECUTABLE target through the CMake File API,
builds that target by default, and launches RAD. `--no-build` reuses an existing
configuration/artifact; `--dry-run` prepares and prints the launch without
opening a GUI. Debug and Development are the supported stepping presets.
Browser, Release, Profile, and sanitizer presets need separate workflows.

The launch description contains executable, argument vector, working directory,
and target/preset identity. Default working directory is the repository root;
`--cwd` selects the game's content root. Arguments are data, never shell code.
RAD-specific project syntax and command-line encoding stay in the Python adapter.
Unsupported argument encodings must fail rather than silently change argv.

Session directories live under
`out/debug/rad/<preset>/<target>/versions/<debugger-identity>/`. Launch
description is generated as `launch.json`, including the debugger identity and
managed pin when available; RAD's project and user files retain
breakpoints, watches, and UI. The target is supplied temporarily on RAD's command
line; do not use Save To Project for it (that would create an additional saved
target). Do not commit machine paths or overwrite project/user session state.
The launcher waits for its RAD process. A target-level lock prevents concurrent
launches across debugger versions, including conflicting rebuilds of that target.
Debugging a target directly uses Ludus's existing attached-debugger
assertion path; `scripts/run` remains the diagnostic-helper workflow. Passing
its diagnostic descriptors through RAD is separate work requiring lifecycle tests.

The future stable launch model should add project identity, explicit environment
overrides, source mappings, launch/attach mode, and debug-domain selection.
Local tool paths/preferences stay local; portable project launch settings can be
version controlled. Do not add a speculative configuration language in D1.

## VS Code integration

The proposed [engineering quality architecture](engineering-quality.md#9-editor-and-contributor-experience)
extends the shared-command approach to preset-aware clangd, quality tasks,
coverage and optional editor extensions. That proposal does not change the
implemented RAD setup/launch behavior below or overwrite user preferences.

Commit explicitly named process tasks for RAD setup and debugging. They call
the CLI so terminal and VS Code behavior stay aligned. Contributors select
Tasks: Run Task; optional user shortcuts are documented. No extension is needed
to launch RAD. Breakpoints and stepping live in RAD; VS Code breakpoint dots
are not automatically synchronized. A later small extension could add target
selection and source-line commands. Full VS Code debugger UI support requires
a Debug Adapter Protocol bridge and is a separate product decision.

## Future editor and scripting integration

The editor should expose Build, Run, Debug Game, Debug Editor, Attach, Open Code,
and Open File at Line. RAD is a recommended provider; tool choice is a developer
preference. External-tool launching belongs in editor/tooling services above
Platform, not FoundationBase or the game runtime. Initially a process-based CLI
bridge can reuse this implementation; a native provider can replace it when
distribution and process APIs justify that work.

Editor preferences should display the selected debugger path, managed version
or external/unverified status, and supported operations. Offer explicit setup,
upgrade, rollback, and copied-settings import actions using the same pin and
session rules. An available update must not replace an active session. Feature
availability (attach, IPC, native/script domains) follows tested provider
capabilities rather than assuming every newer RAD release supports every action.

Choose the scripting language/runtime in a dedicated ADR. Native C++ game code
uses RAD and ELF/DWARF. Embedded VM scripts require that runtime's debug hooks
or adapter; RAD cannot be assumed to understand script frames, variables, or
source breakpoints. Define native, script, and mixed debug domains, document
pause semantics, and validate whether both debuggers can coexist. Distinguish
native crash/assertion stops from VM exceptions and script failures.

Start with external code editing and explicit rebuild/restart. Later scripting
features may include project generation, language-server support, source maps,
script diagnostics, and controlled reload. Reload needs its own state migration,
thread safety, breakpoint remapping, and lifetime design; neither native hot
reload nor a built-in coding environment is implied by RAD integration.

## Milestones and acceptance

| Milestone | Deliverable | Acceptance gate |
| --- | --- | --- |
| D0: Design | This document and onboarding contract. | Process ownership, optional tooling, preferences, and scripting boundaries are explicit. |
| D1: CLI and VS Code | Pinned optional setup, doctor reporting, target launcher, tasks, version-isolated sessions, usage guide. | Automated tests verify discovery, arguments, target resolution, retries, failed upgrades, rollback, and state preservation; source build succeeds; launch is exercised. |
| D2: Linux compatibility | Human acceptance on Debug/Development apps and tests, repeated for each pin upgrade. | Source breakpoints, locals, watches, stacks, threads, assertion break/continue and terminate, argv/cwd, restart, XWayland, and copied-settings compatibility work; limitations recorded. Only then call RAD validated for Ludus. |
| D3: Project tooling | Portable project launch settings, local tool preferences, capability metadata, settings import, external-code navigation, game-project/SDK workflow. | Engine checkout and independent game project use the same launch semantics; preferences survive upgrades independently of RAD configuration formats. |
| D4: Editor | Tool service, Build/Run/Debug/Open Code actions, explicit tool setup/update/rollback, separate-process play and attach. | Startup and live attachment, responsive paused editor, process cleanup, upgrade failure/rollback, unavailable-tool errors, and editor/game IPC are tested. |
| D5: Scripting | Runtime ADR, editing/language services, script debugger provider. | Script breakpoints and errors, native/script process ownership, mixed debugging constraints, and restart behavior are demonstrated. |
| D6: Optional polish | RAD IPC, instance reuse, VS Code commands, source navigation, reload. | Per-instance IPC isolation, session preservation, and lifetime tests; DAP/reload only after explicit design approval. |

D2 needs an interactive desktop and a human debugger session. A successful
source build or mocked launcher test alone does not verify stepping or DWARF
evaluation. Track that evidence separately from implementation completion.

## Implementation status

D0 is documented and D1 is implemented. Optional setup has been built and reused
on the development host; CLI target resolution and launch preparation have been
exercised against `ludus_smoke`. Automated tooling tests run in CI without RAD,
downloads, or a desktop. D2 interactive acceptance is pending. The Editor now
implements an external debugging subset of D3/D4: explicit optional setup, local
preferences, SDK game launch, preserved sessions and owned cancellation.
See [Editor debugging](../development/debugging.md#from-the-ludus-editor). Embedded
debugger controls, attach, scripting and D6 integrations remain future work.

Verification on 2026-10-01:

- Pinned RAD release source build and repeated setup succeeded; optional init
  integration also succeeded with already-installed system prerequisites.
- All 22 RAD tooling contract tests passed, including upgrade failure,
  version-isolated state, external replacement, and rollback; existing formatter
  tests passed.
- `scripts/debug --dry-run linux-clang-debug ludus_smoke` configured, resolved,
  and built the target; `--no-build --dry-run` reused it successfully.
- RAD's binary utility converted smoke DWARF to RDI. The ELF contains both
  `.debug_info` and `.eh_frame_hdr`; this does not establish stepping correctness.
- Debug, Development, and ASan/UBSan builds passed with warnings-as-errors.
  Native and sanitizer CTest failures caused by sandbox socket/ptrace
  restrictions passed on rerun outside the sandbox. Live Wayland tests skipped.
- `scripts/check linux-clang-development --all` passed after building the
  Development tree to populate CMake's compiler module-map artifacts.
