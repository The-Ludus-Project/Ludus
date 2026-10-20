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
