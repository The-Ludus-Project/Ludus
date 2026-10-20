---
description: Open the existing Ludus editor workspace in your browser.
---

# Browser editor

[Launch Ludus Editor in your browser →](../editor/index.html)

The browser preview runs the existing Qt Widgets application as WebAssembly.
It opens a writable Cornell Box sample with its sources and shaders so you can
explore a complete project immediately. A desktop browser with WebAssembly and WebGL
is required. The first download includes Qt and the editor; it may take longer
than opening a guide.

## Supported workflows

- Edit the example project's name, provider, source directory, preset, target,
  working directory, and arguments.
- Use **File → Open Project** and select the folder containing
  `ludus.project.json`. The browser copies the folder's files, including source,
  shaders and assets, into its session filesystem, preserving relative paths.
- Use **File → Open Sample** for writable copies of Cornell Box, Live Edit Game,
  Scripted Game or the installed SDK example. Each copy has its own folder;
  sample originals remain available for starting again.
- Select **Play → Play Web Sample** for Cornell Box, Live Edit Game or Scripted
  Game. Each has a shipped WebAssembly player inside the page: no SDK selection,
  setup or local compilation is required. Close it with **Stop and close** or
  Escape. Live Edit Game and Scripted Game support Pause/Resume and Restart;
  Scripted Game opens its encounter after two Space presses on the focused canvas.
- Use **Save and Download Project Folder** or **Download Project Folder** to
  export a `.tar` archive. Both save open project-document drafts before
  exporting. Extract the archive into a folder on your computer, then reopen
  that folder to continue. Save configuration preferences separately using their
  download action. Build outputs and `.ludus` machine settings are excluded.
- Open the **Configuration** tab, load a cooked project bundle or preference
  document, inspect inherited values, edit/reset preferences, and download sparse
  preference overrides.
- Import a scene into a World and navigate its GPU viewport with orbit, pan, zoom
  and focus. The viewport requires WebGPU over HTTPS or localhost; the rest of the
  workspace requires WebGL. See the [viewport profile and limits](../../development/browser-editor.md#scene-import-and-viewport).
- Arrange the existing panels and inspect output and workflow status.
- Browse and filter `content/catalog.json` in **Content**, import or reimport
  WAV/FLAC sources with stable IDs, then create and edit sound/music definitions
  in **Audio**. Saves use the desktop format and expected-digest conflict checks.
  Browser audio playback remains unavailable; **Play draft** is disabled with an
  explanation. Download and open the project on desktop to audition it.

Project downloads save open Audio, World, Scripts and Renderer drafts first.
An invalid draft, pending import or failed save stops the download; fix that
document and retry. Content imports yield before beginning bounded validation,
then run on the browser's single filesystem owner. Input pauses during decoding;
cancellation can revoke a request before this work begins.

Project imports admit at most 4096 files and 32 MiB per session; individual
configuration documents remain limited to 1 MiB. Generated `out`, `build`,
`.git`, `.ludus` and `node_modules` trees are omitted during project import and
export. Project exports are limited to
4096 regular files and 32 MiB. Symbolic links and paths exceeding ustar limits
cannot be exported. Select a source folder without generated build outputs. Invalid
imports preserve the current document. Document formats and validation are the
same as in the desktop application.

## Session storage

Files and edits live in the current browser session. Reloading or closing the
page discards them; browser history and the desktop filesystem do not recover
those edits. Download your changes before leaving. After an edit, the page asks
for confirmation before reload/close. The browser manages download destinations,
so the application cannot confirm whether you accepted or cancelled a download.

## Desktop actions

Web Play runs the shipped sample build, including its compiled scripts and
assets. Workspace source/data edits do not modify that prebuilt player. Download
the project and rebuild on desktop to run those changes. Imported copies retain
Web Play when their `ludus.web.json` identifies a shipped sample. Other imported
projects need their own web build; the console-only SDK example has no game
player. The browser does not compile C++ or cook edited Luau scripts.

Local project creation/setup, CMake builds, native Run/Play, code reload, RAD
debugging, release packaging, and audio playback require the desktop
editor in this increment. Their tool adapters cannot start native processes in
a browser. The browser preview does not include a remote build service.

See the [desktop editor guide](../../development/editor-workspace.md) for native
project setup and rebuilding downloaded changes.
