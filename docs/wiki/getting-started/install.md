# Install and launch

Prepare the desktop editor once, then use it to open, build and run projects.

- **Linux x64:** use the steps below with `linux-clang-development` on a
  Wayland desktop. The rendered editor uses Vulkan. Ubuntu 24.04 is the Linux
  CI baseline and has automatic package setup.
- **macOS:** use the same steps with `macos-clang-development`. Apple silicon
  has native validation. The rendered editor uses Metal and requires macOS 15
  or later. Prepare Xcode Command Line Tools and the
  [macOS build prerequisites](../../development/building.md#macos-support).
- **Windows x64:** use the [Windows editor build and launch instructions](../../development/editor-workspace.md#windows-authoring-viewport)
  from an MSVC developer shell. That route uses the `windows-msvc-runtime`
  preset, Qt Quick and Direct3D 12. See the
  [Windows qualification record](../../architecture/windows-direct3d12.md#d7-qualify-the-windows-profiles)
  for the hosted WARP results and outstanding physical desktop checks.

Qt is a desktop editor dependency. Game projects use the engine SDK.

## 1. Get the source

```bash
git clone https://github.com/The-Ludus-Project/Ludus.git
cd Ludus
```

Keep this engine checkout separate from game projects you create.
On Windows, continue with the linked Windows build and launch instructions
after cloning. The initialization and launcher steps below are for Linux and macOS.

## 2. Use graphical initialization

From the checkout root, launch the setup window:

```bash
./init.sh
```

1. Under **Workflow**, select **Application developer**.
2. Select the Development preset for your host and keep
   **Prepare only the selected preset** checked.
3. Under **Targets**, check **Build Ludus editor** (the 3D preview is included by default).
4. Choose **Initialize**. A progress window lists each setup step, shows
   *Step N of M*, a progress bar and the elapsed time, and can show the live
   output. Leave the launching terminal open for any system package prompts.

Initialization prepares pinned tools and dependencies, installs missing supported
system prerequisites when needed, and builds the editor. First-time setup can
download dependencies and request a sudo password for system packages. Cancelling
the selection window starts no setup actions. Tests are optional here; engine
contributors have additional validation steps in the building guide.

**Checkpoint:** the progress window shows **Ludus is ready** and reports the
prepared editor. If it shows **Setup failed**, fix the reported error, then
choose **Retry**. Merely closing the selection window does not mean setup
succeeded. See [Building Ludus](../../development/building.md) for the progress
window's controls.

For terminal-only initialization, the equivalent choice is:

```bash
# Linux
./init.sh --cli linux-clang-development --persona application --with-editor --preset-only
# macOS
./init.sh --cli macos-clang-development --persona application --with-editor --preset-only
```

If you manage system packages yourself, install the required toolchain and Qt,
then add `--no-system-install` to that command.

## 3. Launch the editor

```bash
# Linux
./scripts/editor --preset linux-clang-development
# macOS
./scripts/editor --preset macos-clang-development
```

The launcher opens the already-built editor; it does not build or install it.
**Checkpoint:** Welcome offers **New Project**, **Open Project** and recent
projects. After this initial launch, project setup and normal builds happen in
the editor rather than through a series of setup commands.

## 4. Choose your first walkthrough

- [Create your first project](first-project.md) walks through the minimal native
  template and its build/run/reopen workflow.
- [Your first rendered scene: Cornell box](../../examples/cornell-box.md) covers
  the Linux editor and Linux/macOS command-line routes for editing materials,
  geometry and lights in a working sample.

## Engine contributors and other hosts

Use [Building Ludus](../../development/building.md) for engine validation,
headless and sandbox requirements, and profile selection. See
[contributing](../contribute/index.md) for review gates or try the
[browser editor](../guides/browser-editor.md) for an in-browser workspace.

## Detailed references

- [Onboarding flags and supported tools](../../development/building.md#one-command-onboarding)
- [Build trees, presets and IDE setup](../../development/building.md)
- [Editor prerequisites and manual build](../../development/editor-workspace.md)
