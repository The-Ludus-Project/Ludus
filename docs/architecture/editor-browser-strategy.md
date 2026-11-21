# Browser access to Ludus

The current increment ports the existing Qt Widgets editor to WebAssembly and
includes it in GitHub Pages. This is a bounded workspace preview: project and
configuration documents can be edited, imported and downloaded. Content catalog
browsing, WAV/FLAC import and sound/music definition authoring share the desktop
document paths. Native tools, game execution/debugging and audio playback remain
desktop workflows. See the
[browser editor guide](../wiki/guides/browser-editor.md) and
[reproducible build instructions](../development/browser-editor.md).

The [GUI systems target](editor-gui-systems.md) selects Qt Widgets for native
Linux/macOS/Windows over a Qt-free authoring core. Keep Qt/Wasm for the current
preview; qualify it for full browser authoring rather than promising universal
frontend parity. If required browser tasks fail through maintainable public
interfaces, use a DOM browser frontend over the same commands/documents and
retire the preview frontend when replaced. The engine renderer owns scene
viewports. [ADR 0023](../decisions/0023-editor-presentation-and-document-core.md)
records this conditional choice; native macOS editor support remains future work.

## Existing footing and missing pieces

Browser parity is evaluated per operation, not by enabling every desktop button:

| Operation | Current browser state | Remaining boundary |
| --- | --- | --- |
| Project/configuration, World, script and render-graph documents | Shared authoring models and formats; explicit import/download | Frontend accessibility, large-model and narrow-window qualification remains separate |
| Content catalogs and audio definitions | Browse, import/reimport WAV/FLAC, create/edit/save sound and music | Playback device and waveform-preview service are still missing browser implementations; editing itself needs no native audio device |
| Ordinary raster viewport | Scene/camera/navigation and direct lighting | [Offscreen shadow preparation follow-up](https://github.com/The-Ludus-Project/Ludus/pull/462) preserves the last displayed image; raster shadows require no ray-tracing feature |
| GPU ray queries | Browser backend reports unavailable | Require the selected API/device capability; compute support alone does not admit rays. An API-level ray flag is not itself evidence of dedicated hardware acceleration |
| Builds, debugger, native Play, external editors | Unavailable in the static browser host; shipped web sample players remain usable | Local processes require a separate authorized host transport or build service |
| Project folders on disk | Opened in place: read on first use; saved back to the folder in Chrome/Edge, read-only with download elsewhere ([host project folders](#host-project-folders)) | Firefox and Safari cannot write to a folder on disk; recovery of unsaved session edits after a reload remains work |
| Durable project storage and recovery | Saves to a folder opened read-write; otherwise session memory plus explicit download | Quota-aware persistent storage for projects not on disk, recovery and tested import/export round trips remain work; a download request cannot certify that a file was saved |

The Content WebAssembly smoke target exercises real MEMFS publication and reads.
The editor's content suite also runs through the browser's cooperative scheduler
on native Qt to check cancellation, stale identities and teardown. Neither test
qualifies the whole Qt/Wasm frontend or browser audio playback.

Ludus already has a pinned Emscripten/WebGPU toolchain, browser Foundation,
canvas Platform and RHI lifecycle/frame-clear targets, an isolated triangle probe,
and the integrated W6 smoke app. These are runtime foundations, not a browser
scene editor. See [ADR 0009](../decisions/0009-browser-webgpu-toolchain.md),
`config/web_toolchain.json`, and `apps/smoke`.

The native editor currently targets Linux x64. Its controller owns
QProcess-based Python/build/debugger/Play adapters. A browser cannot simply start
the user's local CMake/compiler/RAD process. The WebAssembly target uses single-threaded static Qt, Asyncify modal dialogs,
project folders opened in place and explicit file import/download.
[Qt's WebAssembly documentation](https://doc.qt.io/qt-6/wasm.html) describes
asynchronous constraints and matching Qt/Emscripten builds. Do not infer that our
pinned engine WebGPU SDK is ABI-compatible with Qt's current Wasm requirements.

Browser storage also differs from desktop project folders. OPFS is origin-private,
quota-limited storage and is removed when site data is cleared; provide explicit
project export/import and recovery. [MDN OPFS documentation](https://developer.mozilla.org/en-US/docs/Web/API/File_System_API/Origin_private_file_system).
Emscripten's default MEMFS is volatile; IDBFS persistence requires explicit
synchronization. [Emscripten filesystem documentation](https://emscripten.org/docs/api_reference/Filesystem-API.html).

## Host project folders

**Decision (2026-11-17, [issue 1743](https://github.com/The-Ludus-Project/Ludus/issues/1743)).**
The browser editor works on a project folder the user picks on disk, in place,
instead of copying every file into the tab first. A web page can reach only a
folder the user picks; it can never open an arbitrary host path. Nothing is
uploaded: the files stay on the user's machine and are read into the tab's
memory. The browser editor still cannot run CMake or compile C++, so a folder
opened this way supports authoring and preview only.

Browser support, checked on 2026-11-17 against MDN and the web-features
explorer: `showDirectoryPicker()` and read-write `FileSystemDirectoryHandle`s
exist only in Chromium browsers (Chrome and Edge 86+, Chrome for Android 132+);
Mozilla's position is negative and WebKit's opposed. Firefox and Safari list a
folder only through `<input webkitdirectory>`, which gives read-only `File`
snapshots. Handles are structured-cloneable, so IndexedDB can keep one across
sessions; using it again needs `requestPermission()` after a click.

How it works (`apps/editor/browser/host_folder.js`, `apps/editor/src/browser_host_folder.cpp`):

- **Picking lists, it does not read.** Chromium asks for `readwrite` access and
  walks the handle tree; elsewhere the directory input lists its `File`s. The
  generated top-level folders (`out`, `build`, `.git`, `.ludus`,
  `node_modules`) are skipped without being walked. The editor admits the list
  (file count, per-file size, paths, `ludus.project.json` or a scene entry)
  before anything is mounted.
- **A mount type over MEMFS.** The folder is mounted at
  `/browser-folders/<n>/<folder name>`. Every file exists with its size, so
  `stat()` and directory listings need no bytes. A file's bytes are read on its
  first `open()`: the overridden `__syscall_openat` suspends the calling Wasm
  code with `Asyncify.handleSleep` while the file is read, exactly as
  Emscripten's own `fd_sync` suspends for a mount's `syncfs()`. Qt, the
  FoundationFilesystem layer and SceneCook therefore read folder files through
  their ordinary synchronous calls. An open that truncates reads nothing.
- **One memory budget.** `BROWSER_FILE_BUDGET_BYTES`
  (`apps/editor/src/internal/browser_folder.h`) is a quarter of the editor's
  maximum WebAssembly memory (`QT_WASM_MAXIMUM_MEMORY`, 2 GiB, so 512 MiB). It
  bounds the largest file of an opened folder, the resident copies of folder
  files (least recently opened clean, closed copies are dropped and read again
  when next opened), the files copied by single-document, audio and `.zip`
  imports, and a Download Project archive. A folder's total size is not limited;
  at most 4096 files are opened outside the generated folders.
- **Saves reach the folder.** Writes land in MEMFS first and are recorded.
  `rename()` (how QSaveFile and Ludus persistence publish a save) and `fsync()`
  wait until the recorded changes are written with `createWritable()`, which
  Chromium replaces atomically on close, so a failed disk write fails the save
  that caused it and the change is kept for the next save. Plain writes, new
  folders and deletions reach the folder on the next event-loop turn after
  `close()`; a failure there is reported in the editor. Changes inside generated
  folders stay in the session. The page asks before leaving while writes are
  pending.
- **Reopening.** Chromium keeps the last read-write folder handle in IndexedDB.
  **File → Reopen Last Project Folder** asks the browser for permission again
  and lists the folder; **Open Project...** starts its picker in that folder.
- **Fallback.** Firefox and Safari open the same folder read-only and lazily:
  edits stay in the session and **Download Project** saves them. A `File` that
  changed on disk after picking cannot be read again; the editor reports it and
  asks to open the folder again.

Rejected alternatives: raising the copy limit (memory and startup grow with the
whole project, and saves still need a download); a WasmFS OPFS or custom
backend (its asynchronous backends need pthreads and cross-origin isolation,
which the Pages host cannot provide, and Qt is built against the legacy FS);
JSPI (not available in every target browser and not what the pinned Qt build
uses); synchronous `XMLHttpRequest` of blob URLs (deprecated on the main
thread); and reading a file in an editor-level event loop before each use
(Content, SceneCook and Qt read files at hundreds of call sites).

Limits and coupling: a nested Wasm call made while another call is suspended
cannot wait for disk and fails with `EIO` (Qt queues its own events during the
suspension, so this needs a JavaScript callback that calls Wasm directly).
Qt sends those queued events as one batch when the call resumes. Qt 6.10.3
sent a batch by its starting length, so a nested event loop that emptied the
queue first ended Qt's event loop and froze the editor. The editor's Qt build
backports the upstream reentrant sender (see
[Ludus changes to Qt](../development/browser-editor.md#ludus-changes-to-qt)).
Qt cancels the browser default of input only when it handles the queued
event, after the browser has already applied that default. A queued key
reached Qt after Qt's hidden input element had moved its caret or taken the
text. A queued click in a Qt window moved focus from that element to the
document body, because nothing else in a Qt window is focusable, so keys typed
before Qt handled the click never reached Qt at all. While a host-folder call
is suspended, `browser/suspended_input.js` therefore cancels, in the capture
phase, the browser default of trusted `keydown`/`keyup` events and of trusted
`pointerdown`/`mousedown` events inside a Qt window. The events still reach
Qt's queue in their place among other input; Qt types text from `keydown`
itself and moves focus when it handles the click. Holding keys back and
dispatching them later would let a queued click overtake them. IME composition
keys, the copy, cut and paste shortcuts and clicks outside Qt's windows keep
their default.
`apps/editor/tests/browser_suspended_input_tests.cjs` covers the cancellation,
and the [World edit qualification](../development/tools/web-browser-tests.md#full-authoring-world-edit-qualification)
types an edit while a folder write holds the editor suspended.
Renames across mounts fail with `EXDEV`, which Qt turns into copies. Symbolic
links cannot be created in a folder mount. The overrides reproduce Emscripten
4.0.23's `__syscall_openat` and `__syscall_renameat` over the public FS API and
rely on MEMFS node fields; re-verify them in a browser when
`config/web_toolchain.json` changes the Emscripten pin.
`tools/web-browser-tests/host_folder_fs.mjs` checks the mount through a Qt-free
Wasm harness; the editor acceptance test opens and saves a project through it.

## Investment order

The ROI ranking below is engineering judgment based on current code and missing
services. It is not a measured market forecast or a calendar/cost estimate.

| Rank | Product slice | Relative investment | Value and gate |
| --- | --- | --- | --- |
| 1 | Website with a playable Ludus demo, documentation and downloads | Low/medium, reuses existing runtime | Lets people try the engine without setup. Ship after browser/device startup and failure UX are reliable |
| 2 | Browser content playground: inspect/tune a small authored example and export changes | Medium, bounded data and operations | Useful for learning/sharing. Requires S3/S4 document contracts and an actual editable vertical slice |
| 3 | Browser editor for curated asset/scene workflows | High, storage/import/render/input integration | Potential reach and onboarding value. Proceed only after round-trip correctness and representative performance are proven |
| 4 | Full online C++ engine development with arbitrary projects/builds | Very high, ongoing service operation | Adds isolated builds, queues, quotas, authentication, artifacts, cancellation, storage and debugging. Validate demand and an operating budget before committing |

A static website can host Wasm runtime artifacts. Full arbitrary C++ builds need
an execution strategy: curated precompiled modules, a substantial in-browser
compiler/toolchain investment, or isolated remote build workers. Remote builds
trade local setup for service cost and infrastructure ownership. A downloaded
browser application is not automatically a complete development environment.

## Shared contracts and later expansion

The preview reuses existing private Qt models, validation and serializers. It
does not turn them into a portable editor SDK. Native process, source-watch and
audio adapters are replaced explicitly; the browser does not simulate successful
builds. Projects opened from a folder on disk are read in place and, where the browser
allows, saved back to it; other files use volatile session memory and explicit
downloads. Persistent storage for projects that are not on disk is a later
increment requiring its own round-trip and performance evidence.

For cloud compilation, keep the operation protocol and cleanup/revision contract,
but execute jobs in isolated workers with resource limits and explicit ownership.
The web UI displays the same operation states. User project code does not execute
inside the API service or UI thread. Account, collaboration and billing work follows
validated demand, not the first browser demo.

## Go/no-go evidence

Use the W6 smoke baseline and one representative asset/scene task. Before further
investment, define target browsers/devices and payload budgets; measure download
bytes, cold-start p50/p95, frame/input latency, memory, and failure recovery. Test
missing WebGPU, device loss, reload, quota failure, cancellation, persistence and
export/import correctness. Include keyboard focus and narrow-window tasks.

Advance to curated editing only when the sample round-trips with the desktop
format and users complete the intended task reliably. Advance to remote builds
only with observed demand for C++ online authoring and an explicit cost envelope.
The Qt workspace preview is narrower than a scene editor or online C++ IDE.
Its package and browser document checks do not establish those later capabilities.

## Full-authoring frontend qualification

This is a proposed acceptance gate, not a result of the current document port.
[Qt 6.10 WebAssembly documentation](https://doc.qt.io/qt-6.10/wasm.html) describes
WebGL presentation and basic accessibility support, with complex widgets such
as trees/tables potentially missing support. Removing window chrome improves
layout but does not solve that semantic limitation.

Test Qt/Wasm and any DOM candidate against the same core-backed task fixtures:

- IME composition, selection, clipboard, keyboard focus, non-Latin/RTL text and
  screen-reader operation of forms, trees and tables in declared target browsers.
- Large paged asset/hierarchy models, stable selection and focused buffers under
  background updates; no widget per row or full model rebuild per heartbeat.
- Qt WebGL and engine WebGPU canvas composition, clipping, popups, DPI, input,
  picking, resize/device loss and a useful document UI without a working viewport.
- Bounded import, save/export and reload recovery; quota/denied storage and
  interrupted persistence preserve documents and expose actionable results.
- Payload, ready-to-edit latency, memory and idle cost on named devices, plus
  representative user tasks. A screenshot cannot qualify these behaviors.

Use a container layout without a simulated native title bar; prefer useful
context controls and optional panels to permanently disabled native actions.
On narrow containers, switch panels to tabs/drawers rather than clipping fields.
If a hard task needs fragile Qt-private changes, that is evidence for the DOM
alternative. Do not build both browser presentations indefinitely or require a
DOM rewrite before extracting the shared authoring core. A browser scene editor
still needs its own viewport and host-service implementation whichever UI wins.

### World placement and save recovery comparison

Reuse the [browser viewport runner and World fixture](../development/tools/web-browser-tests.md#full-authoring-world-edit-qualification)
when admitting a frontend. Report the frontend and build, browser version,
device, pixel ratio, viewport path and whether startup required a forced
backend. Record which step a candidate cannot reach. Qt/Wasm is the available
frontend for this fixture. A DOM authoring candidate needs the same core-backed
World commands, viewport and persistence before it can enter the comparison.

Use equivalent fresh copies and the same wording for first-use trials. Ask the
participant to open the project, move the named placement to the requested
position, recover from the controlled save refusal, then reopen and verify the
edit. Keep failure timing and available help fixed. When two frontends can
perform the task, have each participant use both and alternate frontend and
placement order across participants. Record completion, time, wrong turns,
assistance, the last meaningful action, the message and the first attempted
repair. After the attempt, ask what they expected that action to do and which
visible clue informed their choice. Include a paired trial with a brief
unrelated task after the refusal. On return record the first recovery action,
time to resume, any repeated edit and how the participant identifies the
pending value, saved value and available repair.

For the layout trial, enter a valid value before narrowing and widening the
same session across the declared breakpoint or down to the supported minimum
width. Record both widths, whether selection and the value survive, which
keyboard actions reach Apply or Reset, and whether focus remains visible.
Check IME composition, keyboard selection and clipboard, RTL or non-Latin input,
and screen-reader traversal of the inspector, hierarchy and tabular controls
on the declared browsers. Record any missing semantic role and its effect on
the task. Verify that the viewport and selection are usable on the ordinary
WebGL 2 fallback where supported.

Administer the same six-item raw NASA-TLX after each run, with the task named
and a 0–100 scale in steps of five. Explain that higher Performance scores
mean worse perceived performance. Preserve per-item results and use an
unweighted mean if reporting a total. After both runs, give participants up
to two minutes per frontend to sketch or describe where the edit lived at
failure and after recovery. Ask what UI cue informed that account and which
frontend made recovery easier to understand. Compare their account against
the working draft, saved bytes and reopened result. Record protocol departures
and treat a small pilot as descriptive. Link one design decision to an
observed action and the participant's explanation. Record a plausible
alternative interpretation and the observation that would distinguish them.
See the [issue's protocol and references](https://github.com/The-Ludus-Project/Ludus/issues/1272).
