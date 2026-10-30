# Public rendering: fullscreen and portable raster

Link `Ludus::GraphicsRhi` and include `rhi.h` and `render.h` under
`ludus/graphics/rhi/`. This slice supplies one vertex/fragment pipeline, one
uniform binding and one non-indexed fullscreen triangle per frame. Ludus owns GPU
creation, binding, upload synchronization, submission and cleanup. Applications
own shader source, upload layout, colors, simulation, camera and UI.

## Shader build and upload contract

After `find_package(Ludus CONFIG REQUIRED)`, compile an application-owned shader:

```cmake
add_executable(fullscreen main.cpp)
target_link_libraries(fullscreen PRIVATE Ludus::GraphicsRhi)
ludus_compile_shader(TARGET fullscreen NAME display SOURCE shaders/display.slang
    VERTEX vertexMain FRAGMENT fragmentMain
    INCLUDES shaders DEFINES DISPLAY_SCALE=1 DEPENDS generated_settings.slang)
```

Set `LUDUS_SLANG_COMPILER` to the pinned host tool. Linux/browser builds also
require `LUDUS_SPIRV_VALIDATOR`; macOS builds emit MSL directly.
Python 3.10+ is required. Configure/build never downloads tools. Slang 2026.1.2
compiles both targets; SPIRV-Tools validates Vulkan 1.1 SPIR-V at build time. The
manifest records exact expanded commands, emitted entries, profiles, sizes and
artifact hashes. WGSL receives actual browser shader/pipeline validation through
resource creation; successful compilation alone does not establish runtime proof.

### macOS Metal

Run `./scripts/shader-probe bootstrap` on macOS to acquire the SHA-256-verified
Slang 2026.1.2 release for arm64 or x86_64. Configure with
`-DLUDUS_SLANG_COMPILER="$PWD/out/shader-tools/slang/bin/slangc"`.
`ludus_compile_shader` emits separate `<name>.vertex.metal` and
`<name>.fragment.metal`, reflection JSON, a manifest, a depfile and the same
generated `Vertex()` / `Fragment()` descriptions used by other backends.
Source, include and define changes rebuild the artifacts; no-op builds do not.
Slang's emitted `#line` directives name the source and its includes relative
to the source directory or an `INCLUDE` directory, never by absolute path, so
the generated header and manifest hashes are identical in every checkout. A
compiler cache shared between checkouts (see
[building](building.md#compiler-cache-across-checkouts)) can otherwise reuse a
translation unit whose embedded MSL literal has another length. An included
file outside those directories fails the build; add its directory to `INCLUDE`.

Slang 2026.1.2's Metal backend drops `[noinline]`, and the Metal compiler then
inlines every helper into the entry point. The shader helpers (fullscreen,
raster/compute and render-graph stages) restore it: an emitted function whose
`#line` source line, or the line above it, applies `[noinline]` to that name is
defined with `__attribute__((noinline))`. Without that, the reference path
tracer's resampled light sampling made its pipeline take 34–54 s to build on an
Apple M5. On the macOS 15.7 CI runner the build failed with a bare "Compilation
failed", and every later pipeline in the process failed too. With it, the
pipeline builds in about 6 s. The rule matches by name, so keep `[noinline]` on
the line of the function name or the line just above it. Methods, whose emitted
names carry their type, are not matched. Code without `#line` directives is not
matched either. That includes render-graph packages compiled through the in-process
shader worker (`ludus-shader-worker`), whose Metal stages the Metal compiler may still
inline.

The macOS header supplies `Msl` and `MslEntry`. Source is compiled as MSL 2.3
during `CreateShader`; entries and stages are checked against the actual library.
Pipeline reflection rejects used textures/samplers/storage or buffers outside
read-only buffer 0, and checks occupied bytes against the declared uniform size.

`Start` is synchronous and selects Metal under native Auto. It accepts a borrowed
Cocoa window/content view or Headless, exposes a conservative 16384 texture limit
on Mac2 / Apple4 devices and later, and caps uniforms at 16 KiB. Cocoa uses a
`CAMetalLayer` with BGRA8Unorm, two drawables and bounded acquisition; hidden,
minimized, zero-size or unavailable drawables skip. Keep the Platform window alive
through shutdown. RHI restores its previous layer when disconnected.
Headless uses a private BGRA8Unorm render target. Both report Unorm attachments.
Two command-buffer-fenced uniform slots protect CPU/GPU reuse; submitted buffers
retain resources after handle destruction. Shutdown drains submitted work and
aborts open frames. Command-buffer errors fail the session at a subsequent frame
boundary, requiring shutdown/restart; there are no facade mutations on GPU threads.

Tests verify clear/readback, live Cocoa presentation, shader and binding errors,
changing uniforms, 40-frame bursts, resize, maximum-sized uploads and restart.
GPU cases explicitly skip when no Metal adapter is present; policy/window checks
still execute. `LUDUS_TEST_COCOA=1` enables the WindowServer presentation case.
Clang 18 ASan cannot start on the local macOS 26.6 host; macOS 14 CI executes
the sanitizer suite. This slice provides the existing fullscreen API; general
meshes, compute, textures, depth and blending are covered by the later R1–R4 slices below.

### Optional GLSL ES 3.00 (WebGL 2) backend artifact

Set `LUDUS_SPIRV_CROSS` to the pinned build-time SPIR-V -> GLSL ES translator
(`config/spirv_cross_toolchain.json`, SPIRV-Cross `vulkan-sdk-1.4.313.0`) to also
emit GLSL ES 3.00 for the WebGL 2 fallback. With it unset, SPIR-V/WGSL builds are
unchanged. When set, each stage's validated SPIR-V is translated to
`<name>.<stage>.essl`, the browser build of the generated header carries both WGSL
and GLSL ES, and `ShaderDescription` exposes `GlslEs` / `GlslEsEntry` (which link
through `main`). The manifest adds `glsl_es_entries`, a `glsl_es_layout` derived
independently from the emitted std140 block, the `300 es` profile and translator
provenance. See ADR 0013 and `docs/development/webgl-shader-feasibility-evidence.json`.

Author vertex inputs with `SV_VulkanVertexID` / `SV_VulkanInstanceID` (not
`SV_VertexID` / `SV_InstanceID`): the latter require base-vertex/instance
(`SPV_KHR_shader_draw_parameters`), which has no GLSL ES / WebGL 2 equivalent and
the translator rejects. For a non-indexed, zero-base-vertex draw the values are
identical on Vulkan, WebGPU and WebGL 2.

The generated `display.h` provides
`ludus::shaders::display::Vertex()` and `Fragment()` descriptions, with explicit
backend entry names and minimum uniform sizes from each target's reflection.
Include it in application implementation code. Only the selected target's bytes
are embedded in the application. Shader description storage is borrowed during
CreateShader only. Raw descriptions can instead supply SPIR-V words and WGSL
text with separate explicit entry names. Use validated artifacts; runtime shader
creation does not replace full offline SPIR-V validation.

The shader has set/group **0**, binding **0**, one uniform block visible to both
stages, vertex-index-generated positions, and location 0 RGBA fragment output.
No vertex attributes, textures, samplers, storage, compute, blending, depth,
culling, multiple draws, dynamic offsets or push constants are provided.
Uniforms are 16..16384 bytes in multiples of 16. The 16 KiB ceiling permits
full-resolution simulation snapshots while staying within OpenGL ES 3.0's
minimum uniform-block limit. Shader reflection and runtime admission share this
ceiling. CPU staging remains fixed (128 KiB per active backend for eight slots);
updates allocate nothing. Eight resources of each kind are
available; exceeding the pool or exhausting never-reused IDs fails explicitly.

CPU layouts are application contracts. Read separate SPIR-V, WGSL and Metal reflection
and emitted layout; use explicit padding and C++ standard-layout/offsetof/size/
alignment assertions. Different targets may need different upload structs. The
helper carries occupied size to prevent undersized bindings; it does not infer
CPU field types or assert that packing matches. The diagnostic's independently
checked fields are resolution float2 at 0, elapsed float at 8, direction float3 at
16 and tint float4 at 32: 48 bytes, alignment 16 for each target, padding at
12..15 and 28..31. These measurements apply to that shader only (ADR 0010).

### Browser backend selection (WebGPU / WebGL 2)

On the browser, `Start(app, window)` selects `BackendSelection::Auto`: it attempts
WebGPU (including its compatibility retry) and, only on a capability/startup
failure, makes exactly one WebGL 2 attempt. `Start(app, window, selection)` forces
`WebGPU` or `WebGL2` for diagnosis; a forced backend that is unavailable fails
explicitly rather than switching. Native accepts only Auto, selecting Vulkan on Linux and Metal on macOS.
`StartupInfo::SelectedBackend` reports the committed backend, and `WebGpu`/`WebGL2`
carry bounded per-attempt errors for QA. The GLSL ES artifact requires
`LUDUS_SPIRV_CROSS` at build time (ADR 0013/0013); otherwise the browser build is
WebGPU-only. `apps/smoke` is the worked example of a backend-agnostic renderer
driving this through the public API. See ADR 0014 and
`docs/development/webgl-fallback-sandbox-handoff.md`.

## Readiness and ownership

### Explicit R0 device ownership

Include `ludus/graphics/rhi/device.h` for the first explicit R0 contract slice.
It links through the same `Ludus::GraphicsRhi` target and shares the facade's
single session, authoritative resource registry, callback isolation and backend
teardown. Every explicit operation validates its device owner before delegating.
Frame operations also validate the borrowed surface owner. A headless target is
a surface in this slice; independent device-only/offscreen resource APIs remain
future work. Serialize all operations on the main thread.

```cpp
namespace rhi = ludus::graphics::rhi;
rhi::DeviceHandle device;
rhi::SurfaceHandle surface;
rhi::DeviceDescription request;
request.Limits.MinUniformBufferSize = 48;
request.Preferred.Compute = true;
rhi::StartupInfo failure;
const auto started = rhi::CreateDevice(app, window, request, device, surface, &failure);
// Ready or Pending owns both outputs. Otherwise inspect started and, for a
// synchronous backend startup failure, failure. Admission preserves all outputs.
if (started == rhi::DeviceStatus::Ready || started == rhi::DeviceStatus::Pending)
{
    rhi::DeviceInfo info;
    const auto polled = rhi::GetDeviceInfo(device, info);
    if (polled == rhi::DeviceStatus::Ready)
    {
        // Use device-aware CreateShader/CreateUniform/CreatePipeline, GetStatus,
        // UpdateUniform and DrawFullscreen overloads; check every result.
        // Inspect info.Enabled.Compute before choosing the GPU compute variant.
    }
    (void)rhi::DestroyDevice(device);
}
```

Poll once per application tick while Pending; continue only after Ready. Keep
the borrowed Platform window/canvas alive until `DestroyDevice`. Required `Compute` or `IndirectRendering` rejects forced WebGL 2 before
launching startup; other backends validate their enabled compute limits before Ready.
Required `PortableRaster` validates the negotiated bounded
profile before Ready, including each browser Auto attempt; see the
[portable raster contract](#portable-raster-r1). `GetCompiledBackends` reports compiled code, independent of
adapter availability. Supported/Enabled report implemented fullscreen, portable raster and bounded compute operations
only after Ready; they make no claim about raw adapter features. The copied
request retains its preferred features, including unavailable ones. Effective
limit requirements apply to every Auto attempt through the existing negotiation.

Use `SetFrameTarget(device, surface, target)`, `BeginFrame(device, surface)` and
`EndFrame(device)` for explicit frames. Skipped opens no frame. The output form
`GetFrameInfo(device, surface, info)` preserves `info` on invalid ownership,
pending startup or a closed frame. Resource overloads preserve the existing
fullscreen ownership, failed-resource inspection and InUse rules; this slice
does not introduce deferred destruction or a second resource registry.

WebGPU also accepts `FrameTarget::ShouldPresentToSurface = false` for an
offscreen-only frame. Use nonzero logical dimensions, then record texture
attachment passes or compute work through the normal frame/submission APIs.
This mode does not configure, acquire, clear or present the canvas. Surface
attachment passes and compatibility fullscreen draws return `InvalidState`;
`GetFrameInfo` reports the logical target extent. Other backends return
`Unsupported` from `SetFrameTarget` and preserve the previous target. The default
remains a surface frame. This lets browser shadow preparation retain the last
presented image while its candidate resources are pending.

Device owners never wrap/reappear across restart; callback attempt tokens remain
separate so fallback retries keep the same device owner. Stale/foreign devices
and surfaces return InvalidHandle without changing another session. Device loss
stops explicit work with DeviceLost, clears enabled capabilities and invalidates
resources; the owned lifecycle record remains queryable until destruction.
Published asynchronous failure likewise still requires `DestroyDevice`. A
synchronous startup failure preserves null outputs, copies optional startup
diagnostics, tears down and leaves the session idle. Successful `DestroyDevice`
clears its device argument; clear stale surface/resource variables yourself before
reusing them as creation outputs.

Use either the explicit lifecycle or the facade lifecycle for a session.
Duplicate starts are rejected. The compatibility `Shutdown` still cancels the
shared session, making all explicit handles stale; it cannot leave an explicit
owner pointing to a later facade session. Existing facade-only callers retain
their behavior.

The reference-backend lifecycle suite tests admission, copied requirements,
output preservation, cancellation, late completions, fallback generations,
cross-owner misuse, loss and non-wrapping owner exhaustion. R1/R2 add typed
slot/generation resources, retained command batches and deferred retirement;
R3 adds ordered graph compilation. [Profile negotiation](#profile-format-and-limit-negotiation)
adds enabled limit/format admission; independent devices/surfaces remain [architecture extensions](../architecture/rhi-gdi.md#remaining-work-and-decision-gates).

### Profile, format and limit negotiation

`DeviceDescription::RequiredProfile` adds copied raster, compute and format
constraints to the existing workload flags and fullscreen minima. Admission checks
the selected backend's enabled limits before publishing Ready and before resource
creation. Every Auto fallback attempt checks the same copied request. A nonzero
required raster constraint or format request implies portable raster admission;
a nonzero compute constraint implies compute admission. A forced WebGL 2 compute
requirement rejects before launching startup.

```cpp
rhi::DeviceDescription description;
description.RequiredProfile.Raster.MinBindings = 3;
description.RequiredProfile.Raster.MinVertexBuffers = 2;
description.RequiredProfile.Raster.MinUniformRange = 48;
description.RequiredProfile.Raster.MaxUniformOffsetAlignment = 256;
description.RequiredProfile.FormatCount = 1;
description.RequiredProfile.Formats[0] =
{
    .Format = rhi::RasterFormat::Rgba8Unorm,
    .Uses = { .Sampled = true, .Filterable = true, .ColorAttachment = true, .Blendable = true },
    .MinDimension2D = 96,
};
```

Zero limit fields impose no additional constraint. `Min*` fields ask for at least
that effective capacity; `MaxUniformOffsetAlignment` and
`MaxStorageOffsetAlignment` ask for an alignment no larger than that value.
Workgroup axis limits and invocation product are independent bounds, not a promise
that every combination fits. Limits are not memory reservations: resource creation
still checks dimensions, mip packing, aggregate upload size and allocation errors.

While Ready, `DeviceInfo::Profile` contains the immutable raster/interface/compute
limits and five `FormatCapabilities` entries indexed by `RasterFormat`. Include
`profile.h` to use the profile vocabulary directly, or `raster.h` to call
`GetFormatCapabilities(device, format, output)`. Invalid owners or malformed format
enums preserve query output; pending, failed or lost devices zero it. A known but
unavailable format returns Unsupported with zero output. `DeviceInfo` profile
snapshots are also zero outside Ready, so an earlier ready snapshot must not be
used after loss/restart.

Format use flags describe implemented operations. All supported formats allow
original-format sampled views and linear filtering. Only RGBA8 linear/sRGB support the
current RGBA color/private-depth attachment and blend path, within backend
format limits. R8/RG8 remain sampled only. L4 adds RGBA16F when `HdrColor` is enabled: it supports
sampling/filtering/blending through an enabled color attachment. R9 also accepts
original binary16 uploads and sampled mip chains. Attachment/blending capability
is independent of sampled HDR support.
SampleCounts is a bit mask (value 1 for
one sample); R10 independently enables four samples and color resolves. Storage texture uses remain false. R8 enables
bounded RGBA8Unorm texture copy source/destination uses; other formats remain false.
Creation uploads and frame readback alone do not constitute those uses. Vulkan
checks combined sampled/upload/copy/attachment usage and private-depth bounds; portable
backends intersect their guaranteed normalized formats with Ludus's implemented
profile. Texture and offscreen pipeline creation enforce the admitted snapshot.

A request contains up to seven unique format entries; invalid format values, counts,
duplicates, empty use sets or invalid sample counts return InvalidDescription
without launching a backend or publishing outputs. A required constraint miss
reports `StartupError::RequirementsUnsatisfied`, `RequirementFailure::Profile`
and `StartupInfo::ProfileFailure`. That bounded diagnostic records the first
unmet field, requested/effective bound, format and axis where relevant. Browser
attempt records retain their own profile failure; a successful fallback clears the
final failure while preserving the first attempt's reason.

`PreferredProfile` uses the same vocabulary without failing admission or elevating
limits. Inspect `DeviceInfo::UnmetProfilePreference` for its first unmet constraint
and `UnmetPreferences` for unavailable preferred workload flags. These diagnostics
are valid only while Ready. Supported/Enabled remain implemented operations on
the enabled device; this API does not enumerate raw adapter maxima or enable a
feature based on its backend name. The pinned WebGPU default-device request policy
is preserved. Independent devices/surfaces remain a separate extension.

### Compatibility facade

1. Start RHI with the Platform window/canvas and poll GetStartup. Continue only
   when Ready. Browser startup is Pending; failures require Shutdown before Start.
2. Create vertex/fragment shaders and one uniform into default handles. Ready or
   Pending transfers ownership of a handle. Poll GetStatus for each; Pending is
   never drawable. Wait for all three Ready before CreatePipeline, then poll it.
   Pending dependencies return NotReady without creating a pipeline; Pending
   creation always supplies an owned output handle.
   Failed creation can retain a Failed handle for inspection/destruction; partial
   backend objects are already released. Invalid descriptions/admission failures
   leave the output unchanged. Never retry into a nondefault handle.
3. Update the complete uniform byte range. It must be updated before first draw.
   Upload storage is copied immediately, so application stack data is safe.
4. SetFrameTarget from framebuffer dimensions, then BeginFrameStatus. Skipped
   opens no frame: retain resources and retry next tick. After Ready,
   GetFrameInfo reports actual pixels and Unorm/Srgb attachment conversion. Update
   resolution from that extent before the draw if native negotiation changed it.
5. DrawFullscreen only with a Ready pipeline, once inside that frame. UpdateUniform
   is allowed before the first draw, including after begin; updates after draw
   are rejected. EndFrameStatus submits. Do not end a skipped/failed begin.
6. Between frames destroy pipeline, then shaders/uniform. Dependencies of Ready
   or Pending pipelines return InUse. A failed pipeline holds no dependencies.
   Shutdown can replace individual destruction and also abort an open frame.

Copied handles refer to the same ownership; destroying one invalidates every
copy. Destroy does not clear your local value: assign `{}` before reusing it as a
creation output. Stale/default/previous-session handles return InvalidHandle.
IDs are monotonic across shutdown/restart, and asynchronous callbacks carry only
IDs in process-owned storage. They neither retain application pointers nor write
into a reused resource slot. Serialize all operations on the main thread.

Vulkan allocates/map-persistently uploads two uniform slots at creation; draw
copies the latest CPU shadow into the slot whose frame fence has completed.
Resource destruction waits for GPU use; presentation fences retire WSI objects
before resize/shutdown. WebGPU writes are ordered with submissions and submitted
commands retain references when host resources are released. The engine creates
no shader/buffer/pipeline resources or heap allocations per draw. Existing
backend frame encoder/texture objects continue to follow the frame lifecycle.

## Errors, resize and restart

Inspect each returned status and GetStartup after failures. Failed or DeviceLost
sessions release resources and require Shutdown/Start plus recreation and upload.
A zero-size target skips without invalidating resources. Frame acquisition
outdated/timeout conditions skip; native suboptimal presentation schedules resize.
Native attachment format changes fail rather than reuse an incompatible pipeline.
GetFrameInfo outside an open frame returns zero dimensions.

Fragment coordinates have a top-left origin; positive viewport heights/no
culling match the diagnostic on all targets. Shader output is numeric RGBA;
Unorm attachments quantize it, while Srgb attachments apply linear-to-sRGB RGB
conversion. GetFrameInfo reports the attachment conversion, so applications can
choose their output convention explicitly. Palette and display choices belong to
the application. Monitor/browser color management is outside these pixel tests.

See `tests/sdk_consumer/render.cpp` for complete exception-free public usage,
layout assertions, skipped frames, resize, dependency rejection and restart.

## Portable raster (R1)

Include `ludus/graphics/rhi/raster.h` and link the same `Ludus::GraphicsRhi`
component. The first portable raster profile supplies immutable buffers,
sampled textures, complete views, samplers, reflected stages, binding layouts,
binding snapshots and indexed/instanced triangle pipelines on Vulkan, Metal,
WebGPU and WebGL 2. It uses the explicit device and existing acquired frame
surface/headless color target. It does not introduce a second device session.
Request `DeviceDescription::Required.PortableRaster = true` when this workload
is mandatory. [R4](#buffer-compute-and-indirect-rendering-r4) separately enables
buffer compute and indexed indirect rendering on Vulkan, Metal and WebGPU;
WebGL 2 requires an authored CPU/direct variant.

`GetRasterCapabilities` reports effective allocation, texture dimension,
uniform offset/range, record and per-frame draw bounds. Limits are verified on
the negotiated device/context, independently of the backend name. Creation
requires a ready device and a null output; it is forbidden in an open frame.
Only Ready or Pending publishes ownership. WebGPU creation returns Pending while
both validation and out-of-memory scopes drain; poll the typed `GetStatus`
once per tick. A dependency that is still pending returns NotReady without
publishing an output. A published failed resource must still be destroyed.
All public operations are serialized on the device's owner thread.

For native live development, set `RasterShaderDescription::ShouldPrepareAsynchronously`
before creation. The backend consumes/copies the source and metadata before
returning Pending and prepares it on one dedicated driver worker. Graphics and
compute pipelines inherit that policy from their shader inputs; their description
can also explicitly select it. Pending owns a handle: poll `GetStatus`, retain the
active replacement target, and never bind a candidate before Ready. One-shot
callers retain synchronous native creation by default; WebGPU keeps its existing
asynchronous API and WebGL its context-thread policy.

Native preparation admits at most `MAX_WORKER_QUEUE_TASKS` jobs, including
uncollected completions. Queue/startup/allocation failure is explicit and never
falls back to blocking driver compilation. Captured shader bytes and task storage
are charged to RHI host storage. The worker never mutates facade records, resource
tables or render-pass caches. Owner-thread polling publishes results; pending
callbacks retain dependencies even after public handles are detached. Shutdown
joins the preparation lane before releasing the device, shaders, layouts or pass
cache. Vulkan pipeline work uses no shared `VkPipelineCache`. See the
[development reload transaction](render-graph-authoring.md#native-shader-worker-and-live-development-builds).

The historical R1 profile below is deliberately bounded. L1 adds targets/depth,
L2 adds R8 coverage, and [L3 material/content versions](../architecture/renderer-systems.md#implemented-l3-material-and-content-versions)
expanded the original tables to 32 records per kind, sampled R8/RG8/RGBA8 mip chains
and explicit mip samplers. [L4 lighting](../architecture/renderer-systems.md#implemented-l4-bounded-forward-lighting)
expanded tables to 64, enables up to sixteen queried vertex attributes, and adds
capability-gated single-mip sampleable/filterable/blendable RGBA16F attachments.
Public declarations are the current API contract.

The original R1 profile:

| Facility | Contract |
| --- | --- |
| Records | Configured `Storage.ResourcesPerKind` (default 64), including pending/detached records retained by dependencies or submitted work |
| Buffers | Complete immutable initial bytes up to enabled `MaxBufferSize`, subject to logical resource budgets; Vertex, Index16, Index32 or Uniform role |
| Uniforms | Bound range at most 16 KiB, range size a multiple of 16, offsets aligned to the queried limit (at least 256); allocation may be larger than a bound range |
| Textures | RGBA8 linear or sRGB sampling, within the enabled per-format/device dimensions; one mip/layer/sample |
| Uploads | Complete top-left RGBA8 rows, explicit pitch divisible by four; caller bytes consumed/copied before return; aggregate upload at most the maximum buffer size |
| Views | Complete original-format sampled views; no reinterpretation, partial sampled range or attachment view |
| Bindings | One group/set, up to sixteen implemented slots intersected with enabled binding/descriptor limits; uniform ranges, sampled 2D textures and non-comparison samplers |
| Vertex input | At most two streams, per-vertex or divisor-one per-instance, stride divisible by four, 4..256 on native/WebGPU and 4..252 on WebGL 2; up to the queried maximum (at most sixteen) unique float2/3/4 attributes |
| Draw | Triangle list, Index16/Index32, zero base vertex/first instance, configured `Storage.DrawsPerFrame` (default 1024); fixed primitive-restart indices are rejected |
| State | No culling, one color target, one sample; explicit depth comparison/write and premultiplied-alpha blend; defaults retain less-than |

Draws use canonical clip depth [0,1], Y-up geometry and texture UV (0,0) at
the first uploaded row. Vulkan uses a negative-height viewport; the GLSL ES
artifact converts clip depth. The frame's private depth attachment clears to
one. Pipelines remain valid on resize because the session target format stays
fixed. [R2 lifetime services](#lifetime-services-r2) add retained batches and
bounded buffer upload/readback rings; [R3](#ordered-raster-graphs-r3) adds offscreen
raster passes and [R4](#buffer-compute-and-indirect-rendering-r4) adds buffer compute.
[R8](#mutable-transfers-and-mip-initialization-r8) adds completion-gated buffer
updates and bounded RGBA8 texture update/copy/readback. Mip generation and storage
textures remain extensions.
Initial Vulkan texture uploads normally use a synchronous setup copy. A timed-out
copy remains Pending and retains staging until its fence signals; poll GetStatus.
This is not a
streaming upload strategy or a performance claim.

### Shader generation and binding snapshots

```cmake
ludus_compile_shader(TARGET renderer NAME mesh RASTER
    SOURCE shaders/mesh.slang VERTEX vertexMain FRAGMENT fragmentMain)
```

The RASTER option emits `RasterShaderDescription`, separate per-stage artifacts,
and a manifest with per-target bindings, vertex inputs, occupied uniform sizes
and member offsets. Tool acquisition and pinning follow the shader workflow
above. Vulkan/Metal layouts are independent: match CPU packing to the selected
artifact and verify offsets/size/alignment. Hand-authored artifacts must supply
independently verified target metadata. Metal pipeline reflection and the actual
linked WebGL program independently reject incompatible used resources/inputs.

Browser generation verifies emitted WGSL and GLSL ES uniform member offsets and
sizes, rather than accepting equal total sizes alone. The first browser parser
accepts flat float/vector uniform members and rejects unsupported shapes.
It compiles WGSL stages separately because pinned Slang 2026.1.2's combined
reflection omits per-stage resource-usage flags. The tool matches emitted named
float inputs to stage reflection and explicitly normalizes their locations;
inter-stage outputs and builtins are not rewritten. GLSL ES varyings are named
from their independently verified SPIR-V location map so ES 3.00 can link stages
by identifier. Mismatched interfaces reject generation. A texture paired with
multiple samplers in one stage is outside this first GLSL profile.

Create a binding layout covering both shader stages, using the union of their
requirements and visibility. `CreateBindingSet` requires exactly one entry per
layout binding, with correct kinds, uniform roles, offsets and accessible sizes.
The snapshot retains its layout and referenced buffers/views/samplers; a view
retains its texture. A pipeline retains stages and layout independently of
binding contents. `DrawIndexed` requires the exact layout identity, not just
structural similarity, and a declared vertex slice for each stream. It checks
every mesh-local index against VertexCount, accessible vertex/instance records,
index bounds and completion capacity before issuing native work.

Create resources outside frames, then set a target, BeginFrame, DrawIndexed and
EndFrame. Check every result. Portable draws and DrawFullscreen cannot share
one frame. The complete public-only example is
[`tests/sdk_consumer/raster.cpp`](../../tests/sdk_consumer/raster.cpp), with its
[application-owned shader](../../tests/sdk_consumer/shaders/raster.slang).

### Destruction and validation evidence

Typed Destroy takes the handle by reference, invalidates public resolution and
zeros that variable. Other copies become stale immediately. Physical release
waits for immutable dependencies, the current frame's CPU leases, successful
GPU completion, and pending backend validation scopes. Resource generations,
callback request IDs and ordered submission ordinals never wrap/reappear.
A capacity failure can therefore occur after public destruction while old work
is still live. Shutdown/loss invalidates all records and drains/drops the native
device; late browser callbacks cannot affect the next session.

Vulkan general allocations use the private MIT-licensed
[Vulkan Memory Allocator 3.3.0](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/releases/tag/v3.3.0),
pinned with header/license hashes in `config/vulkan_memory_allocator.json`.
VMA handles mapped allocation flush alignment. The existing ordered queue's
fences establish resource retirement; Metal uses command-buffer completion,
WebGPU queue work-done callbacks, and WebGL nonblocking sync polling.
The minimum retirement mechanism accompanies the first general resources;
R2 exposes this same retirement ledger through the services below; dynamic
descriptor arenas and texture streaming remain deferred.

The native RHI pixel fixture draws two textured instances, then a farther,
differently tinted background; it checks texture orientation, instancing and
depth after public resource destruction. Installed shader-helper checks compile
and run the public-only SDK consumer, verify per-target interfaces, dependency
rebuilds and binding rejection. The browser SDK harness
`tests/sdk_consumer/raster-browser-test.mjs` checks pixels on forced WebGPU,
forced WebGL 2, Auto and capability fallback, plus invalid shader/pipeline and
device-loss paths in pinned Chromium. Browser software-GPU results are separate
from physical GPU and hosted-browser acceptance. These checks make no throughput
or allocation-performance claim. See the architecture's
[phase evidence](../architecture/rhi-gdi.md#implementation-phases-and-acceptance).

## Lifetime services (R2)

Include `<ludus/graphics/rhi/lifetime.h>` with a ready device that enabled
`PortableRaster`. These services share R1's registry and dependency graph;
there is no second device or ownership ledger. All calls run on the device's
owner thread, between frames unless a query or tracked `EndFrame` says otherwise.

The first service profile has four command batches, 256 draw packets per batch,
four upload slices and four readback slices independently, 256 KiB per slice,
and sixteen independently owned pipeline requests. These are historical R2 bounds;
R6 configures batch/packet/pipeline-request limits. R8 configures transfer
rings within the initial four-slice, 256 KiB upper bounds. Query
`GetLifetimeCapabilities`; retained/cancelled work counts against capacity.
Records still count against the R1 per-kind limits. `CapacityExceeded` is
retryable after safe retirement, without waiting inside an admission call.

### Record, finish, submit or discard

`BeginCommands` creates a Recording batch. `RecordDraw` validates and copies
one complete draw, retaining geometry, the binding snapshot, pipeline and their
resource dependencies. No backend command is emitted during recording. Destroy
public handles after acceptance if desired: the retained packet uses the same
physical records. `FinishCommands` closes recording once. Submit the Finished
batch to its device/surface or `DiscardCommands` either Recording or Finished
work; discard emits no GPU work.

```cpp
rhi::CommandBatch batch;
rhi::SubmissionToken completion;
// Check each result; these calls require Ready before advancing.
rhi::BeginCommands(device, batch);
rhi::RecordDraw(device, batch, draw);
rhi::FinishCommands(device, batch);
rhi::SubmitCommands(device, surface, batch, completion);
```

The snippet shows call order; the complete checked consumer is
[`tests/sdk_consumer/raster.cpp`](../../tests/sdk_consumer/raster.cpp).
A skipped target returns `NotReady` and preserves the Finished batch for retry.
Preflight or completion-capacity rejection preserves it too. Once encoding
starts, an error may accompany an accepted partial submission; inspect both
status and completion. A successful submission consumes the batch and shifts
its leases to GPU retirement. A later resize acquires the current target without
changing immutable pipeline identity. Shutdown/loss invalidates batches and
releases their physical dependencies at the backend's safe teardown boundary.

`EndFrame(device, completion)` tracks direct and clear-only frames as well.
`GetStatus(device, completion)` is a zero-wait query: `Pending` means accepted
ordered work is outstanding, `Ready` means GPU execution completed. A complete
higher token covers earlier accepted work on the same queue/session. Tokens are
borrowed, require no release, never wrap, and are invalid across device restart.
GPU completion does not prove presentation or pixel correctness.

### Upload and readback rings

`RequestBufferUpload` copies complete immutable initial bytes before return.
The ticket owns a ring slice and destination buffer; the caller can immediately
reuse its byte storage. Poll its status on later owner/event-loop turns, then
`TakeUploadedBuffer` transfers the Ready buffer into a null output.
`Release(ticket)` cancels publication, preserving staging and the private result
until GPU work and creation validation retire. Uniform/vertex/index role and
size/alignment rules still apply. Index16 uploads pad only the private copy to
four bytes; public size and index validation keep the original byte count.

`RequestBufferReadback` retains a Ready buffer and copies a bounded range to a
separate ring. Offsets and sizes must be multiples of four within the original
buffer size. After status becomes Ready, `CopyReadback` copies into caller
storage; it never exposes a native mapping and preserves destination bytes on
Pending/rejection/failure. Release the ticket even after a terminal request
failure. Destroying the source handle before completion is safe. Logical
cancellation does not recycle an in-flight copy or mapping.

Vulkan uses the existing private VMA adapter, cached mapped staging, narrow
transfer/host barriers and zero-timeout fence queries. Direct3D 12 uses the
existing direct queue, upload/default/readback heaps and exact fence queries;
its [D4a port record](../architecture/windows-direct3d12.md#transfers-readback-and-lifetime-d4a)
records the admitted transfer formats, offscreen support and validation scope.
Metal uses shared staging
and submitted blits. WebGPU uses copy buffers, asynchronous validation/work-done
and read mapping; mapping success is separate from queue progress. WebGL 2 uses
type-compatible copy buffers, zero-timeout sync queries and `getBufferSubData`
only after completion. That final browser copy can incur IPC/CPU cost; this
profile promises no driver time budget or throughput improvement. Native
shutdown may wait for submitted work; browser callback cells stay occupied
across shutdown until late callbacks drain safely.
Admission skips those occupied callback cells so the remaining free slices stay
usable after restart.

### Pipeline requests

`RequestPipeline` copies the description and retains immutable shader/layout
incarnations immediately. Equivalent descriptions share one pending/ready/failed
record; comparison includes every active field, canonical attribute locations,
fixed state and device session, with no pointer/padding/hash identity.
`PollLifetime` launches at most one queued pipeline per call. WebGPU uses
asynchronous driver creation; Vulkan, Metal and WebGL create on the context owner
and may occupy that call. There is no portable worker-thread compilation or
persistent driver-cache promise in this slice.

`GetRequestedPipeline` borrows the Ready pipeline while its request is owned.
Do not Destroy that borrowed handle: release the request. Other requests and
accepted batches retain independent leases. The last request removes the cache
entry; callbacks and CPU/GPU work keep physical records until safe retirement.
Cancelling a queued request before launch creates no backend pipeline. Failures
remain explicit; no backend switch or fallback pipeline is inserted.

Reference tests cover finish/discard/submit, destruction at each boundary,
capacity and delayed completion, cancellation, memory/validation failure and
restart. Native pixel tests render from detached retained records; native
transfer tests compare all four buffer roles. The installed SDK consumer checks
uploads/readbacks, shared pipeline requests, completion and final-frame
resource release on forced WebGPU/WebGL 2 and Auto. The browser harness adds
pipeline, transfer and mapping failures and device loss. Software-GPU browser
results remain separate from physical GPU/browser acceptance.

[R8](#mutable-transfers-and-mip-initialization-r8) extends these same rings with
mutable buffers, texture transfers and mip initialization. Descriptor suballocation,
extended pass APIs and a separate GraphicsDevice module remain later extensions.

## Ordered raster graphs (R3)

Include `<ludus/graphics/rhi/graph.h>` and inspect
`DeviceInfo.Enabled.OrderedRasterGraph`. Requiring this feature also requires
portable raster admission. The compiler and direct raster passes use the existing
RHI registry, ownership leases and ordered submission tokens. All calls run on
the owner thread; setup, compilation and execution start between frames.

R6 makes graph, resource, pass, declaration, draw and transient-pool ceilings
configurable through `DeviceDescription.Storage`. The historical first profile
was four graphs, sixteen logical resources per graph, thirty-two
ordered raster passes, sixteen declarations per pass, 256 retained draws per
graph, and 512 semantic dependency report entries. Capacity rejection occurs
before frame acquisition. The historical R3 attachment profile was one color and
private same-size depth, one mip/layer/sample. [R10](#attachment-extensions-r10)
extends it with explicit color views, stencil, private aspect loads and color
resolves, while retaining conservative whole-allocation hazards. R4 adds buffer
compute; general copy passes and physical heap aliasing remain deferred.
[R5](#measured-pass-merging-r5) adds explicit opt-in merging of compatible
consecutive raster passes.

### Author, compile and execute

Create an `OrderedGraph`, import each physical buffer/texture once, reserve new
attachment versions with `NextGraphVersion`, then append `GraphPassDescription`
values in intended execution order. Each pass copies its diagnostic name/source,
resource declarations and complete draw packets, retaining their physical
records. It does not retain callbacks or caller arrays. Declarations must cover
vertex/index slices and every immutable binding entry, including unused extra
bindings that contribute to WebGPU's rendering scope. Samplers are retained by
binding snapshots and have no memory-content hazard declaration.

Buffer declarations carry byte ranges and roles. Reports explicitly identify
conservative whole-buffer hazard tracking; the implementation does not claim
subrange scheduling. Textures declare their complete color image. A pass cannot
sample its own color attachment or declare conflicting uses of one physical
resource. Clear establishes full initialization; Load requires defined prior
contents; Discard followed by partial draws does not establish initialization.
Store Discard invalidates contents. The acquired surface begins cleared by the
existing frame path and must finish with defined stored color for presentation.
Depth Clear uses explicit `ClearDepth` in [0,1], defaulting to one; Depth Load
requires a stored prior depth image. A read-only
depth pass requires Load and rejects pipelines that write depth. `DepthWrite=false`
allows a depth-testing pipeline in that pass.

L1 adds `RasterDepthCompare` (Less, LessEqual, Greater, GreaterEqual and Always).
Pipeline requests compare that field explicitly, so reverse-Z and conventional
variants cannot alias. Existing R1 descriptions keep Less/clear one. Pass rectangles
use top-left physical pixels and half-open edges. An explicit viewport must be nonempty
and fit the actual attachment; scissors intersect drawable bounds and an empty
intersection encodes no draw. Disabling either rectangle selects the complete actual
attachment. Clears are whole-attachment regardless of rectangles, preserving R3's
initialization contract. Use separate offscreen targets and Load composition for
independently cleared split views; see [L1 usage](../architecture/renderer-systems.md#views-and-depth-l1).

Offscreen viewport bounds validate during setup. Surface bounds validate against the
actual acquired extent before switching direct passes; rejection preserves the current
pass. A frozen graph can encounter a changed surface extent only after acquisition:
an invalid surface viewport consumes that graph and returns InvalidDescription with
any accepted clear-only completion published separately. It does not fault the device.
`GetTextureDescription` copies an owned texture's immutable descriptor without native
objects; `IsNull(TextureHandle)` identifies the surface selector, not resource readiness.

Versions identify contents, not new storage. Reads must name the current produced
version at that authored position. A later producer, an overwritten version,
a missing declaration or an undefined load/root fails compilation. In-place
writes produce RAW, WAR and WAW dependencies against the same physical image.
No pass is silently reordered. Present/surface passes, explicit side effects,
and Export/History/Readback roots retain observable work; future-frame history
writes are roots even when this frame never reads them. Culling follows content
dependencies rather than retaining overwritten work solely for WAW/WAR edges.
Reference compilation disables culling while retaining the same validation.

`CompileOrderedGraph` returns totals and copied diagnostic prefixes in `GraphReport`,
including names, source locations, culling flags, first/last live uses, and
RAW/WAR/WAW dependencies. Iterate the prefix arrays with `ReportedPasses`,
`ReportedResources` and `ReportedDependencies`; full counts may exceed those
arrays. Use the indexed diagnostic queries for the complete compiled plan.
`GRAPH_EXTERNAL_INDEX` denotes external boundaries, unused resources and
inapplicable error/group indices. No GPU work
occurs during compilation. Successful compilation freezes setup. Discard a
rejected plan and rebuild if its declarations need changing.

`ExecuteOrderedGraph` rechecks frozen import revisions, packet readiness,
completion capacity and backend pass preparation before acquiring the current
surface. Target skip and preflight failure preserve graph/token for retry. Once
encoding begins, the graph is consumed even on failure; partial native/browser
work faults the session rather than claiming rollback. Accepted execution
publishes an R2 completion token and commits texture validity/use into the same
registry used by direct drawing. Discarding an unexecuted graph drops CPU leases
without GPU work. Shutdown/loss invalidates every graph incarnation.

The fully checked packet authoring is
[`tests/sdk_consumer/ordered_frame.h`](../../tests/sdk_consumer/ordered_frame.h),
used by both native pixels and the installed-SDK consumer. It compiles an unused
color pass, a depth-tested instanced scene, a Load pass with read-only depth,
and a scene-texture composite to presentation. Pipeline `Target` selects the
surface format or a matching RGBA8/RGBA16F offscreen format; dimensions are dynamic.

### Imports, history and transient objects

`GetTextureState` returns the registry's exact accepted-content snapshot:
non-wrapping revision, last submission dependency, semantic use, and color/depth
validity. `ImportGraphTexture` requires that snapshot and a final semantic use.
The compiler and executor reject stale snapshots, including changes made by
accepted direct passes or sampled direct draws. Pending same-queue dependencies
are ordered by GPU commands and barriers, without a CPU completion wait.
History uses persistent owned attachment textures, roots their produced version,
and imports the resulting snapshot on the next execution. The caller decides
which persistent image is the history source/destination; there is no automatic
frame-index rotation. Readback roots preserve a consumer's data; scheduling and
copy-out remain separate lifetime services. R8 adds bounded texture transfers and
readback between frames; see [mutable transfers](#mutable-transfers-and-mip-initialization-r8).

`TextureDescription.Attachment=true` allocates undefined color/private depth
with an empty upload. Direct `BeginRasterPass` switches attachments inside an
acquired frame with the same load/store checks. Direct frame end makes written
attachments sampleable for subsequent draws and records that final use. R2
command batches remain surface-only; offscreen packet sequences use R3 graphs
or explicit direct passes. Direct draws and R2 batches require a texture's last
committed/draft sampled use to cover its binding visibility. Choose `SampledBoth`
when exporting to arbitrary direct bindings; incompatible final color or narrower
sampled use returns `InvalidState` before a draw/batch submission. Graph execution
can transition these imports according to its declarations.

`CreateGraphTexture` acquires from a configured whole-description pool (default eight objects) during
setup, before compilation. This early bounded acquisition lets callers build
ordinary immutable texture views/binding snapshots before graph preflight; it
is not interval-based physical aliasing. Every graph resource stays physically
distinct through execution. Pool reuse requires all graph/view/binding/packet
leases to end, validation callbacks to drain, and actual GPU completion. Idle
objects may be evicted when descriptions change, such as resize. Fresh/reused
contents start semantically undefined. The returned texture handle is borrowed:
Destroy rejects it, graph consumption/discard invalidates it, and a later lease
gets a new generation. Destroy owned views/binding sets when no longer needed
or they keep the pool object retained. Persistent imports cannot borrow another
graph's pool image; transient resources cannot escape through roots.

### Backend lowering and acceptance

Vulkan retains the legacy 1.1 render-pass path, privately cached compatible pass
objects and VMA attachment allocation. One mapper translates semantic texture
uses to color-output, shader-stage and depth-test barriers/layout transitions;
there are no ALL_COMMANDS barriers or newer feature requirements. WebGPU closes
rendering scopes before sampling an earlier attachment, uses creation flags and
private depth views, and relies on ordered queue validation/completion rather
than exposing fake barriers. Metal uses separate render encoders with explicit
load/store state. WebGL 2 uses owned FBOs, depth renderbuffers and owner-context
commands. Its portable top-left texture convention requires a private flipped
color blit at stored attachment boundaries; this copy cost is deliberate and
has no performance-improvement claim. Discard still remains semantically
undefined when a browser backend security-clears its storage.

Reference regressions cover packet retention, missing uses, forward/stale
versions, feedback, undefined loads/stores/roots, RAW/WAR/WAW reports, history
roots, direct/graph revision conflicts, retry/partial failure, identity/capacity
and pool retirement across GPU completion and view leases. Native pixel tests
exercise offscreen scene/UI/composite on Metal and Vulkan. The installed browser
SDK harness adds forced WebGPU/WebGL graph/reference variants, validates pixels,
compares complete optimized/reference images, and renders 120 frames. It retains
all R1/R2 failure, fallback and transfer scenarios. Pinned Chromium software-GPU
acceptance remains separate from physical-GPU/hosted-browser acceptance.

## Buffer compute and indirect rendering (R4)

Include `<ludus/graphics/rhi/compute.h>` for compute pipelines and limits, and
`<ludus/graphics/rhi/graph.h>` to execute dispatches. Vulkan, Metal and WebGPU
expose `DeviceInfo.Enabled.Compute` and `IndirectRendering` only when their
negotiated buffer profile is available. Vulkan requires compute support on its
existing graphics queue. WebGL 2 exposes neither. Requiring either feature
checks every Auto attempt before Ready; forced WebGL 2 fails before startup.
Preferences allow an application to select its own CPU/direct-draw variant after
Ready. The engine does not translate or emulate a compute kernel on WebGL 2.

### Buffers, kernels and limits

`BufferRole::Storage`, `StorageVertex` and `StorageIndirect` allocate buffers
with complete initial bytes. The latter two also support vertex/instance input
or indexed indirect arguments, respectively. Their role never changes. Initial
sizes and storage binding lengths are multiples of four; binding offsets obey
`GetComputeCapabilities().StorageOffsetAlignment`. Storage allocations obey `Raster.MaxBufferSize`; each bound range independently
obeys `Compute.MaxStorageRange`. Layouts obey `Compute.MaxStorageBindings`,
`Interface.MaxBindings` and each enabled per-stage descriptor count. Uniform buffers can also be read by
compute. Compute storage entries use `StorageRead` or `StorageReadWrite`,
`RasterVisibility::Compute`, and a reflected minimum element stride. Read-only
`StorageRead` also admits `RasterVisibility::Fragment` on the compute/storage
profile (Metal, Vulkan and WebGPU). Ordered raster passes declare that exact
fragment read and receive compute-write-to-fragment-read synchronization. Vertex
storage and raster storage writes remain unsupported; WebGL has no storage
profile. The renderer's GPU image recipe uses fragment reads to present an image
with constant draw geometry.
Custom raster programs using this profile pass `RASTER FRAGMENT_STORAGE` to
`ludus_compile_shader`; it emits native/WebGPU artifacts and omits the unsupported
WebGL artifact. Built-in recipes declare this requirement in their manifest. Storage textures remain deferred; existing buffer
updates follow the completion and lease rules of R8.

Compute WGSL uniform verification also admits signed/unsigned 32-bit scalars
and vectors, plus bounded fixed vec4 integer arrays. Their emitted alignment,
occupied range and named offsets must match that target's independent reflection.
Integer fields use the existing opaque public uniform shape; they do not acquire
a float material schema. Raster/WebGL uniform admission retains its float-only
rules. This distinction avoids reinterpreting counters/random keys as floats.

The installed shader helper accepts one compute entry:

```cmake
ludus_compile_shader(TARGET my_renderer NAME cull
    SOURCE shaders/cull.slang COMPUTE computeMain)
```

The generated `ludus::shaders::cull::Compute()` description selects the native
MSL/SPIR-V or browser WGSL artifact, with target-specific reflection and verified
workgroup dimensions. No GLSL ES compute artifact is generated. The bounded
helper accepts structured-buffer scalar32, vector2 or vector4 elements
(`float32`, `uint32`, `int32`); arbitrary structures, resource arrays and
specialization-controlled local sizes are outside this helper profile.
`CreateRasterShader` accepts its Compute stage, then `CreateComputePipeline`
retains that shader and its exact group-zero layout. Poll Pending creation and
check every result, as with raster resources. Pipeline creation, binding
snapshots and destruction share the existing RHI registry and R2 leases.

Local dimensions are nonzero, at most 256/256/64 along x/y/z, and at most 256
invocations in total, further bounded by enabled device limits. Dispatch counts
are nonzero and at most 65535 on each axis, again clamped to enabled limits.
`GetComputeCapabilities` is authoritative. The renderer checks these limits and
reflection; bounds checks, shader-local synchronization and race freedom are
kernel obligations. There is no indirect dispatch or separate asynchronous queue.

### Ordered dispatches and indirect commands

Set `GraphPassDescription.Dispatch` to one `ComputeDispatch`; omit raster draws
and attachments. The graph copies and retains the packet. Multiple dependent
dispatches need separate authored passes. Declare every buffer binding, including
unused extra entries, with matching byte range and Compute visibility. Read-only
storage names the current version. Read/write storage reserves the next version
with `NextGraphVersion`; it also depends on the preceding contents, preserving
bytes outside the declared write range. Complete creation bytes establish initial
contents. Whole-buffer RAW/WAR/WAW tracking remains conservative; there is no
subrange scheduling or physical aliasing. Export/History/Readback buffer roots
preserve observable compute work. Frozen imports reject accepted writes from
another graph, and successful submissions advance the shared content revision.

An indirect `RasterDraw` supplies a ready `StorageIndirect` buffer and a
four-byte-aligned `IndirectOffset` containing `IndexedIndirectArguments`.
Declare those twenty bytes with `GraphAccessMode::Indirect`. The same packet
still supplies mesh/index/instance slices and nonzero maximum `IndexCount` and
`InstanceCount`, which are validated before execution. The producing kernel
must keep actual counts within those maxima and write zero `FirstIndex`,
`BaseVertex` and `FirstInstance`; packet `IndexOffset` selects the mesh. Zero
actual instance count culls the draw. The GPU arguments are not read back on the
rendering hot path, so kernel correctness is part of this contract. The profile
issues one indexed indirect command per packet, without indirect count buffers.

Vulkan lowers declared buffer uses to compute shader, vertex input, uniform
shader or draw-indirect access/stage barriers on the existing queue. It retains
writer visibility across multiple readers and subsequent R2 readback/copies.
Metal uses tracked resources and separate compute/render encoders; WebGPU uses
storage/vertex/indirect creation flags and ordered compute/render usage scopes.
Neither exposes invented native barriers. Completion, deferred destruction,
preflight retry and partial-encoding failure follow R2/R3.

### Acceptance fixture

The shared [`ordered_frame.h`](../../tests/sdk_consumer/ordered_frame.h) fixture
has an authored CPU culling function and the
[`cull.slang`](../../tests/sdk_consumer/shaders/cull.slang) GPU variant. A single
invocation compacts four candidates into two visible instances and writes one
indexed indirect command. GPU output buffers start at zero. Native tests read
back the generated command through R2 and compare the complete rendered image
against the CPU/direct variant. The installed browser consumer also reads back
the command. Its harness compares compute/reference images to the CPU oracle,
checks WebGL 2 and Auto fallback, rejects required compute on WebGL 2 before any
frame, and preserves the existing readiness/loss/transfer scenarios over 120 frames.

Reference tests cover negotiated failure, layouts/local/dispatch bounds, missing
uses, mutable-buffer RAW/WAR/WAW and history roots, culling, frozen revisions,
retry, retention through completion and failure after encoding begins. Metal and
pinned Chromium software-GPU checks run locally; Linux Vulkan, formal pinned
SPIR-V validation and supported-host sanitizers are CI gates. Browser software
GPU evidence remains separate from physical GPU/browser qualification. This
fixture establishes correctness; it makes no performance improvement claim.


## Measured pass merging (R5)

`CompileOrderedGraph(device, graph, report, GraphCompileOptions{true, true})`
opts into consecutive raster-pass merging. The first option controls culling,
independently of merging. The existing `bool reference` overload preserves its
R3/R4 behavior: culling is disabled in reference mode and merging stays disabled.
Use `{true, false}` to compare the same live workload without merging, or
`{false, false}` to inspect every legal authored pass. Validation, owner-thread
use, frozen import revisions, retry, failure and completion contracts remain
those of [ordered graphs](#ordered-raster-graphs-r3).

The current rule requires consecutive authored, live raster passes with the same
physical color/depth attachment. Both must Store color and depth; the later pass
must Load both, with matching depth-read-only mode, viewport and scissor. Read
use lists must match field by field, including order, physical resource, version,
range and visibility; only the validated color output version may advance.
Different pipelines/bindings/draw packets remain legal and execute in authored
order. Compute, declared side effects, a culled gap, clears, discards, changed
read scopes or rectangles prevent merging. No barrier executes inside a group:
its identical external read scope is synchronized before its first pass. The
last store happens at group end. Every logical pass still updates the registry's
color/depth/version ledger and retains its packets through R2 completion.

This adapts ordinary attachment ordering from Khronos's
[Vulkan Specification, Fragment Operations](https://docs.vulkan.org/spec/latest/chapters/fragops.html)
and the W3C/GPUWeb [resource usage rules](https://www.w3.org/TR/webgpu/#resource-usages).
Thanks to their authors for specifying the ordering and usage-scope constraints.
It adds no subpasses, attachment feedback, query scope, aliasing or queue overlap.
Vulkan/Metal/WebGPU reduce render-pass/encoder starts. WebGL2 also avoids repeated
framebuffer setup and intermediate flipped sampling blits; final sampling still
uses the established texture-origin conversion.

`GraphReport` separates authored/live passes from `RasterPasses`, `ComputePasses`,
`MergedPasses` and `DrawCount`. `RasterGroup` maps each live raster pass to its first
authored group member; `GRAPH_EXTERNAL_INDEX` denotes compute/culled entries. Dependencies, names,
setup locations and logical lifetimes remain intact. Counts describe a successful
plan, excluding the facade's acquisition clear, final barriers, driver-internal
commands and presentation. A failed compile publishes no group/cost estimates.

`PlanStorageBytes` reports the graph control record and its owned payload capacities,
including unused reserved capacity; shared table-page slack and compile scratch
are accounted by `GetStorageUsage` instead. `BufferBytes` and `TextureBytes` sum all imported logical payloads,
including unused imports, initialized mip levels and private 32-bit attachment
depth. They exclude surface storage, native padding, staging, other graphs,
retired resources and unused pool objects. These values are not physical backing
or memory high-water measurements. Compilation and grouping use bounded storage;
no additional engine heap allocation occurs for grouping. Driver/browser
allocation counts remain unavailable in the current harness.

### Paired native measurements

Build the native test target with the pinned toolchain and shader tools, then run:

```bash
python scripts/benchmark-rhi \
  --executable out/build/<preset>/modules/graphics/rhi/ludus_rhi_render_tests \
  --backend metal --device '<actual GPU, OS and driver>' \
  --output out/rhi-benchmark --runs 3
```

Use `--backend vulkan` on the native Vulkan host. Record actual device identity;
software Vulkan evidence must be labelled as such. The harness compares one
binary with merging on/off, alternates which mode runs first, warms 16 pairs per
workload and records 128 pairs for each of 1/8/16/31 layers. Every completed RGBA
image must match before a result is accepted; independent red/blue pixels verify
painter order and reject identically empty results. It writes raw sample logs and JSON
with p50/p95/p99 CPU distributions, executable hash, revision, dirty state, host,
pass/draw counts and logical memory. It requires a complete sample set and fails
on an image or execution failure. Timing thresholds are not CI correctness gates.

The shared SDK fixture draws a textured/instanced scene, painter-ordered overlays
with differing hues/alpha and immutable binding snapshots, then samples the attachment into the
surface. Setup CPU time includes authoring/compile; execute CPU time includes
preflight/acquire/encode/submit. The subsequent full-image readback forces real
completion outside those timers. Its host wait duration is neither GPU duration
nor input-to-display latency. The original R5 harness leaves GPU timestamps,
presentation latency and physical backing unavailable (`null`), never zero.
[R7 measurement](#gpu-measurement-and-qualification-r7) adds optional device-clock
intervals and scoped native API allocation observations.

Initial evidence, 2026-10-11: base `f9cc7c8` plus this R5 implementation, pinned
Clang 18.1.8, Development/RelWithDebInfo, no PCH, Apple M5 (8 GPU cores), Metal 4,
macOS 26.6 (25G72), headless 96x64. Three runs passed 3,072 measured submissions
and every paired full-image comparison. Generated results live in
`out/r5-painter-measurements/report.json` and `run-*.log` in the task worktree.

| Layers | Raster scopes off/on | Median execute CPU off/on (µs, range across three runs) | p95 execute CPU off/on (µs, range) |
| --- | --- | --- | --- |
| 1 | 2 / 2 | 23.9–25.9 / 23.7–26.0 | 27.5–94.7 / 28.1–82.2 |
| 8 | 9 / 2 | 47.4–65.9 / 22.2–31.5 | 51.1–107.6 / 26.1–91.0 |
| 16 | 17 / 2 | 85.0–105.6 / 27.6–33.0 | 95.6–208.2 / 35.0–54.2 |
| 31 | 32 / 2 | 209.2–272.0 / 42.0–58.8 | 245.0–405.6 / 52.8–89.2 |

Setup medians remained approximately 2.4–2.9, 6.3–8.8, 13.0–15.3 and 32.0–42.8 µs
across the corresponding paired workloads. Both modes used 82,704 bytes of fixed
plan-slot storage, 308 logical buffer bytes and 49,168 color/depth bytes; neither
mode changed draw count or frame buffering. Host load produced visible tail variability. The single-layer control had no
consistent benefit. **Decision: adopt only as an explicit opt-in** for compatible
pass-heavy workloads. These tiny headless scenes establish an explainable CPU
benefit, not a universal renderer/GPU/latency budget. Measure the target consumer
before enabling it. Parallel recording, async queues, bindless services and native
heap aliasing remain deferred pending their own workloads and evidence.

Unit regressions cover boundary mismatches, retained detached packets, versioned
attachments, undefined discarded roots, skipped acquisition/retry and GPU-gated
retirement. Native tests compare complete merged/unmerged images at all four
sizes; the installed browser consumer compares `merge`/`merge-reference` on forced
WebGPU/WebGL2 and tests Auto fallback. Existing fullscreen, raster, multipass,
compute/CPU, upload/readback, resize and loss suites remain acceptance regressions;
those cases have no new speedup claim. Physical/hosted browser, Vulkan/Wayland and
presentation qualification are separate from software Chromium and headless Metal.


## Scalable resource and plan storage (R6)

Set `DeviceDescription.Storage` before `CreateDevice`. The ready
`DeviceInfo.Profile.Storage` copies those maxima and byte budgets; changing the
original description cannot change an active device. Counts must be nonzero and
below `UINT32_MAX`, and both byte budgets must be nonzero. These are admission
ceilings, not eager reservations or promises that every combination fits memory.
Query enabled raster limits as well as the copied storage profile.

```cpp
rhi::DeviceDescription description;
description.Storage.ResourcesPerKind = 128;
description.Storage.ResourcesPerGraph = 160;
description.Storage.PassesPerGraph = 64;
description.Storage.DrawsPerPlan = 1536;
description.Storage.DrawsPerFrame = 2048;
```

The registry and Vulkan/Metal/WebGPU/WebGL resource tables grow in stable pages.
Batch and graph payloads grow during setup, and compilation allocates its scratch
and full diagnostics before execution. Frame admission prepares completion
records before acquiring the target. Vulkan also prepares its configured
`RasterPassStates` metadata cache before a frame; the default retains the
historical 192 format/load/store/depth combinations. Other backends encode that
state directly. Execution cannot grow engine storage.
Whole-batch/graph frame-limit rejection happens before native draw work and
preserves the plan for retry or discard. General limits do not enlarge the
fullscreen compatibility facade or fixed upload/readback staging rings. Optional
RT0 records now use the same stable-page host accounting with independent
`StorageLimits::RayResourcesPerKind`; `RayInstances` requests the static TLAS
instance ceiling reported by `RayCapabilities::MaxInstances`. Instance identity
representation/device bounds and per-resource ray byte budgets remain enforced.
Ray allocation telemetry retains `GetRayAllocation`/`GetRayResourceStats`;
structure/buffer/scratch bytes stay in those categories rather than general
`ResourceBytes`. See [renderer L5.1](../architecture/renderer-systems.md#l51-budgeted-scene-capacity).

`GetStorageUsage` copies current and peak accounting without allocating or
waiting. `HostBytes` includes registry/backend metadata, optional retained CPU texture copies, owned packet/use/report
arrays, compiler scratch, index shadows, page slack and overlapping allocations
while growing. `ResourceBytes` counts logical buffer bytes and initialized texture
mip extents, including private 32-bit depth for attachments. RGBA16F color counts
eight bytes per texel. Pending creation, detached dependencies and submitted uses
retain the charge until physical retirement. These counters exclude allocator
headers, native driver padding/metadata, facade targets and transfer staging; they
are not physical or resident GPU-memory measurements (R7).

Resource/plan operations report `CapacityExceeded` for capacity/budget pressure
and `OutOfMemory` for allocation failure. Direct frame admission reports
`DeviceStatus::Failed` when metadata preparation cannot fit/allocate, before
acquiring the target; the ready device remains available for retry. Startup
reports failure if a new host budget is below already committed metadata. Failed creation preserves caller outputs and refunds logical
resource reservations. Setup can retain successfully allocated spare capacity
when a later allocation fails; that capacity stays charged and reusable. Stable
metadata pages are retained across shutdown/restart so late browser callback
addresses remain valid. A new device cannot choose a host budget below those
committed pages. Peaks describe the process storage epoch, rather than resetting
on each device. Shutdown/loss still releases native objects and invalidates old
handles, plans and completion tokens.

`GraphReport` keeps bounded copied prefixes for compatibility; `PassCount`,
`ResourceCount` and `DependencyCount` cover the complete plan. After a compile attempt, call `GetGraphPassDiagnostic`,
`GetGraphResourceDiagnostic` and `GetGraphDependency` with authored/indexed
positions. Queries copy into caller-owned output, allocate nothing, and remain
valid until graph mutation, discard or consumed execution. Missing diagnostics,
invalid indices or stale handles preserve output.

The [public SDK fixture](../../tests/sdk_consumer/r6.cpp) creates at least 72
objects of every supported general resource kind, executes 405 prepared draws
across 40 graph passes, and submits a separate 1,100-draw batch. Browser tests
compare complete graph/direct images on forced WebGPU, forced WebGL 2 and both
Auto paths. The native target `ludus_rhi_r6_consumer` exercises the same public API.
Reference-backend tests additionally cover all nine kinds at 80 objects each,
retained slot reuse, pending byte pressure, allocation rollback, checked host
arithmetic, high-index WAR hazards and more than 512 diagnostic dependencies.
These fixtures prove capacity/correctness; hardware performance remains R7.


## GPU measurement and qualification (R7)

`measurement.h` adds optional frame probes, scoped allocation/retirement snapshots
and selected-adapter observations. The public declarations own the API contract.
Enable `DeviceDescription::EnableGpuTiming` before startup, then arm between frames:

```cpp
#include <ludus/graphics/rhi/measurement.h>

// device/surface were created with EnableGpuTiming = true.
const auto armed = ArmGpuTiming(device);
if (armed == RasterStatus::Ready)
{
    // Submit an ordinary frame, command batch, or ordered graph.
    // Later, on the owner thread, poll without waiting:
    GpuTiming sample;
    if (GetGpuTiming(device, sample) == RasterStatus::Ready &&
        sample.State == GpuTimingState::Available)
    {
        const auto elapsedNanoseconds = sample.DurationNanoseconds;
        // Record elapsedNanoseconds alongside a separate CPU sample.
    }
}
```

There is one optional probe per session, independent of general resource/plan
capacity. A skipped acquire keeps it armed; a recording frame also remains armed
until submission. An armed/pending sample cannot be overwritten. Terminal samples
persist until re-armed; destroying the device cancels them. Native teardown uses
the existing drain before releasing query storage. WebGPU map callbacks carry a
non-reused generation, so late callbacks after loss/destruction cannot publish into
a new probe. Timing-disabled sessions issue no timing commands.

The observed interval encloses the acquired frame's clear, graph pass transitions,
raster and compute commands. It is an elapsed device-clock interval, including GPU
stalls; WebGL can include idle gaps while the CPU feeds commands. It excludes the
later readback and display journey and does not measure GPU busy-cycle totals.
CPU acquire/encode/submit, queue completion, GPU elapsed time and presentation
completion remain separate observations. No result correlates device and CPU clock
epochs or substitutes CPU elapsed time when a GPU result is unavailable.

| Backend | Timing implementation | Precision and unavailable results |
| --- | --- | --- |
| Metal | Retained command buffer; `GPUStartTime`/`GPUEndTime` after successful completion | API exposes seconds, but no precision guarantee; tick/valid-bit observations remain unknown. Invalid timestamps and command errors produce explicit terminal states. |
| Vulkan | Two timestamps enclosing the graphics command buffer, nonblocking availability polling | Queue `timestampValidBits` and device `timestampPeriod` are reported. Modular rollover is allowed only when the independent CPU elapsed bound is shorter than one counter period; ambiguous wraps are disjoint. A CPU bound validates the counter, never supplies the GPU duration. |
| WebGPU | Optional `timestamp-query`, requested only when supported and opted in; two empty compute marker passes and asynchronous resolve/readback | Four valid indices work around the pinned emdawn bridge's handling of absent indices. Indices 0/3 enclose every frame encoder. Privacy quantization/accuracy is unknown; a genuine quantized zero remains available. Unsupported timing does not fabricate a duration. |
| WebGL2 | Optional `EXT_disjoint_timer_query_webgl2` elapsed query | Query availability is polled after returning to the event loop. `GPU_DISJOINT_EXT`, unsafe JavaScript integer results and context loss invalidate the result. Counter bits are reported; tick/precision are unknown. |

Thanks to Apple for [command-buffer timing](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime)
and [resource allocation sizes](https://developer.apple.com/documentation/metal/mtlresource/allocatedsize),
Khronos for [Vulkan timestamp queries](https://docs.vulkan.org/spec/latest/chapters/queries.html)
and the [WebGL timer-query extension, revision 4](https://registry.khronos.org/webgl/extensions/EXT_disjoint_timer_query_webgl2/),
and W3C/GPUWeb for [pass timestamps and query resolution](https://www.w3.org/TR/webgpu/).
The implementation adapts their lifecycle and availability contracts; it does not
copy their sample code or assume common CPU/GPU epochs.

### Memory observations and reporting cost

`GetStorageUsage` retains R6's host/logical high-water counters.
`GetAllocationTelemetry` counts general raster/compute buffer and texture logical
bytes through deferred retirement. Its resource API bytes use Metal `allocatedSize`
or Vulkan VMA allocation spans, including private depth and retained texture-upload
backing. Detached resources remain charged until dependencies and GPU completion
permit physical release. Views/bindings do not double-count shared backing.

Vulkan also reports actual `vkAllocateMemory` block bytes in the raster allocator,
including pool slack, transfer rings and ray allocations sharing that allocator.
That broader scope is explicitly separate from general resource binding spans;
it excludes the facade and swapchain. Thanks to AMD GPUOpen for
[Vulkan Memory Allocator 3.3.0 statistics](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/statistics.html),
which distinguish block bytes from allocation bytes. Metal has no corresponding
allocator-block observation. WebGPU/WebGL texture backing is opaque, so complete
resource API byte totals are unavailable on those backends. No partial sum becomes
a complete total. Physical residency remains unavailable everywhere; API allocations,
VMA block bytes and unified-memory allocations must never be relabelled as physical
VRAM/residency. General scope excludes facade/surface storage, transfer rings and
ray objects; use the broader Vulkan block field or the existing ray contract for
those separately identified scopes.

Peaks of API spans and allocator blocks are **observed peaks at query calls**;
intervening allocation spikes can be missed. Sample at meaningful setup, submission,
retirement and resize boundaries. The R6 logical/host peaks are continuously tracked
by their owning counters. Reporting traverses current resource records and, on
Vulkan, VMA statistics; it waits for no GPU and allocates no engine storage, but is
not a constant-time hot-path operation. Timing setup/polling and extra marker/resolve
commands also cost work. The paired harness records arm and poll/report CPU cost and
runs the same workload with instrumentation disabled to expose that cost.

### Named qualification records

```bash
python scripts/benchmark-rhi \
  --executable out/build/<preset>/modules/graphics/rhi/ludus_rhi_render_tests \
  --backend metal --device '<GPU, OS and driver label>' --gpu-timing \
  --qualification-name '<stable hardware-host-workload name>' \
  --output out/rhi-qualification --runs 3
```

The runner preserves raw logs and writes a versioned `report.json` with the actual
selected-adapter observation, caller's host label, host OS, relevant enabled profile
(raster/compute/ordered graph and resource/draw maxima), source revision/dirty state,
executable hash, capture time, 96x64 workload, shader-enabled binary, warmup and
sample identities. Each 1/8/16/31-layer merged/unmerged workload has CPU and available
GPU p50/p95/p99 distributions, explicit GPU state counts, precision observations,
logical/API/allocator memory scopes and sampled peaks. It rejects missing/duplicate
samples, nonfinite values and a duration attached to an unavailable result. Timing
thresholds are evidence, not correctness gates. Every paired complete image and
independent painter-order pixel must pass first. Controls use the same executable,
adapter and workload with timing disabled; their results are retained separately.

A record observes physical GPU identity only when the selected backend identifies
hardware and reports its name. Software or privacy-hidden identities remain open.
Physical residency, input-to-display observations and unexecuted host/surface
journeys stay `null`/`open`. Local localhost software-browser coverage does not
qualify a deployed hosted-browser journey or a physical browser GPU. A destroy/
restart test does not qualify actual hardware loss/recovery.

Initial R7 evidence, 2026-10-12, base `5014834` plus this implementation (dirty tree
recorded): Clang 18.1.8, Development/RelWithDebInfo, no PCH, Apple M5, macOS 26.6
(25G72), headless 96x64. The named `apple-m5-metal-headless-r7` record passed three
runs: 3,072 instrumented samples plus 3,072 control samples, with every paired full
image correct. Resource logical bytes were 98,628; resource API allocation bytes
and observed peak were 197,044. Physical residency, allocator block sizes and display
latency remained unavailable. Raw evidence lives in the task worktree's ignored
`out/r7-qualification/`; the checked-in evidence is this summary and reproducible
fixture/runner.

| Layers | GPU median off/on (µs, range across three runs) | GPU p95 off/on (µs, range) |
| --- | --- | --- |
| 1 | 25.38–52.88 / 25.25–53.96 | 26.00–68.83 / 26.00–69.75 |
| 8 | 66.54–67.75 / 25.04–25.25 | 69.25–73.38 / 25.75–26.62 |
| 16 | 128.38–349.54 / 25.12–27.88 | 367.75–415.46 / 25.75–29.12 |
| 31 | 473.12–544.79 / 28.25–31.00 | 503.46–885.00 / 28.79–36.38 |

Here off/on means **pass merging**, with timing enabled in both modes. Median probe
arm cost was 41–83 ns and combined poll/memory-report cost 375–792 ns; these are CPU
observations on this workload, not universal overhead guarantees. Variability in
unmerged runs is retained rather than filtered out.

| Named journey | Current evidence |
| --- | --- |
| `apple-m5-metal-headless-r7` | Paired physical GPU measurements and scoped memory above; enabled raster/compute/ordered graph, 64 resources per kind and 1024 frame draws. |
| `apple-m5-cocoa-r7` | Real WindowServer/Metal test: zero-size skip, 160→80→96 pixel target resize, acquired dimensions, completed timing, pending teardown, stale owner and device restart. Display latency and hardware-loss recovery remain open. |
| `chromium-140-software-local-r7` | Installed SDK, pinned Emscripten 4.0.23/emdawn and Chromium 140.0.7339.186: WebGPU/WebGL multipass pixels/timing, unsupported optional feature, delayed map callback after device loss, plus R1–R6 conformance. Physical GPU/hosted deployment remain open. |
| `vulkan-physical-wayland-r7` | Open: requires a named physical Vulkan GPU, driver and compositor/session, resize/loss journeys and measurement records. CI software Vulkan correctness is separate. |
| `hosted-browser-physical-r7` | Open: requires a deployed origin, actual browser/OS/GPU observation, canvas resize/device-loss/recovery and CPU/GPU distributions. Privacy-hidden identity/residency/display observations remain open. |

R7's measurement API and qualification workflow are implemented. Physical Vulkan/
Wayland and deployed physical-browser qualification remain explicit acceptance
work; the initial Metal and software-browser results do not close those gates.


## Mutable transfers and mip initialization (R8)

Include `ludus/graphics/rhi/lifetime.h`. The ready portable raster profile
supports existing-buffer updates for every enabled buffer role, and 2D
`Rgba8Unorm` texture updates, GPU copies and readbacks on Vulkan, Metal, WebGPU
and WebGL 2. Other transfer formats return `Unsupported`; R9 extends texture
formats/types/views. `FormatCapabilities::Uses.CopySource` and `CopyDestination`
report the enabled RGBA8Unorm transfer profile and can be required at startup. Allocation descriptions, sizes, formats and roles stay fixed.
Every call uses the device/context owner between frames.

### Mutate, complete and release

`RequestBufferUpdate(device, buffer, offset, bytes, size, ticket)` copies caller
bytes before returning. Offset and size must be multiples of four, nonzero,
and within the original buffer allocation and configured transfer limit.
Index-buffer CPU validation copies change with accepted updates, so later draw
validation checks the updated indices. A pending destination returns `NotReady`
without publishing a ticket. Only `Pending` with a newly owned ticket accepts a
mutation; preflight failures preserve the output and destination.

The conservative mutation rule rejects `InvalidState` while a binding/view,
retained batch, graph, ray input or transfer holds a destination lease, or a
submitted frame's actual GPU completion is outstanding. Release those owners
and poll completion before retrying. Even disjoint byte/mip rectangles share
this whole-resource rule. Retained records and host-visible memory are never
rewritten beneath accepted work. Transfer commands use the existing ordered
queue; no separate transfer queue or automatic resource renaming is promised.
Accepted writes advance the R3 content revision after successful completion.
Graphs import the resulting revision once the transfer has completed and its
ticket is released; retained graphs reject external mutation.

`TransferTicket` owns notification, staging and destination/source leases.
`GetStatus` or `PollLifetime` polls without waiting. The destination stays
`Pending` until actual completion; a terminal transfer failure makes it failed
rather than exposing uncertain contents. Release the ticket after completion
before mutating again. `Release` can cancel notification early, but accepted
GPU writes still execute. Destroying public handles also leaves physical backing
retained until completion. Cancellation is not rollback.

### Texture rectangles and rows

`TextureRegion{mip, x, y, width, height}` selects a nonempty top-oriented rectangle
within one allocated mip. `RequestTextureUpdate` consumes exactly
`rowPitch * height` bytes, with four-byte row alignment and at least `width * 4`
bytes per row. Padding is ignored. The service packs texels into owned,
256-byte-aligned staging rows on every backend; both caller bytes and internal
packed bytes must fit the configured slice. Checked arithmetic rejects
out-of-range coordinates, mip indices, pitches and overflow before native work.
CPU-retained texture content remains immutable; updates/copy destinations with
`RetainCpuContent` return `Unsupported` so ray/reference shading cannot see a
stale shadow. Partial updates preserve the remaining initialized texels.

`RequestTextureCopy` copies equal-sized rectangles of distinct `Rgba8Unorm`
textures entirely on the GPU. Source mips must be initialized. Same-texture
copies reject, including disjoint rectangles; no overlapping-copy semantics are
inferred. It retains the source and consumes an upload-direction request slot,
but does not stream its pixels through caller memory.

`RequestTextureReadback` shares `ReadbackTicket`, `GetStatus`, `CopyReadback`
and `Release` with buffer readback. The source mip must be initialized and the
caller pitch must meet the same row rules. Call between frames on the ready
owner thread with a null ticket. An accepted request returns Pending and publishes
an owned ticket; poll its status and release it once, including on cancellation.
Once Ready, copy-out returns
`rowPitch * height` bytes in top-oriented row order and zeroes row padding.
Pending, undersized output and failures preserve caller bytes. Native mappings
are never exposed. WebGL reads the sampled backing's logical row order and
maintains its flipped render backing when an attachment is mutated, preserving
later `Load` passes.

### Incremental initialized mip residency

Set `TextureDescription::DeferredInitialization = true` and pass an empty
upload to allocate undefined `Rgba8Unorm` sampled mips. Attachments and copied
CPU content cannot use this option. Each undefined mip requires a complete-mip
update/copy: rectangle patches cannot prove that the remaining texels exist.
Only successful GPU completion marks that mip initialized. Initialized mips may
subsequently accept partial updates and readbacks.

`GetTextureResidency` returns allocated mip count, an initialized-mip bit mask
and logical initialized texel bytes. These are content-validity observations,
not physical residency, sparse allocation or an eviction policy. All backing is
charged to the R6 resource budget at creation, including undefined mips.
The initial sampled view spans the complete allocation and rejects until every
mip is initialized. This makes R8b's incremental initialization usable safely
with R8b. R9a's partial sampled views can now expose initialized mips before the
remaining mips are defined. R3 hazards remain conservative whole-resource hazards.

For example, an 8-by-4 texture with three mips begins with mask `0` and no
initialized bytes. Complete mip uploads publish masks `1`, `3`, then `7`, with
128, 160 and 168 logical bytes respectively. A full-resource sampled view is
legal only after mask `7` and transfer completion.

### Staging budgets and qualification

`DeviceDescription::Storage.TransferSlices` configures 1–4 requests per direction.
`TransferBytes` configures a four-byte-aligned 256–262144-byte staging slice;
invalid configuration rejects startup. Defaults preserve R2's four 256 KiB
slices. `GetLifetimeCapabilities` reports the configured values. Upload creation,
mutations and texture copies share upload-direction slots; buffer/texture
readbacks share readback slots. Completed owned tickets and cancelled unfinished
work retain their slots. Backends allocate slices lazily, bounded by twice
`TransferSlices * TransferBytes` per session; the existing fixed CPU packing/
copy-out scratch is separate. Transfer staging remains separate from R6's logical
resource budget. R7 Vulkan allocator-block telemetry includes its allocator
staging; general resource allocation telemetry keeps its documented scope.

Metal retains shared staging until command-buffer completion. Vulkan retains
mapped slices, command buffers and fences, flushes uploads, invalidates readback
and applies whole-resource transfer/host/shader dependencies. WebGPU waits for
validation, queue progress and read mapping independently; late callbacks keep
cells occupied across restart. WebGL uses fenced pixel-pack/unpack buffers and
zero-timeout polls. A post-command GL error closes the session before unsafe
reuse. Native teardown may wait; requests and polls do not issue blocking GPU
waits. Driver calls can still incur host cost.

[R8 implementation, PR 219](https://github.com/The-Ludus-Project/Ludus/pull/219), starts from
merged R7 revision `6828502`. The R8 reference tests cover output preservation, checked ranges/pitches,
initialized sampling, copied source bytes, retained bindings/batches/graphs,
unfinished frame ordinals, copy-source destruction, cancellation, pressure,
terminal failure and loss/restart. The native and installed-SDK `r8.cpp` journey
uses two 4 KiB slices, updates uniform/vertex data, incrementally initializes
three mips, copies a rectangle after which its source is destroyed, checks
padded buffer/texture round trips, and renders/readbacks the resulting four-color
image. Browser conformance compares complete images on forced WebGPU, forced
WebGL 2, Auto and authored fallback. Pinned CI additionally runs Vulkan validation,
sanitizers, browser validation, static analysis and public SDK documentation.
The browser harness also rejects an injected invalid texture copy and proves
that delayed completion after device destruction cannot publish old work.
Software Chromium acceptance does not establish physical-browser performance;
R7's open hardware/host qualification gates remain open.


## Texture types, formats and views (R9)

R9a adds original-format sampled color views and uploaded `Rgba16Float` mip
chains. R9b adds `D2Array` and `Cube`; R9c adds `D3` volumes and optional
`Bc1RgbaUnorm`/`Bc3RgbaUnorm` encoded textures. These operations use the existing
RHI identities, R2 retention and R3 whole-allocation GDI hazards. They do not
introduce bindless descriptors, finer synchronization or sparse backing.

### Creation and packing

`TextureDescription.Dimension` selects `D2`, `D2Array`, `Cube` or `D3`.
`DepthOrLayers` is one for D2, an immutable layer count for D2Array, exactly six
for Cube, or the base depth for D3. Cube faces must be square and upload in
**+X, -X, +Y, -Y, +Z, -Z** order. Volume depth halves at each mip, clamped to one;
array layers and cube faces never shrink. Attachments and retained CPU content
remain D2; deferred initialization and mutable R8 transfers remain RGBA8 D2.

Uploads contain every mip in ascending order. Each mip contains all of its
layers/faces/depth slices in ascending order. For an uncompressed format a
slice occupies `RowPitch * mipHeight` bytes. For BC it occupies
`RowPitch * ceil(mipHeight / 4)` encoded bytes. BC1 uses eight bytes per 4-by-4
block; BC3 and the R18 BC5 profile use sixteen. Pitches must cover `ceil(mipWidth / 4)` blocks and be
multiples of the block byte count. Base BC width/height must be multiples of four
unless `HasUnalignedBlockExtents` is enabled (native Vulkan/Metal profiles);
small tail mips and unaligned native base edges still occupy complete blocks. There is no compression,
format conversion, mip generation or implicit padding insertion.

Mips must be tightly concatenated; supplied offsets and the exact total byte
length are validated before backend work. Uncompressed pitches are four-byte
aligned (binary16 RGBA pitches are eight-byte aligned). HDR bytes are original
IEEE binary16 channel encodings. Creation consumes/copies all caller bytes and
metadata before returning; `Pending` owns asynchronous validation/upload work.
The complete borrowed blob, including row padding, retains the existing
`RasterCapabilities.MaxTextureUploadBytes` limit; the logical allocation separately
consumes `StorageLimits.ResourceBytes`. This is independent of buffer allocation size
and of the R8 transfer-ring slice size.
Logical allocation bytes include every slice and rounded-up compressed block,
and independently consume `StorageLimits.ResourceBytes`. The dimension/layer
maxima do not reserve memory or remove these byte limits. Required mip counts
are bounded by the selected dimension's largest enabled spatial extent; array
layer and cube face counts never add mip levels.

### Views and bindings

The legacy `CreateTextureView(device, texture, output)` selects the allocation's
full dimension and range. The overload taking `TextureViewDescription` selects
mip and layer ranges; zero counts mean the remaining range. Shader LOD zero
refers to `BaseMip`, and shader array layer zero refers to `BaseLayer`. An
array/cube may expose a single layer as D2. Cube views select all six faces;
D3 views select mips, with no layer/depth-slice reinterpretation. Formats cannot
be reinterpreted. `TextureAspect::Color` is enabled; Depth/Stencil explicitly
use the separately enabled [R10 private-aspect sampling profile](#attachment-extensions-r10).

Only selected sampled mips must be initialized. Thus a deferred RGBA8 texture
with only mip two initialized can expose `{ .BaseMip = 2, .MipCount = 1 }`;
the legacy full view still rejects. Attachments retain the existing clear/store
rules. A view retains its allocation after the public texture owner is destroyed.
All views, bindings, batches, graphs and submitted uses still exclude mutable
transfers on that whole allocation, even for disjoint ranges. GDI recognizes all
new sampled binding kinds and checks initialized views before executing reads.

`RasterBindingKind::Texture2DArray`, `TextureCube` and `Texture3D` must match the
view dimension. A mismatch rejects binding creation before native work. The
offline shader helper reflects each target independently and checks the emitted
WGSL and combined GLSL ES sampler dimension. Metal also verifies its native
pipeline reflection. Descriptor arrays and compute-stage texture bindings
remain outside this profile. `SamplerDescription.W` selects volume-depth
addressing; mip filtering must be enabled to sample LODs beyond level zero.

### Enabled backend matrix

| Operation | Vulkan | Metal | WebGPU | WebGL 2 |
| --- | --- | --- | --- | --- |
| Normalized D2/array/cube/volume upload and sampling | Queried | Enabled | Device limits | Context limits |
| Original RGBA16F upload and sampling | Queried | Enabled | Enabled | Enabled |
| Original-format mip/layer color views | Native views | Native views | Native views | Budgeted sampled copies |
| BC1/BC3/BC5_UNORM D2/array/cube upload and sampling | Enabled BC feature + format queries | Device BC support | Enabled `texture-compression-bc` | Unsupported |
| BC volumes, cube arrays, format reinterpretation | Unsupported | Unsupported | Unsupported | Unsupported |
| HDR partial view without float color attachment support | Queried | Enabled | Enabled | Unsupported |

`FormatCapabilities.MaxArrayLayers`, `MaxCubeDimension`, `MaxDimension3D` and
`PartialViews` describe enabled operations, not raw hardware potential. Array
layers and volume extents use independently enabled backend limits; mip chains
remain bounded at sixteen. R18 removes the historical 4096 sampled 2D/cube ceiling. Vulkan
queries combined image usage/type/creation flags; browser limits come from the
enabled device/context. Compression is independently optional. Required and
preferred format entries may specify `Dimension`, `MinDepthOrLayers` and
`PartialViews`; startup reports unmet constraints before Ready.

WebGL 2 has no native texture views. A partial sampled view owns a GPU copy of
its range, rebased to mip/layer zero. Its logical bytes consume `ResourceBytes`
while retained, and destruction follows the usual completion gate. Full views
share the backing. Float snapshot copies require the enabled float color-buffer
extension. These copies preserve sampled texel order without introducing an
attachment flip. API allocation/residency remains unknown on the browser paths;
logical copy bytes are not a physical-residency claim.

The public `tests/sdk_consumer/r9.cpp` journey validates rendered readbacks for
partial initialized mips, array layers, all six cube directions, volume slices
and tail mips, binary16 HDR values, and BC1/BC3/BC5 where enabled. It destroys texture
owners before drawing to exercise retained views. The same fixture is built
natively and through the installed browser SDK; browser QA runs forced WebGPU,
forced WebGL 2, Auto and Auto fallback. Reference regressions cover malformed
slice/block packing, range bounds, typed binding mismatches, CPU view-relative
mips and conservative mutation exclusion. Physical host qualification remains R7b.

## Reference-scene texture profiles (R18)

R18a adds `RasterFormat::Bc5RgUnorm` for original BC5_UNORM blocks, including
ATI2 DDS payloads prepared by a caller. It uses the R9 D2/array/cube upload,
partial-view, typed-binding, graph-retention and completion contracts. Each
4-by-4 block occupies sixteen bytes, with independent red and green endpoint
palettes and index bits. Native sampling returns `(R,G,0,1)` in normalized
linear space. Normal reconstruction belongs to the caller. Signed BC5, BC
volumes, compressed attachments, retained CPU texels and mutable compressed
transfers remain unsupported; no decoder, compressor or automatic conversion
is introduced. Format capability queries explicitly reject the WebGL profile
and devices without enabled BC compression. `HasUnalignedBlockExtents` reports
native base-edge support, and `RequiresUnalignedBlockExtents` admits that
requirement before Ready. This covers prepared ATI2 payloads whose base sizes
are not divisible by four. The pinned WebGPU profile requires block-aligned
compressed base dimensions and returns `Unsupported` for unaligned bases;
callers supply a separately authored supported texture. The RHI does not pad
or resize it silently.

R18b publishes enabled texture width/height limits instead of clamping general
raster to 4096. Vulkan intersects native image/type/usage format limits with the
device limit. Metal uses the existing startup-selected Mac2/Apple4-or-later
16384 guarantee. WebGPU requests the adapter's reported 2D maximum before
creating its device, then publishes the resulting **device** limit; the adapter
alone is not an enabled profile. WebGL uses its sampled context limit, with separate
cube limits and attachment limits intersected with depth-renderbuffer/viewport
bounds. Existing required/preferred `Raster.MinTextureDimension2D` and
`Formats[].MinDimension2D` admission checks apply before Ready. Required misses
fail with requested/available diagnostics; preferred misses retain the enabled
profile and report the first unmet preference. A per-format/use limit may be
lower than `Raster.MaxTextureDimension2D`; callers must check both.

R18c separates allocation size (`Raster.MaxBufferSize`), complete creation-upload
bytes (`Raster.MaxTextureUploadBytes`) and bound compute range
(`Compute.MaxStorageRange`). Vulkan uses its Vulkan 1.1 maintenance3 allocation
limit and storage-range properties; Metal uses `maxBufferLength`; WebGPU requests
adapter buffer/storage maxima before creation and publishes enabled **device**
limits. WebGL has no queryable buffer-size maximum, so its implemented ceiling
is the signed 32-bit byte envelope. Successful admission does not reserve memory:
logical allocations separately consume `StorageLimits.ResourceBytes` and may
fail with `CapacityExceeded` or `OutOfMemory`, preserving outputs and ownership.
A complete 8240-by-8200 RGBA8 blob requires both an upload limit and remaining
resource budget large enough for its roughly 258 MiB payload; width alone does
not establish support. There is no implicit resizing or conversion.

Binding metadata and copied dynamic-offset/shader-name tables now have sixteen
slots (`RASTER_BINDING_COUNT`). `Interface.MaxBindings` and
`Binding.MaxBindings` report the enabled entry/index envelope. Independently
check `Binding.MaxUniformBuffersPerStage`, `MaxSampledTexturesPerStage`,
`MaxSamplersPerStage`, `Compute.MaxStorageBindings` and
`Binding.MaxDescriptorsPerSet`, `MaxUniformBuffersPerSet` and `MaxSamplersPerSet`.
Vulkan set-wide uniform/sampler limits additionally constrain mixed-stage layouts.
Texture array elements count as descriptors;
visibility in both raster stages counts in each. Dynamic-offset limits remain
separate. Layout admission rejects excessive per-stage counts before backend
allocation, even when total entries fit. Required/preferred requests use
`Raster.MinTextureUploadBytes`, `MinUniformBindings`, `MinTextureBindings`,
`MinSamplerBindings`, `MinUniformBindingsPerSet`, `MinSamplerBindingsPerSet`,
and `Compute.MinStorageBindings` alongside existing
`MinBufferSize`, `MinBindings` and `MinStorageRange`. Required misses fail
before Ready; preferred misses report requested/available diagnostics without
changing the enabled profile.

Array layers and volume dimensions are independently queried for Vulkan image
format/type/usage, enabled WebGPU device limits, and WebGL context limits. Metal
uses the selected Mac2/Apple4-or-later guarantee of 2048 layers and 2048 volume
texels per axis. Formats may expose lower or zero limits; compressed volumes
remain unsupported. `Formats[].Dimension` and `MinDepthOrLayers` admit each
requested profile before Ready. Initialized mip chains remain bounded at sixteen.
R8 streaming still uses its separately configured bounded slices and supported
formats/operations. R18c does not enable new compute, descriptor kinds, indirect
commands or ray operations.

The offline raw-texel cooker accepts dimensions through 32768. Its default
complete-chain budget remains 64 MiB; an authored `max_upload_bytes` integer
can explicitly select a budget through 512 MiB. The cooker checks the entire
padded footprint before processing texels. This offline envelope is not a
runtime device guarantee; callers still negotiate and budget the upload.

The extended `tests/sdk_consumer/r9.cpp` fixture checks independent BC5 channel
palettes in both interpolation modes, encoded index ordering, blue/alpha,
small tail mips, array layers, a cube face and unaligned native base edges, then reads back the rendered
pixels. It retains views after texture-owner destruction. Its patterned image
uses 8240 texels when admitted, otherwise the actual lower enabled width, and
up to 2049 admitted rows. Explicit row padding keeps its complete upload above
64 MiB on lower dimension profiles; a 2048-limit WebGL regression checks this
path and its pixels. It samples array layer 256 when enabled (otherwise the highest admitted
layer of a reported 256-layer array) and a 512-deep volume mip chain when
enabled, otherwise a reported lower volume depth. A WebGL browser regression
reports a 256-texel volume limit and verifies the admitted mip chain and pixels. These prepared textures exercise the old ceilings
without loading a reference scene. Unit tests cover copied
required/preferred requests, unsupported BC5, enabled 8192/16384 limits,
malformed mip/slice/pitch lengths, native rollback and resource-byte budgets.
The `tests/sdk_consumer/r18c.cpp` installed-SDK fixture allocates a 67109120-byte
storage buffer, binds a 16777472-byte range, and reads its far-end authored value
in a compute graph with five storage and four uniform descriptors. GPU output
readback must equal 20. Native Metal/Vulkan and real Chromium WebGPU exercise
this producer/consumer path; WebGL explicitly lacks compute and separately
qualifies the R9 textures. Exact-limit/over-limit and per-stage admission tests
cover unpublished handles, resource-budget rollback and preferred diagnostics.
These correctness fixtures do not complete R7b/R7c physical reference-scene
qualification.

Thanks to Microsoft, [Block Compression (Direct3D 10), BC5](https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression),
for the two-channel block contract; Apple, [Metal Feature Set Tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf),
for the selected-family dimension guarantee; and W3C's
[WebGPU device required limits](https://www.w3.org/TR/webgpu/#dom-gpudevicedescriptor-requiredlimits),
for the adapter-to-enabled-device distinction; and Khronos Group,
[Vulkan maintenance3 allocation limits](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMaintenance3Properties.html),
for the enabled allocation bound. The milestone ownership and
remaining binding/compute/ray/resource work live in the
[canonical RHI/GDI assignment](../architecture/rhi-gdi.md#follow-up-milestones-r6r18).

## Attachment extensions (R10)

R10a adds up to four offscreen color attachments, private depth sampling and
independent stencil allocation/testing/sampling capabilities. R10b adds
capability-enabled four-sample color attachments with pass-end color resolves.
R10c adds writable views of individual attachment mips and array/cube layers,
with initialization tracked separately for color, depth and stencil. R13's finer
hazard tracking remains planned: synchronization and mutation exclusion still
cover the complete allocation, even for disjoint views or aspects.

### Allocate and select attachments

`TextureDescription.Attachment` allocates undefined color and a private depth
image. `Stencil=true` selects a private depth/stencil image. Each allocation's
dimensions, mip count, layers, sample count and stencil choice are immutable.
Color attachments remain RGBA8 linear/sRGB or enabled RGBA16F; R8/RG8, compressed
formats and volumes are sampled-only profiles. Single-sample attachments permit
enabled mip chains, arrays and cubes. Four-sample allocations require D2, one
mip and one layer; direct sampling and R8 transfers reject them.

Create a `TextureViewDescription` with `Attachment=true`, dimension D2 and
exactly one selected mip/layer. This view retains its color allocation and the
corresponding private depth/stencil subresource. It can be created before a
clear, but cannot enter a sampled binding set. A regular color view or
`Aspect=Depth/Stencil` selects a sampled view instead. Depth/stencil sampling
requires a single-sample allocation and D2 view of one layer; shader mips are
relative to `BaseMip`. There is no public format reinterpretation, standalone
depth allocation or depth/stencil resolve in this profile.

`FormatCapabilities` publishes `AttachmentViews`, `MaxAttachmentMipLevels`,
`MaxAttachmentLayers`, `MaxAttachmentCubeDimension`, `DepthSampled`,
`StencilAttachment`, `StencilSampled` and the enabled `SampleCounts` mask.
`RasterInterfaceCapabilities.MaxColorAttachments` describes the pass count.
Format requirements can independently require attachment views, depth sampling,
stencil allocation or stencil sampling; existing dimension/mip/layer/sample
constraints apply to attachment uses too. Required misses reject before Ready;
preferred misses report their exact diagnostic without silently changing a pass.

| R10 operation | Vulkan | Metal | WebGPU | WebGL 2 |
| --- | --- | --- | --- | --- |
| Multiple offscreen RGBA color attachments | Queried, up to four | Four | Four | Context limits, up to four |
| Writable mip/array/cube-face views | Queried native views | Native views | Native views | D2 mip zero only; larger allocations reject |
| Private depth loads | Queried sampled depth | Enabled | Enabled | Unsupported |
| Stencil allocation and per-face operations | Queried depth/stencil | Enabled | Enabled | Enabled |
| Private stencil integer loads | Queried sampled stencil | Enabled | Enabled | Unsupported |
| Four samples and color resolve | Combined color/depth/stencil query | Device sample-count support | Enabled for admitted colors | Per-format color/depth sample query |
| Surface MSAA/MRT, depth/stencil resolves, sampled MSAA | Unsupported | Unsupported | Unsupported | Unsupported |

WebGL's authored alternative uses single-sample D2 attachments and the enabled
stencil test; shaders can use authored color data when private aspect sampling
is unavailable. The SDK fixture explicitly selects these alternatives. Browser
software-GPU correctness is separate from R7's open physical-host qualification.

### Pass and pipeline compatibility

The legacy `RasterPassDescription.Color` path remains one D2 mip-zero,
single-sample color/private-depth pair or the acquired surface. For an explicit
offscreen pass, set `Colors` to borrowed `RasterColorAttachment` entries and
`ColorCount` to their count; `Color` must be null. `BeginRasterPass` consumes the
entries before returning; `AddGraphPass` copies them and retains the views.
Every source has the same selected extent and sample count. The first color
view supplies private depth/stencil. Sources and resolve destinations must be
distinct allocations under the conservative hazard contract.

Each color entry has its own Clear/Load/Discard and Store/Discard. The selected
subresource is initialized only by Clear, or Load of already-defined contents,
followed by Store. Clear covers the full selected subresource, independently of
viewport/scissor. Draw coverage does not repair undefined initialization. Depth
and stencil have independent load/store controls; their read-only modes require
Load and reject depth-writing or nonzero stencil-write-mask pipelines.

An optional `Resolve` selects a single-sample writable view with matching format
and selected extent. The source must have four samples and Clear or defined
Load. Resolve initializes the destination color before the source store, so
source `Store=Discard` is valid. It does not initialize destination depth/stencil.
R8 transfers remain single-sample RGBA8 D2 operations, and update the same selected
mip initialization ledger after actual completion.

`RasterPipelineDescription.ColorFormats` is the copied offscreen format sequence;
empty retains the legacy `Target` compatibility. Explicit pipelines specify the
matching `SampleCount` and private `Stencil` choice. Stencil front/back faces
have independent comparison/fail/depth-fail/pass operations and shared eight-bit
read/write masks and reference. Disabled depth preserves private depth even when
stencil testing is active. Unsupported combinations and mismatched pass/pipeline
formats, counts, samples or stencil choices reject before an accepted draw.

`TextureDepth2D` reflects `Texture2D<float>` and uses unfiltered scalar loads;
`TextureStencil2D` reflects `Texture2D<uint>` and uses unsigned loads. These
bindings reject color views, each other's aspect and writable attachment views.
The shader tool checks the emitted WGSL scalar type and GLSL sampler dimension;
unfiltered GLSL loads carry an explicit no-sampler sentinel. Comparison samplers,
depth filtering and compute-stage texture access are outside the profile.

### GDI declarations, retention and evidence

Import each attachment allocation once. Declare each color source's new version
as `ColorWrite`, or `ColorReadWrite` when its color load is Load; declare every
resolve destination as a separate `ColorWrite`. Private depth/stencil operations
belong to the first color allocation's version. Sampled declarations cover the
source allocation, and the compiler verifies the actual retained view's selected
initialized mips/layers/aspect. Missing attachment/resolve declarations, feedback,
undefined loads and reads of uninitialized siblings reject during compilation.
Whole-allocation export/readback roots still require every color subresource to
be defined. `RasterTextureState` reports whole-allocation color/depth/stencil
validity; its revision also protects partial initialization changes.

Advanced attachment passes preserve their boundaries; optional R5 merging does
not move resolves or stencil load/store effects. Plans retain their attachment
views even after the caller destroys view and texture owners. Native objects,
including cached Vulkan framebuffers, retire after actual completion. Logical
resource bytes include all mips/layers, enabled samples and private depth/stencil;
WebGL's separate render/sample color backing is also charged. API allocation and
physical residency remain separate R7 measurements.

The installed SDK's `tests/sdk_consumer/r10.cpp` journey compares direct and GDI
pixels for two color outputs, MSAA resolves with discarded sources, selected
array/cube-face mips, depth/stencil loads, stencil comparison accept/reject, and
detached attachment owners. It performs seventeen RGBA8 readbacks on each
selected backend, with explicit fallback cases for unavailable profiles. Native
tests add malformed range/sample/resolve, typed-binding, undefined-sibling,
declaration and completion-retention checks. Existing resize/loss suites continue
to exercise the shared device/frame/resource lifecycle.


## Scalable bindings and optional bindless (R11)

R11a adds completion-gated descriptor arenas and packet-local dynamic buffer offsets.
R11b adds reflected, fully populated fixed arrays of sampled color `Texture2D`
descriptors and an independently requested non-uniform indexing profile. The
[architecture milestone](../architecture/rhi-gdi.md#follow-up-milestones-r6r17)
owns the assignment; the API comments own its detailed caller contracts.

| Operation | Vulkan | Metal | WebGPU | WebGL 2 |
| --- | --- | --- | --- | --- |
| Dynamic uniform offsets | Enabled up to negotiated limits | Enabled | Enabled up to negotiated limits | Enabled via uniform range binding |
| Dynamic compute storage offsets | Enabled up to negotiated limits | Enabled | Enabled up to negotiated limits | Unsupported with compute |
| Fixed color texture descriptor arrays | Enabled up to configured/native limits | Enabled within 31 texture registers; array ranges cannot overlap | Unsupported | Unsupported |
| Dynamically uniform array selection | Enabled only with the actual device feature | Enabled | Unsupported | Unsupported |
| Non-uniform fixed array selection | Explicit request, Vulkan 1.2 and enabled sampled-image indexing feature | Explicit request and verified native lowering | Unsupported | Unsupported |
| Fixed color array/cube/volume descriptor arrays | Enabled within sampled-image limits | Enabled within texture register limits | Unsupported | Unsupported |
| Fixed sampler descriptor arrays, constant selection | Enabled within sampler limits | Enabled within sampler register limits | Unsupported | Unsupported |
| Fixed uniform/compute-storage descriptor arrays, constant selection | Enabled within each buffer limit; no dynamic offsets | Unsupported | Unsupported | Unsupported |
| Unsized sampled color descriptor arrays | Explicit independent request; Vulkan 1.2 runtime and variable-count features | Unsupported | Unsupported | Unsupported |
| Logical partially populated arrays | Explicit layout flag; retained element-zero fallback fills holes | Same fallback contract | Unsupported | Unsupported |
| Publish replacement after prior binding/submission | Immutable snapshot publication | Immutable snapshot publication | Publish after asynchronous replacement reaches Ready | Immutable snapshot publication |

### R11c table allocation and publication

`RasterBinding::IsRuntimeSized` admits an unsized sampled color shader array.
Request `DeviceDescription::Required.HasRuntimeTextureArrays` or its preferred
counterpart independently of `BindlessTextureIndexing`. Vulkan enables both
`runtimeDescriptorArray` and `descriptorBindingVariableDescriptorCount` only
when requested and available. The table occupies `RUNTIME_BINDING_ARRAY_SLOT`
(binding 15); `RUNTIME_BINDING_LENGTH_SLOT` (14) is reserved for RHI-owned length
metadata. Other consumer bindings remain available. The layout `Count` is the
allocation ceiling; `CreateRuntimeBindingSet` chooses a positive actual length
up to that ceiling. Fixed bindings retain their original lengths. Native layout
support and variable-count limits are checked before layout publication.

Cook with `NATIVE_BINDING_ARRAYS RUNTIME_BINDING_ARRAYS` and include the installed
`runtime_bindings.slang`. Declare the unsized texture array at the reserved table
slot, and use `BoundRuntimeBindingIndex(index)` before indexing. Per-invocation
indices additionally need the existing `NonUniformResourceIndex` annotation and
independent indexing capability. The helper clamps against an immutable uint4
length buffer belonging to the bound snapshot. The emitted SPIR-V verifier checks
the exact unsigned-vector layout, byte offset and first-component load; ordinary
caller uniforms cannot substitute for the owned length. Unbounded access, wrong
control layouts and unknown pointer paths reject. Generated shader `Count` is
one, the verified minimum safe allocation. A single artifact can therefore grow
or shrink between any positive admitted lengths without compilation or CPU
readback. Hand-authored fixed bounds still impose their declared minimum.

Do not supply the length entry in `CreateBindingSet` or
`CreateRuntimeBindingSet`. RHI creates and initializes its immutable uniform
buffer during snapshot preparation and retains it with that snapshot. It
consumes one buffer record, one uniform descriptor, and its checked host/resource
bytes. Descriptor budgets charge the actual allocated table length, including
fallback holes, plus this control descriptor and ordinary bindings. Replacements
need overlap capacity until previous work completes. The owned control buffer
has no public resource identity to import into GDI; its immutable read and
lifetime follow the retained binding snapshot. All consumer resources still
require ordinary graph declarations.

Runtime arrays require an authored fixed-array/scalar alternative on Metal and
browser targets; no toolchain pins change. The existing texture-indexing limits
remain independent of runtime allocation support.

`RasterBinding::HasPartialBinding` allows omitted array entries. Element zero
must be supplied and is copied into every hole as a retained fallback. Therefore
even a shader branch accessing a logical hole has a valid descriptor. Host
`GetBindingIndex` rejects holes; non-uniform reachability declarations enumerate
populated elements. GDI continues to require all physical resources, including
fallbacks. This is logical partial population, not sparse descriptor allocation:
the full allocated length consumes native and engine descriptor budgets. Native
`PARTIALLY_BOUND` is not enabled or claimed.

`PublishBindingSetUpdate(device, current, replacement)` publishes a separately
prepared Ready snapshot with the exact same layout. Call it on the owner thread,
including after an earlier draw in an open frame; prepare replacements before
beginning the frame. Publication performs no allocation, collection or native
descriptor mutation. Success consumes `replacement`, invalidates the old public
identity and its host index tokens, and makes subsequent work use the replacement.
Previously retained graphs, batches and submissions keep the old snapshot and
all its resources through completion. Failed or pending publication preserves
both handles. This supports safe updates after earlier binds/submissions through
snapshot replacement; it does not mutate a recorded command's descriptors or
promise Vulkan's native `UPDATE_AFTER_BIND` most-recent-before-submit semantics.
No native mutable-descriptor feature is inferred from this API.

Fixed sampler and buffer descriptor arrays use constant selection verified in
the emitted target. Buffer arrays exclude dynamic offsets. Vulkan accepts
uniform buffers and the existing compute storage kinds; storage textures and
raster storage access remain R12. Metal supports fixed sampler and sampled
color descriptor arrays, with separate nonoverlapping sampler/texture register
ranges; buffer descriptor arrays remain explicitly unsupported.

Inspect `DeviceInfo::Profile.Binding`. Array count never implies non-uniform
selection. Set `DeviceDescription::Required.BindlessTextureIndexing` when the
workload cannot run without it, or `Preferred.BindlessTextureIndexing` when an
authored scalar/constant-array alternative exists. A missing required feature
rejects device admission; an unavailable preference is reported in
`UnmetPreferences`. Foreign Vulkan devices cannot enable new features and do not
infer indexing from adapter support. Portable scalar binding remains available.

### Budgets, immutable snapshots and offsets

`StorageLimits::BindingDescriptors` limits populated descriptors across live,
pending, detached and submitted snapshots. `DescriptorsPerSet` configures the
per-snapshot bound, clamped by the backend. Descriptor payloads use budgeted Ludus
storage during creation; command encoding neither grows metadata nor creates
new sets. `ResourcesPerKind` still limits snapshot records, and `HostBytes` and
`ResourceBytes` retain their distinct metadata/backing scopes.

Vulkan shares pools among snapshots with identical descriptor-type counts,
including separate dynamic uniform/storage types. It frees sets only after
completion and destroys an empty arena. Pool reservations are amortized in
bounded groups, with checked sizing; allocation/creation failure rolls back the
unpublished snapshot and native reservation. Metal/WebGL retain bounded copied
binding payloads; WebGPU uses immutable bind groups. Replacing a snapshot creates
a new identity and retains the old backing through its last submitted use.

Declare `RasterBinding::HasDynamicOffset` for a buffer entry. Supply additional
bytes in `RasterDraw::BindingOffsets.Bytes[binding]` or
`ComputeDispatch::BindingOffsets`. Absent/static entries require zero. The RHI
checks alignment, addition overflow and `snapshot.Offset + dynamicOffset + Size`
against the complete buffer before any native work. Native dynamic offsets are
ordered by binding number, regardless of authored layout order. GDI declarations
must cover the effective range. Batches/graphs copy offsets, so later mutation of
caller packets cannot change retained work. Dynamic offsets do not authorize
mutation of retained or in-flight buffer contents; the R8 exclusion rules remain.

### Cook, declare and retain array access

Use the installed shader helper with the explicit native profile:

```cmake
ludus_compile_shader(TARGET app NAME material RASTER NATIVE_BINDING_ARRAYS
    SOURCE material.slang VERTEX VertexMain FRAGMENT FragmentMain)
```

The helper supplies the RHI-owned `raster_bindings.slang` include directory.
Declare a fixed color texture array with `LUDUS_SHADER_BINDING`, and bound a
runtime unsigned index before access, for example
`Images[NonUniformResourceIndex(min(index, 3U))]` for four elements. The cooker
reflects counts and derives Constant/Uniform/NonUniform access from emitted
SPIR-V. It admits constant in-range indices and verified unsigned min/clamp
bounds; unsized arrays additionally require the explicit R11c allocation bound
described above. Unknown pointer/access paths and unbounded indices reject.
Metal uses the existing pinned SPIRV-Cross MSL 2.4 lowering for this
profile, with independently checked emitted registers/counts and runtime Metal
pipeline reflection. Direct Slang Metal lowering does not admit the pinned
`NonUniformResourceIndex` operation. Pins are unchanged. This helper profile
requires the pinned translator and an authored browser alternative.

`CreateBindingSet` requires exactly one entry for every `(Binding, ArrayElement)`
unless `HasPartialBinding` supplies the R11c fallback contract above. Duplicates,
wrong views and out-of-range elements reject. The immutable
snapshot is the exhaustive reachable-resource table. Every resource stays
retained, including array entries whose branches are inactive. Obtain a borrowed
`RasterBindingIndex` with `GetBindingIndex`, and resolve it against that exact
snapshot with `GetBindingArrayElement`. Destruction/replacement invalidates host
index resolution; numerical shader indices are not transferable ownership.

For NonUniform arrays, `RasterDraw::ReachableBindings` must declare each populated
array element exactly once using tokens from the bound snapshot. Missing, duplicate,
stale or foreign declarations reject before encoding. GDI additionally requires
sampled use declarations for every reachable physical texture, and retains the
whole table through completion. Hazards remain conservative for complete
allocations until R13. The verified shader bound prevents descriptor-table
escape; resource lifetime is never inferred from whichever branch happened to
execute on one frame. Hand-authored shader artifacts must independently verify
the documented bounds and supply `HasVerifiedArrayBounds`.

### Correctness and measurement evidence

`ludus_rhi_r11_consumer` and the installed `ludus_sdk_r11_consumer` compare static
snapshots, dynamic offsets, GDI execution and constant/non-uniform native arrays
against the same independently checked color stripes. GPU compute readback verifies
a dynamic storage write at byte 256 with an untouched prefix/suffix. The native fixture destroys
array texture/view owners before drawing and the snapshot before readback;
retained backing must still produce the expected pixels. Optional non-uniform
execution is reported unavailable when a Vulkan device cannot enable it.
The source fixture enables its native array cases when the pinned translator is
configured (`LUDUS_R11_NATIVE_ARRAYS`); the installed native SDK fixture requires
that translator and always qualifies the array profile. `r11.html` exercises dynamic
offset pixels, rejected misalignment and exact GDI ranges on forced WebGPU/WebGL 2;
WebGPU also checks the dynamic storage write. Registry tests cover creation rollback, descriptor
pressure across delayed completion, stale indices, retained array elements,
72-descriptor payloads, copied compute offsets and incorrect graph ranges.
Reflection regressions cover fixed/runtime counts, target registers, indexing
modes and unsafe bounds.

The native fixture reports CPU creation time for 32 static snapshots versus one
reused dynamic snapshot, alongside populated descriptors and actual Vulkan pool
reservations. R7 `AllocationTelemetry` adds `BindingDescriptors`,
`RetiringBindingDescriptors`, `DescriptorArenaPools` and
`ReservedArenaDescriptors`. Descriptor counts are not driver bytes or physical
residency; driver descriptor allocation bytes remain unavailable. Query
`GetStorageUsage` for budgeted metadata bytes. These observations compare the
chosen binding workload, not GPU duration, display latency or universal speedup.
Named physical GPU qualification remains an R7b gate; optional bindless never
becomes the default merely because it is implemented.

R11a/R11b implementation is recorded in [PR #239](https://github.com/The-Ludus-Project/Ludus/pull/239).
At code revision `3dace88df9db11a0126fae3d7e2ae7241e5bf01b`, the
[Linux Development and SDK gate](https://github.com/The-Ludus-Project/Ludus/actions/runs/38376845936/job/115228342822)
passed source and installed R11 Vulkan execution under validation layers with no
validation errors. Both constant and optional non-uniform arrays ran. The installed
fixture reported 1,412,602 ns to create 32 static snapshots versus 41,968 ns for one
dynamic snapshot, 33 populated descriptors and 3 native pools reserving 48 slots.
Host metadata grew by 165,656 bytes for the static snapshots (including cold
page/pool growth), then 24 bytes for the additional dynamic snapshot. This is one
software-driver observation, not a repeated benchmark or physical-GPU qualification.
The [macOS native gate](https://github.com/The-Ludus-Project/Ludus/actions/runs/38376845936/job/115227742875)
passed its installed Metal shader/consumer step, and the
[packaged browser gate](https://github.com/The-Ludus-Project/Ludus/actions/runs/38376846022/job/115234164137)
passed the installed SDK's forced WebGPU/WebGL 2 pixel and dynamic-offset journeys.
Pinned formatting, native/browser analysis, Linux/macOS sanitizers and API
coverage gates also passed for that code revision.

Thanks to Khronos Group, *Vulkan Specification*,
[vkCmdBindDescriptorSets](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBindDescriptorSets.html),
and *Vulkan Guide*, [Descriptor Arrays](https://docs.vulkan.org/guide/latest/descriptor_arrays.html),
for dynamic-offset ordering and independently enabled indexing/lifetime rules;
and to the Slang and SPIRV-Cross teams for the target reflection/native lowering.

## Raster world coordinate grid

`ViewDescription::WorldGrid` opts a raster view into an adaptive world grid. Set
`IsVisible`, the positive `FocusDistance` in world units, and the drawable `Width`
and `Height` in pixels. Perspective views use the XZ ground plane. Orthographic
views choose XY, XZ or YZ from the camera's dominant forward direction. The grid
includes red X, green Y and blue Z axes. `BuildWorldGridLines` exposes the same
bounded layout for a UI that has no GPU scene yet.

`DrawView`, `RasterViewport` and conventional `Submit` draw one instanced guide
batch after opaque geometry and before screen overlays. Conventional and reverse-Z
depth tests use the scene's comparison direction with depth writes disabled.
The guide does not consume scene mesh/item slots, change scene bounds or source-ID
reports, enter shadow caster views, or appear in reference/path tracing captures.
The draw count includes its batch. Grid buffers belong to the immutable prepared
view and retire through ordinary RHI retention; pending or failed preparation
preserves the caller's previous view. Grid scratch and per-view ownership records
are charged to the renderer host budget, and GPU buffers/pipelines consume ordinary
RHI storage budgets. The ribbon grid needs no new shader recipe.

### Analytic grid

`Renderer::InitializeGrid` opts into the analytic grid after `Initialize` is Ready,
with the cooked `renderer_grid` stages: twelve vertex locations (the flat program's
position and instance streams, with the instance fields reused) and no bindings.
A visible grid then draws one world-space quad on the grid plane, whose fragment
stage measures each pixel's distance to the nearest minor, major and axis line
(`BuildWorldGridPlane` supplies the plane, spacing, colours and fade distances).
Coverage is computed in pixels, so lines keep their width at any zoom, are
antialiased without a resolve, and fade out where neighbouring lines would be
closer than ten pixels. A radial fade replaces the abrupt edge of the ribbon
grid. Only the coordinate axis off the plane stays a ribbon. The pipelines
blend premultiplied coverage and keep the ribbon grid's depth convention, so the
draw count of a grid view is two instead of one.

The ribbon grid remains the fallback: without `InitializeGrid`, with a flat
program that binds uniforms or visibility inputs, or when the grid shaders fail,
views keep their 165 ribbons. While the shaders are pending `PrepareView` returns
Pending for grid views. The editor opts in when it starts a viewport generation,
as `PreviewShaderProgram::Grid`.

To measure the pass cost on the host's device, run
`LUDUS_RENDERER_BENCHMARK=1 ludus_renderer_render_tests "[grid]"`. It prints
`GRIDBENCH,<variant>,<size>,<frames>,<ns>` lines for the same 1080p frame with no
grid, with ribbons and with the analytic plane, each including clear and readback;
subtract `none` for the pass cost. Set `LUDUS_GRID_IMAGE=<prefix>` to also write
both images as PPM files.

Measured on the software Vulkan device (lavapipe, 8 cores, 1080p, 40 frames, eleven
runs), a frame with the grid hidden took 0.11 ms, with ribbons 0.12 ms and
with the analytic plane 0.17 ms, including clear and readback. The analytic
pass therefore costs about 0.06 ms more than the ribbons on a software rasteriser,
which shades every covered pixel on the CPU. A hardware GPU has not been measured
yet; run the benchmark there before changing the default.

The editor enables this option on its camera publications, including project
render graphs. Games keep the default disabled option. Custom render-graph shaders
still control their own vertex/fragment behavior; attachment discard/load and
multi-pass depth behavior remain the graph's authored contract.

## Native texture sharing

`native_interop.h` provides a Qt-independent, same-device, same-queue color-read
bridge on Metal and Vulkan. It admits an existing native context for headless
RHI rendering and publishes a leased GPU texture for an external sampler.
The bridge performs no CPU pixel readback, image upload, staging copy or silent
backend fallback. GPU readback in the SDK fixture is solely its final pixel oracle.

The initial profile is one initialized `Rgba8Unorm`, D2, single-mip/layer,
single-sample color attachment. Export its resolved single-sample destination
when producing with MSAA. Depth/stencil, HDR, arrays, cubes, volumes, foreign
writes, different queues and cross-device external memory/semaphore imports are
outside this profile and return `Unsupported`. Formats and numeric RGBA values
are unchanged; the consumer must choose compatible alpha and display-encoding
rules. The editor's first tone-mapped preview is opaque.

### Admit the actual compositor context

`NativeInstance`, `NativePhysicalDevice`, `NativeLogicalDevice` and
`NativeCommandQueue` are distinct borrowed roles. Explicit constructors wrap the
matching native objects; `GetObject()` performs the conversion at the framework
boundary. `NativeMetalTexture` and `NativeVulkanImage` similarly distinguish image
objects/bits. These types add no ownership and cannot be interchanged implicitly.

Use `CreateNativeDevice` with `NativeDeviceDescription`, rather than creating a
second device on the same physical GPU. Both the logical device and command queue
must match the consumer. Vulkan also carries the instance, physical device and
created queue family/index; Metal requires the command queue's device to be the
provided device. Vulkan admission requires an instance and physical device at
least Vulkan 1.1 and does not infer enabled features from adapter support: BC
compression and Vulkan native ray queries are disabled for an admitted context.
The existing single-session limit remains; this bridge does not complete R14's
independent multi-device/surface milestone.

The source owner supplies `Retain`/`Release` callbacks. A successful retain
promises the native objects remain valid until release, including loss and
shutdown cleanup. RHI owns its allocations, command pools and fences, and never
destroys the foreign Vulkan instance/device. Metal references are strongly held.
Callbacks run on the owner thread, must not reenter RHI, and release exactly once
after successful retain. A rejected request preserves output handles. Structural
native-profile rejection occurs before retaining the source; failures found during
backend/capability negotiation balance a successful retain with release. Native handles must be valid objects
with accurate provenance; arbitrary pointer values cannot be safely probed.

All RHI work and foreign queue access must be serialized on the RHI owner thread.
`GetNativeInteropCapabilities` reports the enabled profile and R6
`ResourcesPerKind` bound for each native lease kind. Native lease tables and
backend bookkeeping use the existing host allocation budget. The bridge pins
existing texture allocations rather than allocating copied presentation images.

### Produce, borrow, submit and retire

1. Pin the context with `AcquireNativeDevice`, and compare the consumer's actual
   native context with `GetNativeDeviceInfo`. Registry ID/UUID describe the
   adapter; agreement on the adapter alone does not establish image compatibility.
2. Render the image through ordinary direct/GDI commands and retain its accepted
   `SubmissionToken`. `AcquireNativeTextureRead` requires that token to match the
   texture's most recent accepted RHI use. The producer may still be in flight.
3. Acquire the read before the foreign command buffer is enqueued or submitted.
   Vulkan records a memory/layout barrier that makes producer writes visible to
   every supported shader stage on the shared queue;
   Metal uses tracked texture hazards and an ordered command-buffer publication.
   The descriptor carries the exact native image, extent, original format, RHI
   incarnation/state revision and Vulkan shader-read-only layout. Copying a
   descriptor or handle adds no ownership.
4. Wrap and sample the image on that same context. Do not write the image, change
   its ownership/layout, or enqueue new foreign reads after requesting release.
   RHI attachment/resolve writers and mutable transfers reject while any native
   read is live. A replacement texture can render while an old incarnation is held.
5. After every consumer has actually submitted its reads, request
   `ReleaseNativeTextureRead`. Recording completion is insufficient. A GPU marker
   follows foreign queue work. `Pending` retains the handle/backing; query
   `GetStatus`, destroy foreign wrappers, and repeat release after completion.
   The descriptor getter rejects once release starts. Multiple readers each hold
   their own lease and completion obligation. `DrainNativeTextureRead` is an
   explicit blocking maintenance/teardown operation, never a presentation path.
6. Destroy every foreign borrower of an exported context, then
   `ReleaseNativeDevice`. The original owner of an admitted context remains
   independent of this exported lease. Release rejects while its image reads live.

`DestroyDevice`/`Shutdown` immediately invalidate ordinary RHI handles and callback
tokens. With native leases held, the bridge conservatively retains the old
session's native allocations/device until all consumers complete and pins release.
Owned presentation targets are detached at logical shutdown, so ordinary Platform
window/view lifetime does not extend through that image retirement. Borrowed headless
Vulkan pools/targets remain pinned until final retirement; admission and cleanup
never drain the foreign device's unrelated queues. Maintenance waits cover only
accepted RHI frame, texture-upload and transfer fences.
`GetNativeDeviceInfo` then reports `Retiring`; no new reads may be acquired and
new sessions reject while retirement remains pending. Texture/device backing
therefore survives renderer destruction, resize/replacement and cancellation.
GPU loss is terminal: stop new native use and retire leases; the source lifetime
handshake still applies. Do not delete the foreign device behind an outstanding
source retain.

### Qt integration boundary

The editor adapter embeds a native `QQuickWindow` in its World splitter and
admits the device/queue actually returned by Qt. Its basic render loop serializes
all engine and Qt submissions on the GUI owner thread. Texture wrappers remain
read-only and retain RHI leases through consumer submission/completion.
Qt-specific code belongs in the editor adapter. Qt's
[Metal texture import](https://doc.qt.io/qt-6/qnativeinterface-qsgmetaltexture.html)
and [Vulkan texture import](https://doc.qt.io/qt-6/qnativeinterface-qsgvulkantexture.html)
wrap original native images on the scene-graph render thread without owning them.
They do not perform the RHI lifetime/synchronization protocol above.

For `QQuickWidget`, admit the device actually returned by its
[renderer interface](https://doc.qt.io/qt-6/qsgrendererinterface.html).
[Qt 6.4.3's widget implementation](https://github.com/qt/qtdeclarative/blob/v6.4.3/src/quickwidgets/qquickwidget.cpp)
selects its top-level backing-store device and overwrites a separately supplied
`QQuickGraphicsDevice`. Multiple preview widgets in that top-level window can
share one RHI session only when every native identity matches. Moving between
top-level windows or recreating the scene graph requires controlled retirement
before the source device disappears and re-admission of the new context.

Metal's native device/queue queries and image wrapper are available within the
editor's Qt 6.4 minimum. The documented Vulkan graphics queue family/index queries
require Qt 6.6; the Qt adapter must report `Unsupported` on older Qt Vulkan builds
rather than guess a created queue or include private Qt headers. The editor then
uses its own headless device and CPU readback instead (see
[the editor workspace guide](editor-workspace.md)). Native Vulkan
consumers with known valid queue provenance do not depend on Qt.

Produce/acquire at
[`beforeFrameBegin`](https://doc.qt.io/qt-6/qquickwindow.html#beforeFrameBegin),
before Qt enqueues its consumer frame. Seal a frame's foreign reads at
[`afterFrameEnd`](https://doc.qt.io/qt-6/qquickwindow.html#afterFrameEnd), which
follows submission. `afterRendering` describes recording and is insufficient.
Use direct connections on the owner/render thread; `QQuickWidget`'s render loop
is not threaded. The adapter must coordinate scene-graph invalidation, native
source cleanup and widget destruction with the source retain/release contract.

### Validation

`ludus_rhi_native_interop_consumer` and its relocated installed-SDK counterpart
create a separately owned native device, admit that exact context into RHI,
produce an oriented four-color image, and sample its original native texture
through independent Metal/Vulkan GPU commands. The Vulkan fixture additionally
requires timeline semaphores (Vulkan 1.2) to keep foreign queue work blocked until
after RHI admission and logical shutdown return; the bridge itself retains its
Vulkan 1.1 minimum. They destroy ordinary texture and
device owners before verifying all sixteen consumer pixels, then prove balanced
source release after consumer completion. The fixture includes no RHI-private
headers. Unit tests cover mismatched logical device/queue, preserved outputs,
source retention failure, multiple readers, graph/direct/transfer write exclusion,
replacement, stale identities, GPU-loss status and deferred shutdown. Browser
backends explicitly reject native sharing. Native CI additionally runs the Vulkan
journey under validation layers; hardware/workload qualification remains R7 evidence.

Thanks to The Qt Company for the native interface and QQuickWidget contracts,
and to Khronos for
[Vulkan synchronization and fence ordering](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).
The adopted bridge uses one serialized shared queue; generalized external memory
and cross-queue synchronization are separate future profiles.

### Compute reads of raster attachments

Compute-only layouts may bind one non-array `Texture2D` color view or
`TextureDepth2D` depth view at each declared slot. These are unfiltered loads;
compute samplers, other texture dimensions, stencil and texture arrays remain
outside this profile. Slang uses `Texture2D<float4>` for color and
`Texture2D<float>` for depth. Reflection must match the exact view kind.

Declare the physical image version as `GraphAccessMode::Sampled` with
`RasterVisibility::Compute`, and use `RasterTextureUse::SampledCompute` when that
is the imported image's final consumer. The ordered graph checks the selected
aspect's initialization, keeps its raster writer live, orders attachment writes
before compute loads, and retains the views and backing through completion.
Color initialization alone does not permit a depth load after depth Discard.
See [raster first hits](render-graph-authoring.md#raster-first-hits-and-secondary-rays)
for the renderer-owned visibility/depth integration.
