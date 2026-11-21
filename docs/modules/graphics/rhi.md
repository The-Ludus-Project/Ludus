# Graphics RHI

Link `Ludus::GraphicsRhi`. Include `<ludus/graphics/rhi/rhi.h>` for lifecycle and
frames and `<ludus/graphics/rhi/render.h>` for the bounded fullscreen rendering
slice. The selected private backend is Vulkan on Linux, Metal on macOS,
[Direct3D 12 on Windows](../../architecture/windows-direct3d12.md), and WebGPU
or WebGL 2 on Emscripten. Browser Auto uses the pinned Emdawnwebgpu port before one eligible
WebGL 2 fallback. Public descriptions use engine types
and opaque handles; installed consumers need no backend or private headers.

The [public rendering guide](../../development/fullscreen-rendering.md)
describes resource ownership, asynchronous readiness, frame updates and errors,
including the [R3 ordered raster graph](../../development/fullscreen-rendering.md#ordered-raster-graphs-r3)
from `<ludus/graphics/rhi/graph.h>`. It shares the RHI resource/completion registry.
The [R4 compute profile](../../development/fullscreen-rendering.md#buffer-compute-and-indirect-rendering-r4)
adds buffer compute and indexed indirect draws on Vulkan, Metal and WebGPU,
with an explicitly authored CPU/direct variant for WebGL 2. The corresponding
Windows limits and admission rules are in the
[Direct3D 12 profile](../../architecture/windows-direct3d12.md#compute-and-ordered-graphs-d4b).
The [R5 measured pass-merging path](../../development/fullscreen-rendering.md#measured-pass-merging-r5)
adds explicit investigation controls, planned scope/cost reports and a paired
image/CPU measurement harness. Merging remains opt-in for compatible workloads.
The [Sandbox handoff](../../development/fullscreen-rendering-handoff.md)
contains SDK commands, shader artifacts and validation evidence.

Serialize calls on the main thread. `Start`/`GetStartup` establishes one session;
browser startup is asynchronous. Keep the Platform window/canvas alive through
`Shutdown`. Start is Busy until Shutdown, including failed/lost sessions. Shutdown
is idempotent and invalidates resource IDs and pending callbacks before releasing
backend state. Delayed callbacks never retain application memory or affect a new
session. Legacy synchronous native entry points remain for compatibility; do not
mix the two lifecycles. The resource API requires the Start lifecycle.

All backends accept `SetFrameTarget` between frames. Zero dimensions skip frame
acquisition. `GetFrameInfo` gives the actual acquired extent and attachment
encoding after a successful begin. Resize retains resources unless the native
surface format changes, in which case the session fails and requires restart.
Frame acquisition timeout/outdated surfaces skip; device loss and unrecoverable
surface/validation failures stop the session. Only end a frame that began Ready.

Native Vulkan uses two fenced command/uniform slots, a graphics/present queue
family, per-image presentation semaphores and maintenance1 presentation fences.
Wayland requires surface/swapchain maintenance1 for safe resize/shutdown; missing
support fails explicitly. Headless windows use an owned offscreen image for
native rendering tests. Building requires only the existing Conan Volk/headers;
running requires a Vulkan loader and usable device. Browser packaging propagates
the existing pinned `--use-port` through the SDK target.

Native Metal uses two command-buffer-fenced shared uniform slots, BGRA8Unorm
Cocoa drawables and private headless textures. Submitted command buffers retain
their resources when public handles are destroyed. Shutdown waits for submitted
work and restores the borrowed view's previous layer. GPU errors are observed at
frame boundaries on the main thread. Nil/hidden/zero-size drawables skip frames.
Runtime shader creation compiles MSL 2.3 and pipeline reflection enforces the
single read-only buffer-0 contract. No Metal types leak into public headers.

The existing smoke/probe private WGSL interop remains a test boundary. External
applications use the public rendering API and exported `ludus_compile_shader`.

For native compute kernels using the scene's software traversal and runtime buffer
lengths, `ludus_compile_shader(... COMPUTE Entry SOFTWARE_RAY_QUERIES)` uses the
same SPIR-V-to-Metal translation and structured-buffer validation as scene render
graphs. It requires the pinned SPIRV-Cross on Metal and the pinned SPIR-V validator
on Vulkan. It declares no TLAS/ray-query capability and emits no browser artifact.
The option excludes `NATIVE_RAY_QUERIES`, raster stages and Emscripten. This is a
build-time profile; devices still negotiate ordinary compute before execution.

Neither compiler tools nor probe headers are installed as engine dependencies.

## Follow a Vulkan frame on Linux

Stephanie Hurlburt’s [*Beginners Guide to Vulkan* (2017)](https://www.khronos.org/blog/beginners-guide-to-vulkan)
collects introductions for programmers new to graphics APIs. Start with the
maintained [Khronos tutorial overview](https://docs.vulkan.org/tutorial/latest/01_Overview.html),
under **What it takes to draw a triangle**, to see how device and queue selection,
image acquisition, command recording, and presentation fit together. The tutorial
uses Vulkan 1.4, dynamic rendering, and Vulkan-Hpp. Ludus’s baseline uses Vulkan 1.1
render passes and checks explicit results through the C API.

- **Engine.** Start with the [World demo’s `Application`](../../../apps/world_demo/application.cpp).
  It creates a Platform window and calls `rhi::CreateDevice` with portable raster
  required. Follow `render` as it updates the frame target and calls the renderer
  to present. In the [Vulkan backend](../../../modules/graphics/rhi/src/rhi_vulkan.cpp),
  look for `vkAcquireNextImageKHR`, `vkQueueSubmit`, and `vkQueuePresentKHR`.
  [Native object relationships](../../architecture/rhi-gdi.md#reading-native-vulkan-object-relationships)
  explains what can be reused after a submission completes and what must wait for
  presentation.
- **Editor.** Open a room from the World demo and inspect it in the
  [World viewport](../../development/editor-workspace.md#world-renderer-and-camera-preview).
  **Play saved World** builds and launches the game with the saved room. Hosted
  Play on Linux needs an X11 or Wayland display, `VK_KHR_external_memory_fd`, and a
  matching adapter and driver in both processes. If hosting is unavailable, the
  viewport explains why and offers **Play in a separate window**.
  [Configure Play](../../development/editor-workspace.md#s4-scene-authoring).
- **App.** Build the [World demo](../../examples/world-demo.md#run-the-application)
  and launch it from a Linux desktop. With the Khronos validation layer installed,
  run the command below, expect `Reference presented 3 frames`, and check the log
  for validation errors. The `ludus_rhi_render_pixels` test checks offscreen
  rendering and readback separately. For an installed SDK app, use
  [`Ludus::GraphicsRhi` and the public shader helper](../../development/fullscreen-rendering.md)
  and exercise the [Linux Release package](../../development/game-packaging-publishing.md#build-and-inspect-the-package)
  on a target with a Vulkan loader, usable driver, and display.

  ```bash
  VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
    out/build/linux-clang-development/apps/world_demo/ludus_world_demo --frames 3
  ```

## Verify Vulkan rendering and compute without a compositor

Sascha Willems's [*Headless Vulkan examples* (2017)](https://www.saschawillems.de/blog/2017/09/16/headless-vulkan-examples/)
use a bounded submit-and-readback run for both raster and compute. On Linux,
Ludus's headless RHI path likewise creates no Vulkan window surface or swapchain.
It still needs a Vulkan loader and a driver that exposes the requested features.
Mesa lavapipe can run the checks below as a software Vulkan device.

From the checkout root, use the Linux Development SDK, dependency prefix and
shell from [the installed-SDK setup](../../examples/cornell-box.md#manual-setup-and-command-line-workflow).
Install the Khronos Vulkan validation layer and prepare the pinned SPIRV-Cross
translator with `./scripts/bootstrap-spirv-cross` if needed. Build and run the
installed R11 consumer with both display environment variables unset.

```bash
cmake -S tests/sdk_consumer/raster -B out/r11-raster-sdk -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH="$LUDUS_SDK_PREFIX" \
  -DLUDUS_SLANG_COMPILER="$LUDUS_SLANG_COMPILER" \
  -DLUDUS_SPIRV_VALIDATOR="$LUDUS_SPIRV_VALIDATOR" \
  -DLUDUS_SPIRV_CROSS="$PWD/out/shader-tools/spirv-cross/bin/spirv-cross"
cmake --build out/r11-raster-sdk --target ludus_sdk_r11_consumer
env -u DISPLAY -u WAYLAND_DISPLAY VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
  ctest --test-dir out/r11-raster-sdk -R '^ludus_sdk_r11_consumer$' \
  --no-tests=error --verbose
```

Expect one passing test. Check the log for `Validation Error` and `VUID-` even
if CTest passes. The [consumer](../../../tests/sdk_consumer/r11.cpp) compares
offscreen color stripes and checks a compute storage write with untouched bytes
on both sides. It uses public texture and buffer readback tickets. Check loader,
driver and layer availability if device creation fails. See [R11 qualification](../../development/fullscreen-rendering.md#correctness-and-measurement-evidence)
for the optional array cases and their capability requirements.

In an app, submit the producer work, request a readback, poll the ticket until
Ready, copy the bytes, and release the ticket. See the [public readback API](../../../modules/graphics/rhi/include/ludus/graphics/rhi/lifetime.h)
for formats, row pitch and ticket ownership. The [Editor viewport](../../development/editor-workspace.md#world-renderer-and-camera-preview)
can fall back to GPU rendering with CPU pixel transfer into the Editor. Use the
[windowed check above](#follow-a-vulkan-frame-on-linux) to exercise presentation
and hosted Play separately.

## Raster color writes

`RasterPipelineDescription::ShouldWriteColor` defaults to true. Set it to false
for a depth-only draw: all color attachment write masks become zero, including
on WebGPU where a void fragment stage requires that state. Attachment clears,
loads and stores still follow the render pass description. The write policy
participates in pipeline request identity.

## Raster fill mode

`RasterPipelineDescription::FillMode` defaults to `RasterFillMode::Solid`.
`RasterFillMode::Lines` rasterizes only each triangle's three edges as one-pixel
lines, for wireframe diagnostics; indexed triangle lists remain the only
topology, so the same mesh buffers draw either way. Line width and anti-aliasing
are not configurable. Edge fragments are depth-tested and written like filled
ones.

Creation returns `Unsupported` unless `RasterCapabilities::HasLineFill`. Metal
(`setTriangleFillMode`, set per draw) and Direct3D 12 (`D3D12_FILL_MODE_WIREFRAME`)
always report it. Vulkan reports it when the device enabled `fillModeNonSolid`:
the RHI enables the feature on devices it creates whenever the adapter supports
it, and an admitted host device declares it through
`NativeDeviceDescription::HasFillModeNonSolid`. WebGPU and WebGL have no line
fill mode and never report it. The fill mode participates in pipeline request
identity.

## Vulkan diagnostics

Implementation files formatting VkResult use the private
`internal/vulkan_diagnostics.h` adapter. Ordinary diagnostics use `LUDUS_LOG_*`;
Foundation and public headers gain no Vulkan dependency from formatting.

### Check Vulkan API usage for performance advisories

Arm's [PerfDoc](https://github.com/ARM-software/perfdoc#perfdoc) moved to the
Khronos validation layer. Its [best-practices checks](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/best_practices.md)
flag Vulkan API usage worth investigating during development. The Arm checks
can flag attachment usage on desktop Vulkan. Quantify any proposed change on
the target GPU with validation disabled.

Install a Khronos validation layer that supports
[`validate_best_practices` and `validate_best_practices_arm`](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/layers/vk_layer_settings.txt).
From the checkout root, run the [windowed World demo](#follow-a-vulkan-frame-on-linux)
with these per-launch settings.

```bash
mkdir -p out
cat > out/vulkan-best-practices-settings.txt <<'EOF'
khronos_validation.validate_best_practices = true
khronos_validation.validate_best_practices_arm = true
EOF
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
  VK_LAYER_SETTINGS_PATH="$PWD/out/vulkan-best-practices-settings.txt" \
  out/build/linux-clang-development/apps/world_demo/ludus_world_demo --frames 3 \
  > out/world-demo-vulkan-best-practices.log 2>&1
```

Confirm the run still presents three frames and inspect the log for validation
errors and best-practices message IDs. If the log is quiet, confirm the layer
loads with `VK_LOADER_DEBUG=layer` and check that its installed version supports
both settings. Record the layer version, driver and workload with any advisory.
Keep the normal core and synchronization checks for correctness. For an
attachment load/store change, use the
[RHI attachment lifetime trial](../../architecture/rhi-gdi.md#attachment-lifetime-and-pass-groups)
to compare output, GPU time and attachment traffic where available.

Apply the same launch settings to a standalone Linux SDK consumer to inspect
its own Vulkan calls. To investigate hosted Play, reproduce the saved workload
in the game process and compare with a separate-window run. The editor
[graphics debugging workflow](../../architecture/graphics-debugging.md#editor-hosted-game-and-sdk-workflow)
identifies the rendering process and capture boundary.

## Capability negotiation

`GetStartup().Capabilities` reports effective limits of the current fullscreen
API only while Ready. Adapter limits are clamped to the engine resource bounds;
no general graphics/compute features are implied. The snapshot becomes zero on
failure/loss/shutdown. `MaxFrameDimension2D` bounds requested frame dimensions;
surface negotiation and memory availability still determine actual acquisition.
`MaxUniformBufferSize` respects device limits and the engine's 16 KiB capacity.
`UniformBufferSizeAlignment` is the application's size granularity, not a native
offset alignment. Resource counts are per-kind capacities, not free-slot counts.

Use `Start(app, window, selection, DeviceRequirements{...})` to require minimum
frame dimensions or uniform bytes. Requirements are copied and checked before
Ready and application resource creation. Existing overloads require no extra
minima. Auto checks both backend attempts; forced selection never falls back.
WebGPU retains its default feature/limit request policy. A minimum checks the
enabled device, without elevating its limits to the adapter's maximum support.
`RequirementsUnsatisfied` identifies the first unmet requirement through
`UnmetRequirement`. Native builds accept only Auto and report
`BackendUnavailable` for forced browser policies. Busy does not alter a session.

The [profile negotiation guide](../../development/fullscreen-rendering.md#profile-format-and-limit-negotiation)
covers copied required/preferred raster/compute/format constraints, enabled
capability snapshots and per-attempt fallback diagnostics.

The [architecture implementation record](../../architecture/rhi-gdi.md#implementation-phases-and-acceptance)
records the bounded R0–R5 baseline and its evidence. Its
[remaining-work gates](../../architecture/rhi-gdi.md#remaining-work-and-decision-gates)
separate consumer extensions and qualification gaps from optional research.
