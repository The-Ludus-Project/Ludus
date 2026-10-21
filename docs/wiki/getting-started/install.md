# Install and launch

Prepare the desktop editor once, then use it to open, build and run projects.
The supported native editor host is **Ubuntu 24.04, Linux x64**. The engine uses
C++23 with Clang/LLVM 23 and LLD; Qt stays outside the engine SDK.

## 1. Get the source

```bash
git clone https://github.com/The-Ludus-Project/Ludus.git
cd Ludus
```

Keep this engine checkout separate from game projects you create.
For the rendering tutorial, use a Wayland desktop with a Vulkan-capable driver;
see [Building Ludus](../../development/building.md) for system prerequisites.

## 2. Use graphical initialization

From the checkout root, launch the setup window:

```bash
./init.sh
```

1. Under **Workflow**, select **Application developer**.
2. Keep **Build preset** set to `linux-clang-development` and
   **Prepare only the selected preset** checked.
3. Check **Build Ludus editor (3D preview included by default)**.
4. Choose **Initialize**. A progress window lists each setup step, shows
   *Step N of M*, a progress bar and the elapsed time, and can show the live
   output. Leave the launching terminal open: a sudo password prompt appears
   there, and the window tells you when it is waiting for one.

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
./init.sh --cli --persona application --with-editor --preset-only
```

If you manage system packages yourself, install the required toolchain and Qt,
then add `--no-system-install` to that command.

## 3. Launch the editor

```bash
./scripts/editor --preset linux-clang-development
```

The launcher opens the already-built editor; it does not build or install it.
**Checkpoint:** Welcome offers **New Project**, **Open Project** and recent
projects. After this initial launch, project setup and normal builds happen in
the editor rather than through a series of setup commands.

## 4. Choose your first walkthrough

- [Your first rendered scene: Cornell box](../../examples/cornell-box.md): open a
  working sample, run it, change materials and geometry, and experiment with light
  and shadows. Includes runtime screenshots and expected results for each edit.
- [Create your first project](first-project.md): generate a minimal native project
  in a new folder and learn the project's build/run/reopen workflow.

## Engine contributors and other hosts

Use [Building Ludus](../../development/building.md) for engine validation,
headless and sandbox requirements, the development container, macOS and profile
selection. The native editor is currently Linux-only; the Cornell box guide has
an independent command-line route for macOS. See
[contributing](../contribute/index.md) for review gates.

## Detailed references

- [Onboarding flags and supported tools](../../development/building.md#one-command-onboarding)
- [Build trees, presets and IDE setup](../../development/building.md)
- [Editor prerequisites and manual build](../../development/editor-workspace.md)
