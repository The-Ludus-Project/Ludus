# Ludus RHI and Graphics Device Interface Architecture

**Status:** Implemented bounded R0–R5 baseline and broader target architecture;
baseline reconciliation recorded 2026-10-11 at `5659894`; R0 negotiation and
R6 storage implemented at `5014834`; R7 measurement/workflow implemented with
physical Vulkan/Wayland and deployed-browser gates open (2026-10-12).
The R6–R18 milestone assignment includes the reference-resource gap review of 2026-10-13.
The initial design was drafted on 2026-10-04 before opening the article map,
then refined through [five complete reference chapters](rhi-gdi-gems-review.md)
and primary API documentation. The [implementation record](#implementation-phases-and-acceptance)
separates shipped profiles from the [remaining work](#remaining-work-and-decision-gates).
Public declarations and the [rendering guide](../development/fullscreen-rendering.md)
define the current API and its bounds. The broader contracts below are design
targets wherever that implementation record does not identify a shipped path.

R5 adds opt-in raster pass merging. A paired Metal overlay workload measured
lower CPU acquire/encode/submit cost with full-image agreement; this is
workload-specific CPU evidence, not a GPU or presentation claim.

Ludus should use a **small explicit RHI, a Graphics Device Interface that plans
and manages GPU work, and a renderer that owns scene policy**. Make resource
lifetime, pass dependencies, pipeline state, and capability selection explicit.
Use one physical queue and predictable pass order first; add native optimizations
through measured capability paths. This gives Vulkan and WebGPU room to perform
well while keeping WebGL 2 a useful raster backend.

Here **GDI means Graphics Device Interface**, the proposed engine device services
layer, with `GraphicsDevice` as its proposed module name. These services currently
live inside `GraphicsRhi`; there is no separate `GraphicsDevice` module.
This interpretation is a naming assumption;
Windows GDI is outside the scope. RHI means rendering hardware interface. These
are proposed roles, not two duplicate wrappers around every graphics call.

The [low-level renderer systems architecture](renderer-systems.md) owns the
scene submission, views/virtual screens, materials, lighting and overlay
policy above this boundary, including the implemented L0–L3 profiles. Its
[Gems review](renderer-systems-gems-review.md)
records later renderer-specific refinements. Device/resource lifetime and pass
lowering remain owned here; broader renderer features retain their own
acceptance gates.

L1 renderer integration adds explicit depth comparisons/clear values and pass
viewport/scissor rectangles to these existing owners. Its usage and lifetime rules
are in the [renderer L1 guide](renderer-systems.md#views-and-depth-l1); the
[R3 contract](../development/fullscreen-rendering.md#ordered-raster-graphs-r3)
owns actual attachment bounds, full clears and completion semantics.

## Current implementation and compatibility

The current RHI has Vulkan on Linux, Metal on macOS, and WebGPU or WebGL 2
in the browser. General APIs share the compatibility facade's owner and registry.
Their capabilities describe implemented operations on the enabled device.

| Existing boundary | Implemented scope | Design boundary |
| --- | --- | --- |
| [Lifecycle](../../modules/graphics/rhi/include/ludus/graphics/rhi/rhi.h), [explicit device](../../modules/graphics/rhi/include/ludus/graphics/rhi/device.h) and [facade](../../modules/graphics/rhi/src/rhi.cpp) | One main-thread device/session and borrowed surface, copied requirements, asynchronous browser startup, owner validation and explicit loss/restart | Enabled raster/compute/format constraints are negotiated before Ready; independent devices/surfaces remain extensions |
| [Fullscreen resources](../../modules/graphics/rhi/include/ludus/graphics/rhi/render.h) | Eight resources per kind, one uniform binding, one fullscreen triangle and legacy `InUse` rejection | Preserve this compatibility contract separately from general resources |
| [Portable raster](../../modules/graphics/rhi/include/ludus/graphics/rhi/raster.h) | Typed generational resources, reflected immutable bindings/pipelines and indexed/instanced draws; renderer L1–L3 extend depth, targets and sampled mip formats | R8 adds bounded mutation/copy/readback; extended attachment/view and descriptor APIs remain deferred |
| [Lifetime services](../../modules/graphics/rhi/include/ludus/graphics/rhi/lifetime.h) | Retained command batches, actual completion tokens, bounded buffer/texture transfers, initialized mip primitives and canonical pipeline requests | Partial sampled views, sparse memory/eviction and persistent driver cache remain extensions |
| [Ordered graph](../../modules/graphics/rhi/include/ludus/graphics/rhi/graph.h) | Authored order, in-place versions, RAW/WAR/WAW reports, culling, completion-gated object pooling and opt-in raster merging | Conservative whole-resource hazards; no heap aliasing, parallel recording or async queues |
| [Compute](../../modules/graphics/rhi/include/ludus/graphics/rhi/compute.h) | Single-queue buffer dispatch and indexed indirect drawing on Vulkan, Metal and WebGPU | WebGL 2 requires an authored CPU/direct variant; storage textures and indirect dispatch are deferred |
| [Vulkan](../../modules/graphics/rhi/src/rhi_vulkan.cpp) and [Metal](../../modules/graphics/rhi/src/rhi_metal.mm) | Both lower bounded raster/compute, R6 storage, R7 measurement and R8 transfers; Vulkan general allocations use private VMA 3.3.0, while legacy frame targets retain their allocation path | Keep native allocation, synchronization and presentation retirement private |
| [Browser dispatcher](../../modules/graphics/rhi/src/rhi_web.cpp) | WebGPU startup with one requirement-aware WebGL 2 fallback and Platform-managed canvas replacement | Preserve ADR 0014 and callback isolation |
| [Shader build helper](../../cmake/shaders/compile_shader.py) | Pinned Slang emits SPIR-V/WGSL and macOS MSL 2.3; pinned SPIRV-Cross emits GLSL ES 3.00, with bounded raster/compute reflection and target layout validation | Reject unverified layout forms; broader package/feature support is future work |

Retain `Start`, `GetStartup`, frame statuses, and `DrawFullscreen` as a facade
through the migration. Keep existing resource ownership and `InUse` rejection
semantics for that facade until an explicitly documented migration changes them.
The new API's deferred destruction must not silently change old call behavior.
The historical [Milestone 2](milestone-2.md) predates the current common API and
is not a statement that today's repository has no RHI.

Preserve startup-only fallback, forced-selection failure, Platform canvas
ownership, zero-size skips, and explicit loss/restart. Never change backend in a
ready session, conceal shader/validation defects through fallback, or revive an
old handle after restart. Supporting a new feature does not imply upgrading the
pins in `config/web_toolchain.json`, `config/shader_toolchain.json`, or
`config/spirv_cross_toolchain.json` as part of this design.

## Module boundaries and ownership

Today the runtime dependency is
`GraphicsRenderer -> GraphicsRhi -> Foundation/Platform`. Graph, pipeline-request,
upload/readback and pool services are implemented inside the RHI registry.
The diagram and ownership split below describe a possible future extraction of
those services into `GraphicsDevice`, not an additional shipped wrapper or registry.
Extract it only when a consumer and maintenance benefit justify the boundary.

```mermaid
flowchart TD
    App[Game and editor] --> Renderer[GraphicsRenderer scene and material policy]
    Renderer --> Device[GraphicsDevice services and pass planning]
    Device --> RHI[GraphicsRhi device API]
    RHI --> Vulkan[Private Vulkan backend]
    RHI --> Metal[Private Metal backend]
    RHI --> WebGPU[Private WebGPU backend]
    RHI --> WebGL[Private WebGL 2 backend]
    RHI --> Platform[Platform window and canvas services]
    Tools[Offline shader tools] --> Package[Shader package and reflection]
    Package --> Renderer
    Package --> Device
```

The target dependency direction after that extraction would be
`GraphicsRenderer -> GraphicsDevice -> GraphicsRhi -> Foundation/Platform`.
Shader package arrows describe input data. A device need not have a surface;
Platform still owns any borrowed window/canvas. Initially instantiate one device
and one surface. Make those relationships explicit so offscreen work and future
editor views do not depend on a global current window.

| Owner | Responsibilities | Keep outside this owner |
| --- | --- | --- |
| `GraphicsRhi` | Native device/resources, one authoritative handle registry and submitted-use ledger, resource retention, encoders, binding/pipeline objects, semantic dependency lowering, submission/completion, surface acquisition/presentation | Entities, asset paths, materials, visibility, frame effects, streaming priority |
| `GraphicsDevice` | Upload/readback scheduling, frame storage, pipeline requests/prewarming, pass declarations and compilation, cross-frame import planning, transient resource pooling, retirement servicing | Native API objects, shader business meaning, gameplay quality policy |
| `GraphicsRenderer` | Immutable scene extraction, views, visibility, materials, effect variants, draw sorting/batching, desired quality and fallback | Vulkan flags, WebGPU objects, GL selectors, native heap placement |
| Content/resource layer | CPU source ownership, load priority, durable identity, recreation recipes, mip/residency decisions | Physical release of in-flight GPU allocations |
| Platform | Windows/canvases, input/listeners, legal browser canvas replacement | Graphics context/resource lifetime |

GDI calls the RHI's retain/release and completion operations; it does not keep a
second registry or competing generation counter. The RHI remains correct for an
expert direct client, which must provide the same resource-use and dependency
contracts explicitly. Direct work and GDI work share one submission owner and
state ledger, owned by RHI and updated on accepted submissions. GDI uses its
snapshot to plan imports; it cannot independently commit a different device
state. Unregistered backend calls cannot mutate tracked state.

Suggested layout follows the repository's public/private convention:

```text
modules/graphics/rhi/
  include/ludus/graphics/rhi/       small device, resources, commands, surface APIs
  src/internal/                   registry, use ledger, retention, dispatch, validation
  src/backends/vulkan/            device, resources, bindings, commands, surface
  src/backends/webgpu/            device, resources, bindings, commands, surface
  src/backends/webgl/             context, resources, state cache, commands
modules/graphics/device/
  include/ludus/graphics/device/   uploads, readbacks, pipeline requests, passes
  src/internal/                   pass compiler, pools, import planning
modules/graphics/renderer/        existing scene/material consumers
```

The backend subdirectories and separate device target are proposed; RHI and
Renderer already exist. Split large backend translation units by ownership as features grow; avoid a class per native function. Public
headers use Ludus aliases, small descriptions, opaque typed handles, and spans.
Backend SDK headers, heavy standard-library headers, owning caches, and private
headers stay behind `.cpp` boundaries. Explicit includes, `#pragma once`,
exception-free failure paths, `noexcept`, and header parse budgets apply.

For a future multi-device implementation, bind one small **private function table
per device** at startup. Current backends use the private single-session dispatch
boundary. Native single-backend builds can use direct binding; the browser uses its existing
selection boundary. Dispatch at meaningful encoder/resource operations, not
through an interface object attached to every draw/resource. Native encoders
record their actual command buffers; WebGPU records its encoders; WebGL executes
on the context owner. Worker-produced draw packets contain engine data, not a
second universal native-command bytecode or serialization framework.

## Capability negotiation and portable behavior

A backend name is not a feature guarantee. Record four distinct facts: compiled
backends, adapter-supported features, device-enabled features/limits, and the
renderer path selected for this workload. The current API publishes `DeviceInfo`
with implemented Supported/Enabled
features, plus `GetRasterCapabilities`, `GetComputeCapabilities` and
`GetLifetimeCapabilities`. `DeviceInfo::Profile` now supplies an immutable enabled raster/interface/compute
and format snapshot, with copied required/preferred admission constraints. The
legacy `StartupInfo::Capabilities` uses `DeviceCapabilities` for fullscreen limits.
The [profile negotiation guide](../development/fullscreen-rendering.md#profile-format-and-limit-negotiation)
owns current request, diagnostic and fallback semantics; raw adapter catalogs
and new native feature enabling remain future extensions.
Requests include required and preferred
features; unmet requirements fail before resource creation, while preferences
select a supported path. Browser Auto fallback must validate the application's
requirements against the fallback as well.

The broader target uses workload bundles with individually queryable capabilities:

| Workload bundle | Intended operations | Vulkan | Metal | WebGPU | WebGL 2 |
| --- | --- | --- | --- | --- | --- |
| Portable raster | Direct indexed/instanced draws, uniforms, sampled textures/samplers, color/depth passes, uploads/copies | Implement | Implement | Implement | Implement within verified limits |
| Portable compute | Compute dispatch, storage buffers/textures within format/access limits | Query and implement | Query and implement | Query enabled limits/formats | Unsupported |
| Indirect rendering | Indirect draw/dispatch and specified argument layouts | Query each operation | Query each operation | Query each operation and optional features | Outside the core baseline |
| Native advanced | Descriptor indexing/bindless, mesh shaders, ray tracing, multiple physical queues, heap aliasing, external sharing | Optional independent paths | Only individually verified capabilities | Only individually verified enabled features | Unsupported unless a specific extension is deliberately added |

This target table includes operations beyond the shipped profiles. Portable
raster, lifetime services and ordered raster graphs run on all four backends;
buffer compute and indexed indirect draws run on Vulkan, Metal and WebGPU.
Neither storage textures nor indirect dispatch
is enabled today; the rendering guide owns the exact per-profile restrictions.
No operation is enabled merely because a latest spec
or a proposal mentions it. The pinned browser C API and real runtime must expose
and pass its conformance case. WebGPU is not restricted by Ludus's WebGL baseline;
compute renderers remain available on suitable devices.

Formats need a matrix of sampled/filterable/renderable/blendable/storage/copy
uses, sample counts, dimensions, and compatible view formats. Query and validate
limits for each profile: bindings per stage, uniform range/offset alignment,
vertex attributes/stride, attachment count, texture dimensions, and copy layout.
Do not hardcode desktop counts or promise all float attachments are blendable.
A renderer chooses its format/effect variant before creating the frame plan.

Portable raster deliberately constrains several API mismatches:

- A sampled `TextureView` uses the original format/dimension and complete
  supported sampling range. An attachment view selects a legal mip/layer.
  Independently restricted sampled ranges and format reinterpretation are
  capabilities; WebGL 2 must not fake independent views by changing shared
  texture base/max-level state between incompatible bindings.
- Vertex formats, topology, index types, instance attributes, and sampler
  behavior have a reviewed portable set. Base-vertex and first-instance behavior
  are explicit capabilities. The initial portable draw contract uses zero
  values, mesh-local indices, and deliberate buffer slices, rather than an
  invisible CPU reindexing path.
- Backend copy restrictions, texture pitch/block alignment, and format
  compatibility are checked. An optional raster conversion is a declared GDI
  pass, not a hidden promise that every copy operation works everywhere.
- WebGL 2 general compute is unsupported. An ocean or particle effect can choose
  a raster ping-pong implementation or a CPU implementation; these are authored
  renderer variants with their own costs. Do not simulate compute/indirect work
  through transparent synchronous readbacks.

The current [WebGL 2 specification](https://registry.khronos.org/webgl/specs/latest/2.0/)
is an API constraint reference. Extension availability and the pinned Emscripten
surface must be checked during implementation; do not treat experimental WebGL
compute specifications as the deployed WebGL 2 baseline.

## Resource and command contracts

Expose only the concepts needed by real consumers. This table describes target
concepts, not exact public symbol names. Current recording uses `CommandBatch`
and `OrderedGraph`; there is no general public `CommandEncoder` interface.

| Concept | Contract |
| --- | --- |
| `Device` and optional `Surface` | Explicit owner/session; surface images are borrowed frame attachments |
| `Buffer`, `Texture`, `TextureView`, `Sampler` | Immutable description and legal usage; mutable contents through scheduled writes/copies |
| `Shader` and `BindingLayout` | Validated target artifact and explicit layout identity |
| `BindingSet` | Immutable resource/range snapshot retaining its dependencies |
| `GraphicsPipeline`, optional `ComputePipeline` | Immutable shader/layout/state combination, independent of particular uniforms |
| `CommandEncoder` and pass encoders | Single owner, explicit begin/end, retained resource references, finish or discard |
| `SubmissionToken`, `UploadTicket`, `ReadbackTicket` | Explicit asynchronous state, owner identity, cancellation/loss semantics |

The old `PipelineDescription.Uniform` coupling ends at the compatibility facade.
A new pipeline specifies vertex inputs, topology, raster/depth/stencil/blend
state, sample count, attachment formats, layout, and shader versions. It does
not own a particular material's data. Viewport, scissor, and declared dynamic
buffer offsets are encoder data; target dimensions are not a pipeline key unless
an explicit specialization makes them so.

The target error vocabulary below describes distinctions the APIs must preserve;
individual headers define their actual status enums and output contracts.
Calls return an explicit status/result, with bounded diagnostics where supported.
Creation accepts a null output handle; Ready/Pending creates exactly one owned
incarnation, while validation/admission failure leaves it null and rolls back
private reservation. A published Pending incarnation consumes its generation
on cancellation/failure. No output overwrites a previously owned resource.
Separate Unsupported, InvalidDescription, InvalidHandle, InvalidState,
OutOfMemory, BudgetExceeded, Pending, and DeviceLost.

Descriptions and borrowed input bytes are consumed/copied before the call
returns, or the API takes an explicit ownership lease. A Pending result never
borrows the caller's stack, string, span, or editor memory. Callback storage is
bounded and engine-owned. Capacity exhaustion and integer/alignment overflow
are checked before creating partial backend state. Shipping checks retain
identity, range, capacity, and device-state validation; deeper hazard reporting
is a development layer. Diagnostics use `LUDUS_LOG_*` and cannot affect success.

Each asynchronous request carries its device owner, request/resource generation,
and operation identity. Completion publishes into a pre-reserved request record;
the owner drains it without reentrant device mutations. Configure or synchronize
the pinned API's callback mode explicitly, including spontaneous delivery.
Request bookkeeping outlives callback invocation, and stale delivery still
releases any callback-owned native result according to backend rules. Exhaustion
is rejected before launching the request; a mandatory completion cannot be
dropped like optional telemetry. Shutdown invalidates identities before teardown
and does not assume that cancellation prevents a late callback.

An encoder is a simple state machine:
`Recording -> Finished -> Submitted -> Retired`, or `Recording/Finished ->
Discarded`. It cannot be submitted twice in the initial contract. Nested passes,
commands in the wrong pass type, operations after finish, and failed-resource
binding return errors. GDI preflights declared plans and complete immutable draw
packets before execution, including WebGL requirements and bindings. Expert
direct calls validate each operation before issuing it. A later invalid direct
operation does not roll back earlier accepted WebGL calls; that path reports its
partial-work/failure state explicitly.

## Identity and physical lifetime

Use the following runtime identity rules for new public handles:
a typed owner token plus slot and non-wrapping generation, with null distinct
from a valid slot. Owner denotes the device registry lifetime, not its pointer.
Existing fullscreen IDs remain compatible. UUIDs identify content; they are not
GPU resource lookup keys. Keep hot slot metadata separate from diagnostic names
and creation records. Return IdentityExhausted/CapacityExceeded; never wrap or
silently evict a live slot.

Logical destruction prevents new handle resolution immediately. Physical
release requires **both** no retained CPU references and completion of all GPU
uses. A built packet, finished encoder, pending submission, binding set, and
readback request can retain a resource record before any submission token
exists. Therefore last submitted fence alone is insufficient. Each retaining
owner releases its references on discard, completion, or cancellation. Avoid
cycles: a view retains a texture, a binding set retains resources/layout, and a
pipeline retains its shader/layout; none retain their consumer.

An already accepted encoder keeps its retained records even if public ownership
is destroyed. Destroy cannot add new encoding through the stale handle. Handle
slots may be recycled only after their incarnation is safely detached and its
generation advanced; detached backend records/storage remain unreusable until
all leases and completion obligations end. The reference implementation can
retain the slot until physical retirement for simpler debugging.

On submit acceptance, assign an owner/queue-scoped monotonic `uint64` ordinal.
Its progress state is Pending, Complete, Failed, or DeviceLost; zero/foreign
owner is invalid, and ordinal exhaustion is explicit. A higher Complete ordinal
covers earlier accepted work on that ordered queue. Multiple physical queues,
when added, require one last-use token per queue unless a proven wait establishes
completion dominance. A displayed frame number, CPU fence, or queue-submit
return is not GPU completion.

| Backend | Initial completion lowering | Important limit |
| --- | --- | --- |
| Vulkan | Existing fences mapped to ordinals; timeline semaphores only when negotiated | Command/descriptor pools and staging cannot reset before completion |
| WebGPU | Queue work-done callbacks, coalesced at submission boundaries, generation-checked | Completion is asynchronous; buffer mapping has its own success/state |
| WebGL 2 | Sync after a submitted batch; zero-timeout polling on later event-loop turns, explicit flush where needed | Do not spin, wait with long timeouts, or substitute `glFinish` |

WebGPU's queue and mapping semantics are specified separately. A mapped
readback buffer proves that buffer is ready, not global queue completion. A
render pass has one usage scope, including resource-binding commands; do not
assume that only the final draw's active resources participate.
[WebGPU synchronization, mapping, and queues](https://www.w3.org/TR/webgpu/).

Presentation has a separate surface-generation obligation. Vulkan submission
completion does not establish present completion. Preserve the current per-image
semaphores and maintenance1 presentation fences during resize/shutdown;
timeline semaphores do not replace WSI binary acquire/present synchronization.
The [Vulkan swapchain semaphore guide](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)
explains this distinction. Browsers control compositor presentation, so report
GPU completion and browser presentation availability separately; do not invent
an exact display-complete token.

On loss, invalidate device/surface generations, fail/cancel pending tickets,
stop new work, and run backend loss teardown. Do not convert failed work to
successful completion to satisfy a recycler. Content may recreate from recipes
in a fresh session, with fresh handles. Normal new-device shutdown can be
requested and polled while browser events continue; a main-thread destructor
must not wait for callbacks that require that thread. Preserve existing
`Shutdown` behavior while adding the new lifecycle, and keep the canvas/window
alive until the relevant teardown contract finishes.

## Memory uploads and readbacks

CPU allocation follows [Foundation memory policy](memory-management.md), using
existing fallible `Array`/`StaticArray` seams and explicitly reserved storage.
GPU allocation belongs to RHI. GDI manages budgets and schedules, without
assuming that FoundationMemory arenas are already implemented. Frame storage
resets only after its CPU packet jobs and GPU storage leases have retired.

General Vulkan resources already use a **pinned private VMA 3.3.0 adapter**.
Keep that allocation boundary rather than extending the legacy frame-target
allocation-per-resource helper into a second allocator. VMA returns explicit
errors and does not require
C++ exceptions; its implementation belongs in one private translation unit.
Respect memory requirements, dedicated allocations, heap budgets, and
non-coherent flush/invalidate alignment. Host allocation callbacks require a
separately verified Foundation-compatible failure/alignment contract.
[VMA integration](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/quick_start.html),
[Vulkan allocation guidance](https://docs.vulkan.org/guide/latest/memory_allocation.html).

Do not put VMA handles/heaps in the common API. WebGPU/WebGL implementations
manage their own memory; expose logical sizes and estimates, marking physical
heap telemetry unavailable. Report requested bytes, logical live bytes,
allocated backing, retired-but-pending bytes, staging/readback bytes, and
backend-known budgets separately. Do not add suballocated payload to backing
and call the sum GPU memory usage. Streaming admission/eviction priorities
remain content policy.

Upload service accepts owned source blocks or immediately copies borrowed data
into a bounded staging ring. It schedules early, returns an `UploadTicket`, and
publishes destination use plus dependency information for pass imports. Distinct
states describe source bytes consumed, copy queued, and destination usable.
A consumer on the same queue can use a correctly ordered upload before host
completion; an asynchronous queue consumer needs an explicit wait/ownership
transfer. Recycle CPU staging only when the backend has consumed it or the GPU
copy completes, according to the actual API contract.

For small dynamic data, allocate aligned slices from retained upload blocks:
persistently mapped Vulkan memory where suitable, ordered WebGPU writes, and
WebGL uniform/vertex uploads to unoccupied ranges. Reuse binding objects with
dynamic offsets where legal; do not allocate a new descriptor for every object.
Validate maximum bound range separately from overall buffer size. Ring pressure
returns Backpressure or a budgeted growth request at a safe point; it never
silently overwrites in-flight bytes or grows in a warmed draw loop.

Keep streaming uploads **outside the transient render graph** initially. Import
one ready resource/dependency per consumed result; do not create one graph node
for every streamed block or hidden descriptor edit. This is a refinement from
Kapoulkine and Moore in the [reference review](rhi-gdi-gems-review.md).

Readback service schedules into staging, returns a ticket, and yields an owned
CPU result after completion and any required mapping/invalidation. Vulkan uses
host-readable staging; WebGPU uses copy-to-map-read staging; WebGL uses a pixel
pack/copy buffer and sync where supported, then retrieves bounded data. Browser
mapping remains asynchronous. Copy-out itself can cost time even after a fence.
No synchronous per-draw queries or readbacks in normal renderer behavior.
Cancellation removes the consumer but keeps physical staging retained until safe.

## Bindings shaders and pipelines

The [render graph authoring proposal](render-graph-authoring.md) adds an offline
graph/schema compiler and editor workflow above this boundary. It emits explicit
uses, reflected layouts and finite pipeline recipes through the existing graph
and pipeline-request seams. Shader configurations belong to the asset author;
resource connectivity cannot manufacture permutations. RHI retains sole ownership
of handle lifetime, hazard lowering and accepted-submission state.

Use explicit layouts and immutable binding snapshots. A recommended raster
scheme groups View/Frame, Material, and Draw data by update frequency, with
Draw data in uniform slices. These are renderer conventions, not three mandatory
sets in every pipeline. Compute and post-effects can have smaller purpose-built
layouts. Generated target mappings flatten logical groups to WebGL uniform
binding points/texture units and map them to Vulkan sets or WebGPU groups.
Sampled texture/sampler pairs must have a verified GLSL lowering; unsupported
pairing patterns fail the package build. Count actual per-stage requirements.

Keep frame descriptor pools separate from long-lived material sets on Vulkan.
Batch allocation/update work, bulk reset frame pools on completion, and preserve
binding/layout compatibility across pipelines. Never patch in-flight descriptors
or reuse a binding slot based only on a frame index. Material edits and texture
replacement publish a new binding snapshot; old packets use the old retained
version. Shader hot reload similarly builds new pipelines and atomically
publishes a ready compatible version at a frame boundary.

The WebGL backend owns a context-local shadow cache for program, VAO, framebuffer,
texture/sampler units, uniform ranges, blend/depth/raster state, and dirty uploads.
Apply required state at a draw boundary and eliminate identical backend calls.
Use indexed slots/revisions, not a heap-allocated observer per uniform. Reset
known state on context recreation and invalidate touched cache categories if an
explicit foreign integration is ever allowed. Do not query GL to reconstruct
state per draw. **Large uploads still schedule before drawing**; state deferral
must not delay the bulk transfer work that should overlap GPU execution.

Keep the pinned offline shader chain: Slang to SPIR-V/WGSL and macOS MSL 2.3,
plus SPIR-V through SPIRV-Cross to GLSL ES 3.00. General packages add versioned
schema, source/tool hashes, target/profile/features, explicit entry names,
resource bindings/access, vertex/fragment interface, and layout identity. Native consumers do not need
host compiler tools at runtime. The helper already supports bounded raster and
buffer-compute reflection alongside the legacy one-uniform profile. Broader
package forms must continue rejecting every unverified layout.

Reflect the selected target and validate emitted artifacts, not only source
language declarations. Generate host upload structures/static layout checks or
explicit serializers; use a common layout only when proven identical across
targets. Names are authoring/debug metadata. Encoding uses numeric bindings and
validated layout IDs, with no shader-source parser or string lookup per draw.
Slang distinguishes types from their target/context-dependent layouts.
[Slang reflection documentation](https://shader-slang.org/slang/user-guide/reflection.html).

Pipeline requests are deduplicated by canonical field-wise keys containing
shader content/version, layout, fixed state, vertex interface, attachment formats,
samples, and specializations. Hash collisions compare the complete key. Do not
hash object pointers, padding, or a backend struct dump. A renderer draw packet
already holds a ready pipeline handle; it does not rebuild a key and create a
pipeline during draw encoding.

Prewarm the finite technique variants required by installed content and graphics
settings, with a manifest of missing variants. Vulkan creation can run in
budgeted worker jobs with correct private cache synchronization; WebGPU uses
asynchronous creation when the pinned C API supports it; WebGL compiles on the
context owner and may use a verified parallel-compile extension. Offline source
compilation does not eliminate runtime driver pipeline compilation.

A Pending pipeline yields a predeclared compatible error/fallback technique or
an explicit skipped effect. A Failed required pipeline stops that content path
with diagnostics, never backend Auto fallback. The fallback must also exist and
be ready. Persist native driver caches only with validated identity/version and
bounded input; reject incompatible/corrupt caches and rebuild. Keep portable
technique manifests separate from driver blobs. Limit permutations before trying
to solve their explosion through more compilation threads.

## Pass declarations and deterministic compilation

The shipped [R3/R4 contracts](../development/fullscreen-rendering.md#ordered-raster-graphs-r3)
use complete retained packets and conservative whole-texture/whole-buffer hazards.
R10 implements selected attachment initialization and bounded offscreen resolves;
subresource hazard tracking, copy passes and physical aliasing remain target extensions. `CreateGraphTexture` acquires from a bounded whole-object
pool during setup; compilation validates and reports resource lifetimes, rather
than allocating transient storage from those intervals.

The first GDI execution API is an **ordered pass list with declared resources**.
That list is compiled as a graph for validation, dependencies, culling, and
lifetimes; no heuristic reordering is necessary. A renderer writes the intended
order. Reading a version produced only by a later pass is an error, not permission
to silently move the pass. Independent passes can overlap on the GPU where legal
without changing their authored CPU order.

Each pass has a stable name/source location, queue class, immutable owned packet,
explicit side effects, and resource uses. Buffer uses declare ranges; texture
uses declare mip/layer/aspect. Access modes distinguish read, write, read/write,
and relevant stage visibility: uniform, sampled, vertex/index, attachment,
storage, transfer, indirect, and present. Track hazards at actual backend
granularity; conservative whole-buffer tracking is acceptable initially, but
must be visible as such in diagnostics.

A logical compute pass initially contains one dispatch. Multiple dependent
dispatches require separate passes, or a later explicit internal use schedule
with synchronization; a broad StorageReadWrite declaration does not establish
ordering between them. Shader-local data races and workgroup barriers remain
kernel correctness responsibilities. Optional raster storage writes similarly
cannot rely on ordinary attachment draw order to synchronize arbitrary buffers.

Raster attachments specify format/view, extent, samples, Clear/Load/Discard,
Store/Discard, optional resolve, and depth/stencil read-only behavior. Every read
requires defined prior contents. Load requires a valid import/producer; discard
or partial write does not establish full initialization. These declarations
allow bandwidth savings, and the backend supplies required security
initialization rather than exposing undefined memory. Resolve is an attachment
operation where supported; do not always store a whole MSAA image and resolve
it later. Public subpasses are unnecessary for the first API.

Compilation proceeds through one inspectable implementation:

1. Validate requirements, handles/leases, descriptions, declared actual bindings,
   pass types, attachments, and capacity before executing anything.
2. Create logical resource versions. Read/write in-place access carries explicit
   RAW, WAR, and WAW dependencies against the same physical resource. A new
   version does not automatically allocate a new texture or remove hazards.
3. Verify producers and authored ordering. Reject incompatible same-pass uses,
   illegal feedback, cycles, and references to discarded/uninitialized contents.
   Reflection supplies binding types/visibility; it cannot discover every shader
   runtime access, so author declarations stay authoritative.
4. Mark roots: present, exported/history resources, readbacks, and explicit side
   effects. Cull only passes that contribute to none. Future-frame history writes
   are observable roots, even if this frame does not sample them.
5. Compute first/last uses and validate retained transient objects. The current
   setup-time pool matches complete descriptions and gates reuse on completion.
   Interval-driven allocation or heap aliasing would be separate extensions;
   fresh/aliased storage starts undefined.
6. Produce semantic dependencies and a backend lowering plan, including upload
   imports, final uses, presentation, and cross-execution obligations.
7. Encode and submit, transferring leases to submission records. Commit the
   RHI's persistent state ledger only for accepted work. If execution has begun and
   fails, do not pretend that a partial plan rolled back; fault affected state or
   the session as required by backend error semantics.

No per-pass owning lambda may outlive captured stack references. Begin with
plain execution functions plus frame-owned packet data, explicit retention, and
an encoder limited to resources declared by that pass. Development validation
compares actual binds/commands against declarations, including extra WebGPU
bindings that contribute to its usage scope. Resolve handles once per packet
where possible, retaining all public boundary checks.

Packets expose their complete bindings and draw descriptions for preflight.
Execution functions traverse those validated packets; commands that need
additional resources or capabilities require rebuilding the plan. Backend
runtime errors can still happen after preflight and follow the partial-failure
rules above; validation is not a promise of transactional GPU execution.

Imported persistent resources carry known content validity, initial/final uses,
owner and pending dependency tokens. One owner-managed ledger connects graph
executions and direct submissions. A same-queue completion dependency can be
lowered without a host wait; native queue ownership still needs appropriate
barriers. History resources and double-buffered render targets are allocated
against completion, not `frameIndex % 2` alone. Plan-cache reuse is optional;
if later added, its key includes topology, requirements, layouts, attachment
compatibility, and external-state contracts, not just resolution.

### Concrete portable frame

| Ordered pass | Declared inputs | Outputs and required lifetime |
| --- | --- | --- |
| Scene | Uploaded geometry, View/Material/Draw bindings | Linear scene color cleared/stored; depth cleared and retained only as needed |
| Transparency | Scene color and depth as attachments, transparent packets | Same color loaded/stored; read-only depth; painter/depth ordering preserved |
| UI | Scene color as attachment, UI textures/packets | Linear color loaded/stored; painter order and linear blending preserved |
| Composite | Scene color sampled, acquired surface | Surface cleared/written with exactly one output encoding and handed to presentation |

Vulkan inserts the color-write to fragment-sample dependency and legal layout
transition. WebGPU closes the first rendering scope before sampling scene color
in a new pass. WebGL changes framebuffer and samples the offscreen texture in a
legal ordered draw. The graph exports why each dependency exists. It never
samples a texture while attaching it for conflicting write access. A compute
post-effect is a different selected variant, not part of the WebGL plan.

## Backend lowering and optional optimization

Semantic dependencies carry producer/consumer access, stages, subresources,
and execution placement. They are not Vulkan enums renamed into engine types.
Only the backend translates them to actual synchronization. Batch compatible
dependencies at the same boundary without broadening their scope unnecessarily.
RAW/WAW generally need memory ordering; WAR may need execution ordering only.
Queue submission order alone is not a complete Vulkan memory dependency.
[Vulkan synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html).

| Backend | Lowering rule |
| --- | --- |
| Vulkan | Maintain one tested semantic mapper. Retain the Vulkan 1.1 barrier/render-pass path; negotiate synchronization2, timeline semaphores, and dynamic rendering as independent optional paths. Preserve narrow stages, correct layouts, queue ownership, load/store and WSI retirement |
| WebGPU | Use resource creation flags, legal scopes/pass boundaries, ordered queue operations, error scopes and asynchronous status. No fake explicit barriers, heaps, or queue families in the public API |
| WebGL 2 | Apply context state snapshots, legal FBO operations, copies/uploads and batch sync on its owner. Reject unsupported commands before execution. No desktop GL persistent mapping or memory-barrier assumptions |

Choose the existing legacy Vulkan lowering initially. If adopting an optimized
Vulkan baseline later, negotiate and test newer paths deliberately, then remove
an obsolete lowering only through a supported-device policy change. Permanent
duplication of two unmeasured implementations is a maintenance cost.

Transient pooling reuses equivalent resource objects after all uses finish.
Native heap aliasing is a separate later feature: match memory requirements and
resource classes, order old/new users, issue backend-required alias transitions,
and initialize the new contents. Lifetime intervals on a CPU pass list are
insufficient once queues overlap. WebGPU/WebGL can pool objects without claiming
control over physical allocation aliasing.

The shipped [R5 merging contract](../development/fullscreen-rendering.md#measured-pass-merging-r5)
is explicit opt-in through `GraphCompileOptions::MergeRasterPasses`; the default
keeps logical raster passes separate. It merges only compatible consecutive
passes while preserving draw order and legal resource scopes. Broader merging
with resolve or other attachment forms needs separate correctness and measurement
proof. One logical pass is not
necessarily one submission. Start with one graphics submission per rendered
frame plus budgeted upload work; split to feed the GPU sooner only when latency
and utilization measurements justify it. No fixed historical millisecond,
command-buffer, or submit count from a book is a shipping budget.

Renderer packets are compact contiguous data with stable pipeline/binding/mesh
references. Sort opaque objects within the declared pass by pipeline/material
with suitable depth buckets; preserve transparency, UI painter order, queries,
and side effects. Batch adjacent compatible UI draws and instanced geometry.
Add GPU culling/indirect submission as a compute-capable renderer path; it still
buckets by pipeline and retains direct raster fallbacks.

## Threads surfaces and rendering conventions

One owner thread controls startup, surfaces, submission, state-ledger updates,
loss, and retirement. It remains the browser main/context thread initially.
Jobs can extract immutable scene packets, prepare uploads, and request pipelines
through bounded queues; publication establishes CPU synchronization, separately
from GPU completion. Creation mutations do not race frame encoding.

Native parallel recording is optional. The compiler fixes execution order and
initial states before jobs start; each worker receives a disjoint packet and
private recording pools per completion domain. Vulkan command/descriptor pools
are never concurrently mutated or reset before GPU completion. Join finished
buffers in deterministic order and submit through the owner. Small passes remain
serial. Measure worker idle time, command memory, join cost, and input-to-display
latency as well as throughput; extra buffering is not automatically a win.

A surface publishes acquired extent, format/encoding, sample count, transform,
and generation. Surface attachments cannot escape their acquired frame. Resize
invalidates extent-dependent pools/plans and reacquires images; pipeline rebuild
is required only for format/sample/layout/specialization changes. Preserve the
current facade's restart-on-format-change contract until its replacement is
implemented and documented. Suspended/zero-size surfaces skip acquisition and
drawing; they do not require zero-size GPU textures. Browser composition clears
or replaces drawing buffers, so history uses engine-owned textures.

Choose canonical shader clip coordinates with Y up and depth in `[0, 1]`, and
pixel rectangles with a top-left origin. WebGL lowering converts depth to its
clip range; viewport/scissor/winding and render-target sampling transforms are
explicit per backend. Do not blindly flip every texture or double-transform
compiler-adjusted output. Imported image origin is package metadata. Verify
ordinary sampling, render-to-texture sampling, front face, depth, readback row
order, and viewport/scissor through diagnostic scenes on every backend.

Keep linear light through scene rendering/blending. Declare texture encoding,
attachment conversion, output color space, and canvas alpha/premultiplication
separately. An sRGB presentation color-space label does not make a UNORM
attachment blend in linear light. For a browser UNORM final target, composite UI
into the linear offscreen color and encode once in the final output shader;
otherwise blending UI after encoding would be wrong. Use a verified sRGB
attachment conversion when available, with no duplicate shader encoding. HDR,
wide gamut and surface transforms are future negotiated output policies, not
implicit consequences of backend selection.

## Debugging and measurement

Every resource, pipeline, pass, encoder, upload and submission gets a stable
numeric identity and optional bounded name/source context. Report the offending
pass, handle owner/generation, actual versus declared use, prior writer, format,
layout, and completion obligation. Store native errors alongside engine status
rather than flattening every failure to a generic rendering error.

The current `GraphReport` exposes authored/culled passes, dependency reasons,
resource versions/lifetimes, raster groups and logical plan/buffer/texture byte
counts. These are not driver allocations or physical backing estimates. The
[R5 guide](../development/fullscreen-rendering.md#measured-pass-merging-r5) owns
report definitions and measurement limits.

Extend the readable execution report as needed with lowered barriers, binding
layout, pipeline
keys, pool assignments, upload/readback traffic, pending retirements, and
surface generation. Normal logging and profiling may drop optional records
under pressure; record diagnostic loss and never crash device work because a
name, capture, or telemetry allocation failed. Do not build a custom capture
viewer before existing logs/artifacts answer the needed questions.

Current `GraphCompileOptions` independently controls culling and merging;
serial recording, one physical queue and no heap aliasing are structural bounds,
not additional runtime switches. Future investigation modes can include
fresh transient resources and disabled state-cache elision. All modes still produce valid dependencies and initialization.
A graph dump maps execution functions back to setup sites. Debug changes can
hide timing or lifetime bugs, so compare the original optimized plan as well.
Epic's [RDG debugging documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)
illustrates why deferred execution needs those controls; this design adopts the
principle without requiring Unreal's framework.

Use Vulkan validation including synchronization validation, GPU debug markers,
and RenderDoc/vendor captures where the actual platform supports them. Browser
validation/error scopes, program logs and extension-based timers complement
image tests. GPU timestamps are optional: report clock domain/availability and
browser precision, never substitute CPU encoding time or zero for unavailable
GPU duration. Do not compare unrelated queue clocks without calibration.

## D3D12 Metal and compute expansion

The current macOS Metal backend implements the same bounded device, raster,
lifetime, ordered graph, buffer-compute and merging profiles as Vulkan, within
its enabled capabilities. The [public rendering guide](../development/fullscreen-rendering.md#macos-metal)
owns setup and platform limits. D3D12 remains a future backend for those contracts.
Their binding systems, residency, heaps, native barrier choices, command allocator
reuse and completion mechanisms stay private. Future backend conformance must prove all common semantics before the renderer uses it.
D3D12 allocator reset requires completed GPU uses, and Metal resource
synchronization depends on its selected API and hazard mode; portable dependencies
remain meaningful in both.
[Microsoft allocator lifetime](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandallocator-reset),
[Apple resource synchronization](https://developer.apple.com/documentation/metal/resource-synchronization).

CUDA and other compute infrastructures are **future execution domains**, not
raster backends that implement empty graphics methods. Keep the current API
surface-free where possible, but do not implement a universal GPU interface now.
When a compute consumer arrives, extract the genuinely shared buffer/range,
capability, lifetime and completion vocabulary into a small common module;
implement kernel artifacts/arguments/streams in its compute executor. There is
no promise that a Slang raster package automatically becomes a CUDA kernel, or
that CUDA and graphics workloads share storage merely by running on one GPU.

The [native texture sharing contract](../development/fullscreen-rendering.md#native-texture-sharing)
provides retained same-logical-device, same-queue color reads and admission of an
existing Metal/Vulkan device. It is independent of external compute and does not
complete cross-device external memory/semaphores or R14's multi-device scope.

A later GDI external-compute node imports declared inputs/outputs and completion
obligations. Native zero-copy interop is a separate bridge requiring matching
physical-device identity, compatible export/import handle types, layout/format,
allocation/dedicated requirements, OS handle ownership, semaphore protocol,
and explicit release/acquire of ownership. Keep resources retained across both
APIs. Unsupported interop uses an explicitly scheduled host copy or an alternative
kernel path, with its cost visible. Browser WebGPU/WebGL have no ordinary
CUDA memory-import path to expose. NVIDIA documents device UUID matching and
external memory/semaphore interoperability.
[CUDA interoperability](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/graphics-interop.html).

## Implementation phases and acceptance

All initial bounded profiles are implemented. Phase completion does not claim
that every broader target contract in this document is available.

| Phase | Shipped baseline | Contract and acceptance owner |
| --- | --- | --- |
| R0 Contracts | Copied requirements, explicit device/target owners, required/preferred workload admission, reference lifecycle validation, copied enabled profile/format constraints and compatibility facade | [Explicit device ownership](../development/fullscreen-rendering.md#explicit-r0-device-ownership); general slot/generation identities arrived with R1/R2 |
| R1 Portable resources | Typed resources, reflected immutable bindings/pipelines and indexed/instanced raster; renderer L1–L3 extend attachments, depth and sampled formats | [Portable raster](../development/fullscreen-rendering.md#portable-raster-r1) and linked renderer extensions |
| R2 Lifetime services | Completion tokens, retained batches/records, buffer upload/readback rings, private Vulkan allocator and canonical pipeline requests | [Lifetime services](../development/fullscreen-rendering.md#lifetime-services-r2) |
| R3 Ordered graph | Authored order, in-place versions, RAW/WAR/WAW reports, roots/culling, completion-gated object pool and attachment lowering | [Ordered graphs](../development/fullscreen-rendering.md#ordered-raster-graphs-r3) |
| R4 Buffer compute | Single-queue storage buffers, reflected dispatch, indexed indirect draw and authored CPU/direct variant | [Compute and indirect rendering](../development/fullscreen-rendering.md#buffer-compute-and-indirect-rendering-r4) |
| R5 Measured optimization | Opt-in raster merging, independent culling/merging controls, paired benchmark and image comparison | [Measured pass merging](../development/fullscreen-rendering.md#measured-pass-merging-r5) |

### Baseline and evidence record

The initial milestone series ends with [R5, PR 189](https://github.com/The-Ludus-Project/Ludus/pull/189),
merged at `859d02d`; its evidence reconciliation used repository revision `5659894`.
[R0 negotiation, PR 201](https://github.com/The-Ludus-Project/Ludus/pull/201), subsequently
merged at `fb6842c`, adds copied enabled limits, format/use constraints and
required/preferred admission. The follow-up roadmap was assigned against comparison revision `0344d67`;
current statuses and evidence are recorded below. R6 was merged in
[PR 215](https://github.com/The-Ludus-Project/Ludus/pull/215); R7 in
[PR 216](https://github.com/The-Ludus-Project/Ludus/pull/216). R8 is submitted in
[PR 219](https://github.com/The-Ludus-Project/Ludus/pull/219).
R5 source revision `e4a6b7a83ef9a1572e77c92af55ca57c681c5b98` passed all 28
executed CI checks, including native/browser builds, sanitizer and static-analysis
checks, installed/relocated SDK consumers and packaged browser rendering tests.
The [device lifecycle tests](../../modules/graphics/rhi/tests/device_tests.cpp),
[raster tests](../../modules/graphics/rhi/tests/raster_tests.cpp),
[render tests](../../modules/graphics/rhi/tests/raster_render_tests.cpp),
[compute tests](../../modules/graphics/rhi/tests/compute_tests.cpp) and
[installed raster consumer](../../tests/sdk_consumer/raster.cpp) are reproducible
acceptance sources, not substitutes for target-hardware qualification.

The [rendering guide](../development/fullscreen-rendering.md#paired-native-measurements)
owns diagnostic workloads, reproduction commands and the paired R5 measurements.
Use the toolchain pins in `config/tool_versions.json`, `config/web_toolchain.json`,
`config/shader_toolchain.json` and `config/spirv_cross_toolchain.json` at the
comparison revision. The headless Metal runs establish a workload-specific CPU
benefit with identical full images and logical byte counts. GPU duration,
input-to-display latency and driver allocation/backing measurements are
unavailable and recorded as such. Software Vulkan/browser correctness evidence
does not establish physical GPU performance or complete Wayland/hosted-iframe
qualification.

This bounded baseline is ready for selected trials from the
[research queue](rhi-gdi-gems-review.md#research-after-the-initial-implementation).
Before each trial, pin its comparison revision, backend/profile, workload and
reproduction command; retain the original path and report unavailable measurements
explicitly. Reading a source does not adopt it. Adoption needs a consumer,
correctness evidence and measurements relevant to the proposed benefit, with
maintenance cost assessed. API/specification checks remain normal implementation
work; the queued conference/journal sources are not prerequisites for this baseline.

### Remaining work and decision gates

R0 capability, format and limit negotiation is implemented. The remaining
one-device/surface scope, resource table bounds and restricted operations are
assigned below. R6 implements configurable, budgeted storage, and R7 provides
measurement and qualification tooling with explicitly open host acceptance.
R8 adds mutable transfers and incremental initialized mip primitives; partial
sampling is supplied by implemented R9a; R9 also supplies its documented texture
types and formats. R11 adds the bounded binding and table profiles below; R12–R17 retain their
separately tracked subprofiles, and R18 is implemented. They extend the shipped
R0–R5 profiles rather than reopening their original acceptance.

<a id="follow-up-milestones-r6r17"></a>

#### Follow-up milestones R6–R18

This is the canonical RHI/GDI milestone assignment. The scope is device APIs,
GPU resources, shader interfaces, command execution and graph services. Scene
import, asset decoding, materials, visibility algorithms, animation, lighting,
and path-tracing estimators belong to their existing owners. Low-level tests
may supply prepared scene-sized geometry, textures and command streams without
requiring those higher-level systems to be implemented first.

| Milestone | Deliverable | Dependencies | Acceptance gate |
| --- | --- | --- | --- |
| **R6: scalable resource and plan storage (Implemented)** | Replace fixed general-resource/backend tables and GDI graph/batch storage with stable, budgeted capacity. Negotiate configured maxima before Ready; grow backing between frames, without changing published limits or allocating during command execution. Keep the fullscreen facade's separate compatibility bounds. | R0/R1/R2/R3 | Exceed the historical 64 records per kind, 16 resources/32 passes per graph, 256 draws per retained batch/graph and 1024 draws per frame in independently budgeted fixtures. Include at least 72 small resident textures and 405 prepared draw packets. Test stable handles through growth, allocation rollback, checked capacity arithmetic, generation exhaustion, pending/detached/in-flight retention and loss/restart. Optional RT0 now uses stable pages with independently configured ray resource and instance counts; [renderer L5.1](renderer-systems.md#l51-budgeted-scene-capacity) owns its integration and capacity evidence. Counts prove capacity, not full reference-scene performance. |
| **R7: GPU measurement and qualification (Measurement/workflow implemented; host gates open)** | R7a: optional GPU timing and actual allocation/retirement telemetry, with logical, API-allocated and physical/resident bytes distinguished. R7b: named physical-GPU and host/surface qualification records. R7c: prepared reference-scene resource/command qualification, including large textures, hundreds of draws, thousands of instances and measured peak memory. | Existing R2/R3/R5; extend telemetry for each later milestone | Query lifecycle, clock domains, precision/disjointness, reporting overhead and unavailable results are explicit. CPU acquire/encode/submit time is not GPU duration or display latency. Qualification records pin hardware, workload and enabled profile, include resize/loss and selected Wayland/hosted-browser journeys, and report measured CPU/GPU distributions and peak memory. Unavailable physical residency/display observations are not invented; their qualification remains open. |
| **R8: mutable transfers and residency primitives (Implemented, RGBA8 transfer profile)** | R8a: existing-buffer updates and bounded 2D texture upload/update/copy/readback. R8b: incremental initialized mip residency and staging budgets with explicit source ownership, completion and cancellation. | R8a: R6 plus existing R2/R3; R8b initialization: R8a; sampling partial residency: R9a initialized-view contracts; use R7 evidence | Round trips, offset/pitch/range checks, partial initialization, RAW/WAR/WAW and cancellation/pressure/loss tests. Mutation must reject, order or rename around retained/in-flight uses; never overwrite host-visible GPU data still in use. Reuse staging only after actual completion. Later texture types/block formats extend these checks in R9. Residency here does not promise sparse GPU memory or consumer eviction policy. |
| **R9: texture types, formats and views (Implemented, sampled color/BC profiles)** | R9a: partial sampled views/mip/layer/aspect ranges and uploaded HDR formats. R9b: arrays/cubemaps. R9c: 3D textures and compressed formats, each with explicit enabled format/use support and target reflection. | R6/R8a; partial views initially use conservative R3 hazards | Validate block-edge layouts, mip/slice bounds, cube orientation, 3D addressing, initialized sampling and retained view lifetimes. Installed SDK pixels/readback qualify every claimed backend/profile. Unsupported format/use combinations reject explicitly; array/cube/3D and compression capabilities are independent. |
| **R10: attachment extensions (Implemented, bounded offscreen profiles)** | R10a: multiple color attachments, sampled depth and stencil. R10b: MSAA/resolve. R10c: partial attachment views and extended GDI load/store declarations. | Existing R1/R3 plus R6; R9a/view aspects for sampled depth and R10c; no dependency on all texture types or transfers | Validate per-format/sample limits, attachment compatibility, resolves, depth/stencil behavior, feedback and undefined loads. Prove graph/direct image agreement, loss/resize retirement and authored alternatives. Conservatively track full-resource hazards until R13a; stencil, attachment counts and MSAA are separate capabilities. |
| **R11: scalable binding and optional bindless (Implemented, bounded binding and table profiles)** | R11a: descriptor arenas and dynamic offsets. R11b: reflected binding arrays and separately enabled descriptor indexing/bindless, with explicit reachable-resource declarations and completion-gated reuse. R11c (Implemented): wider fixed color texture/sampler/buffer descriptor kinds, separately enabled bounded runtime texture arrays, logical partially populated tables with retained fallback descriptors, and safe publication of replacement snapshots after earlier binds/submissions. Native in-place UPDATE_AFTER_BIND semantics are not claimed by this immutable snapshot profile. Each operation retains its documented backend and descriptor budgets. | R11a: R6 and existing R1/R2; R11b: R11a plus verified target reflection/capabilities; R9 only for new texture types/views | Alignment/range checks, descriptor exhaustion/rollback, in-flight replacement and stale index tests. Arrays do not imply runtime-sized/non-uniform indexing or update-after-bind. Retain all declared reachable resources; unsafe or undeclared access rejects. Compare binding CPU cost and retained memory through R7. Bindless is optional and does not remove table/memory budgets. |
| **R12: expanded compute and indirect commands** | R12a: broader indexed/non-indexed indirect arguments and indirect dispatch. R12b: raster-stage storage buffers and storage textures. R12c: optional native multi-draw/GPU-provided draw counts. Preserve R4's bounded indexed-indirect path. | R12a/R12c: R6 and existing R4/R3; R12b: R6 and target shader reflection, plus R9 only for new texture/view types; bindless and mutable transfers are not prerequisites | Validate argument ABI/alignment/count/stride and enabled offsets, producer-to-command/shader dependencies, zero work and overflow. GPU-produced command fixtures match direct draws without reading counts back to the CPU. R12c reports native support separately from CPU loops over indirect commands. Retain a bounded single-command path and authored WebGL CPU/direct alternative. Visibility/compaction algorithms remain renderer policy. |
| **R13: finer GDI hazards and transient aliasing** | R13a: buffer ranges and texture mip/layer/aspect tracking. R13b: transient backing/heap aliasing with explicit initialization and lifetime intervals. | R13a: R6 and existing R3; incorporate R8–R10/R12 uses when those profiles exist. R13b: R13a plus RHI allocation/alias capabilities and R7 memory evidence | Adversarial overlapping ranges/views, cross-frame imports, alias transitions/reuse, RAW/WAR/WAW, feedback and undefined loads. Aliasing excludes imported/persistent/in-flight backing unless ownership and last use explicitly permit it. Reject invalid plans before side effects. Compare images/synchronization with whole-resource, non-aliased execution and measure peak API-allocated memory. |
| **R14: independent devices and surfaces** | Multiple device/surface owners, raw adapter catalogs separated from enabled profiles, per-owner registries/completion and independent loss/presentation retirement. | R6 plus existing R0/R2 ownership; independent of aliasing, bindless and GPU-driven commands | Concurrent owners, stale/cross-owner rejection, independently resized/lost surfaces, pending browser callbacks and separate render/present completion. One owner's failure must not corrupt another. Preserve the single-owner facade. A separate GraphicsDevice module remains an ownership decision, not a duplicate registry. |
| **R15: parallel recording and multiple queues** | R15a: native worker-owned recording/pools, deterministic owner-thread submission. R15b: optional queue domains, async work, release/acquire and completion joins. Preserve serial/single-queue execution. | R15a: existing R2/R3 plus R6 and R7 evidence. R15b: existing R3/R4 plus R6/R7 and explicit per-queue state/completion; R8a for transfer queues. Neither requires R12, R13b or R14; R13a is an optimization for overlap, not a correctness prerequisite | Worker/pool ownership, cancellation/loss, delayed completion and cross-queue RAW/WAR/WAW under stress. Whole-resource synchronization remains a legal conservative baseline. Validate ownership transfers and wait graphs without deadlocks; retain all backing until every participating queue completes. Prove image agreement and measured join/submit cost, tail latency and overlap memory. |
| **R16: ray-tracing resource foundation** | R16a: native acceleration-structure handles, static triangle BLAS/TLAS builds, instances and scratch sizing. R16b: supported update/refit and compaction. R16c: independently supported procedural bounds and motion geometry/instances, with explicit build inputs and limits. Declare build/read/copy uses in GDI and completion-gated retirement. | R6 plus existing R0/R2/R3 buffer/lifetime contracts; R8 for mutable input journeys; R7 for qualification. No scalable descriptors, bindless, aliasing, multi-device or multi-queue prerequisite | Input role/usage, size/alignment and build/update limits are explicit. Test rollback, invalid geometry, retained geometry/scratch/instances, topology-change rejection/rebuild, stale handles, loss and compaction retirement. Initial R16a can use immutable uploaded buffers. Build/lifetime completion does not claim traversal correctness: build-to-query and hit/miss qualification are completed jointly with R17a. |
| **R17: ray queries and ray pipelines** | R17a: reflected acceleration-structure binding and compute-stage ray queries/intersectors. R17b: independently supported dedicated ray pipelines, trace commands and shader/function tables. R17c: broader query stages and procedural intersection/motion traversal, gated with R16c and target reflection. | R17a: R16a and existing R4 compute/reflection; extend ordinary R1 binding for acceleration structures. R17b: R16a and a verified backend/toolchain pipeline profile. R11, R12c, R13b and R15 are not prerequisites | Independent triangle intersection fixtures verify hit/miss, ray intervals, transforms, IDs, masks and admitted opacity behavior. Validate build-to-read synchronization, shader interfaces, table ranges, payload/recursion limits where relevant and in-flight replacement. Native query support never implies dedicated pipeline support. Test every claimed lowering; unsupported backends reject or use an authored raster alternative. Shading/denoising/path-tracing algorithms remain outside RHI/GDI. |

| **R18: reference-scene texture and resource profiles** | R18a (Implemented): original BC5_UNORM two-channel sampled D2/array/cube uploads and views, including independently negotiated native unaligned base edges. R18b (Implemented): enabled texture width/height negotiation beyond the historical 4096 ceiling. R18c (Implemented): enabled buffer/complete-upload and compute-storage ranges, sixteen-slot binding metadata with per-stage count admission, and independent backend array/volume limits. | R18a/R18b: existing R0/R6/R9; R18c: R0/R6 and existing R4/R8, with R12 only for new operations; R7c for scene qualification | BC5 block/pitch/mip/slice validation, capability rejection and retained-view lifetime; installed SDK pixels prove both channel palettes, zero blue and opaque alpha. Required/preferred dimensions use enabled device/context and per-format/use limits, not raw adapter claims. Include 8240-wide prepared textures, exact-limit/over-limit rejection and checked byte budgets without implicit resizing/transcoding. Larger limits require explicit admission, arithmetic/rollback and producer/consumer fixtures; no limit is raised solely to silence a failure. |

R6 storage and R7a measurement support are implemented; R7b physical Vulkan/Wayland
and deployed physical-browser journeys remain open. See the
[R7 usage and named qualification evidence](../development/fullscreen-rendering.md#gpu-measurement-and-qualification-r7).
[R8a transfers and R8b mip initialization/staging budgets](../development/fullscreen-rendering.md#mutable-transfers-and-mip-initialization-r8)
are implemented for the RGBA8 transfer profile. R8b initializes mips incrementally
and gates the existing full-resource view until all mips are defined. R9a adds sampling of initialized subsets through partial color views; its
[texture contract and SDK journey](../development/fullscreen-rendering.md#texture-types-formats-and-views-r9)
record the enabled dimensions, compression profile and backend limitations. R10 adds [bounded attachment extensions](../development/fullscreen-rendering.md#attachment-extensions-r10).
R11a/R11b are Implemented in [PR #239](https://github.com/The-Ludus-Project/Ludus/pull/239), with the
[binding profile and SDK/CI evidence](../development/fullscreen-rendering.md#scalable-bindings-and-optional-bindless-r11)
recording dynamic offsets, fixed native texture arrays and independently enabled non-uniform indexing.
R12a reuses R4 and R15a reuses R3 without waiting for the texture/attachment
series. R14 is a separate ownership
track. R16a/R17a provide an early native RT query track; scalable bindings,
native ray pipelines, compaction and async queues do not block that baseline.
Numbers identify scope; only the subprofile prerequisites define order.

A dependency on an interface extension does not require every optional
subprofile of that milestone. Later resource types add their own reflection,
hazard and lifetime cases to the existing tests. Fine-grained synchronization,
bindless, aliasing and queue overlap must retain conservative reference paths.
RHI exposes validated operations; GDI schedules their declared uses. It does not
silently supply renderer visibility, material, lighting or streaming-priority
algorithms.

Each named subprofile is tracked separately as **Planned**, **In progress**,
**Implemented** or **Qualified**, with PR/revision, backend/profile matrix and
evidence links. R6 is Implemented with the
[storage contract and SDK fixture](../development/fullscreen-rendering.md#scalable-resource-and-plan-storage-r6);
R7a is Implemented and R7b physical host gates remain open. R8a and R8b's
initialized-mip/staging primitives are Implemented with their
[transfer contract and SDK journey](../development/fullscreen-rendering.md#mutable-transfers-and-mip-initialization-r8);
R9a/R9b/R9c and R10a/R10b/R10c are Implemented with explicit backend profiles;
[the R10 contract and SDK journey](../development/fullscreen-rendering.md#attachment-extensions-r10)
record MRT, private aspect loads, stencil, color resolves and selected attachment initialization.
R11a/R11b are Implemented with the bounded backend matrix and correctness evidence
linked above. R11c is Implemented in [PR #266](https://github.com/The-Ludus-Project/Ludus/pull/266)
with the independently enabled table profiles documented in that same contract;
physical-host qualification remains R7b/R7c. R12–R17 subprofiles remain Planned,
except for the existing
[RT0 static native-query profile](renderer-systems.md#l6-implemented-scope-rt0-and-rt1)
overlapping R16a/R17a. That immutable native build/query path is implemented;
it does not complete their broader GDI build/read declarations or R16b/R17b's
refit/compaction and dedicated ray-pipeline scope, nor R16c/R17c motion/procedural extensions.
R18a/R18b are Implemented in [PR #242](https://github.com/The-Ludus-Project/Ludus/pull/242); R18c is Implemented in [PR #246](https://github.com/The-Ludus-Project/Ludus/pull/246). The [R18 texture contract and SDK fixture](../development/fullscreen-rendering.md#reference-scene-texture-profiles-r18) record the scoped format, dimension and byte-budget behavior. BC5 addresses ATI2 DDS data in the
reference sets; width/height negotiation addresses images as large as 8240 by
8200. R18c removes the former 64 MiB allocation/complete-upload ceiling, 16 MiB compute
storage-range ceiling and four-storage/eight-total binding profile. Enabled limits
remain independent of logical byte budgets and the bounded R8 transfer rings;
R12 retains ownership of new compute/indirect operations.
The R4 indexed indirect path still admits one command per packet with zero
FirstIndex/BaseVertex/FirstInstance; R12 owns nonzero offsets, non-indexed
commands, indirect dispatch, multi-draw and GPU-provided counts. R13/R15
optimize memory/scheduling; R14 independent owners is a separate track.
R7c remains Planned: capacity fixtures alone do not qualify a full reference
scene or establish physical-GPU performance.
Implemented means
the declared operations and failure paths pass SDK/correctness gates; Qualified
adds the named hardware/workload measurements. Unavailable evidence stays open.
An umbrella milestone cannot be marked complete by selecting only its easiest
subprofile: every named subprofile needs its own recorded disposition and all
promised operations must be implemented on their stated supported matrix.
Unsupported backend entries are explicit capability results, not silently
omitted deliverables. Optional optimizations require measured benefit before
default enablement; they need not improve every workload.

Portable raster remains supported on Vulkan, Metal, WebGPU and WebGL 2.
Compute/indirect extensions preserve the declared WebGL CPU/direct alternative.
Bindless, parallel/multi-queue execution and RT require explicit capability
selection; no milestone silently changes toolchain pins, enables unimplemented
native features or removes a supported backend. Implementation PRs specify the
backend/profile matrix, required/preferred admission, authored alternatives and
failure behavior before claiming completion.

Thanks to Khronos Group's [Vulkan Guide, Descriptor Arrays](https://docs.vulkan.org/guide/latest/descriptor_arrays.html)
and [Ray Tracing](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html)
(sections VK_KHR_acceleration_structure, VK_KHR_ray_query and
VK_KHR_ray_tracing_pipeline), and Apple's
[Ray tracing with acceleration structures](https://developer.apple.com/documentation/metal/ray-tracing-with-acceleration-structures)
for the independently gated binding/query/pipeline distinctions used here.
These are API checks for the milestone review on 2026-10-12, not adoption of a
new renderer algorithm. R17 must not impose Vulkan's shader-binding-table model
on Metal intersectors/function tables; the backend-specific package and lowering
must be verified against Ludus's pinned toolchains before support is enabled.

The earlier candidate for D3D12 or external compute interoperability remains
consumer-triggered work outside this assignment. A new backend or bridge needs
its own scope and evidence; it is not implied by R14 or the native RT track.
Research trials continue to use the [review queue](rhi-gdi-gems-review.md#research-after-the-initial-implementation)
and must retain a baseline/control path. Persistent driver pipeline caching and
other unassigned optimizations remain consumer-triggered extensions.

### Qualification targets

The following remains the acceptance checklist for new extensions and broader
shipping profiles, not a claim that every case or metric has already been proven.

The validation suite must include resource failure rollback; handle and token
exhaustion; binding replacement while old packets remain; shader hot reload with
layout changes; mapping/copy pitch/range errors; RAW/WAR/WAW transitions; persistent
cross-frame data; feedback and undefined loads; graph capacity failure before
side effects; zero-size/minimize/restore; format changes; startup fallback,
failed canvas replacement and stale browser callbacks; and separate render/present
retirement. A null/reference backend validates plans and lifecycle only; it is
not evidence of GPU correctness or image quality.

Implementation uses the pinned native tools and required warning-clean builds,
unit tests, ASan/UBSan, format/tidy, header self-sufficiency/foundational includes,
parse-budget gates, and installed-SDK consumers. Browser builds use the pinned
web toolchain. Test Vulkan headless and real Wayland, real forced browser backends
and Auto, physical GPUs and hosted HTTPS/iframe scenarios. Software GPUs and
mock callbacks are additional evidence, not substitutes for those environments.
Game-project setup/repair must also follow AGENTS.md preset/toolchain checks.

Benchmark the existing fullscreen baseline and representative textured/instanced,
UI/transparent, multi-pass, upload-pressure, resize and compute scenes. Record
median and tail CPU/GPU frame times where measurable, input-to-display latency,
submit/command/state-change counts, driver compilation stalls, descriptor
traffic, upload/readback bytes, live/backing/pending memory, waits and high-water
capacities. Count engine warm-path allocations separately from opaque driver or
browser allocation. Reserved engine buffers can be allocation-free after warmup;
this design makes no promise of allocation-free driver internals.

Establish budgets per supported profile on actual target hardware. Require an
image-correctness/synchronization result before accepting a speedup, and reject
optimizations that improve one workload while making the supported baseline
unmaintainable. The first success criterion is a small readable implementation
whose cost and lifetime can be explained from a frame report.

## Decisions from the reference review

The [reference review](rhi-gdi-gems-review.md) records reading scope, exact pages,
and adopted/rejected techniques. Its concrete refinements are integrated above:
explicit ordered graph compilation; separate streaming dependencies; VMA-backed
Vulkan allocation; completion-based staging/pool reuse; frequency-based immutable
bindings; private WebGL state deferral without delaying large uploads; explicit
attachment load/store/resolve intent; enumerated/prewarmed pipeline variants;
compact packets with measured recording granularity; and debugger controls that
preserve resource-use validation.

Do not adopt a universal software command interpreter, mandatory bindless path,
public Vulkan subpasses/heaps, automatic arbitrary pass reordering, live resource
relocation, per-draw uniform observers, or always-on async queues. Each introduces
an ownership or portability cost without a current Ludus consumer proving its
benefit. The architecture leaves measured extension points for them without
making them prerequisites for a correct portable frame.


## Historical first implementation slice: capability negotiation

`StartupInfo.Capabilities` is a value snapshot of the implemented fullscreen
API's effective limits while the session is Ready. It is zero during startup,
failure, device loss and shutdown. Raw adapter support is not advertised as an
engine operation. Frame dimensions are bounded by the backend's image/viewport
(and Vulkan framebuffer) limits; uniform sizes are capped by the negotiated
binding/allocation limits, the 16 KiB engine capacity, and 16-byte size granularity.
Resource capacities and the single draw per frame describe the compatibility
facade, not the current general raster/graph APIs.

The four-argument `Start(app, window, selection, requirements)` copies minimum
frame/uniform requirements before asynchronous requests. Every backend attempt
checks these before publishing Ready. Auto may fall back once when requirements
are unmet; forced selections fail. `RequirementsUnsatisfied` and the first unmet
limit remain visible in startup diagnostics. Unsupported compiled backend policy
reports `BackendUnavailable`. Existing Start overloads impose no extra minima.
This slice retains WebGPU's default feature/limit request policy: it checks the
enabled device's limits without elevating them to raw adapter maxima. General
required/preferred feature enabling belongs to the explicit device API.

The smoke renderer declares its own requirement from its actual uniform type.
Lifecycle tests exercise rejection, accepted boundary values, reduced limits,
Busy requests, copied inputs, stale completion, loss and both fallback outcomes.
Browser tests inject insufficient negotiated limits on WebGPU and WebGL to prove
that fallback and rejection occur before any resource creation or frame.

This historical slice retained the singleton owner and monotonic facade handles.
The explicit device API and R1/R2 subsequently added owner-validated typed
generational resources and retained lifetime services; R3 added the reference
plan validator. Independent multi-device/surface ownership and a separate
`GraphicsDevice` module remain possible extensions, as recorded in
[remaining work](#remaining-work-and-decision-gates).

## Optional static ray-query profile

`ray.h` exposes independently requested native static triangle BLAS/TLAS and
asynchronous closest/visibility batches. Public declarations own validation,
lifetime and failure contracts; the [RT0/RT1 implementation profile](renderer-systems.md#l6-implemented-scope-rt0-and-rt1)
owns backend/toolchain, memory accounting and reference transport details. Required
ray requests reject unsupported startup; preferred requests preserve raster.
Browser backends return Unsupported and never silently select CPU traversal.
Texture CPU content is separately opt-in for initialized RGBA8 mips; retained
views keep the upload shadow alive after the texture owner detaches.


### Native compute library adapters

`CreateNativeComputePipeline` admits a storage-buffer-only adapter into the ordinary
compute registry. The immutable binding snapshot, layout and context are retained
through graph and GPU completion. Prepare runs between frames and can return
Pending while a worker initializes
private resources, without touching bound buffer contents. Context and bindings
remain retained until preparation completes, including cancellation; shutdown
drains pending setup before native resources disappear. Submit must use the exact
prepared private queue without waiting or reentering RHI.
Dispatch uses the exact snapshot and unit workgroup counts. The shared
`MAX_NATIVE_COMPUTE_PASSES_PER_FRAME` limit is checked before encoding, so excess
work preserves the graph and completion output. The initial Metal
implementation commits the preceding RHI encoder, signals a native event,
orders the library's private queue after that event, and orders subsequent RHI
work after the library's completion event. Completion of the last RHI command
buffer covers all preceding operations. Private preparation never submits on a
borrowed framework queue. The shutdown-only Drain callback completes private
preparation before buffers or the private queue are destroyed. Prefix command
buffers remain
retained for error checks. The renderer owns OIDN policy and library objects; RHI
contains no denoising dependency. Vulkan/browser adapters are unsupported.
See the [OIDN graph contract](../development/render-graph-authoring.md#denoise-ray-output-with-oidn)
for quality, signal and accounting limitations.

## Native spatial reconstruction

`spatial_upscale.h` prepares device-scoped MetalFX Spatial effects outside frames.
The current profile accepts distinct single-sample RGBA8 D2 textures with one mip
and layer, exact configured dimensions, initialized source color and an offscreen
private attachment destination. Output dimensions cannot be smaller than input.
Linear and Perceptual modes describe stored channel interpretation; no implicit
AA, tone mapping or display conversion occurs. GetSpatialUpscaleCapabilities
reports device support; unsupported backends do not create substitute effects.

Effects use the existing raster resource generations, per-kind instance bound,
frame retention and completion retirement. Encoding uses the current Metal command
buffer after ending the raster encoder. Validation failures leave the current pass
intact; a successful native operation defines destination color, invalidates private
depth/stencil, and requires BeginRasterPass before subsequent raster draws.
Framework-private GPU allocations are opaque, and cannot be reported as measured
texture bytes. Prepare/reuse effects outside frames because creation may compile
vendor pipelines. See [graph integration](render-graph-authoring.md#upscaling-operations-and-presets)
and the [authoring workflow](../development/render-graph-authoring.md#run-metalfx-spatial).
