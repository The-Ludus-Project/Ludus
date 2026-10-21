# Troubleshooting

Read the operation's first useful diagnostic in **Output**, then check the
selected project, profile and engine identity. Retry after correcting the input;
avoid repeatedly rebuilding an incompatible or stale setup.

| Symptom | Check and next step |
| --- | --- |
| Editor executable missing | Prepare it with `./init.sh --cli --with-editor`; the editor launcher only launches existing builds |
| Qt/platform plugin error | Verify Qt 6.4+ Core/Gui/Widgets and the appropriate native display plugin; use the supported Linux x64 host |
| New Project cannot create a folder | Use an absolute Location, a valid name and an absent destination; check parent-folder permissions |
| Automatic engine selection fails | Check an explicit Advanced override or `LUDUS_SDK_PREFIX`; invalid overrides do not silently fall back |
| Missing or stale local CMake presets | Run Check Setup, then explicit Repair Project Setup with the intended compatible engine |
| SDK incompatible or lock unresolved | Inspect flavor, compiler/ABI/components and locked release; install the required release or choose a compatible local override |
| Compiler target/SDK disagreement or detection override error | Follow [target and SDK recovery](platform-targets.md#recover-from-target-and-sdk-errors); correct the toolchain/SDK rather than redefining detected facts |
| Operation reports Busy | Another managed operation owns that build tree; let it complete or stop it before retrying |
| Close Project disabled | Stop owned build/run/Play work and wait for confirmed cleanup |
| Save failed or external file conflict | Keep the draft; inspect permissions and reconcile the external file before saving again |
| Panel missing | Use View or the Window menu to show it, or Window → Reset Window Layout to restore placement |
| `./init.sh` fails with `No module named 'encodings'` after a Python upgrade | Rerun `./init.sh`: it now detects a `out/host-tools/venv` built by a removed or different Python, prints `Recreating stale virtual environment (built with Python 3.12, now 3.14)` and rebuilds it. If it refuses because the path is a link, move that link aside and rerun |
| Browser runtime cannot start | Follow the browser/device acceptance guide; documentation hosting does not establish WebGPU support |

## Gather a useful report

Record the Ludus commit/SDK identity, host OS, tool versions, build profile,
intended game OS/architecture, project action, relevant Output and the smallest
reproducible steps. Keep the build host and game target distinct. State the
expected result and what actually happened. Remove credentials and unrelated
personal information from attached logs.

Use [GitHub issues](https://github.com/The-Ludus-Project/Ludus/issues) for actionable reports.
For build-tool checks in an engine checkout, `./scripts/doctor` reports the tool
environment. See [project setup](../../development/project-sdk-workflow.md) and
[build/debug guidance](build-and-debug.md) for targeted recovery.
