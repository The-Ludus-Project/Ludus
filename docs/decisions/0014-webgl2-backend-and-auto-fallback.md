# ADR 0014: WebGL 2 backend and WebGPU -> WebGL 2 Auto startup fallback

- Status: Accepted for the browser fullscreen slice (WebGL 2 fallback prompt 2).
  Real software-GPU smoke and Drift ocean rendering verified. Physical GPU and
  hosted acceptance remain open; current results are in the sandbox handoff.
- Depends on: ADR 0009 (browser WebGPU toolchain), ADR 0011 (public fullscreen
  rendering), ADR 0013 (browser GLSL ES shader toolchain).

## Context

The browser RHI was WebGPU-only, selected at compile time: `rhi.cpp` (facade) +
one backend TU (`rhi_vulkan.cpp` native, `rhi_webgpu.cpp` web). `StartupInfo`
`SelectedBackend` merely echoed the compiled backend's `Kind()`. The fallback
contract requires a browser demo that runs when WebGPU cannot initialize, while
preserving Vulkan (native) and WebGPU, and reusing the public RHI and its opaque
resource handles. ADR 0013 proved the generated GLSL ES 3.00 renders in real
WebGL 2.

## Decision

### Public API (call-site compatible)

- `Backend` gains `WebGL2`. A new `BackendSelection { Auto, WebGPU, WebGL2 }` and
  an additive `Start(app, window, selection)` overload select the browser backend.
  The existing two-argument `Start(app, window)` is retained and means Auto on the
  browser, Vulkan natively, so all existing call sites and the
  `noexcept(Start({}, {}))` assertions are unchanged.
- `StartupInfo` reports the actually selected backend plus bounded per-backend
  attempt diagnostics (`AttemptInfo WebGpu`, `AttemptInfo WebGL2`, `Requested`).
  `ShaderDescription` carries `GlslEs`/`GlslEsEntry` (ADR 0013); no backend objects
  are ever exposed.

### Runtime dispatch in one browser artifact

Both browser sub-backends are compiled into the one web library, each in its own
leaf namespace (`webgpu`, `webgl`) via a per-source namespace
macro, with no duplicate exported backend symbols. A new dispatcher
(`src/rhi_web.cpp`) defines the `backend::` facade contract and routes
`Kind`/`Start`/frame/resource calls to the active sub-backend. Native keeps
compiling `rhi_vulkan.cpp` as `backend::` directly (Vulkan changes limited to the
new `Start` selection parameter and a `Supports` predicate).

### Auto fallback policy

Auto starts WebGPU, including its existing Core->Compatibility feature-level retry.
Only a genuine capability/startup failure (instance/adapter/device/surface/
rendering/device-lost) triggers exactly one WebGL 2 attempt, via a facade
`internal::SetFallback` hook consulted by `Fail()` before a Pending session is
finalized. Shader validation and invalid-call defects, and any failure after the
session is Ready, are never concealed by fallback. Forced WebGPU or WebGL2 pins a
single sub-backend and fails explicitly if unavailable (never silently switches).
`internal::SelectBackend`/`RecordAttempt` record the committed backend and bounded
attempt errors for QA.

### Canvas ownership

Platform owns the `<canvas>` DOM node and its listeners; a graphics backend
acquires its own context by selector. A canvas that committed a WebGPU context
cannot acquire WebGL, so the dispatcher tears the failed WebGPU attempt down
(`webgpu::Shutdown`) before the WebGL 2 attempt; the WebGL backend acquires
the real WebGL context after Platform replaces the managed canvas and rebinds
its observers/input listeners. A restart also prepares a fresh canvas so changing
API or recovering from loss is supported. Replacement failure marks the one
WebGL attempt before finalizing, preventing recursive fallback. No throwaway
GL contexts accumulate across restarts. No DOM node is replaced behind
Platform's back. Generation tokens isolate attempts and
sessions: late WebGPU callbacks carrying a stale token are dropped, shutdown
clears the fallback hook before teardown, and restart reselects per policy.

### WebGL 2 backend

Private GLES3 code (`src/rhi_webgl.cpp`) implements only the fullscreen contract:
compile the generated GLSL ES stages, link a program, bind one std140 uniform
block at binding 0 (`glBindBufferBase`), a VAO for the attributeless
`gl_VertexID` fullscreen triangle, `glBufferSubData` upload (no per-frame
allocation), `glDrawArrays(GL_TRIANGLES, 0, 3)` into the default framebuffer, and
viewport-based resize. It rejects a missing/oversized uniform block before
drawing, verifying `GL_UNIFORM_BLOCK_DATA_SIZE` against the upload size. Context
loss uses the Emscripten `webglcontextlost` callback -> `internal::Fail(DeviceLost)`;
old handles are invalidated and restart recreates on a usable context. The default
framebuffer is reported as UNORM in `FrameInfo`.

### Installed link settings

The installed `Ludus::GraphicsRhi` INTERFACE link options gain
`-sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2 -sFULL_ES3=1` alongside the existing
emdawnwebgpu port, so an Auto `find_package(Ludus)` consumer carries both browser
paths. Native packages are unchanged.

## Consequences

- The browser build links two backends; the facade's `CreateShader` validation
  gained a GLSL ES arm keyed on the active `Kind()`. The smoke app's input gating
  now recognizes any browser backend, not just WebGPU.
- WebGPU-only private interop (`ProbeDevice/Format/Pass`) is re-exported by the
  dispatcher from `webgpu`; the WebGL 2 sample path (prompt 3) uses the
  public rendering API instead.
- Verified here (software GPU): the installed-SDK consumer forced to WebGL2
  renders the diagnostic with correct orientation/color in pinned Chromium.

## Observed limitations

- Hardware (non-software) WebGL 2 and hosted HTTPS/iframe acceptance are not
  covered; verification used ANGLE/SwiftShader in pinned Chromium 140.0.7339.186.
- The pinned `spirv-val` requires glibc 2.38, unavailable on the glibc-2.34 build
  host, so the full pinned `ludus_compile_shader` GLSL ES generation was exercised
  with a from-source SPIRV-Cross + spirv-val (ADR 0013); the engine-backend render
  here embedded the Prompt 1 hash-verified GLSL ES to isolate the backend.
- clang-format/clang-tidy 18 are the pinned gates; only clang-format 23 was
  available here (it disagrees only on the repo's compact designated-initializer
  brace style and macro-wrap columns).

## Update: browser editor viewport (November 2026)

The browser editor's viewport owns its canvas outside Platform, so Auto's
`ReplaceCanvas` swap cannot recover it after a WebGPU attempt. The editor
therefore applies the same policy itself with forced selections: it starts
WebGPU and, on a startup failure only, removes the canvas, creates a fresh one
and starts WebGL 2. A device that reached Ready never switches. The page's
`viewport-backend` query forces either API. See
[Viewport graphics API](../development/browser-editor.md#viewport-graphics-api).

The WebGL 2 backend now also admits offscreen-only frames
(`FrameTarget::ShouldPresentToSurface == false`): it binds no default framebuffer,
so the canvas keeps its last presented image while texture passes, such as shadow
preparation, run. Surface passes and fullscreen draws are rejected in that mode,
as on WebGPU.

## References

- `docs/development/webgl2-backend-evidence.json`
- `docs/development/evidence/webgl2-engine-backend.png`
- [Emscripten OpenGL / WebGL 2 support](https://emscripten.org/docs/porting/multimedia_and_graphics/OpenGL-support.html)
- [Emscripten HTML5 context loss/restoration](https://emscripten.org/docs/api_reference/html5.h.html)
- [Canvas context mode rules](https://developer.mozilla.org/en-US/docs/Web/API/HTMLCanvasElement/getContext)

The recovered prompt evidence is historical. Current integration uses the pinned
validator unchanged, verifies the installed SDK and native sanitizers, and adds
regression coverage for failed canvas replacement and stale callbacks.
