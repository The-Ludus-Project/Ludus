# Graphics RHI

Link `Ludus::GraphicsRhi`. Include `<ludus/graphics/rhi/rhi.h>` for lifecycle and
frames and `<ludus/graphics/rhi/render.h>` for the bounded fullscreen rendering
slice. The selected private backend is Vulkan on Linux, Metal on macOS, and WebGPU or
WebGL 2 on Emscripten. Auto uses the pinned Emdawnwebgpu port before one eligible
WebGL 2 fallback. Public descriptions use engine types
and opaque handles; installed consumers need no backend or private headers.

The [public rendering guide](../../development/fullscreen-rendering.md)
describes resource ownership, asynchronous readiness, frame updates and errors,
including the [R3 ordered raster graph](../../development/fullscreen-rendering.md#ordered-raster-graphs-r3)
from `<ludus/graphics/rhi/graph.h>`. It shares the RHI resource/completion registry.
The [R4 compute profile](../../development/fullscreen-rendering.md#buffer-compute-and-indirect-rendering-r4)
adds buffer compute and indexed indirect draws on Vulkan, Metal and WebGPU,
with an explicitly authored CPU/direct variant for WebGL 2.
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

## Raster color writes

`RasterPipelineDescription::ShouldWriteColor` defaults to true. Set it to false
for a depth-only draw: all color attachment write masks become zero, including
on WebGPU where a void fragment stage requires that state. Attachment clears,
loads and stores still follow the render pass description. The write policy
participates in pipeline request identity.

## Vulkan diagnostics

Implementation files formatting VkResult use the private
`internal/vulkan_diagnostics.h` adapter. Ordinary diagnostics use `LUDUS_LOG_*`;
Foundation and public headers gain no Vulkan dependency from formatting.

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
