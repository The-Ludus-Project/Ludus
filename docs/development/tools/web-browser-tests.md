# Packaged browser CI tests

This test-only runner executes the exact extracted Release ZIP in pinned
Playwright 1.55.1 / its bundled Chromium under Xvfb with a forced SwiftShader adapter.
Node is pinned to 22.16.0 in CI and npm dependencies have a committed integrity
lock. None of these files or dependencies ship in the game ZIP.

The software-GPU job follows the compile/package job and downloads its ZIP.
It checks real WGSL rendering by reading orange triangle pixels and animated
clear pixels, rather than trusting only frame counters. It also tests keyboard/
pointer movement, blur, hidden canvas, controlled RAF callback suspension, resize, DPR, a
sandboxed iframe, fullscreen, stop/restart and device destruction. Separate
contexts inject missing WebGPU and adapter/device rejection, delay startup for
cancellation, or abort asset requests. These contexts use a real DOM and the
production wasm; injected failures and callback suspension remain explicitly synthetic.
Native tab background/freeze/resume is a separate hosted hardware requirement:
Playwright's focus emulation prevented reliable lifecycle transitions in this CI setup.

Existing Node provider tests cover pipeline cancellation, callback generations,
validation errors and ownership release without claiming pixels. This browser
runner adds compositor/shader/DOM coverage. It fails if SwiftShader cannot render;
there is no skip or automatic mock fallback for the pixel case.

The CI-only launch arguments are recorded in report.json. The forced adapter
follows [Chromium's SwiftShader Vulkan pixel-test configuration](https://chromium.googlesource.com/chromium/src/+/354553e304d2ac575cd02d5dcb0d1551a09eac6f%5E%21/).
Experimental CI flags never establish flag-free player compatibility and must
not be copied into a user-browser acceptance procedure.

Reproduce on a disposable Linux CI/test host after preparing the package:

```bash
python3 -m zipfile -e out/packages/ludus-web-smoke-release.zip out/browser-qa/extracted
cd tools/web-browser-tests
npm ci
npx playwright install --with-deps chromium
xvfb-run -a npm test
```

The runner accepts extracted-directory, ZIP and output-directory paths as its
three optional arguments. Its report records the ZIP hash, browser version,
adapter, launch flags, HTTP requests and individual results. PNG screenshots
and report.json go under out/browser-qa/results, uploaded even on CI failure.
No special isolation headers are served. Real hardware/itch.io acceptance is
separately tracked in [hosted acceptance](../webgpu-hosted-acceptance.md).

## Browser editor viewport

After building the [browser editor payload](../browser-editor.md), run its actual
Qt/Wasm import and viewport acceptance with the same test dependencies:

```sh
cd tools/web-browser-tests
node editor_viewport.mjs ../../out/editor-pages ../../out/editor-viewport-qa
```

This separate runner opens a World, imports a small PBRT scene through SceneCook
and MEMFS, checks visible GPU pixels in Solid/Albedo/Lit, exercises directional shadow
preparation, navigates with Qt mouse
input, resizes, hides/shows its tab, destroys the real GPU device and tests an
unavailable WebGPU API. Explicitly synthetic pipeline delay/rejection also checks
that pending and failed candidates retain prior pixels and can recover. It saves
screenshots and a report with the adapter, browser version and launch flags. The default uses SwiftShader; set `LUDUS_QA_GPU=hardware`
for an installed physical adapter and optionally `LUDUS_QA_BROWSER` to an explicit
browser executable. These test flags do not establish flag-free hosted acceptance.

The Qt controls are canvas-drawn, so this runner uses a fixed 1440×1000 layout
and explicit input coordinates. Update its interaction steps when that layout
changes. Frame counters are paired with composited screenshot checks; neither
Qt widget pixels nor a completed shader build alone proves scene rendering.
The material checks sample a small triangle-interior patch away from the World
axes, which remain colored in Solid mode. Reports retain those RGB samples and
failure screenshots so an overlay or layout change cannot silently qualify a
material result.

## Browser editor project folders

[Project folders on disk](../browser-editor.md#project-folders-on-disk) have two
runners. `host_folder_fs.mjs` checks `apps/editor/browser/host_folder.js`
without Qt, through a small WebAssembly harness and mock folder handles over a
temporary directory; build and run it with
`python scripts/build-web-editor --test-host-folder`. `editor_host_folder.mjs`
drives the real editor payload in Chromium:

```sh
cd tools/web-browser-tests
node editor_host_folder.mjs ../../out/editor-pages ../../out/editor-host-folder-qa
```

It creates a project folder with a 40 MiB asset in Chromium's origin-private
file system, returns its `FileSystemDirectoryHandle` from a stubbed
`showDirectoryPicker`, opens it with the **Open** toolbar button, edits the
project name in **Project Settings**, saves, and reads the descriptor back from
the folder. The report records how many file bytes the editor read. Pass a
project folder as the third argument to copy that project instead (generated
folders are skipped); set `LUDUS_QA_NAME_FIELD='[x,y]'` when its settings page
places the Name field elsewhere, and `LUDUS_QA_OPEN_WORLD='[x,y]'` to also open
a World from the Project Explorer. The origin-private folder exercises the same
handle API as a folder on disk, but not the browser's permission prompt.
## Offscreen WebGPU frames

A web configuration with `LUDUS_BUILD_WEB_PROBES=ON` builds
`ludus_web_offscreen_probe` alongside the existing frame semantics probe. Serve
`out/build/<configuration>/tools/web-frame-probe` over localhost and open
`offscreen.html` in a WebGPU browser. It presents one red canvas frame, renders
three green offscreen texture frames, and reads back the texture through the
public RHI. Passing requires green readback pixels and exactly one canvas
texture acquisition; visually confirm that the canvas remains red.

The fixture exposes `data-state`, `data-stage`, `data-offscreen-frames` and
`data-surface-acquisitions` on its status paragraph. This is a real-browser
qualification fixture; it does not replace the Node provider semantics tests
or establish that an imported World's shadow output is correct.

## Full-authoring World edit qualification

The viewport runner also exercises [the World authoring qualification](../../architecture/editor-browser-strategy.md#full-authoring-frontend-qualification).
It imports the same redistributable triangle through SceneCook, uses its cooked
mesh and material for 9 and 49 independently selectable placements, and opens
a fresh copy of each World from Chromium's origin-private folder. WebGPU and
WebGL 2 use the same camera, scene, browser window and pixel ratio. One WebGL 2
run simulates ordinary startup without WebGPU. The other forces WebGL 2 using
the editor's supported override. The seed project uses one plain World and
changes its authoring data to construct the two sizes before the timed runs.

Each report records the browser, viewport API, output size, editor startup
payload, time to the editor-ready marker and time from Open World to matching
pixels. The test picks the named origin marker and changes its X placement
through Properties while checking every other placement. It measures the
pointer event to publication of the validated World draft, which the matching
recovery checkpoint corroborates. It then measures from core acceptance to
the first ready browser frame carrying the published extraction identity.
A screenshot checks the marker itself. Screenshot sampling adds browser and
capture overhead and is reported separately. The test checks that an older
accepted extraction does not draw after the latest one. It types that latest
edit while it holds the earlier edit's recovery checkpoint write open, so the
editor is suspended in a folder call and Qt queues the click and keys.
`typedDuringFolderWrite` records the unsent Qt events and the focused element;
the edit must still apply exactly. Draw calls and upload
bytes cover the instrumented browser graphics calls. Snapshot bytes, hardware
memory traffic and CPU picking time need independent instrumentation. The
accessibility-tree snapshot records browser-exposed roles for manual follow-up.

The final edit survives a narrow and wide layout, a refusal targeted at the
World file, a retry after write access returns, and a fresh open of the result.
The test checks the saved bytes before retrying and the identity and transforms
after reopening. Refusal and resize screenshots preserve the visible state and
any repeated warnings. Set `LUDUS_QA_BROWSER` to the named Chromium-family
executable under test and record the host, device and run conditions. A forced
backend or simulation only establishes that particular execution path.

A failed step records `failureState` in its sample: the latest published and
drawn extraction identities, the viewport canvases and `qtEvents`, read from
Qt 6.10's private suspend/resume control. If its
`isWaiting` stays false while `unsent` browser events remain, Qt's event loop
has stopped, rather than the renderer. The run fails on any page error, including the Qt
`TypeError` that once stopped that loop (see
[Ludus changes to Qt](../browser-editor.md#ludus-changes-to-qt)).
