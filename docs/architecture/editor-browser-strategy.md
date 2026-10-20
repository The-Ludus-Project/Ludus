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
| Durable project storage and recovery | Session memory plus explicit download | Quota-aware persistent storage, recovery and tested import/export round trips remain work; a download request cannot certify that a file was saved |

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
the user's local CMake/compiler/RAD process. The WebAssembly target uses single-threaded static Qt, Asyncify modal dialogs
and explicit file import/download.
[Qt's WebAssembly documentation](https://doc.qt.io/qt-6/wasm.html) describes
asynchronous constraints and matching Qt/Emscripten builds. Do not infer that our
pinned engine WebGPU SDK is ABI-compatible with Qt's current Wasm requirements.

Browser storage also differs from desktop project folders. OPFS is origin-private,
quota-limited storage and is removed when site data is cleared; provide explicit
project export/import and recovery. [MDN OPFS documentation](https://developer.mozilla.org/en-US/docs/Web/API/File_System_API/Origin_private_file_system).
Emscripten's default MEMFS is volatile; IDBFS persistence requires explicit
synchronization. [Emscripten filesystem documentation](https://emscripten.org/docs/api_reference/Filesystem-API.html).

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
builds. Files use volatile session memory and explicit downloads. Persistent
project storage and a scene/game viewport are later increments requiring their
own round-trip and performance evidence.

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
