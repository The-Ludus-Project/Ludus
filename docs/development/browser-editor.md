# Building the browser editor

The Pages application compiles `apps/editor` with Qt Widgets for WebAssembly.
The UI, project/configuration models and serializers are shared with the desktop
editor. Browser file import/download and explicitly unavailable native-process/audio-playback
adapters are private implementations selected by the editor target. The game
viewport remains a separate graphics concern; this port does not replace Qt
with the engine's unfinished UI toolkit.

Qt can remain the common editor shell for Linux, macOS and the browser. macOS
editor setup, launching and native validation remain separate work, as does
its Metal game viewport. Platform-specific shells would duplicate commands and
workspace behavior without solving the renderer/backend gap.

## Reproduce the Pages payload

Use Python 3.12 or newer on Linux x64, with Clang/LLD 18 installed:

```sh
python scripts/build-web-editor --bootstrap
python scripts/build-web-editor --check-package
python -m unittest discover -s scripts/python -p test_editor_web.py -v
```

The bootstrap installs the pinned CMake/Ninja, Emscripten SDK, official Qt host
tools and verified Qt source archive into ignored `out/` directories. Qt's pin
is in `config/editor_web_toolchain.json`; the compiler/WebGPU pins remain in
`config/web_toolchain.json`. Qt is built from source with the engine SDK because
prebuilt WebAssembly Qt requires its matching Emscripten version. A source build
with Ludus's newer pinned SDK needs validation whenever either pin changes.

The same build also prepares the pinned Luau compiler/analyzer and shader tools,
then builds Cornell Box, Live Edit Game and Scripted Game as standalone static
web players. These compile the existing sample C++ implementations; Scripted
Game cooks its actual Luau package on the build host and links the Wasm provider.
Cornell Box yields asynchronous device/pipeline creation through Asyncify. The
players use the public browser RHI with WebGPU/WebGL 2 fallback and independent
canvases; closing their iframe releases the entire player runtime. Native
GameHost processes and dynamic module loading are not needed for web playback.

Player assets under `players/<sample>/` are required and hash-checked alongside
the editor payload. Every Wasm module uses the bounded single-threaded memory
policy. The package includes Luau and WebGPU notices. A project opts into a
shipped player with `ludus.web.json` (`version: 1`, `player` equal to a shipped
sample identifier). Arbitrary URLs and unknown identifiers are rejected.

This static Qt build uses no C++ exceptions, one thread and Asyncify for the
existing modal dialogs. It needs no cross-origin isolation headers. The editor
starts with 32 MiB of WebAssembly memory and may grow to 2 GiB (`QT_WASM_MAXIMUM_MEMORY`), enough to cook and
preview photographic scenes in process. Do not silently
switch it to shared memory: Pages cannot supply arbitrary response headers.

An existing host Qt installation of the pinned version can be supplied with
`--qt-host /absolute/path/to/Qt/6.10.3/gcc_64` (or `macos`). To rebuild modified
Qt sources in `out/editor-tools/src/qtbase`, use `--rebuild-qt`. The source and
rebuild scripts are available to users of the statically linked LGPL Qt build;
license texts and bundled dependency notices accompany the payload.

The bounded import wrapper uses Qt 6.10.3's private WebAssembly event callbacks
and file reader behind one private `.cpp` boundary. Its admission callback checks size
before reading, and Qt owns the suspend/resume dispatch. This is an intentional
version coupling: editor upgrades must verify this adapter in a browser. Browser
downloads use a separate JavaScript bridge with copied bytes and explicit names.
Project and scene folders do not use the import wrapper; they open in place
through `browser/host_folder.js` (see [Project folders on disk](#project-folders-on-disk)),
which couples to the pinned Emscripten's file system as recorded in the
[design](../architecture/editor-browser-strategy.md#host-project-folders).

Static analysis still uses Clang 23. `web_build.analysis_commands` enables
`-frelaxed-template-template-args` for the standard template-matching rule that
is already the default in the newer SDK compiler; this is required to parse
Emscripten's `val.h`. The language mode remains C++23 and diagnostics remain errors.

### Ludus changes to Qt

The build applies `QT_SOURCE_CHANGES` from `scripts/python/editor_web.py` to the
extracted Qt source before building it, and ships the resulting diff as
`licenses/Qt-source-changes.diff` beside `NOTICE.txt`. Each change replaces one
exact excerpt. A source that matches neither the pinned excerpt nor the change
stops the build; delete `out/editor-tools/src/qtbase` and rerun `--bootstrap` to
restore the pinned source. Changing the list rebuilds Qt.

The first change backports qtbase commit
[3a217d2592](https://code.qt.io/cgit/qt/qtbase.git/commit/?id=3a217d2592)
from Qt 6.11.0: the suspend/resume control reads the queue of browser events
before sending each one. In Qt 6.10.3 a nested `processEvents()` (Qt's posted-event
wakeup runs one) could empty the batch that its caller was sending. The caller
then read past the end of the queue, and the JavaScript `TypeError` ("reading
'index'") ended Qt's event loop. The editor stopped drawing and responding to
input. Browser events form batches while host-folder file calls
suspend the editor, so projects opened from disk hit it often. Remove the
change when the Qt pin includes it.

The second change is Ludus's own. A pointer event sent from that queue after
the browser finished dispatching it has an empty `composedPath()`, and Qt 6.10.3
read `className` from its missing first element; that `TypeError` also ended
the event loop. Such an event now counts as not targeted at a Qt element, so Qt
skips pointer capture for it. Qt 6.11 already returns false there (qtbase
[01d48cd89c](https://code.qt.io/cgit/qt/qtbase.git/commit/?id=01d48cd89c)).
Any JavaScript exception thrown while Qt sends a browser event stops the editor
the same way, so verify event handling in a browser when changing the Qt pin.

## Project folders on disk

**File → Open Project...** opens a project folder from your computer in place.
Nothing is uploaded and the folder is not copied: the editor lists its files,
then reads each file from disk the first time it is opened. A 37 MB project
such as Dolomites Survival opens without reading its assets until a World or
scene needs them. The folder appears in the editor as
`/browser-folders/<n>/<folder name>`.

| Browser | Picker | Saves |
| --- | --- | --- |
| Chrome, Edge (`showDirectoryPicker`) | The browser asks to let the site view and edit the folder | Written to the files in the folder. **File → Reopen Last Project Folder** opens the same folder in a later session after the browser asks again |
| Firefox, Safari | The browser asks to upload the folder's files to the page; nothing leaves the computer | Kept in the tab only. The editor says the folder is read-only; use **Download Project** to keep edits |

A save is on disk when the editor reports it saved: QSaveFile and Ludus
persistence publish with `rename()`, which waits for the folder. A failed disk
write fails the save, keeps the edit in the tab and names the file, the cause
and the next step. Plain file writes and deletions reach the folder a moment
after they finish; the page asks before leaving while any are pending. Files
under `out`, `build`, `.git`, `.ludus` and `node_modules` are neither listed nor
written back: build outputs and caches stay on the desktop, and the browser's
own caches stay in the tab.

Limits come from one constant, `BROWSER_FILE_BUDGET_BYTES`: a quarter of the
editor's 2 GiB maximum WebAssembly memory, 512 MiB. No single file may exceed
it, and the editor keeps at most that much of the folder's files in memory,
reading older ones again when they are next opened. The folder's total size is
not limited; at most 4096 files outside the generated folders are opened. The
same budget caps files copied by single-document, audio and `.zip` imports and
a **Download Project** archive. Every refusal names the file or the count, the
limit and what to do next (move or split the file, remove files the project does
not need, or use the desktop editor).

**Import scene… → Scene folder…** opens a scene folder the same way, read-only.
One scene file with its companions, or a `.zip`, is still copied into the tab.

The browser cannot run CMake, compile the project's C++ or start its game, so
a folder opened here is for authoring and preview; build and run it with the
desktop editor. Check the file-system layer without Qt, through a small Wasm
harness and mock folder handles:

```sh
python scripts/build-web-editor --test-host-folder
```

## New projects

**File → New Project** creates the same files as the desktop editor's New
Project: the CLI's `minimal` template, its version-2 descriptor and an
unresolved lock. The browser cannot run that Python tooling, so the editor build
renders the template once at configure time
(`ludus_tools.create.write_session_template`) with every value except the
project's identity, and embeds it as a Qt resource. Creating a project then
substitutes only two placeholders: `{{ TARGET }}` with the CMake target derived
from the name exactly as `ludus create` derives it, and `{{ NAME }}` (only inside
a JSON string) with the JSON-escaped name. `test_ludus_tools.SessionTemplate`
checks that the substituted template equals `create_project` output byte for
byte; the editor's tests check the same target derivation cases.

The dialog asks only for a name and creates the folder below the session's
`projects` directory; an existing folder of that name is refused before anything
is written. The engine requirement is the editor's own engine version and the
preset is `linux-clang-development`. No SDK is selected, nothing is configured
or built, and no GitHub workflow is offered. Download the project folder, open
it in the desktop editor and use **Project → Repair Project Setup…** to select an engine and
build it.

## Scene import and viewport

World authoring works on the session's in-memory (MEMFS) file system, where a
project folder opened from disk is mounted in place. **Import scene…** offers a
scene folder, read in place like a [project folder](#project-folders-on-disk),
or one `.zip` (stored or deflated entries, no ZIP64) or a `.pbrt` with its
companion files, which are copied into MEMFS. The linked SceneCook importer
then cooks the scene to LUSCN in process, exactly as on the desktop; MEMFS has no `flock`, so the cook lock is skipped there
(one thread owns the file system). MEMFS also has no hard links: immutable
artifacts use an existence check followed by rename without yielding. Existing
identical artifacts are reused, while conflicting contents reject publication.
These writes publish session memory and promise no disk durability. Native cooks
retain their atomic hard-link publication. The build is single-threaded, so the cook
runs on the UI thread after the status text has painted; large scenes block
input until the cook finishes.

The interactive viewport renders imported scenes through the engine's GPU raster
renderer on a separate **WebGPU** or **WebGL 2** canvas (see
[Viewport graphics API](#viewport-graphics-api)). Qt retains its own canvas for widgets. Scene meshes, textures and materials use the same GPU generation and
camera path as the native editor; the renderer tone-maps its display image and
composites it to the browser surface in one submission, without CPU pixel
readback or image upload. Orbit, pan, zoom, focus, shading and resize update the
current scene. Output is bounded to 1024 physical pixels on its longest side.

**Solid** is the neutral navigation visualization; **Albedo** shows unlit surface
colors; **Lit** uses admitted authored direct lights, emission and environment.
Authored raster shadows use the shared shadow atlas path. Shadow preparation
records offscreen submissions without acquiring or clearing the visible
canvas (WebGL 2 leaves the canvas drawing buffer untouched), so pending candidates retain the last presented view. The browser
profile does not run the native worker-based static lighting bake. Emissive geometry does not imply real-time indirect lighting.
Pending resource preparation retains the last presented view; rejected candidates
report a failure without replacing it. A completed image is required before an
import is published.

### Viewport graphics API

The viewport starts **WebGPU** first. WebGPU needs a supported browser and device
and a secure context (HTTPS or localhost). When WebGPU cannot start (no `navigator.gpu`,
no compatible adapter or device, or a canvas or limit failure), the viewport creates a
fresh canvas and starts **WebGL 2** instead, with the same shaders compiled to GLSL ES
3.00 by the browser build. The statistics overlay's first line names the API in use,
for example `API: WebGL 2 (WebGPU unavailable: no compatible GPU adapter)`.
Only startup failures fall back: a device that started and later fails or is lost
reports that failure instead of switching APIs. A scene import keeps waiting for its
first image while the viewport retries on WebGL 2, including when WebGPU fails only
after its device request (for example when no adapter is found).

To pick the API yourself, add `viewport-backend` to the page address:
`?viewport-backend=webgl2` forces WebGL 2, `?viewport-backend=webgpu` forces WebGPU
(with no fallback), and `auto` or no value lets the editor choose. A forced API that
cannot start is reported with the step to remove the override.

If neither API can start, the viewport names each one with its reason and the next
step (turn on hardware acceleration, use a browser with WebGPU or WebGL 2, or use the
desktop editor), and document editing remains usable. It never rasterizes on the CPU.
An import that was waiting for the viewport fails with that same cause and next step,
for example `Scene import failed: The 3D viewport is unavailable: neither WebGPU (…)
nor WebGL 2 (…) could start in this browser. … Previous scene and level retained.`
The page mirrors the failure in `#screen[data-viewport-error]` and the text drawn over
the empty viewport in `#screen[data-viewport-prompt]`, which the browser acceptance
test reads.
Device loss stops the GPU session and preserves documents. Reopen the
scene or restart the preview to request a fresh device. The separate canvas
follows the Qt viewport rectangle and hides with its tab, popups and modal dialogs;
mouse and keyboard navigation continue through Qt.

Clicking an object selects it, and the selection outline, transform handles and drag
readout appear over the frame as in the desktop editor, with WebGPU and WebGL 2 alike.
A second transparent canvas above the viewport canvas
(`#ludus-gpu-viewport-N-overlay`) shows them; it follows the same rectangle and lets
pointer input through. The outline is found on the CPU, as in the desktop editor's
readback view; see [Select and transform objects in the World viewport](editor-workspace.md#select-and-transform-objects-in-the-world-viewport).

Render graphs can be edited and saved here but do not run: the viewport always
uses its built-in renderer. The graph canvas says so in a notice, and greys out
ray tracing, compute and Denoise nodes as **Desktop editor only**. See
[which nodes cannot run here](render-graph-authoring.md#see-which-nodes-cannot-run-here).

## Preview the complete website

Build MkDocs and the API reference first, then copy the editor payload; MkDocs
cleans its output directory on each build:

```sh
python -m mkdocs build --strict
python scripts/build-api --bootstrap
cp -R out/editor-pages/. out/wiki/editor/
python scripts/check-wiki
```

Serve the complete site beneath `/Ludus/` to match the production project path.
Test loading, editing, importing and downloaded JSON in an actual browser after
changing Qt, the SDK or the editor. Artifact checks cannot prove the application
runs. The [browser editor guide](../wiki/guides/browser-editor.md) describes the
supported workflows and session storage contract.

## Pages and caching

The wiki workflow validates the editor independently and downloads its artifact
only after MkDocs/API generation. The final editor cache key includes editor,
engine/dependency, CMake/toolchain, loader and HTML sources. Ordinary guide and
API prose updates reuse that payload. A second cache retains the source-built
Qt and SDK for editor source changes. Each cache hit still runs package
validation; only the main Pages deployment publishes the combined artifact.
