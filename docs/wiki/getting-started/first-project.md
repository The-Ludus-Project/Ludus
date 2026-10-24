# Create your first project

This walkthrough uses the desktop editor and the bundled minimal native template.
Prepare and launch Ludus using [Install and launch](install.md).

## 1. Choose a name and location

Select **New Project** on Welcome or press **Ctrl+N**.

- **Name** becomes the new project folder's name.
- **Location** is its parent folder. Use **Browse** to select another location.
- **New folder** previews the full destination before creation.

Choose a destination that does not already exist. A new project never overwrites
an existing folder. Normally, leave **Advanced engine selection** collapsed.

Tick **Add a GitHub Actions build and test workflow** if the project will live in
its own GitHub repository. See
[continuous integration](../../development/project-sdk-workflow.md#continuous-integration-github-actions)
for the repository variables the workflow needs.

## 2. Create and verify

Choose **Create Project**. Ludus selects a native Development engine, prepares
missing editor-associated tools/SDK inputs when necessary, and verifies the
generated project's configure, build and test setup. First-time preparation can
download dependencies. Follow **Output** for progress; **Stop** cancels owned work.

Creation uses a staging folder. Before publication, failure or cancellation keeps
the current project and discards the stage. A successful project opens in the
workspace. Generated build artifacts are discarded when the stage moves, so the
next Configure establishes a build tree at the final location.

## 3. Build and run

Review **Project settings**, then choose **Build and Run**. The selected
executable runs in its own window/process. The editor is currently a project and
tools workspace; an embedded scene viewport is a later stage.

## 4. Close and reopen

Use **File → Close Project** or **Ctrl+Shift+W**. Save or discard pending project
settings/audio changes, or cancel to continue editing. Stop active owned work
before closing. Welcome shows the project in recent history; the File menu also
keeps **Recent Projects** available while working.

## 5. Learn to change a rendered scene

Creating a minimal project teaches setup and the build/run workflow. Continue
with [Your first rendered scene: Cornell box](../../examples/cornell-box.md) for
hands-on engine use: run an existing sample, separate materials from lighting,
change a wall color, move a block, dim the light and inspect the frame lifecycle.
Each edit has an expected result. Open that sample as a separate project rather
than replacing the minimal project's files.

## Equivalent CLI creation

Install the CLI in a dedicated virtual environment from your engine checkout:

```bash
python3 -m venv out/ludus-cli-venv
out/ludus-cli-venv/bin/python -m pip install ./scripts/python
out/ludus-cli-venv/bin/ludus project create "$HOME/Ludus Projects/My Game" \
    --name "My Game" --tools "$PWD"
```

`--tools` selects the trusted, prepared Ludus checkout and enables verified
creation. This flow shares the editor's engine discovery. A metadata-only create
without `--tools` is a different operation and does not prove build readiness.

## Where the engine comes from

Creation checks, in order: an explicit SDK override, absolute `LUDUS_SDK_PREFIX`,
the tooling checkout's current Development install, then automatic preparation
of that checkout. An invalid explicit/environment override fails clearly.
Automatic preparation installs different SDK variants in separate folders and
preserves older installations used by existing projects.
Game projects keep machine-specific paths in ignored local settings.

Read [project setup](../../development/project-sdk-workflow.md) for release locks, custom SDKs,
repair and compatibility checks. See the
[shared creation backend](https://github.com/The-Ludus-Project/Ludus/blob/main/scripts/python/ludus_tools/creation_engine.py)
and [editor guide](../../development/editor-workspace.md).
