# Low-level renderer systems architecture

Status: L0 scene submission, L1 views/depth, L2 mutable versions/overlays and
L3 material/content versions and L4 lighting baseline implemented, October 11, 2026. L4 starts from merged
L3 at `f9cc7c8`. L5 portable scaling is implemented with workload-specific measurement gates; L6 implements the RT0/RT1 static-query and progressive-reference scope below; other advanced effects remain plans.
Correctness fixtures do not establish a measured speedup.
The initial decisions below were saved before opening the Gems article map.
The [reference review](renderer-systems-gems-review.md) records the subsequent
article readings and the concrete revisions now incorporated below. Its
[conference and journal plan](renderer-systems-gems-review.md#conference-and-journal-research-plan)
maps GPC, REAC and other sources to Ludus's owners, phases and validation gates;
recommended full readings remain distinct from completed source inspection.
Existing RHI, camera, resource and text documents retain their respective detailed
contracts.

Build a small data-oriented `GraphicsRenderer` over the existing
[RHI and GraphicsDevice design](rhi-gdi.md). Use immutable frame snapshots,
explicit output views, versioned mesh/material resources, bounded submissions,
and a fixed readable pass sequence. Start with CPU visibility and conventional
forward shading. Add clustered lighting and GPU visibility as independently
measured capability paths; keep a direct-draw reference path for debugging.

“Best” means predictable correctness and useful performance for a specified
workload. It does not mean that one technique wins on desktop, integrated GPUs,
tile GPUs and browsers. An AAA architecture needs scalable contracts and strong
inspection tools; it need not begin with every expensive rendering technique.

## Initial decisions before article review

| Decision | Rationale |
| --- | --- |
| Renderer owns scene policy; GDI owns work planning; RHI owns native state and lifetime | Preserve existing owners rather than introduce a second device wrapper |
| Compact immutable snapshots and packets, one serialized submission owner | Separate scene mutation from resource use and make captures explainable |
| Stable view identity distinct from camera, target, viewport and logical screen | Support split screen, editor previews and offscreen work without global current state |
| CPU frustum culling, material sorting and adjacent instancing first | Small portable correctness oracle and useful initial optimization |
| Forward raster baseline; clustered forward extension | Share opaque and transparent lighting and retain a simple WebGL path |
| Offline shader packages, explicit material templates and immutable instances | Bound permutations and remove shader compilation/name lookup from draws |
| Ordinary ordered passes with declared uses | Precise hazards and pooling without an opaque scheduler |
| Completion-scoped upload/frame storage and transactional version publication | Avoid in-flight overwrites and half-published reloads |
| Linear-light shading/composition with explicit final output encoding | Prevent gamma errors and inconsistent UI blending |
| Triangle-expanded debug lines and painter-ordered glyph/sprite quads | Portable width and ordering with reusable batching |
| Baked lightmaps/probes plus dynamic direct lighting | Useful quality without requiring real-time global illumination |
| Reference modes, bounded traces and workload gates before advanced features | Optimize with evidence while retaining inspectable behavior |

## Repository fit and current limits

Read [AGENTS.md](../../AGENTS.md), [ADR 0003](../decisions/0003-standard-library-usage-policy.md)
and the [foundational include model](foundational-headers.md) before implementation.
Use Ludus aliases and owned containers/algorithms, explicit results, `noexcept`
on infrastructure paths, and small public headers. New borrowed-view APIs belong
to Ludus; the R1 `std::span` exception does not automatically extend to Renderer.
No public native API types, runtime STL caches, exception-based dependency or
global registration constructors are implied by this proposal. Document new
public contracts with Doxygen and source attribution beside affected code.

| Inspected owner | Shipped boundary | Renderer implication |
| --- | --- | --- |
| [R1 portable raster](../development/fullscreen-rendering.md#portable-raster-r1) | Vulkan, Metal, WebGPU and WebGL 2; indexed instanced triangles, immutable resources, one acquired color target and private depth | A small flat/unlit consumer can use it now; general pass attachments, streaming and modern depth policy need explicit extensions |
| [RHI/GDI architecture](rhi-gdi.md) | R2 lifetime services, R3 ordered graph and R4 buffer compute/indirect profiles are implemented; R5 remains proposed | Reuse retained batches/completion and authored graph passes; storage textures, raster-stage storage and general mutable uploads remain deferred |
| [Camera systems](camera-systems.md) | C0 evaluation is implemented; advanced rigs/history remain proposed | Consume `CameraSample`; renderer owns projection, jitter, render-origin conversion and history |
| [Text](text-font-rendering-research.md) and [FontSystem](../../modules/text/include/ludus/text/font_system.h) | Native CPU shaping/rasterization plus L2 GraphicsText R8 atlas/Renderer adapter; browser consumes copied coverage fixtures, CPU font bootstrap remains pending in the [F0–F6 ledger](../../.kiro/specs/text-font-rendering/tasks.md) | Keep shaping independent; standalone F0–F6 gates and browser CPU font dependencies remain incomplete |
| [Resources](resource-management.md) | Content/audio portions exist; general coordinator and graphics streaming are proposed | Do not block the first scene on a universal asset manager |
| [Frame update](frame-update.md), [world](game-world.md), [UI](ui.md) | Separate simulation, presentation and UI ownership | Extraction adapters sit above Renderer; it must not depend on an ECS or UI toolkit |

The R1 compatibility frame still clears depth to one and defaults to less-than
depth. L1 adds explicit pass clear depth and pipeline comparison state. FoundationMath's
intended 3D convention is reverse-Z `[0,1]`. Pair reverse-Z projections with clear zero and Greater explicitly. The L1
fixtures prove ordering on native and browser backends; existing consumers keep
their conventional default.
Existing R1 consumers keep their documented behavior. L3 expanded the initial 16
records per kind to 32 and supplied immutable R8, RG8 and linear/sRGB RGBA8 mip
chains. L4 expands records to 64 and adds the bounded lighting profile below. One binding group remains the portable profile;
this bounded content slice does not establish a general streaming/lighting contract.

## Portable scene submission (L0)

`Ludus::GraphicsRenderer` now supplies the first portable static scene consumer.
Its public contract is [renderer.hpp](../../modules/graphics/renderer/include/ludus/graphics/renderer/renderer.hpp).
It depends on FoundationMath and GraphicsRhi, with FoundationContainers privately;
application adapters provide ordinary values without an ECS, UI or gameplay-camera
link. Native and browser builds export the same component.

Compile the installed flat shader during the application build:

```cmake
find_package(Ludus CONFIG REQUIRED COMPONENTS GraphicsRenderer)
target_link_libraries(my_app PRIVATE Ludus::GraphicsRenderer)
ludus_compile_shader(TARGET my_app NAME renderer_flat RASTER
    SOURCE "${Ludus_DIR}/shaders/renderer_flat.slang"
    VERTEX vertexMain FRAGMENT fragmentMain)
```

Create a ready portable-raster device, then initialize `Renderer` with the generated
`renderer_flat::Vertex()` and `Fragment()` descriptions. Poll pending setup on later
owner-thread turns. `CreateMesh` copies finite position-only geometry and uint32
mesh-local indices; poll a pending mesh before snapshot admission. `CreateSnapshot`
copies `SceneItem` transforms, flat materials and nonzero unique application source
IDs, and retains each mesh incarnation. Editing the original items or releasing a
mesh cannot change an accepted snapshot. A new snapshot cannot use a detached mesh.

`PrepareView` copies a finite conventional world-to-clip transform, derives visibility
and creates one immutable instance buffer during setup. Sorting/instance scratch is
reserved in renderer-owned storage at initialization, keeping preparation within the
browser default stack rather than requiring application stack overrides. Repeated `Submit` only records
bounded retained R2 packets; it performs no renderer heap allocation or resource
creation. The prepared view remains reusable. Releasing its source snapshot/meshes
is legal. The application sets the drawable extent/clear through RHI `SetFrameTarget`
between frames before submission; zero extent retains work for retry. Releasing the
view drops CPU ownership while accepted GPU uses retire in
RHI. Poll the returned completion independently from presentation. A skipped target
retains a finished batch for retry; terminal failure is still explicit. Reset the
renderer between frames before destroying its borrowed device.

L0 compatibility defaults use six mesh slots, four snapshot slots, eight prepared
views and 256 objects per snapshot/view. [L5.1](#l51-budgeted-scene-capacity) makes these capacities configurable. Retained versions consume slots until their last consumer releases.
The six meshes require twelve RHI buffers and four nonempty views require four more;
other RHI consumers and GPU retirement can exhaust the configured RHI budget
earlier. Setup failures preserve public outputs; Ready/Pending creation alone grants
ownership. Large geometry is also constrained by enabled RHI buffer limits. These are
bounds of this initial profile, not performance targets or a streaming budget.

Opaque objects use the view transform, depth clear one and less-than depth testing.
Use near-zero/far-one projection; do not pass FoundationMath's reverse-Z factories
unchanged. Use the L1 view builder and explicit frame/pass path below for reverse-Z. Overlays use their own
local-to-clip affine transform, ignore the world view and render after opaque draws
without depth testing. Overlay inputs use straight linear RGBA; preparation converts
RGB to premultiplied values. Unauthored flat materials default to opaque magenta.
General textures, arbitrary shaders/material templates, submeshes and live
instance updates remain later phases. L1 adds viewport/scissor and offscreen composition.

Visibility reuses FoundationMath's outward-rounded affine AABB transform and checked
frustum classification, including reflection/shear and boundary contact. Overlays are
classified against the canonical clip volume. Opaque mesh sorting uses the new
caller-scratch `FoundationContainers::StableSort` only within mutually disjoint
projected-bound runs. Near-plane crossings block reordering. Overlapping geometry
keeps input order, including coplanar depth ties; painter overlays always keep input
order. Adjacent compatible packets instance through a fixed 80-byte record containing
four clip-transform rows and a color. This conservative ordering may batch less than
a general opaque state sort; measure before extending it. `ViewReport` records actual
source order, visibility and draw counts. `ViewDescription::Reference` disables
visibility, sorting and instancing while preserving the same opaque/overlay semantics.

The public-only [SDK consumer](../../tests/sdk_consumer/renderer/CMakeLists.txt)
and [shared scene](../../tests/sdk_consumer/l0_scene.h) exercise the installed shader,
detached source ownership and 120 submissions. Native pixel tests and the
[browser harness](../../tests/sdk_consumer/renderer-browser-test.mjs) compare optimized
and unsorted direct images, with independent color samples to reject equally blank
images. Unit tests cover empty and 256-object scenes, tangency/shear, painter ordering,
coplanar overlap, invalid/stale identities, pending dependencies, capacity recovery,
skipped targets and GPU retention. Metal and pinned Chromium WebGPU/WebGL 2 are local
qualification paths; Linux Vulkan validation and supported-host sanitizers are CI
gates. Software GPU checks make no physical GPU performance claim.

## Views and depth (L1)

The [view API](../../modules/graphics/renderer/include/ludus/graphics/renderer/views.hpp)
provides allocation-free `ResolveViewMapping`, `SplitViewRectangle`, physical/window
pointer conversion and `BuildViewDescription`. A resolved mapping carries the actual
drawable dimensions, rounded viewport, owning region, origin, per-axis scale, crop
transform and IntegerFit fallback. Input uses top-left pixel-edge coordinates and
half-open regions. Window input receives explicit physical pixels per window unit;
DPI is applied once. Picking rejects bars/cropped exterior unless clamping is requested.
Zero extent returns NotReady without replacing outputs. Checked shared split edges
cover odd dimensions without cracks. Mapping extents are bounded to 2^24 so binary32
pixel edges remain representable.

`BuildViewDescription` reuses FoundationMath's right-handed reverse-Z factories for
centered orthographic, finite perspective and infinite perspective. It uses the logical
screen aspect. Its default `ProjectionSpace::LogicalScreen` produces an uncropped
offscreen image; presentation applies Fill crop once. For direct output-region drawing,
request `ProjectionSpace::OutputRegion` and use the mapping viewport/scissor; this
applies crop to world geometry and overlays during preparation. Do not present that
already cropped image. Infinite
perspective uses five-plane culling. Near maps to one, far to zero. Prepared views
retain their depth convention; reverse-Z preparation requires the L1 profile ready.
`Submit` remains the conventional L0 compatibility path; L1 uses `DrawView` inside
an acquired frame and finishes with R2 `EndFrame(device, completion)`.

Cook the installed composition shader alongside `renderer_flat`, then call
`InitializeViews(renderer_composite::Vertex(), renderer_composite::Fragment())`
after the flat renderer becomes Ready. Poll `PollViews` on subsequent owner turns
until Ready. L1 setup adds five pipelines, an immutable nearest/clamp sampler and
one index buffer. Pipeline creation stays outside frame drawing. It consumes the
existing RHI budgets rather than increasing them; a full L0 allocation can explicitly
reject L1 setup. Failed L1 setup leaves the L0 profile usable but requires Reset to
release its partial candidate before restarting.

Create linear RGBA8 RHI attachment textures with `TextureDescription.Attachment=true`.
`DrawView` selects a compatible surface/offscreen flat pipeline and pairs the prepared
view's depth convention with Less/clear one or Greater/clear zero. Its pass has explicit
load/store, viewport and scissor. Clears always cover the complete attachment; they
are never interpreted as per-view rectangular clears. To preserve neighboring split
views with independent clear colors/depth, draw each into its own offscreen target,
then composite into the shared surface with `DrawPresentation`. Ordinary R3 direct
pass switching ends the producer and establishes the sampled use; RHI's single
ledger retains attachments, bindings and geometry until actual completion.

`PreparePresentation` copies immutable quad geometry and a resolved mapping and
retains the sampled texture incarnation through its view/binding snapshot. The
source pixel aspect must match the logical screen aspect; its shading resolution
can differ. Four presentations are bounded separately, each using one vertex buffer,
view and binding set. Ready/Pending alone grants ownership. Poll presentation status
between frames; it may create the binding snapshot after asynchronous dependencies
are ready. Repeated draws create no renderer resources or heap allocations.
Destroying the original texture owner cannot invalidate an accepted presentation;
undefined sampled contents still reject. Top-left UVs are shared across backends,
including WebGL's existing stored-attachment flip.

Presentation geometry is prepared between frames. `DrawPresentation` compares its
mapping dimensions with the **actual acquired extent** before changing the pass;
a resize mismatch returns NotReady. End that frame, resolve a replacement mapping
using the acquired dimensions, prepare the replacement, then release the old
presentation between frames. Rendering resolution changes similarly create a full
replacement target/presentation pair before publication; old GPU uses retire through
completion. No automatic target mutation, history allocation or hidden DPI state is
introduced. PreparedView/Presentation identities carry owner, slot and non-wrapping
generation; there is no temporal history in this flat L1 profile.

The public-only installed SDK fixture renders orthographic and infinite perspective
views, verifies zero-extent skips, replaces 48-square targets with 96-square targets,
and resizes presentation from 96x64 to 120x80 over 120 accepted frames. Native pixels
compare optimized finite perspective against direct infinite perspective; browser
oracles compare optimized/direct and cross-backend images at both extents for Fit and
Fill. A native Fill fixture compares logical-source composition with direct output
samples and independently detects double cropping. Independent
samples prove near red survives later far green, different orthographic/perspective
corners, top-right overlay UV orientation, bars and neighboring view preservation.
Unit fixtures cover mapping inverses, explicit DPI, crop/fit/integer fallback, shared
split edges, output preservation, five-plane culling, stale handles, capacity recovery
and texture retention after release. These checks establish correctness, not a speedup.

## Implemented L2: mutable versions and overlays

L2 adds completion-scoped **replacement versions**, rather than exposing unsafe
in-place writes. `ReplaceSnapshot` publishes a copied dynamic instance scene
atomically; earlier prepared views retain the old snapshot. Set
`ViewDescription::ScheduledUpload` to copy instance bytes into R2's bounded staging
ring. Preparation returns Pending, with an owned prepared view. Poll RHI lifetime
and view readiness between frames before drawing. Release the previous version
after accepted submission; its physical resources remain in the authoritative RHI
registry until GPU completion. Source arrays and CPU lists can immediately be reused.
Admission failure keeps the prior snapshot/ready image. Exhausted upload/resource
quotas cause explicit backpressure, never a wait or an implicit eviction.

The [overlay API](../../modules/graphics/renderer/include/ludus/graphics/renderer/overlays.hpp)
uses one reserved CPU triangle arena for solids, thick lines and coverage quads.
Append operations allocate nothing and reject complete commands on overflow.
Homogeneous lines clip the positive-W and canonical near/far planes before
perspective division; physical widths have a one-pixel minimum. Butt caps and a
square for zero projected length are deliberate baseline choices. Each view builds
its own expanded list using that view's clip coordinates and actual viewport size.
World-unit widths, joins, persistent timed commands, worker merging and x-ray
convenience helpers remain later debug features.

Cook installed `renderer_overlay.slang` with the existing shader helper. Initialize
`OverlayRenderer`, then poll its fixed pipelines. `Prepare` copies the complete list
and captures an exact R8 atlas view/binding; four logical versions share existing
RHI quotas. `GetStatus` completes upload ownership and binding setup between frames.
`Draw` consumes the ready version during an acquired frame. Every command loads
and stores initialized color and depth, with read-only depth; the scene pass must
store depth even for mixed always-visible/depth-tested lists. Surface and linear
RGBA8 offscreen pipelines support conventional/reverse-Z tests without modifying
scene depth. Command scissors intersect the pass scissor. Only adjacent compatible
commands merge; font quads and debug triangles retain their authored painter order.
All colors are straight display-linear RGBA, premultiplied once in the shader.
Compose after scene/history production; this baseline has no temporal history.

[GraphicsText](../../modules/graphics/text/include/ludus/graphics/text/coverage_atlas.hpp)
owns a 256-square CPU R8 shadow and at most 512 sized glyph-cache entries. Shelves
reserve a transparent one-texel gutter before packing, with linear filtering and
no mipmaps. Cells never move or evict until an explicit reset. `Publish` copies a
complete page into a new immutable sampled R8 texture. Vulkan retains staging and
polls the upload fence; WebGPU validates asynchronously. Destroying the public
page handle or resetting the CPU shadow cannot change an already prepared label.
Publication, retained old/new pages and pending GPU retirement all count against
RHI resource quotas. Dirty rectangles and range updates are deferred until a
measured need warrants their additional hazard contract.

The native [Text bridge](../../modules/graphics/text/include/ludus/graphics/text/text_adapter.hpp)
consumes existing shaped glyph IDs, 26.6 positions, bearings and original byte
clusters. It copies temporary raster masks immediately, then produces generic
baseline-relative quads. Caller-assigned font content versions prevent cross-font
cache aliases. Warm runs use the cache without reshaping or rasterization. Missing
glyphs use the face's visible `.notdef`; atlas pressure is a status that lets the
caller retain its earlier label. Paragraph layout/fallback remain CPU/UI policy.
CPU Text's FreeType/HarfBuzz bootstrap is still native-only; the browser GraphicsText
target accepts copied R8 masks and shaped placements without pretending to shape
Unicode. The standalone [F0–F6 ledger](../../.kiro/specs/text-font-rendering/tasks.md)
remains authoritative for that larger text milestone.

The simple baseline expands quads into the same triangle stream as debug geometry,
sharing one index buffer and one upload per list, rather than adding a separate
instanced glyph path. There is no per-line/per-glyph GPU allocation. Fixed-quad
instancing remains a measured optimization; it must preserve these exact ordering,
scissor and coverage contracts. Whole-version replacement similarly favors a
small, auditable lifetime contract over premature in-place range mutation.

The public-only [L2 SDK fixture](../../tests/sdk_consumer/renderer_overlays.cpp)
renders 120 accepted frames with new scheduled instance and overlay versions,
without waiting for preceding frame completion. Native and browser pixel oracles
check R8 orientation, painter blending, scissor intersections and near-clipped
thick lines. Reference-backend stress holds submissions outstanding, cancels uploads,
checks old bytes and texture retention, exercises capacity recovery, and invalidates
stale handles on release/restart. CPU fixtures cover Latin, Hangul and Arabic shaping,
cluster provenance, warm cache reuse, transparent gutters and atlas pressure.
These are correctness checks; no throughput or physical GPU speedup is claimed.

## Implemented L3: material and content versions

L3 extends the existing Renderer with [MaterialProgram, MaterialVersion and
MaterialDescription](../../modules/graphics/renderer/include/ludus/graphics/renderer/materials.hpp).
A program owns one immutable shader/layout incarnation and six R2 pipeline-cache
requests: surface/offscreen × conventional depth/reverse-Z/painter. Create and poll
programs outside an acquired frame. A ready program admits immutable material
parameters, a sampler and an exact retained sampled texture view. Draws perform no
shader compilation, binding construction or parameter upload. Mesh creation copies
positions and optional finite top-left UVs into a 20-byte vertex in L3 (40 bytes with L4 normals/bake UVs); L0 flat shaders
ignore UVs. Existing flat/error materials keep their behavior.

```cmake
ludus_compile_shader(TARGET my_app NAME renderer_material RASTER
    SOURCE "${Ludus_DIR}/shaders/renderer_material.slang"
    VERTEX vertexMain FRAGMENT fragmentMain)
ludus_cook_texture(TARGET my_app NAME albedo SOURCE assets/albedo.texture.json)
```

The installed shader ABI has seven vertex inputs (position, four transform rows,
instance color, UV) and fragment bindings 0/1/2 (48-byte uniform/texture/sampler).
Per-target reflection exposes and verifies float4 `Tint`, `UvTransform`, `Options`
at 0/16/32 before admission. Equal-sized incompatible numeric shapes reject. The offline raster layout verifier additionally accepts
bounded float4 and float4x4 arrays and matrices, with independently checked WGSL
and GLSL ES packing, including pinned Slang's narrow matrix storage wrappers;
unsupported nested/runtime layouts reject. Source compilation
and generated manifests remain build-time work, never renderer reload work.

Use `PrewarmMaterialProgram` and `GetStatus` until ready, then `CreateMaterial` and
poll that candidate. `PublishMaterial(candidate, current)` transfers a ready owned
candidate into the current identity and detaches the previous owner. Failed or
pending admission preserves the old material; callers keep drawing its prepared
views. Snapshots and prepared views retain their exact material/program/texture
incarnations, and RHI retains accepted GPU dependencies after public release.
Publication changes future snapshot inputs, not previously captured views. Release
unpublished candidates explicitly. A program's null texture uses its ready white
fallback; a null scene surface uses L0's magenta flat/error material. Opaque and
masked materials use the scene layer and write depth; Painter uses the Overlay
layer and premultiplies straight color/texture alpha exactly once. This remains an
unlit schema; the lit baseline is described below. Packed tangent-space normal detail is available in the opt-in [surface fidelity profile](#material-and-lighting-fidelity).

The raw texture JSON contains integer `width`, `height` (1..4096), `format`
(`R8Unorm`, `Rg8Unorm`, `Rgba8Unorm`, `Rgba8Srgb`), `semantic` (`color`, `data`,
`normal`, `mask`) and complete top-down tightly packed integer-byte `pixels`.
Color/normal require RGBA8; sRGB is color-only. Complete upload footprint must
fit 64 MiB (or the device's lower enabled limit at admission). The cooker defaults to a complete
immutable chain down to 1×1; optional integer `mip_levels` selects a complete prefix. It writes four-byte padded rows, source/upload hashes and
inspectable level offsets/pitches. Fractional box footprints include odd edges;
sRGB RGB is decoded before filtering and re-encoded afterward, alpha stays linear.
Color filtering associates alpha then restores straight alpha, avoiding transparent
RGB bleed. Normal RGB is decoded, averaged and renormalized (degenerate averages
use +Z). Data averages channels independently. Optional mask `cutoff` in (0,1]
selects the nearest attainable discrete coverage using a monotone alpha scale;
the manifest reports coverage error, which need not be zero. Thanks to Khronos's
[KTX 2.0 §3.10.2, Use of Transfer Functions](https://registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html#_use_of_transfer_functions)
for the decode/filter/encode rule. This raw profile includes no image codec,
compressed formats or runtime mip generation.

RHI requires every declared sampled level to be initialized; offsets/pitches and
aggregate upload extent are validated before native work. Sampled views expose the
whole exact format/chain. Attachment textures remain single-mip; L4 adds capability-gated RGBA16F. Mip filters
and finite LOD clamps are explicit; the compatibility sampler clamps level zero.
`MaterialDescription::Lod` selects derivatives (-1) or an explicit initialized
level for deterministic previews, still subject to the sampler's mip/LOD clamps. Streaming residency/partial mip ranges remain
planned. `AsyncUpload` requests nonblocking Vulkan setup with fence-retained staging;
mip/data uploads also use that path. The cooker sets it. Legacy one-level RGBA8
setup retains its compatibility behavior; native/browser readiness is always explicit.

Default budgets are two program and eight material records, including detached
CPU-retained incarnations. L3's historical RHI budget of 32 records per kind covered L0/L1's seven pipelines,
L2's six and two overlapping six-variant programs (25); this changes table capacity,
not binding or hardware limits. R2 pipeline, upload and retirement budgets still
apply independently: backpressure returns an explicit status, never overwrites old
content. No unbounded cache, persisted driver blobs or draw-time permutations were
added. Thanks to the [Khronos Vulkan Guide, Pipeline Cache](https://docs.vulkan.org/guide/latest/pipeline_cache.html)
for motivating explicit prewarm; implementation uses Ludus's existing field-comparing
R2 cache.

The public-only [L3 SDK fixture](../../tests/sdk_consumer/renderer_materials.cpp)
cooks and renders color mips, masked coverage, painter blending and transactional
red→green replacement over 120 frames. Native pixel fixtures compare surface and
offscreen conventional/reverse-Z output, including a prepared view retained across
publication and rejected reload. Browser checks compare optimized/reference and
WebGPU/WebGL2/Auto/fallback images before and after replacement. Unit fixtures
exercise bounded overlap, stale identities, retained old versions, exact ABI rejection
and no additional pipeline creation during draws. Cooker fixtures independently
check transfer functions, odd areas, normal normalization and mask quantization.
The installed SDK separately cooks a matrix/vector-array/matrix-array fixture and
checks its target's 240-byte range and 0/64/96/224 member offsets.
These establish correctness; physical GPU performance, streaming throughput and
compressed-content quality require separate qualification.

## Implemented L4: bounded forward lighting

L4 adds [lighting.hpp](../../modules/graphics/renderer/include/ludus/graphics/renderer/lighting.hpp)
and three installed Slang sources: `renderer_lit.slang`, `renderer_shadow.slang`,
and `renderer_output.slang`. It preserves L0–L3 callers and immutable version
publication. Calls are serialized on the renderer/device owner thread; prepare,
prewarm and poll between acquired frames, then draw only ready resources.
Failure preserves caller outputs unless a documented accepted candidate is pending
or failed. Release failed/pending candidates explicitly. No draw compiles a shader,
creates bindings, allocates scene storage or changes an accepted material version.

Initialize flat rendering and views, then call `InitializeLighting(outputVertex,
outputFragment, flatShadowFragment)` and poll `PollLighting`. Cook the lit source
with `FragmentMain` and again with `ShadowMain`, and pass their vertex, lighting
fragment and masked-shadow fragment descriptions to `PrewarmLitProgram`.
`GetStatus(program)` completes the legal depth/painter variants for linear RGBA8,
optional RGBA16F, and conventional-depth masked shadows. Opaque/masked lit materials
belong to the scene layer; painter lit materials belong to Overlay. Lit views draw
into a linear attachment with `DrawView`; legacy surface `Submit` rejects them.

Meshes copy position, base UV, normalized normal and bake UV into a 40-byte vertex.
Lit snapshots require normals and an invertible finite normal transform (inverse
transpose components bounded to 1e12), and world bounds within ±1e6 meters.
Instances occupy 192 bytes, retaining the legacy clip/color prefix. Perspective
specular evaluation uses `ViewDescription::CameraPosition`; orthographic views
use the opposite normalized `CameraForward`. Both camera fields are validated.
The installed ABI has sixteen vertex attributes, a 1040-byte fragment uniform at
binding 0, base texture/sampler at 1/2, bake texture at 3, and shadow texture/nearest
sampler at 4/5. Reflection verifies all member names, offsets and numeric shapes,
including the masked-shadow subset, before admission. Fewer than sixteen enabled
attributes returns Unsupported. The RHI exposes the enabled attribute bound (up
to sixteen); original shaders still use their original eight-attribute profile.

### Material and direct-light model

`CreateLitMaterial` copies metallic [0,1], perceptual roughness [0.045,1], emission,
constant environment irradiance/radiance and the L3 base texture/tint/alpha schema.
Linear emission/environment components are bounded to 10000. The baseline uses
single-scatter isotropic GGX, height-correlated Smith visibility, Schlick Fresnel,
a fixed dielectric F0 of 0.04 and Fresnel-weighted Lambert diffuse. Metallic removes
diffuse. The baseline has no multiscatter compensation, normal-map/tangent profile, BRDF
LUT or environment convolution. Constant specular radiance uses a declared Fresnel
approximation; it is a deterministic fallback, not a full image-based-lighting bake.
`EvaluateLitBrdf` is an allocation-free independent CPU reference returning BRDF ×
max(N·L,0); degenerate/invalid inputs preserve its output.

Thanks to Google and the Filament authors, [*Physically Based Rendering in Filament*,
§4.4 Standard model and §5.2.2 Direct lighting](https://google.github.io/filament/main/filament.html),
for the GGX/Smith/Schlick model and photometric conventions. Code credits the source
and records the simpler single-scatter/constant-environment departures.

Directional intensity is illuminance in lux; point and spot intensity is candela.
Positions/ranges use meters. Point/spot evaluation uses inverse-square attenuation,
a smooth finite range, and for spots a squared smooth cone between distinct inner
and outer half-angle cosines. Zero-distance attenuation is bounded at 1e-4 square
meters. Intensity is finite [0,1e6], hue channels [0,1], range [0.01,1e6]. Every
submitted light is validated, including dropped entries, and identities must be
unique/nonzero. `SelectDirectLights` admits at most 128, retaining the eight smallest
SourceIds in ascending order. `LightSelection::Dropped` reports successful
saturation; excess input capacity rejects and preserves output. Stable selection
avoids frame-order flicker; it does not promise brightness-optimal selection.
Shader radiance clamps finite output to 65504 before attachment conversion. Emissive
materials do not create lights. Clustered lists belong to L5.

### Baked chart admission and replacement

The offline raw-texel cooker accepts optional `bake` metadata only for linear
`Rgba8Unorm`, semantic `data`, and `mip_levels: 1`. It validates nonzero uint64
`source_id`, `scene_revision`, `dependency_revision`; optional `direct_light_id`
(zero means indirect-only); twelve column-major affine `transform` components;
three `scale` components; and integer `gutter` (1..16, default one). Nonzero numeric
components below binary32's minimum magnitude (`2^-149`) are rejected before header
generation; zero and representable subnormal values are accepted. Every gutter texel
must replicate its nearest interior texel, and the interior is at least 2×2.
The generated `Bake(texture)` returns the immutable metadata and texel-center UV
transform; the manifest retains that metadata with source/upload hashes.
See the [SDK bake source](../../tests/sdk_consumer/l4-bake.texture.json) for a complete
example. The input is authored irradiance, not a runtime lightmapper or chart packer.

Materials retain a ready single-mip linear RGBA8 chart. Bake UVs are clamped to [0,1]
before the supplied interior transform, so bilinear level-zero sampling cannot cross
a gutter. A valid bake requires exact SourceId, both revisions and affine transform
on each SceneItem. It replaces environment diffuse irradiance. If `DirectLightId`
identifies a selected light, it also replaces that light's **direct diffuse** term;
dynamic direct specular remains. Indirect-only charts preserve all direct lighting.
A moved object or mismatched revision uses environment diffuse and all dynamic direct
terms, counted by `ViewReport::BakeFallbacks`. No stale chart is silently applied.

### Immutable shadow atlases

`PrepareShadowAtlas(snapshot, description, atlas)` retains the exact opaque/masked
caster snapshot and up to 80 light views in a [4,2048] RGBA8 atlas. Call `GetStatus`,
then draw once with `DrawShadowAtlas` inside an acquired frame and successfully end
that frame before publishing materials using it. Partial drawing failure poisons the
candidate; a failed/aborted frame requires discarding it and preparing another.
Ready rendered atlases may be retained by lit materials after public ownership is
released. Two atlas incarnations support bounded overlap. No in-place rewrite can
change the image of an older prepared view.

Cells leave a clear texel on all sides and separate expanded gutter rectangles.
All cells use conventional Z [0,1], clear depth one, independent of camera reverse-Z.
Depth is packed into RG16 within RGBA8; nearest 2×2 manual PCF clamps each sample to
that cell's interior texel centers. Receiver bias is explicit [0,0.01] in normalized
light depth. Region order chooses the first containing projection, permitting two
explicit directional cascades and spot projections. Point and weighted area
samples require six canonical cube projections: +X, -X, +Y, -Y, +Z, -Z. Use
`BuildPointShadowMatrix` with a positive explicit near distance and far equal to
Light.Range; all six faces and matching source terms are validated before
preparation. Local uncovered receiver regions conservatively occlude, while
uncovered directional cascade regions retain the declared unshadowed fallback. Opaque flat/unlit
geometry casts; masked casters require the lit alpha shader; painters never cast.

`BuildDirectionalShadowMatrix` fits finite unjittered caster/receiver bounds, snaps
XY center to a texel grid and pads one texel. The caller owns cascade splits and
spot projection authoring; no temporal jitter or automatic fit policy is hidden.
Atlas light identity, position/direction/range/cone must match selected material
lights; color/intensity may change without changing visibility. New geometry,
source identity, transform, opacity domain, or masked material incarnation invalidates
cached visibility. The complete caster set is compared conservatively; even a moved
caster outside the main view selects unshadowed direct lighting and increments
`ShadowFallbacks`. Lighting-only opaque material replacement keeps visibility valid.
A new rendered atlas is required to restore shadows; no stale shadow is sampled.

### Linear composition and capabilities

`RasterCapabilities::HdrColor` requires sampleable, linearly filterable, blendable
RGBA16F color attachments. Vulkan queries format features, Metal/WebGPU expose the
supported profile, and WebGL 2 requires `EXT_color_buffer_float`. Rgba16Float is
attachment-only, one mip; unsupported devices return Unsupported and callers choose
the linear RGBA8 LDR path. LDR clips radiance above one before tone mapping; HDR
preserves it. This is explicit quality selection, not a failed-frame fallback.

`PrepareLightingPresentation` retains an immutable source, mapping and manual
exposure. It precreates both output encoding choices and selects one using the
acquired `FrameInfo.Encoding`. Apply exposure and the selected `ToneCurve` once: guarded per-channel
Reinhard by default, or Khronos PBR Neutral (`LightingOutput::Curve`, `RasterViewport::Initialize`'s
`toneCurve`), which keeps hue and saturation below 0.76 and compresses highlights toward white.
The Editor viewport uses PBR Neutral; the engine and the SDK stay on Reinhard. The graph `ToneMap` node defaults
to Reinhard and also offers PBR Neutral, which the engine default graph selects; graph conversion shares
`display_tone_mapping.slang` with this presentation.
For UI composition, tone-map scene into a **linear RGBA8 display intermediate** with
`Encode=false`, blend linear UI there, then present with `ToneMap=false`, exposure
one and `Encode=true`. Hardware sRGB attachments encode automatically; unorm surfaces
use the shader's sRGB curve, so conversion occurs once. Draw-time encoding selection
creates no resource. Old unlit textured materials are not admitted to HDR targets;
flat overlays remain available. Exposure is manual; auto exposure and tone-map curves
beyond Reinhard and PBR Neutral remain planned.

The Editor's Solid and Albedo viewport shows a very subtle vertical gradient behind the scene
(`surface.viewportTop` to `surface.viewport`, `config/design-tokens.json`); Lit keeps the scene's own
background. It costs no pass, draw or attachment: `RasterViewport::Initialize`'s `hasBackdrop` clears the scene
target to alpha zero, and the existing output pass adds the gradient, in display space, under each pixel by
`1 - alpha` (`LightingOutput::HasBackdrop`, `Output.w`). Every scene blend is premultiplied, so a
partly covered pixel such as a grid line keeps its share of the backdrop. The default is off, which leaves
the clear colour, the image and the uniform contents of games, exports and reference captures unchanged.
The shader's endpoint constants must equal the tokens; `test_design_tokens.py` enforces it. Measured on
lavapipe at 1080p over 15 runs of 40 frames, a viewport frame (render to acquire) has a median of 7.8 ms
with the backdrop and 8.6 ms without it: no measurable difference.

Spatial antialiasing is FXAA 3.11 in that same output pass (`LightingOutput::Antialiasing`, `SpatialAntialiasing`
Off, Fast or Balanced; `Output.w` carries `backdrop + 2 * mode`, so the 16-byte uniform did not grow). It runs on
the display colour, after exposure, tone mapping, encoding and the backdrop, so its edge test sees what the viewer
sees; object overlays are drawn into the scene target first and are filtered with the scene. Every pixel loads
itself and its four neighbours and rejects flat ones there, with a square-root brightness in place of the sRGB
encode so the rejection test costs no powers. An edge pixel loads its corners, walks both ways along the edge
until the brightness pair average departs by a quarter of the gradient (four pixels in Fast, sixteen in
Balanced, with the 3.11 thresholds and sub-pixel weights of each) and blends toward the neighbour across the
edge. The taps are exact texel loads, not the half-pixel bilinear taps of the reference algorithm, so the shared
nearest sampler is unchanged and the pair average costs two loads. The editor starts with Fast and offers Off,
Fast and Balanced in the View menu; the choice persists with the navigation state and rebuilds the viewport.
The filter assumes the image maps one to one onto the target, as `RasterViewport`'s does; Off leaves the
single-sample path and every existing pixel unchanged. Measured on lavapipe at 1080p over 15 runs of 40 frames
(a software rasterizer, so pessimistic for the five loads per pixel), the median viewport frame (render to
acquire) is 6.7 ms with Off, 12.5 ms with Fast and 12.2 ms with Balanced (5.7, 10.7 and 10.4 ms in an earlier run; the two modes cost the same because the rejection test of flat pixels dominates); a hardware
GPU is not measured.

Default renderer budgets are eight prepared views (four shadow views plus scene/UI
and overlap), two atlases, two programs, eight materials, four snapshots and four
presentations. RHI tables grow to 64 per kind: two seven-variant lit programs,
L0/L1's seven, L2's six, output/shadow and HDR flat variants exceed L3's 32. These are
bounded record capacities, not wider bindings, allocations or hardware guarantees.

The shared [public-only SDK fixture](../../tests/sdk_consumer/renderer_lighting.cpp)
uses cooked bake metadata, valid/invalid bake revisions, all four atlas cells,
masked casters, a retained atlas across lighting-only replacement and moved-caster
fallback. It renders LDR/HDR, emission, environment and painter samples through a
linear display intermediate over 120 frames. Native pixel tests compare reference
and optimized images and independent interior values. Browser checks cover forced
WebGPU/WebGL 2, Auto and unavailable-WebGPU fallback with both profiles, before and
after caster movement. Unit tests cover stable overflow, malformed ABI/input,
reference BRDF guards, retained atlases and conservative invalidation; cooker tests
cover gutters, revisions and mip admission. These validate correctness, not physical
GPU performance or production bake quality.

### Authored forward graph integration

The [forward-lit graph template](../development/render-graph-authoring.md#use-the-forward-lit-template)
uses the existing authored material program, light selection and environment
preparation. Its private scene resource owner retains shared immutable mesh/texture
incarnations across instances, passes and replacements. Material and light edits
stage new scene generations; camera-only revision promises reuse prepared content.
This graph profile is opaque and unshadowed. Its admission, budgets, shader ABI and
publication boundaries belong to the linked graph workflow and
[scene contract](shared-scene-shader-contract.md#authored-forward-material-execution).

## L5: opt-in portable scaling

The engine default now uses these conservative light lists in the
[clustered deferred graph](../development/render-graph-authoring.md#default-clustered-deferred-lighting),
with shared material/light evaluation and an optional z-prepass. The L5 forward
recipes and their measurements below remain independently available.

L5 adds CPU-built clustered forward lighting, conservative grouped visibility,
and caller-authored screen-size LOD. The ordinary L4 shader and eight-light path
remain available. These additions are opt-in; performance qualification is
workload-specific and does not follow from passing software-GPU fixtures.
Native parallel recording and compute assignment remain optional future work.

### Clustered lighting and inspection

Cook the centralized `renderer_clustered` and `renderer_clustered_shadow`
recipes, then use `PrewarmLitProgram`:

```cmake
ludus_compile_renderer_shaders(TARGET game SHADERS
    renderer_clustered renderer_clustered_shadow)
```

The renderer-owned [recipe manifest](../../modules/graphics/renderer/shaders/recipes.json)
and separate installed `LudusRendererShaders.cmake` helper own source paths,
entry points, and FoundationMath/RHI include roots. Engine targets, editor,
installed SDK consumers and reference-scene use the same recipes. Unknown or
repeated recipes fail configure; changes to the manifest trigger reconfigure,
and the offline compiler's depfile tracks included Slang sources. Custom shaders
still use `ludus_compile_shader`; neither helper acquires tools or compiles at
runtime. Recipes preserve artifact names and shader layouts, including
`VertexMain`/`ShadowMain` for clustered shadows. The installed SDK
[scaling consumer](../../tests/sdk_consumer/renderer_scaling.cpp) demonstrates
this sequence without private renderer access.

Set `LitMaterialDescription::Clusters` to a borrowed `LightClusterDescription`;
`Lights`/`LightCount` provide up to 128 unique lights. Creation copies everything
before returning. Program/profile mismatch is rejected without publication.
`BuildLightClusters` supplies an allocation-free independent inspection API;
invalid input and count overflow preserve its output. Material reports include
all admitted normalized lights, written assignments and overflow cell counts.

The regular 8×8×8 grid has finite bounds in a rigidly transformed space. An
unjittered world-to-view transform gives view-space cells; identity gives
world-space cells. Columns must be orthonormal within 1e-5, bounds lie within
±1e6 meters, and extents are [0.01,2e6] meters. Unlike a projection-shaped or
logarithmic grid, these cells use linear XYZ intervals. Point/spot range spheres
intersect conservative cell AABBs; spots do not prune by cone, and directional
lights intersect every cell. A rounding margin protects tangencies. Assignment
never depends on opaque depth or normals, so it also covers painter recipients.

Each cell stores up to 32 ascending light indices in a fixed stride. Zero
terminates a partial list; byte 255 marks overflow. Overflow and samples outside
the box evaluate **all** admitted lights in stable order, preserving lighting
instead of reducing quality. The reference view also evaluates all lights. `DirectLighting` evaluates that same
full list while retaining optimized geometry packets, isolating shading cost.
`Counts` reports written entries, never attempted append counts. Empty cells
skip direct evaluation. Overflow costs are observable and must be measured.

The portable GPU representation is a 64×64 nearest-sampled RGBA8 index texture
(16 KiB), plus an 8800-byte fragment uniform containing the lights, existing
material/shadow terms and grid transform. It needs eight bindings, sixteen vertex
attributes and that uniform range. Unsupported profiles return Unsupported;
applications explicitly choose L4 instead. This CPU/texture implementation runs
on the existing raster backends, including WebGL 2, without requiring storage or
compute support. Materials own and retire the immutable grid texture and views
alongside their other bindings. Assignment is setup work, not a draw-time rebuild. Its reusable CPU workspace is
allocated only on first clustered material creation; ordinary L4 has no grid
workspace or GPU grid resources. Frequently changing light sets pay assignment
and immutable upload costs again, so this profile targets cached light sets.

Thanks to Ola Olsson, Markus Billeter and Ulf Assarsson,
[*Clustered Deferred and Forward Shading*, HPG 2012, pp. 87–96](https://research.chalmers.se/en/publication/161725),
for the spatial-grouping principle described in their abstract. Ludus uses a
bounded CPU-built regular grid, without their occupied-sample/normal clustering
or historical performance claims. The full-list fallback is a deliberate
correctness choice rather than the earlier proposed saturated eight-light list.

### Grouped visibility and authored LOD

Snapshots cache conservative unions for contiguous groups of sixteen opaque
objects. Rejected groups skip individual tests; fully inside groups admit their
members without further AABB tests. Intersecting groups and overlays retain
individual conservative classification. Input order and painter semantics remain
unchanged. Homogeneous mesh/material runs preserve source order and skip pairwise
overlap searches; other runs retain conservative ordering constraints. Reference
views bypass visibility and packet optimization.
`ViewReport::CulledGroups` and `BoundsTests` expose the work saved. This fixed
hierarchy benefits spatially coherent source order; it is not a spatial index.

An opaque `SceneItem` may supply `LodGeometry`, `LodPixels` and `PreviousLod`.
The snapshot retains both exact mesh incarnations and unions their transformed
bounds. Lit alternatives need normals; compatible material and bake UV authoring
is the caller's responsibility. `ViewDescription::EnableLod`, physical
`LodWidth`/`LodHeight` and fractional `LodHysteresis` select by projected bounding-box
diameter. Near-plane intersections remain conservatively large. Feed
`SelectedLods` back by `SourceIds` when authoring the next snapshot, not by array
position. Shadow and reference views use full geometry. Automatic simplification,
streaming, spatial indexing and occlusion are outside this implementation.

### Correctness and measurement

The [shared SDK fixture](../../tests/sdk_consumer/l5_scene.h) contains 256 objects,
128 spatial lights, overflow and outside-grid variants. Native and browser pixels
compare clustered/optimized against full-list/direct images, with a nonblank
check and a brightness check requiring all 128 directional lights. Pixel-aligned
geometry avoids treating backend edge ownership differences as lighting error. Browser checks exercise WebGPU, WebGL 2, Auto and fallback, LDR/HDR and
120 frames. Unit tests check conservative boundary assignment, malformed input,
output preservation, grouped rejection and retained LOD selection.

Run physical-adapter measurements separately:

```sh
python scripts/benchmark-renderer \
  --executable out/build/<preset>/modules/graphics/renderer/ludus_renderer_render_tests \
  --backend metal --device "actual adapter and OS/driver" \
  --runs 3 --output out/renderer-benchmark
```

The harness checks paired images and reports three variants (clustered, direct reference, direct optimized), cold scene setup, warm view
preparation, recording, serialized completion/readback latency, median/p95/p99,
resolution, counters, known scene resource bytes and tool pins. GPU pass timestamps
are explicitly unavailable; completion/readback CPU latency is not GPU execution
time. The 640×360 fixtures are not an AAA scene benchmark or a frames-in-flight
throughput test. Known memory excludes driver/pipeline allocations, retired
versions and renderer CPU storage. These historical measurements used six meshes/eight materials/256 objects.
L5.1 removes those fixed ceilings; neither stage establishes full Sponza or
Staircase2 import or fidelity. Record adapter-specific results before choosing the clustered profile.

### Apple M5 measurement, October 11, 2026

[Raw measurements and source hashes](../assets/renderer/l5-apple-m5.json) record
three runs on an Apple M5 (8 GPU cores), Metal OS driver on macOS 26.6 (25G72),
640×360 LDR, eight warmup frames and 64 samples per variant per run. Values below
are medians of the three per-run medians/p95s. Host frequency/background work is
uncontrolled; these are serialized CPU-clock microfixture measurements, not GPU
pass timings or an FPS/production-scene claim.

| Fixture | Variant | Prepare median (µs) | Serialized median (ms) | Serialized p95 (ms) |
| --- | --- | ---: | ---: | ---: |
| localized | clustered | 64.2 | 5.064 | 5.339 |
| localized | direct-reference | 87.8 | 6.083 | 6.467 |
| localized | direct-optimized | 62.5 | 5.710 | 6.013 |
| overflow | clustered | 64.0 | 4.572 | 4.812 |
| overflow | direct-reference | 89.7 | 4.954 | 5.242 |
| overflow | direct-optimized | 61.1 | 4.553 | 4.774 |
| outside-grid | clustered | 65.3 | 5.710 | 5.944 |
| outside-grid | direct-reference | 87.7 | 6.083 | 6.382 |
| outside-grid | direct-optimized | 63.2 | 5.727 | 5.943 |

Localized clustering improves this workload versus direct lighting with the same
optimized geometry. Grouped visibility and homogeneous packet admission reduce
preparation cost versus the direct reference. Overflow offers no clustering
benefit and can add cost; outside-grid shading is effectively the full-list path.
The profile remains opt-in. Benchmark GPU bytes cover geometry, instances, light
uniform/index texture and scene target only; presentation/driver/retirement and
CPU workspace memory are excluded. Re-measure after changing lights every frame,
resolution, layout, backend or adapter before choosing a shipping configuration.

## L5.1: budgeted scene capacity

Renderer initialization accepts copied `Limits`, preserving the six-mesh,
eight-material and 256-object compatibility defaults. All registry kinds are
configurable: meshes, snapshots, prepared views, presentations, material programs,
material versions and shadow atlases. `GetConfiguredLimits()` returns the selected
counts; static `GetLimits()` returns defaults. Counts are positive and below
UINT32_MAX, with checked counter/size bounds. `Objects` must fit the enabled RHI
`DrawsPerFrame`; an incompatible request returns `Unsupported`.

Registry arrays, each snapshot's items/groups, each view's packets and reusable
visibility/sort/instance workspaces are reserved exactly once at initialization.
Their addresses remain stable, and preparation reuses CPU workspace without
pool growth. `Limits::HostBytes` covers State, reserved metadata/workspaces and
optional lazily allocated cluster scratch. `GetHostBytes()` reports requested
reserved bytes, excluding allocator overhead, transient mesh upload conversion,
RT1's separately budgeted retained data, RHI resources and driver allocations.
Invalid counts return `InvalidDescription`; checked size overflow or budget
exhaustion returns `CapacityExceeded`; allocation failure returns `OutOfMemory`.
Failed initialization remains uninitialized. Reset releases the reservations.

These are maximum renderer incarnations, not GPU resource reservations. Detached
versions still consume slots while snapshots/views/captures retain them. Renderer
slot recycling advances generations; RHI separately retains submitted resource
uses through completion. Other RHI consumers, shader variants, simultaneous views,
materials, shadows and replacement versions consume the independently configured
RHI counts/bytes. Increasing renderer capacities alone cannot promise that every
resource combination fits.

For example, configure device storage before startup and pass renderer limits
after the device is Ready (shader descriptions come from the installed cooker):

```cpp
rhi::DeviceDescription deviceDescription;
deviceDescription.Storage.ResourcesPerKind = 512;
deviceDescription.Storage.DrawsPerFrame = 2048;
deviceDescription.Storage.DrawsPerPlan = 2048;
deviceDescription.Storage.RayResourcesPerKind = 256;
deviceDescription.Storage.RayInstances = 2048;
// CreateDevice(..., deviceDescription, device, surface), then poll startup.
rr::Limits limits;
limits.Meshes = 128;
limits.Materials = 128;
limits.Objects = 2048;
limits.HostBytes = usize{64} * 1024 * 1024;
// renderer.Initialize(device, flatVertex, flatFragment, limits).
```

`ViewReport` counts cover the entire view. Inline `SourceIds`/`SelectedLods` hold
only the first `Reported = min(Visible, 256)` entries. For full diagnostics, supply
optional borrowed `SourceIdOutput`/`SelectedLodOutput` arrays and `DetailCapacity`
through the input report. Either requested array must accommodate every visible
object; insufficient capacity fails before GPU resource publication. Preparation
failure preserves the report and output arrays. Successful output follows actual
packet/instance order, including source-order reference views and LOD selections.

RT1 retains an explicitly budgeted dynamic instance table instead of 256 embedded
entries. Native RT0 uses R6's stable host-budgeted pages for all ray resource kinds,
with `StorageLimits::RayResourcesPerKind` independent of general raster counts.
`StorageLimits::RayInstances` requests the TLAS ceiling; query
`RayCapabilities::MaxInstances` after startup for the enabled backend limit.
Vulkan also bounds the returned instance identity to its 24-bit custom-index field.
Structure/input/opacity/scratch bytes still require explicit per-resource ray
budgets. Default counts, 4096-ray batches and native per-buffer limits are retained.
No dynamic AS update/refit, scene ingestion, material expansion or virtual residency
is added by this capacity milestone.

Validation covers 20 meshes, 16 material versions, a 513-object snapshot with a
partial final culling group, full-order diagnostics, exhaustion/output preservation,
detached retention, stale handles and recovery. An RT1 CPU capture shades instance
512 after owners detach. Native RT0 tests retain 72 distinct BLAS geometries in a
513-instance TLAS, verify instance identities 0/64/512 and release dependencies
only after the batch retires. Counts establish capacity and lifetime correctness,
not full-scene fidelity or performance.

## Owners and module shape

```text
Game/World/Editor adapters     UI layout       GameplayCamera        Text CPU
             \                   |                  |                  /
              +------ immutable scene, view and overlay inputs ------+
                                      |
                            GraphicsRenderer
                     extraction-free packets and effects
                                      |
                            GraphicsDevice
                   pass plans, frame storage, uploads
                                      |
                              GraphicsRhi
                native objects, accepted uses, completion
                                      |
                     Vulkan / Metal / WebGPU / WebGL 2
```

This diagram is data flow, not permission for RHI to depend on gameplay.
Renderer receives ordinary values and resource leases through application
adapters. GameplayCamera and Text remain independent CPU owners. Start with one
renderer instance per device, multiple views and one owner thread. Proposed
`modules/graphics/renderer/include/ludus/graphics/renderer/` contains small scene,
view, material, debug and frame contracts; private tables, pass implementations,
shader bindings and caches stay in `src/internal/`. Native completion/retirement
remains in RHI and is serviced through GDI; never duplicate its handle registry.

Use ordinary functions and a few private records. Do not add a polymorphic
renderable per object, a public pass class per effect, or a universal render
command bytecode. Pass implementations accept their concrete data and emit RHI
encoder work. The first implementation can record serially; native parallel
recording is an optional private optimization with completion-scoped pools.

Following Donald Revie's separation of representation and rendering context
(*GPU Pro 3*, V.4, sections 4.4–4.8), store complete pass inputs rather than a
mutable global context stack. Material packages explicitly name legal depth,
shadow, color and debug variants. Resolve parameters at cook/load, and bound
variant combinations before admission. Thanks to Revie for this distinction and
the account of configuration/optimization costs; the [review](renderer-systems-gems-review.md)
explains why Ludus does not adopt his general object/XML pipeline.

### Required RHI/GDI extensions

| Needed contract | Owner and migration boundary |
| --- | --- |
| Clear depth, compare/cull state, viewport and scissor | RHI explicit raster state alongside the unchanged R1 compatibility behavior |
| Color/depth attachment views, pass begin/end and resolve intent | RHI encoders; GDI declares uses/imports and preserves shared-target rectangles |
| Scheduled range uploads, mutable instance/constants and R8/mip sampling | RHI validates operations; GDI owns tickets, bounded staging and readiness |
| Reflection/layout growth and ready pipeline requests | Shader tools validate targets; GDI caches/prewarms; Renderer chooses material variants |
| Compute/storage and indirect operations | Independently negotiated RHI capabilities; Renderer has an authored direct/CPU alternative |
| Acceleration-structure descriptions/builds, shader query support and exact retained versions | Optional RHI extension; GDI declares build/read/scratch hazards; Renderer owns ray-scene selection and transport |

Each extension requires native and browser evidence under the pinned toolchains,
including explicit unsupported results for optional features.
Do not increase current R1 limits globally as an incidental renderer refactor.
Keep first consumers within existing limits or explicitly fail their requested
profile. Detailed retention, state and submission rules remain owned by
[RHI/GDI](rhi-gdi.md#resource-and-command-contracts).

## Frame contract and concurrency

1. Service completions, pending resource/pipeline validation and bounded uploads.
2. Admit complete resource candidates at the frame boundary. Acquire exact
   resource versions for this frame; incomplete candidates retain the old version.
3. Accept immutable scene, view and overlay snapshots. Validate identity, ranges,
   finite values and configured capacities before publication.
4. Build render views, CPU visibility/LOD and draw lists from those values.
5. Resolve ready pipelines/bindings and select declared effect variants.
6. Build ordered passes; GDI validates declared subresource uses and plans imports.
7. Record/submit through one RHI owner. On acceptance, transfer retained uses to
   its authoritative ledger; on failure discard unsubmitted reservations safely.
8. Commit temporal state only for accepted render work under the view's policy.
   Presentation outcome is tracked separately; skipped acquisition does not
   advance rendered history. Recycle frame storage only after all CPU and GPU users.

Inputs are copied into owned bounded storage before a submission API returns,
unless it explicitly accepts a transferable lease. Worker tasks receive disjoint
packet ranges and exact leases; their joins are CPU readiness, not GPU retirement.
No callback borrows an application's stack or addresses into a mutable world.
The render snapshot can contain interpolated transforms while simulation remains
fixed-tick. Avoid a dedicated render thread initially: measure extraction,
recording, joins, memory and input-to-display latency before adding buffering.

Creation requires a null output. Invalid description/handle, unsupported feature,
not-ready input, out-of-memory, budget exhaustion and device loss are distinct
outcomes. Failed admission preserves outputs and the previously usable version.
Bounded diagnostics carry source/view/object/pass IDs. Optional debug geometry
may be dropped with a counter; essential scene work must not silently disappear
because a packet buffer overflowed. A frame admission failure returns a status
and preserves the last valid presentation where the surface permits it.

## Primitive submission, visibility and batching

Use a persistent mesh resource plus frame-local instance data. A mesh owns
immutable vertex/index chunks, vertex layout, topology, bounds, submesh ranges
and LOD metadata. A scene item supplies mesh/version, submesh, material/version,
current/previous transform, visibility flags, stable object ID and sort layer.
Visibility flags describe policy; they must not expose native render state.
Large world positions use `float64`; convert to checked origin-relative
`float32` per view. Singular normal transforms, NaNs and unrepresentable bounds
are explicit invalid inputs, with a diagnostic and documented fallback.

Build dense arrays, reuse reserved capacity and keep authoring names out of hot
packets. Initially perform scalar conservative frustum/bounds tests and authored
screen-size LOD with hysteresis. Add spatial indexing, SIMD and occlusion only
after measured need. Animated/deformed bounds must conservatively contain the
posed geometry; delayed visibility must never hide a newly visible object.
Shadow visibility is computed for the light's view independently of main-view
visibility. Streaming hints use visibility but never synchronously load in culling.

Sort opaque packets by pass, coarse depth bucket, pipeline, binding/material,
mesh and stable source ordinal. Compare depth-first versus state-first sorting
on actual scenes; no hardcoded bit allocation establishes an optimal key.
All key collisions resolve with explicit record comparisons. Masked geometry
preserves coverage rules across depth/shadow/color variants. Transparent 3D draws
use stable back-to-front bounds depth with explicit layer overrides; intersecting
surfaces have documented approximation limits. UI and 2D overlays preserve
painter order and merge only adjacent compatible packets.

Instance adjacent draws only when mesh/submesh, layout, pipeline, bindings,
pass state, scissors and blend semantics agree. Instance range limits and
transform handedness/cull mode must be honored. Mesh-local indices and deliberate
buffer slices preserve R1's zero base-vertex/first-instance portable contract;
later native paths may use capabilities without changing scene identity.

Transient triangles/quads use a reserved upload allocator with checked sizes and
per-frame quotas. Persistent terrain, sprites and dynamic meshes use the same
packet path, with geometry preparation in their owners. No per-line/per-glyph
resource creation or virtual call. GPU culling/indirect draws later consume the
same logical scene records and expose the same source IDs; direct CPU rendering
remains available as a comparison mode.

## Views, viewports and virtual screens

Keep these concepts distinct:

| Concept | Meaning and lifetime |
| --- | --- |
| `ViewId` | Renderer-owned stable identity plus generation; one logical output's settings/history |
| Camera sample | Pose/lens input for this frame, not a render target or GPU owner |
| Physical target | Surface image borrowed for the acquired frame, or an owned offscreen texture version |
| Viewport | Top-left physical pixel rectangle and explicit canonical depth range |
| Scissor | Intersection of drawable bounds and clipping rectangles; empty is a no-op |
| Virtual screen | Authored logical extent and fit/fill/stretch/integer-fit policy mapped into a physical viewport |
| Render extent | Shading resolution, optionally scaled independently of presentation/overlay resolution |

Resolve against the **actual acquired extent**, not a requested window size.
Use one mapping for drawing and pointer coordinates: translate to viewport,
subtract fit offsets, divide by scale; reject letterbox bars unless the caller
requests clamping. Keep logical UI units, physical pixels, texture texels and
world units explicit. Round viewport edges once with checked integer arithmetic;
derive neighboring split-screen edges from shared boundaries to avoid cracks.
Scissors use consistent half-open bounds. DPI is input to the logical/physical
mapping and glyph raster size, not a second hidden multiplier.

For logical extent `(Lw,Lh)` and physical viewport `(x,y,W,H)`, fit uses
`s = min(W/Lw,H/Lh)` and centered offset `((W-s*Lw)/2,(H-s*Lh)/2)`.
Map logical point `p` to `(x,y) + offset + s*p`; invert that same mapping for
pointer input. Validate positive finite dimensions before division. Fill uses
the maximum scale; stretch has separate axis scales. The mapping record retains
the final rounded content rectangle so drawing, clipping and hit tests agree.

Zero-sized or hidden views skip work without allocating histories. Fit preserves
aspect with bars; fill crops; stretch is explicit. Integer-fit uses the largest
whole-number scale that fits; when none fits, select fractional fit and expose
that fallback in the mapping record. In a shared target, each view owns a rectangle;
load/store/clear semantics must preserve neighbors. Whole-attachment clears may
not masquerade as per-view clears. Compose offscreen views when scissored clears
or distinct lighting/extents cannot be supported correctly in one pass.

The renderer owns unjittered matrices for culling/picking and jittered matrices
for temporal shading. Use right-handed +Y-up/-Z-forward math, reverse-Z `[0,1]`
after the explicit RHI extension, and per-backend viewport/depth conversion.
History keys include ViewId generation, render extent, projection convention,
origin epoch and effect/layout revision. Reset on camera cuts, restart, incompatible
resize/projection changes and failed history production. Origin rebasing requires
consistent current/previous transforms or reset; it is not a camera teleport.
Dynamic resolution never changes simulation aim or logical UI coordinates.

## Textures, surfaces and GPU memory

Use three separate owners: content texture versions, GDI-managed transient
attachments, and RHI-borrowed acquired surface images. A surface is not an asset.
Texture descriptors declare dimension, format/encoding, extent, mips, layers,
samples and legal usage. Texture views name checked subresource ranges and
compatible interpretation; sampler state is independent. Reject unsupported
combinations before frame recording using per-format capability matrices.

Offline cooking generates semantic mips: color in linear light, normal-map
renormalization, mask coverage preservation where requested, and separate data
texture treatment. Choose compression per target profile, with a verified
uncompressed variant; a backend name never guarantees a codec. Streaming publishes
only initialized mip ranges with legal sampling clamps and exact immutable
binding snapshots. Begin with whole-texture replacement; partial uploads need
RHI range hazards and a residency protocol before they become a feature.

GDI upload tickets distinguish copied source ownership, scheduled destination
work and sampling readiness. Caller bytes may be freed only under that declared
source contract; staging ranges live through their backend consumption obligation.
Budget upload bytes/work per frame and apply backpressure. Never block rendering
on disk, synchronous readback or device-idle to make routine updates convenient.

Separate logical release, CPU lease release, submitted completion and physical
memory retirement. Descriptors/atlas regions cannot be reused while earlier
packets reference them. Report live, pending-upload, transient, cached and
pending-retirement bytes separately. Budgets include temporary old/new overlap
during hot reload, aligned storage and multiple frames in flight.

Start with transient pooling by exact compatible descriptor and completion proof.
Attachment aliasing is a later native path whose lifetime overlap and memory
requirements are validated; distinct WebGPU textures are not promised physical
aliasing. Histories and cached shadows are persistent imports. Every pass states
load/clear/discard and store/resolve intent, resource ranges and readers/writers.
GDI compiles those uses in the author's order; RHI commits state only on accepted
submissions. Undefined transient contents cannot be sampled.

Arseny Kapoulkine's *GPU Zen 2* article 4, sections 4.4–4.6, reinforces this
ordered plan and explicit attachment intent. Thanks for the pipeline/attachment
tradeoffs: precreate the legal material states, and use pass-integrated resolve
when supported and sufficient. A streamed destination enters the pass plan with
a readiness dependency; streaming scheduling itself remains a GDI service.
Ludus does not infer safety from a discard hint or assume all GPUs benefit equally.

Resize creates complete replacement target sets, publishes at a boundary and
retires old sets after use. On device loss invalidate all GPU incarnations and
history, keep recreatable CPU descriptions, and explicitly restart/rebuild.
Use fallback textures/materials only if ready and compatible. An invalid shader
or surface cannot trigger an undocumented backend switch.

## Materials, shaders and pipeline control

The proposed [render graph authoring architecture](render-graph-authoring.md)
owns reusable graph/function/resource nodes, camera output routes, scene coverage,
author-declared compilation configurations, shared schemas, incremental builds
and editor/reload integration. It is an authoring layer over these Renderer and
RHI contracts, not a claim that the current material graph editor is implemented.
Users can consume selected graph outputs; this never synthesizes shader variants.
Depth-only rendering requires an authored technique preserving geometry/coverage.

A material template declares a small shading family, parameter schema, legal
texture semantics, render domain and an explicit finite set of legal static
configuration tuples. Nested graph/function extensions contribute to the same
shader and pipeline count/budget report. A material instance
holds immutable values and exact texture versions. Instance edits create a new
candidate and publish transactionally; old packets retain the old bindings.
Begin with unlit, metallic/roughness lit, masked and premultiplied transparent
families. Limit two-sided and specialized effects to justified variants.
Runtime shader generation and arbitrary per-object specialization remain deferred;
the proposed editor compiles authored graphs offline to immutable packages.

Retain Ludus's pinned Slang/tool chain and SPIR-V, MSL, WGSL and GLSL ES artifacts.
Cook explicit reflection with numeric bindings, stage interfaces, parameter
offsets, matrix packing, sizes and alignment. Validate each emitted target;
equal total buffer sizes do not prove matching members. CPU constants use
generated serialization/packing at the boundary rather than accidental C++
struct layout. Shader source/artifact hashes and compiler/schema versions belong
in the cooked package; unsupported profiles reject cooking or select authored
variants, never silently omit fields.

View/Material/Draw are update-frequency conventions, not a promise of three
physical descriptor groups on the present R1 interface. Map logical data to each
profile's verified layout. Cache immutable binding snapshots, allocate dynamic
constants from aligned completion-scoped storage, and intern compatible layout
identities through GDI rather than relying on structurally similar handles.

Pipeline keys include shader versions, layout identity, vertex input, topology,
attachment formats, samples, raster/depth/blend state and declared specialization.
Viewport size and dynamic constants normally do not belong in the key. Hash
collisions compare descriptions. Enumerate the legal static variant space;
record used variants and prewarm before activation. Pipeline readiness is
asynchronous where the backend permits, with a compatible ready fallback or
explicit not-ready status. Cold first-use stalls remain visible telemetry.
Persist native caches only with validated device/driver compatibility; cache
failure is recoverable. Never claim offline shader compilation eliminates all
driver pipeline work. Reload swaps package, layouts, bindings and pipelines as
one validated version set; failed reload leaves the prior set usable.

## Lighting and the default pass sequence

Use a conventional forward baseline with bounded CPU-selected per-object lights.
Selection is stable and deterministic, with a declared maximum and an overflow
counter/quality policy. L5 adds an opt-in CPU/texture clustered profile across the
portable backends. Future compute assignment requires a compute/storage-capable
profile and its own measurement gate. Cluster bounds and light lists
must be conservative, capacity-checked and inspectable; overflow uses a declared
bounded fallback rather than corrupting memory or silently dropping arbitrary
lights. WebGL 2 retains CPU assignment and the L4 light-list option. Deferred shading, visibility
buffers, bindless, mesh shaders and ray tracing are separate options, not startup
requirements; their bandwidth/material/platform costs need workload evidence.

The review of Billeter, Olsson and Assarsson's “Tiled Forward Shading”
(*GPU Pro 4*, II.4, sections 4.4–4.8) adds a correctness rule: transparent lighting
cannot reuse light rejection derived only from opaque occupied depths. Start
with a regular view-space 3D grid covering the declared near-to-lighting-far range
and conservative light-volume intersections, independent of opaque occupancy.
Infinite-far camera projections still require a finite lighting range and a
declared farther-surface fallback. Later opaque-only depth/normal pruning uses
separate lists. Empty sky samples and every MSAA sample need explicit handling.
Thanks to the authors; their historical timings are not Ludus performance evidence.

Harada, McKee and Yang's “Forward+” (*GPU Pro 4*, II.5, sections 5.3 and 5.6)
motivates compact culling bounds separate from shading properties, occupancy
visualization and independent shadow budgets. Use a fixed-stride capped list per
cluster initially. Bounds-check both the local append and total buffer extent;
store the **written** count, never an unbounded attempted count. Saturation sets
an overflow flag and makes the shading variant select a stable bounded fallback
list prepared on the CPU and uploaded before assignment; this needs no synchronous
GPU readback. Async telemetry reports affected clusters and quality reduction.
Unshadowed lights and shadow assignments have independent limits. Compact
prefix-sum lists, light BVHs and 2.5D masks are
later alternatives. Thanks to the authors; do not copy their demonstration's
fixed workgroup/list capacities or make opaque depth masks authoritative for
transparent shading.

Use linear RGB shading, a documented GGX metallic/roughness BRDF with energy
conservation, roughness floors and numerical guards. Define directional light
illuminance in lux and point/spot luminous intensity in candela; author lumens
only through an explicit solid-angle conversion. Radius/cutoff and temperature
conversion are bounded; world distance is in meters. An emissive material does
not automatically illuminate surrounding objects. Exposure belongs to each view;
the initial mode is manual and stable. BRDF details, artistic approximations and
reference fixtures must be specified in the lighting implementation change.

Static direct lighting may be baked into lightmaps, while irradiance probes
approximate indirect diffuse for moving objects; filtered reflection environments
serve indirect specular. Cook versions carry bake-space transforms, UV/chart data,
normalization, scene/dependency hashes and encoding. Keep baked direct and dynamic
direct contributions distinct to avoid double lighting. A moved baked object/light
invalidates its bake or uses a documented dynamic fallback. Lightmap packing,
probe placement and content quality belong to cook tools; rendering only consumes
ready versions. Missing bakes select a declared neutral/ambient fallback.

Dynamic shadows start with one directional cascaded map and a bounded local-light
atlas. Stabilize cascade texel placement, derive splits from unjittered views,
reserve filtering gutters, bound bias in light-space/receiver units and expose
split/bias/occupancy debug modes. Shadow maps may deliberately use conventional
depth with an explicit sampler/compare contract even when the main view uses
reverse-Z. Static shadow reuse keys include caster/material/light revisions;
dynamic casters invalidate affected regions. Point/spot map costs count against
an explicit per-view/frame budget. No cache claims correctness from “light did
not move” alone.

An initial lit view follows this author-visible sequence:

| Stage | Inputs and outputs |
| --- | --- |
| Upload readiness | Exact prepared versions become available before their first use |
| Shadow passes | Light views and conservative caster lists produce depth maps |
| Optional depth prepass | Opaque/masked geometry produces depth; enabled only by measured benefit |
| Optional cluster assignment | View/light bounds produce capped cluster lists |
| Opaque/masked forward | Geometry, direct lights, bakes and environment produce linear scene color/depth |
| Sky and transparent | Declared depth behavior and ordered blending into scene color |
| Scene effects | Authored AO/temporal/other variants with explicit inputs/history, initially disabled |
| Exposure/tone mapping | Scene-linear HDR, when supported, becomes display-linear color |
| Debug/UI/text | Display overlays compose in linear display space at presentation resolution |
| Output | Encode once according to the actual attachment/output contract, then present |

World-space debug may instead compose into scene color **after** temporal effects
and history production, before exposure/tone mapping; use retained scene depth
for testing. Display overlays are display-referred. Tag the domain so neither is
tone-mapped twice and debug primitives do not enter temporal history.
HDR attachments/blending and multisampling require format/capability checks;
the portable baseline has an authored LDR path. Hardware sRGB output and shader
encoding are mutually exclusive. On a nonlinear unorm target lacking linear
blending, composite into a supported linear intermediate then encode once.
Antialiasing starts with a simple spatial variant or verified MSAA path (the Editor viewport's FXAA is the
spatial variant; see the output pass above); TAA is a later feature needing motion vectors, jitter and history rejection tests.

## Ray tracing and path tracing

Ray tracing supplies intersection and visibility queries. Path tracing uses those
queries to estimate light transport through sampled paths. A ray-traced shadow
does not require a path tracer; a path tracer does not require a native ray
pipeline or recursive hit shaders. Treat traversal, transport, sample reuse and
reconstruction as separate private components with shared scene contracts.

| Rendering mode | Role and initial scope | Cost and fallback policy |
| --- | --- | --- |
| Raster | Portable gameplay and direct-draw correctness path | Existing portable profile remains independently usable |
| Hybrid rays | Raster primary visibility plus selected ray visibility, reflections or indirect diffuse | Effects have independent ray/build/history budgets and authored raster/bake/probe alternatives |
| Progressive reference path tracing | Frozen-scene editor/debug captures; direct and indirect transport for supported materials | Accumulate independent samples without denoising or temporal reuse; explicit unsupported status on devices/content outside the profile |
| Real-time path tracing | Optional high-end gameplay mode with moving scenes | Requires measured sampling/reconstruction quality and tail latency; no universal frame-rate promise |

One `GraphicsRenderer` accepts the same scene/view/material versions for these
modes. Each mode composes a concrete pass plan; do not introduce another asset
manager, camera stack or GPU lifetime registry. Hybrid transport replaces a
specified lighting term rather than adding another copy of it. For example,
ray-traced indirect diffuse replaces the selected probe/lightmap indirect term.
Full path tracing evaluates supported emitters and scattering directly; it does
not also add baked lighting, ambient occlusion or prefiltered IBL approximations.
Record any intentionally mixed approximation in the capture's rendering profile.

### CPU and GPU reference implementation requirement

Building or extending a reference path tracer requires both a CPU oracle and a
GPU reference implementation. Deliver changes to reference transport, materials,
sampling and accumulation in both implementations in the same change. The CPU
oracle remains independently executable for correctness checks and debugging;
the GPU implementation keeps path transport, shading and accumulation on the
device for practical captures. CPU shading with batched GPU intersection queries
is a traversal variant of the CPU integrator, not the required GPU integrator.
Offline output/progress readback is allowed; a per-bounce CPU transport loop does
not establish GPU implementation coverage.

Both implementations must estimate the same declared integral: frozen scene and
resource versions, camera, material/emitter profile, texture filtering, depth and
range limits, sampler version, seed and sample target. Preserve reproducible
sample addressing and replay within each implementation. Scheduling and
floating-point differences may require documented numerical or statistical
tolerances rather than bit-identical images. Reference acceptance uses raw,
scene-linear, independently sampled outputs without denoising or temporal reuse.

Before claiming a reference feature complete, exercise both production paths on
matched fixtures, compare emission/direct/indirect and total radiance, and check
multiple seeds and depth sensitivity where relevant. Cover invalid contributions,
unsupported content, capacity failures, reset/cancellation and resource lifetimes
in each affected path. Record backend/device/toolchain, profile, sample counts
and comparison tolerances with the evidence. GPU capability rejection remains
explicit; selecting the CPU oracle is a caller choice, not a silent fallback.

This is the acceptance requirement for new work. RT1 pairs the `CpuOracle`
traversal with the [GPU-compute reference](#gpu-compute-reference), which runs
the same transport, shading and accumulation in compute kernels. `NativeQueries`
remains CPU transport with native traversal. The separate
[GPU preview](#gpu-resident-progressive-preview) still needs matched profile,
raw-output and production-path evidence before it can satisfy this requirement.
Existing coverage gaps remain open work, not evidence that both implementations
are qualified: GPU-compute pixel parity is so far qualified only on Apple M5 Metal.

## L6 implemented scope: RT0 and RT1

RT0/RT1 are an opt-in static triangle query profile and progressive RGB reference
capture. RT2–RT4, temporal effects, GPU-driven submission and virtual residency
remain plans. The scene import workflow is unchanged; configurable raster/ray capacities
are supplied by [L5.1](#l51-budgeted-scene-capacity); this does not yet establish faithful rendering of Staircase2.

RHI owns immutable native BLAS/TLAS incarnations and asynchronous query batches.
Vulkan negotiates API 1.2, buffer device address, acceleration structures, ray
queries and deferred host operations independently of ordinary Vulkan 1.1 raster
startup. Metal checks `supportsRaytracing` and compiles MSL 2.4. Browser/native
adapters without the optional profile reject native queries; preferred requests
preserve raster, required requests fail startup explicitly. Metal additionally exposes
native ray stage/function-table execution on hardware-ray devices supporting function
pointers. The logical RT node selects this separately from inline-query compute or
custom/software compute traversal; see [ray invocation authoring](../development/render-graph-authoring.md).
Updates/refits, procedural primitives, motion and asynchronous compute remain unavailable.

The installed `ludus_compile_ray_queries(TARGET ... NAME ...)` helper cooks the
fixed query kernel with verified Slang/SPIRV-Cross source and binary provenance.
It emits Vulkan SPIR-V 1.5 and MSL 2.4, preserves all five binding decorations,
and checks the 48-byte input / 32-byte result ABI and 64-thread workgroup. Linux
also runs pinned `spirv-val --target-env vulkan1.2`; macOS instead checks structure
and relies on real Metal compilation/reflection and execution, with that narrower
validation recorded in its manifest. Tools are prepared explicitly with
`./scripts/shader-probe bootstrap` and `./scripts/bootstrap-spirv-cross`.

Native scenes retain shared geometry and an ordered instance/opacity table,
including offscreen instances. Arbitrary finite nonsingular affine transforms,
eight-bit visibility masks, closest and any-hit visibility queries are supported.
Candidate acceptance uses explicit opacity/cutoff and optional retained alpha,
UV transform, nearest/bilinear filtering and clamp/repeat addressing. Geometry,
scene and program owners can detach while accepted batches retain their versions.
Polling never waits; shutdown drains accepted work before destroying native state.
Build/read failures and invalid/stale handles are explicit statuses.

### Reference workflow and frozen profile

After renderer initialization, call `EnableReferenceRendering` **before creating
meshes/material versions**, with positive CPU budget and tile size (1–2048).
Select `NativeQueries` explicitly with a cooked query program and ray-byte budget,
`CpuOracle` explicitly, or `GpuCompute` with its two compute kernels and a GPU
byte budget (see [GPU-compute reference](#gpu-compute-reference)). The CPU oracle
builds a balanced world-space BVH and uses double-precision permuted/sheared
triangle edge tests. No mode performs implicit fallback. Ordinary raster use retains no new CPU geometry or texture bytes.

Create lit L4/L5 material versions and meshes, then an immutable snapshot. Color
textures require `TextureDescription::RetainCpuContent` at upload; RGBA8 unorm/sRGB
initialized mips are retained, attachments/HDR retention are rejected. Capture
admission rejects unavailable mips, unlit surfaces, degenerate triangles,
non-finite or zero-length corner normals and other unsupported content, instead of
quietly replacing them. LOD selection,
bakes, AO, shadow atlases and prefiltered IBL do not enter reference transport.

Shading normals decide which side of a triangle reflects and emits, as in pbrt-v4
(Pharr, Jakob and Humphreys, *Physically Based Rendering*, 4th edition, section 6.5.3,
and `Triangle::InteractionFromIntersection`/`Triangle::Sample`): at every hit and
every sampled emitter point the geometric normal is flipped into the hemisphere of
the interpolated corner normal. Winding that opposes authored normals, and corner
normals on both sides of a face (smoothed normals across a crease), are therefore
admitted; where the interpolated normal cancels, the geometric normal shades.
Reflection is two-sided, as pbrt-v4's diffuse, conductor and layered models are
(sections 9.2 and 14.3): a path that reaches the back of the oriented face scatters
there with both normals mirrored towards the viewer instead of ending. Emission stays
one-sided unless authored two-sided. The CPU oracle, native-query transport,
GPU-compute reference and GPU preview share both rules. Capture profile 7 records
them; comparisons require matching profile versions, so profile 6 captures stay
distinguishable.

`BeginReferenceCapture` copies camera/light/integrator settings and retains the
snapshot. `AdvanceReferenceCapture` advances one bounded stage/tile or polls a
native batch. Even a Ready **setup** has zero samples until advanced. There are
four simultaneous capture slots. With `NativeQueries`, CPU transport/shading submits batched native
queries and explicitly reads their completed results; this implementation is an
offline diagnostic reference, with no GPU-resident transport loop or real-time
path-tracing performance claim. It never hides a wait/readback in raster submission.

The profile uses L4's isotropic single-scattering GGX, correlated Smith, Schlick
Fresnel and Fresnel-weighted Lambert, visible-GGX/cosine mixture sampling with
explicit null events, one light proposal per vertex, power-heuristic MIS and
compensated roulette. The proposal first picks a light uniformly among analytic
lights, emissive triangles and the environment. When it picks an emissive triangle,
a second bounded draw (per-bounce random slot `LUDUS_PATH_RANDOM_EMITTER_SLOT`)
replaces that uniform choice with one from an integer CDF over emitted luminance
times area, after PBR 4e section 12.6.2's power light sampler. Weights are quantized
against 2^24 and every positive one keeps at least one unit, so each emitter's
selection probability is exact and nonzero. Emitter hits look up the same
probability for MIS by binary search over the ascending emitter list. Many small,
bright emitters therefore no longer crowd out large, dim panels (for example the
candle flames and ceiling light of the pbrt-v4 staircase). Analytic lights and the
environment keep their uniform share; a power-weighted choice across categories is
future work. This is the default `ReferenceLightSampling::UniformArea` estimator;
[Light-sampling modes](#light-sampling-modes) describes the resampled alternative. Profile version 2 matches L4 point/spot range windows, the squared smooth cone
and the 1e-4 square-meter near-light attenuation floor. Version 1 used a different
spot interpolation and unbounded nonzero-distance attenuation; old captures retain
that profile identity and cannot be compared as the same integral. Emission is two-sided; reflection is restricted to the geometric and
shading upper hemispheres. Textured RGB decodes sRGB before fixed-mip filtering;
alpha stays linear. The capture mip overrides per-material LOD/mip filtering
and LOD clamps. There is no transmission, volume, spectral transport, coat,
multiple-scattering energy compensation or shading-normal energy correction.
Depth and finite secondary range are declared truncations. Local/world geometry, generated ray origins, UVs and transform components are
bounded to ±1,000,000 for the reference profile. Admission rejects oversized
finite UV scales before address arithmetic; invalid generated origins fail the
uncommitted tile equally in CPU/native mode. Reconstruction error uses a conservative absolute
FP32 affine-product allowance, geometric-normal projection and outward rounding;
near-zero offsets use a minimum normal float to tolerate GPU flush-to-zero.
Traversal uncertainty also includes twice the largest world-space triangle edge
component, multiplied by this profile's `1e-6` allowance on every coordinate axis.
Affine-only component bounds vanish along the normal of a coordinate-zero plane;
the resulting minimum-float offset allowed CPU/native near-zero self-hits in the
full-scene acceptance workload. The extent term preserves positive normal
uncertainty even on those planes and is shared by both traversal modes. Thanks to
Dirk Gerrit van Antwerpen, [*Solving Self-Intersection Artifacts in DirectX Raytracing*,
Error bounds](https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/)
(NVIDIA Technical Blog, 2023), for the cross-axis triangle-extent idea. This
world-space allowance differs from that article's RTX-specific object-space
constants; it is not a proof of robustness for arbitrary unresolved thin geometry
or every native intersector. The shifted-plane GPU regression and prepared-scene
CPU/native acceptance retain the original radiance tolerances.

Philox addresses are `(pixel, committed sample, dimension)` with versioned profile,
sampler and seed. Successful tiles publish compensated raw emission/direct/indirect
sums atomically; a bad contribution fails the tile, preserves committed means and
records its pixel/depth. Increasing only the target continues the same samples.
Changing snapshot incarnation, camera, dimensions, light values, mip, seed or
integrator settings builds a replacement transactionally and increments revision;
failed replacement preserves the previous image. Scene fingerprints are diagnostic
XXH3-128 digests, never identity/reset authority. Retrying the identical failed
request retries its uncommitted sample addresses. Release/disable cancels future
work and retires accepted native uses through completion.

#### Light-sampling modes

`ReferenceCaptureDescription::LightSampling` selects the next-event estimator for
emissive triangles. All modes estimate the same truncated integral under the same
`ProfileVersion`, so their means agree statistically, never per pixel. The mode joins
the integral key: changing it replaces the job and restarts accumulation.
`ReferenceCaptureInfo` records it with its sampler version.

| Mode | Sampler | Emissive next-event estimation |
| --- | --- | --- |
| `UniformArea` (default) | 1, 8 dimensions per bounce | One power-CDF triangle, one uniform area sample, one shadow ray. Existing captures stay bit-identical. |
| `ResampledProjectedSolidAngle` | 2, 128 dimensions per bounce | Resampled importance sampling of one triangle from 32 power-CDF candidates with an unshadowed LTC target, then one projected-solid-angle and one LTC sample of it, so up to two shadow rays per vertex. |
| `ResampledFormFactor` | 3, 128 dimensions per bounce | The same estimator with a cheaper target: Hill and Heitz's approximate horizon-clipped form factors instead of clipped polygon integrals, kept positive wherever the triangle is above the horizon, and its own frozen candidate counts. The selected triangle's samples and MIS weights are unchanged. |

The resampled estimator, its random slots and its BSDF-hit MIS are the
[many-area-light resampling review](many-area-light-sampling-reference-review.md).
The category draw at slot 0, analytic lights and the environment are unchanged.
- **Receiver.** Each vertex builds its shading frame and the renderer's
  [LTC table](../development/ltc-table.md) lookup at its roughness, view and
  `F0 = 0.04 (1 − metallic) + baseColor · metallic`. The diffuse technique's
  albedo is `baseColor · (1 − metallic) · (1 − F0)`, clamped to 0.01 per channel,
  as in the `ris-projected-solid-angle` template.
- **Light samples.** Each technique's direction is intersected with the selected
  triangle's plane. Emission, sidedness, alpha and the secondary range are
  evaluated at that point, as `UniformArea` evaluates its area sample, and the
  shadow segment ends one representable step before it.
- **BSDF hits.** A continuation that reaches an emitter is weighted against the
  surrogate density `P̃ (p₀ + p₁)` of the previous vertex. The path keeps that
  vertex's frame, LTC and albedo luminances, and its `Ẑ` once computed, so both MIS
  sites evaluate the same function of the direction from identical inputs.
- **Cost.** A `ResampledProjectedSolidAngle` vertex evaluates up to 41 triangle
  targets (32 candidates, the selected triangle and 8 normalization candidates),
  each with two polygon clips. A `ResampledFormFactor` vertex evaluates its
  candidates' approximate targets without clipping and prepares polygons only for
  the selected triangle and a BSDF hit's
  ([approximate target](many-area-light-sampling-reference-review.md#approximate-target)).
  `NativeQueries` tiles need three queries per path, so a resampled capture
  requires `3 × TilePixels` within `RayCapabilities::MaxRays` and returns
  `Unsupported` otherwise.
- **Probes.** Static-lighting diffuse probes stay on `UniformArea`; their virtual
  first receiver has no stored frame for the BSDF-hit density. Both resampled modes
  are rejected there.

The opt-in Catch2 tag `[.reference-light-sampling-benchmark]` in
`ludus_renderer_render_tests` compares the two modes on 3200 emissive triangles, a
third of them textured. It reports the transport kernel's device time per sample,
shadow rays per pixel, and MAPE and RMSE against a high-sample `UniformArea`
reference, at equal samples and at equal kernel time. It takes 4–7 minutes on Apple
M5 and is not part of the CTest run:

```bash
out/build/macos-clang-development/modules/graphics/renderer/ludus_renderer_render_tests \
  "[.reference-light-sampling-benchmark]"
```

On Apple M5 Metal (macOS 26.6, `macos-clang-development` with Clang 23.1.3 and
Slang 2026.1.2), on October 29, 2026, at 288 × 192:
- **Cost.** One `ResampledProjectedSolidAngle` sample took 17–38 times as much
  kernel time as a `UniformArea` sample: 20–22 ms against 0.9–1.3 ms with direct
  lighting only, and 82–90 ms against 2.3–4.1 ms at depth 4. It traced twice as
  many shadow rays: 0.92 against 0.46 per pixel with direct lighting only, and
  1.98–2.10 against 0.99–1.06 at depth 4.
- **Equal samples.** The resampled mode had lower error in every case, on the GPU
  and on the CPU oracle. With direct lighting only, MAPE fell by 2.0–2.3× and RMSE
  by 1.6–2.3×; at depth 4, by 1.4–1.6× and 1.6–1.9×.
- **Equal time.** `UniformArea` had lower error in every case: 1.7–3.3× lower MAPE
  and 1.8–3.7× lower RMSE, in three runs.

So `ResampledProjectedSolidAngle` is the lower-variance estimator per sample, but not
per unit of time on this workload. The
[review's evaluation](many-area-light-sampling-reference-review.md#rt1-equal-time-evaluation)
has the method, all measurements and the comparison with the template benchmark.

`ResampledFormFactor`, measured the same way on the same day in two runs, has
`ResampledProjectedSolidAngle`'s equal-sample error within 1% for 4.6–5.3 times less
transport kernel time: 2.6–6.5 times a `UniformArea` sample. At equal time it beat
`UniformArea` with specular direct lighting in both runs. It was within timing noise
of `UniformArea` with diffuse direct lighting, and still lost by up to 1.4× at
depth 4. The [approximate target measurements](many-area-light-sampling-reference-review.md#approximate-target-measurements)
have the tables and the `M` and `K` sweep.

CPU budgets cover renderer-owned mesh/material/job arrays and setup scratch,
including simultaneous old/new jobs during reset. They exclude existing renderer
state, RHI CPU upload shadows/registry storage, caller arrays and tool/driver memory.
Native budgets cover device-wide **requested** ray buffer/AS bytes and pending
build scratch, checked again before query submission. `GetRayResourceStats` reports
resident, pending scratch and detached bytes; pipeline/driver allocation and VMA
backing-page overhead are unavailable, not measured zero. These are admission
budgets, not total process/device-memory measurements. Capacity errors never drop
instances, lights, pixels or samples.

### GPU-compute reference

`ReferenceTraversal::GpuCompute` is RT1's GPU implementation. Cook the
`renderer_reference` and `renderer_reference_reduce` recipes with
`ludus_compile_renderer_shaders` and pass both kernels to the three-argument
`EnableReferenceRendering`, with positive CPU and GPU byte budgets, before any
mesh or material version exists. It needs negotiated compute storage but no
native ray queries. Devices without compute reject with `Unsupported`.

Setup is the CPU oracle's: the same admitted geometry, materials, retained
texture mip and binned-SAH BVH, packed once in the GPU preview's scene layout. Each
`AdvanceReferenceCapture(capture, surface)` step polls setup or a readback, or
submits one ordered graph with two compute passes in a frame of `surface`. The
transport pass traces one sample for every pixel at the image's minimum committed
count. It walks the BVH with float32 ports of the oracle's slab and watertight
triangle tests (`ray_bvh_traversal.slang`, shared with render-graph `ComputeBvh`
ray passes). Each edge function is evaluated in a canonical vertex order, so fast-math
multiply-add contraction cannot open cracks between triangles that share an edge. The
pass applies the same masked opacity, light proposals, power MIS,
visible-GGX/Lambert sampling and compensated roulette. It adds emission, direct
and indirect transport to Kahan-compensated device sums. Every random value uses
the oracle's Philox address, so both implementations trace matching paths. A
single-group reduction totals per-pixel query and event counts into a 48-byte
status record, the only per-sample readback. The committed sums, 48 of each
pixel's 96 accumulation bytes, are copied back in transfer-sized slices after
power-of-two sample counts, every 16th sample, a failure and the target.
`Ready` follows the final copy; until then `CopyReferencePixels` returns the
previous copy, whose counts can trail `MinimumSamples`.

Commits are per pixel. A nonfinite or negative contribution, invalid ray or BVH
stack overflow leaves that pixel's sums and count unchanged and records its
lowest failing pixel, addressed sample, scattering depth and validation stage.
The stage is retained through device reduction and host readback, including camera,
scene-layout and traversal-capacity errors; a failed GPU path does not report `None`.
The capture fails once the other pixels' sums are read back,
with `CapacityExceeded` for a stack overflow. The identical `Update` retries only
the uncommitted sample addresses; changing only the target continues the
accumulation, and other changes rebuild the job transactionally. `Release`
cancels future dispatches; the RHI retains accepted dispatches and readbacks
through completion. The GPU budget charges the packed scene and texels, the
224-byte parameters, 96 accumulation bytes per pixel and the status record. The
RHI resource budget, storage range and dispatch limits can reject a capture
sooner. Transfer rings, pipelines and driver padding are excluded. The trace kernel
also binds the renderer's 3,407,872-byte LTC table at slot 5, uploaded once when
rendering is enabled and charged to the GPU budget then. It reads the table only
for `ResampledProjectedSolidAngle`, which a parameter flag selects at run time.
Kernels cooked before the table binding fail `EnableReferenceRendering` with
`InvalidDescription`.

Normalized vertex colors and decoded sRGB texels stay in [0,1] in both paths.
The FoundationMath CPU/shader sRGB conversion preserves exact black and white
endpoints: GPU power/division rounding must not turn a white texel into an
out-of-range BSDF base color. Nonfinite inputs still fail validation; radiance
and throughput are not clamped. The white-texture raw-AOV regression covers
this case independently of the supplied Staircase2 World acceptance test.

Float32 is the only departure from the oracle's arithmetic. Slab exits widen by
`1 + 2γ(3)` (Pharr, Jakob and Humphreys, PBRT 3rd edition, section 3.9.2). The
watertight edge test omits the paper's float64 fallback for exactly-zero edges.
Near the edge opposite a triangle's first corner, the two float32 barycentric
quotients can round to a sum above one, which surface reloads such as
`GetSurfaceHit` reject. The traversal therefore clamps both to [0,1] and bounds
the third corner's weight by one minus the second's. The oracle's float64
quotients, cast to float32, already satisfy that bound.
Normalization and the environment CDF also use float32. These can change an
individual path at grazing or near-edge hits; they do not change the declared
integral. One dispatch covers the whole image, so very large dense scenes can
approach a device's command timeout; there is no tiling yet.

The CPU oracle and the GPU kernel share one BVH. It is built with a 16-bin
surface area heuristic over triangle centroids (Wald, "On fast Construction of
SAH-based Bounding Volume Hierarchies", RT 2007). Leaves hold at most four
triangles. Any range whose remaining SAH splitting could exceed depth 48 falls
back to a balanced median split, so traversal always fits the 64-entry stacks.
Each interior node records its split axis. Both traversals visit the child on the
ray's near side first, so a closest hit there culls the far child sooner. Closest
hits do not depend on node order, except where a box test's rounding decides a
grazing hit or an exact tie on a shared edge. In the staircase2 capture below, one
of 7,168 CPU-oracle pixels changed by 3e-7 luminance. In the GPU capture, 89 of
115,200 pixels changed and the mean luminance moved from 0.427322 to 0.427327.

`ludus_reference_benchmark` (built with the renderer's GPU tests) times a capture
of a cooked LUSCN scene, such as `ludus-cook cook` output for staircase2. It
reports setup-to-first-sample time, total wall time and the device time of the
transport passes. The device time is not inflated by CPU load from other processes.
It also prints a checksum of the converged radiance. Setting
`LUDUS_REFERENCE_BENCHMARK_OUTPUT` to a directory saves the last image as
float32 RGB:

```bash
out/build/macos-clang-debug/modules/content/scene_cook/ludus-cook cook \
  references/resources/scenes/staircase2 out/bench/staircase2
out/build/macos-clang-debug/modules/graphics/renderer/ludus_reference_benchmark \
  out/bench/staircase2 512 225 64 8 gpu 3
```

On Apple M5 Metal (macOS 26.6, debug build, about 29,000 triangles), the median
BVH needed 7.8–7.9 s of transport device time for 512×225 pixels at 64 samples
per pixel and depth 8. Wall time was 9.5–9.6 s. The SAH BVH with near-first
traversal needs 1.03–1.13 s of device time and 2.6–3.0 s of wall time. Of that
gain, SAH alone gives about 7× and near-first ordering about 10%. Setup to the
first sample grew by about 0.2–0.3 s for the larger CPU build. The CPU oracle at
128×56 and 8 samples went from 9.4–10.0 s to 2.9–3.5 s. Wall time is now mostly
the per-sample dispatch, wait and status readback, not tracing.

On Apple M5 Metal (macOS 26.6, debug build), the
[renderer test](../../modules/graphics/renderer/tests/reference_compute_tests.cpp)
renders the L6 fixture at 16×12 and 64 samples per pixel with both
implementations. The cases are orthographic depth 4 with the emitter and
environment; perspective depth 8 with directional, point and spot lights and
roulette from depth 2; and a single bounce. The largest RGB distance between
CPU and GPU means is zero for every AOV. Closest, null-BSDF and roulette counts
are equal; one of 11,661 visibility queries differed, where a shadow
contribution rounded to zero in one precision. A target-only update equals a
fresh capture bit for bit. The editor's lit viewport fixture, with textures and
point lamps, agrees within 0.002 per pixel. Vulkan and WebGPU compile the
kernels, but their pixel parity is unqualified.

On the same Apple M5 Metal debug build, `ResampledProjectedSolidAngle` is checked
in both implementations by the
[light-sampling unit tests](../../modules/graphics/renderer/tests/reference_light_sampling_tests.cpp),
the [shader parity kernel](../../modules/graphics/renderer/tests/reference_light_sampling_parity_tests.cpp)
and the capture tests in the renderer test above:
- **CPU and GPU.** Raw AOVs agree within the `UniformArea` tolerances: at most
  3.2e-5 RGB distance per pixel at 16×12 and 64 samples per pixel on the L6
  fixture at depth 4 with a sun, the dense glossy sphere at depth 6 and a room
  where most vertices estimate `Ẑ = 0`. Closest, null-BSDF and roulette counts are
  equal; visibility counts differ by at most 3 of 16,160. Matching candidates and
  tickets select the same emitters in both precisions on these fixtures; the
  tests still allow a selection to flip.
- **Against `UniformArea`.** Six seeds of 64 samples per pixel on the CPU oracle
  agree in mean emission, direct, indirect and total radiance at depths 1, 3, 4
  and 6. The largest difference is 1.4 combined seed standard errors (direct light
  in the room); the test allows 5 plus a 2e-4 floor. Primary emission is
  bit-identical. The seed standard error of direct light falls by 1.3× on L6, 1.4×
  on the glossy sphere and 2.6× in the room, and of glossy-sphere indirect light
  by 2×.
- **Edge cases.** An emitter behind every receiver gives zero radiance and no
  shadow rays in both modes and implementations. `Ẑ = 0` applies the inclusion
  bound bit for bit, an empty LTC-space clip disables the specular technique, and
  the light density is bit-identical at both MIS sites. The weights partition
  unity. On the device, 3 of 4,096 drawn receivers differ from the host by more
  than 1e-3 relative in an integral or density above 1e-5; smaller slivers are
  limited by single precision in both.
- **Identity.** `UniformArea` captures on both implementations, including query
  and event counts, are bit-identical to those before the mode was added.

### Raw capture and installed SDK

`CopyReferencePixels` copies raw linear means and committed counts without
exposure/tonemapping. `EncodeReferenceHdr` writes `256 + Width*Height*16` bytes:
magic `LUHDR01\0`, little-endian version/profile/sampler/header sizes at bytes
8/12/16/20, width/height/target/depth/roulette/flags at 24–44, seed/revision and two
64-bit scene digest words at 48–72, secondary range/mip/light count at 80–88,
column-major world-to-clip floats at 96–159, eye at 160–171 and environment at
172–183. Remaining header bytes are reserved zero. Flags encode CPU (1), center
rays (2), orthographic (4), reverse-Z (8), infinite far (16), partial (32),
GPU compute (64) and `ResampledProjectedSolidAngle` light sampling (128); neither
traversal bit marks native queries. The sampler field is 1 for `UniformArea` and 2
with flag 128; `scripts/reference-hdr` rejects any other pairing and records
`light_sampling` in its manifest.
Each top-left ordered pixel contains three FP32 mean RGB channels and a uint32
committed count. The light count/digest identify the retained scene; applications
must save original light/asset inputs for independent scene reconstruction.

The independent [installed SDK fixture](../../tests/sdk_consumer/reference_capture.cpp)
uses only public APIs and FoundationFilesystem atomic publication. Configure
`tests/sdk_consumer/renderer` with the installed SDK, shader compiler, validator
(Linux) and translator paths. Run `ludus_sdk_reference_capture out/reference.luhdr`
for the explicit CPU oracle, or append `native` to require native queries. Convert
without changing radiance with:

```sh
python scripts/reference-hdr out/reference.luhdr --pfm out/reference.pfm --manifest out/reference.json
```

PFM rows are flipped to its bottom-up convention; JSON retains counts, settings,
digest and capture SHA-256. Partial files remain visibly marked. Invalid headers,
counts and negative/nonfinite pixels are rejected.

### Evidence and departures

Unit tests cover deterministic replay, progressive target continuation,
transactional resets, snapshot retention, export/output preservation, BSDF mixture
PDF integration versus accepted mass, furnace energy and null proposals. Physical
Apple M5 Metal execution compares raw direct/indirect/total radiance against the CPU
BVH at 128 samples/pixel with an offscreen emitter and diffuse wall; RGB-vector
distance tolerance is 0.002, with nonzero indirect radiance and zero invalid samples.
Native query tests cover alpha rejection, offscreen instances and detached-owner
lifetimes. A public-only installed-SDK 32×32 capture at 256 samples/pixel also
agreed between CPU/native traversal: maximum RGB-vector error 7.3e-8 and RMS
9.8e-9 on Apple M5; equal scene digests and complete pixel counts were verified. These fixtures establish a baseline, not convergence for the full
reference scene suite or GPU speedups. Linux CI compiles Vulkan, runs validation
layers and preserves raster on software devices without the optional ray profile;
a ray-capable Vulkan hardware qualification remains necessary.

Implementation attribution beside code credits PBRT 4e §6.1/§13.4 for geometric
origin offsets and MIS/roulette, Heitz JCGT 7(4) §3–4/Appendix B for visible-GGX,
Woop/Benthin/Wald JCGT 2(1) §3 for the CPU triangle oracle, Filament §4.4/§5.2.2
for the shared reflection model, W3C CSS Color 4 §10.2 for sRGB transfer, and
Khronos/Apple for native query/build synchronization. The more ambitious plan below
retains future goals; it is not a statement that dynamic scenes, GPU transport,
all parity AOVs, arbitrary content import or full Staircase2 are already qualified.


### Traversal capability and backend boundary

Start with opaque triangle acceleration structures and shader ray queries on
verified native devices. Expose distinct capabilities for structure build/update,
inline queries, dedicated ray pipelines, procedural intersections and motion;
also query limits, alignment, scratch requirements and shader target support.
`Unsupported`, `NotReady`, capacity failure and device loss remain explicit.
Ray hardware, compute availability and bindless support are separate facts.

Khronos's [ray tracing guide](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html)
distinguishes shared acceleration structures from query and pipeline execution.
Queries fit ordinary shader passes; dedicated stages offer different scheduling
choices. Begin with query-based compute effects and a loop-based path integrator.
Keep nearest accepted hit and early-exit visibility as separate shader operations.
Native Metal stage execution uses visible function tables for ray-generation,
closest-hit and miss callbacks, plus intersection function tables for alpha acceptance.
Vulkan native stage pipelines remain unavailable. Backend stage support does not
promise a speedup over inline queries. Shader binding tables, native
addresses, hit-group indexing and recursion limits remain backend details;
do not force Metal's model into a Vulkan-shaped public table abstraction.

The existing Vulkan subset does not establish support: negotiate the necessary
extensions/features/dependencies in a separate RHI profile and validate shader
compiler output. [Metal acceleration structures](https://developer.apple.com/documentation/metal/ray-tracing-with-acceleration-structures)
also require device/OS/compiler evidence; the pinned MSL 2.3 raster target is not
proof that a proposed ray shader works. A newer ray target needs an explicit
toolchain/profile change. Ludus's current WebGPU/WebGL paths have no hardware
ray contract. Render-graph ray passes can select the `ComputeBvh` strategy, which
walks the renderer's packed BVH in ordinary compute with the RT1 reference's
traversal; it is another implementation with its own cost, not a hardware
capability or an automatic fallback. Its kernel already compiles to WGSL, but the
graph ray profile does not yet admit browser targets. Do not hide CPU readbacks
behind it.

### Scene and acceleration-structure ownership

Renderer's private ray scene references exact mesh, deformation, transform,
material and texture versions. BLAS (bottom-level acceleration structures)
describe geometry; TLAS (top-level structures) instance those geometries. RHI
owns native structures and authoritative retention; GDI schedules their work.

- Reuse one static BLAS per compatible mesh/geometry version across instances.
  Key it by layout, topology, build flags and opacity classification. TLAS records
  contain stable instance IDs, masks, transforms and bounded shading-table indices.
  A material reload that changes opacity may require a different structure version.
- The ray scene includes offscreen shadow casters and reflection/indirect geometry.
  Camera frustum culling of raster packets must not remove ray-visible instances.
  Streaming regions, ray masks, trace distance and proxy/LOD selection are explicit
  quality policies. Reference captures require a frozen complete declared scene;
  missing geometry/residency is a status, not an invisible approximation.
- Raster and rays use the same selected pose and object transforms. Skin/morph
  evaluation completes before building/refitting its BLAS and before raster use.
  Refit only when supported and topology/build flags permit it. Rebuild after
  measured traversal degradation; animated geometry can make repeated refits slow.
  Bound rebuild work and expose deferrals or quality reductions.
- Keep BLAS geometry local and use a declared render origin for TLAS, rays and
  shading. Multiple views can share a compatible TLAS or use different origin
  versions. Validate nonuniform/mirrored transforms and geometric-normal winding;
  singular transforms fail admission. Rebasing invalidates incompatible histories.
- Declare deformation-write to build-read, BLAS-build to TLAS-build and final
  build-write to trace-read dependencies. Scratch reuse waits for completion of
  all overlapping builds, not the end of CPU recording. Cross-queue execution
  requires explicit RHI ownership and completion evidence.
- Compaction or relocation publishes a new ready version after its copy/build
  dependency. Retain every old structure and shading resource until its accepted
  uses complete; TLAS versions retain the BLAS versions they reference. Never
  patch an in-flight TLAS address or replace its shading table in place. Build
  inputs remain alive through builds and longer when later shading reads them.

Start serialized on one queue with static geometry and a full TLAS build when
instances change. Add supported updates, amortized static builds, compaction and
queue overlap individually after measuring build time, traversal time and peak
overlap memory. AS caching is session-local initially; native serialization needs
an explicit device/driver compatibility and content validation contract.

### Material and intersection parity

Cook one surface description into raster evaluation and ray evaluation/sampling
variants. Private shader functions define BSDF evaluation, sample and probability
density with matching lobe weights and conventions; a shared name alone does not
prove equivalence. Validate albedo, roughness, normals, UVs, emission, handedness
and texture encoding with raster/ray AOVs (auxiliary output images).

The first reference profile is RGB, triangle surfaces, diffuse/GGX reflection,
opaque and explicitly alpha-masked materials, finite area emitters, analytic
point/spot/directional lights and an environment. Add transmission with Fresnel,
IOR/medium tracking and total internal reflection under a separate tested profile;
volumes, subsurface scattering, hair and spectral transport are further features.
Raster alpha blending is not a physically specified transmission model. Unsupported
materials make a capture incomplete or fail its requested reference profile;
an error-material preview must never be labeled a reference image.

Retain geometric and shading normals separately, including their front/back-side
meaning. Opacity acceptance uses the declared texture/threshold/LOD policy in
both closest-hit and shadow queries; an opaque early-exit query cannot be reused
for alpha-masked casters without candidate filtering. Two-sided masks and
transmission visibility are distinct policies. Ray texture filtering has no
implicit raster derivatives: initially use an explicit fixed-mip reference policy,
then add verified footprints/ray cones as a named filtering profile. Residency
and mip-policy changes invalidate accumulation; compare like filtering models.

Carsten Wächter and Nikolaus Binder's “A Fast and Robust Method for Avoiding
Self-Intersection” (*Ray Tracing Gems*, chapter 6, sections 6.2–6.3) motivates
barycentric hit reconstruction and scale-aware offsets along the oriented
**geometric** normal. Thanks to the authors. A global `0.001` offset, shading-normal
offset or primitive-ID rejection alone is insufficient. Verify the chosen method
under Ludus's transforms/compiler precision; the chapter itself identifies thin
crevices and extreme transforms as limitations. Define normalized directions,
ray interval units and endpoint handling, including offsetting both endpoints
for area-light visibility segments. Expose offset diagnostics instead of scene
authors tuning a hidden epsilon.

### Progressive reference integrator

Jakub Boksansky and Adam Marrs's “The Reference Path Tracer” (*Ray Tracing Gems II*,
chapter 14, especially 14.3.1–14.3.7) supports integrating an inspectable reference
into the existing renderer. Thanks to them: retain their staged primary-ray/AOV
checks, shared scene data and simple iterative transport, while replacing their
DXR-specific setup with Ludus's optional traversal contract. This is a planned
reference implementation, not a claim that their sample or companion code shipped.

1. Freeze a scene snapshot and all exact resource versions. Generate pinhole
   rays from the same camera/projection and viewport conventions as raster,
   sampling within pixel footprints. Compare unjittered center rays first; camera
   near/far clipping is an explicit primary-visibility policy, distinct from
   secondary-ray ranges. Thin lens and shutter/motion integration are extensions.
2. Carry radiance, throughput, sample state and previous sampling information in
   a bounded iterative loop. At a hit, evaluate emission, sample a light and its
   visibility, then sample the BSDF to continue. An escaped path evaluates the
   environment. Start with one light sample and one continuation per vertex.
3. Combine light and BSDF sampling with multiple importance sampling (MIS).
   Light selection probability multiplies the light's conditional density;
   convert area and directional densities to the same measure before weighting.
   Evaluate the competing density for BSDF-hit emitters/environment and handle
   delta lights/lobes separately. This avoids double-counting emission and
   excessive noise from choosing only one proposal. Zero-light and zero-density
   cases terminate or contribute zero safely, without invalid arithmetic.
4. Begin compensated Russian roulette after a defined minimum depth; divide
   surviving throughput by survival probability. A finite maximum depth truncates
   transport and introduces bias: report it in metadata and perform depth
   sensitivity checks. Accumulation converges to the declared truncated model,
   not an unqualified physical ground truth. Energy clamps, path regularization
   and roughness modifications belong to separately labeled preview profiles.
5. Accumulate scene-linear, unexposed radiance in FP32 with a defined numerical
   accumulation scheme and checked per-pixel sample counts. Exposure/tone mapping
   operate on the displayed mean, never on accumulated samples. For long exports,
   combine bounded sample batches with a higher-precision offline accumulator;
   document rounding and count limits. Export linear HDR plus the profile metadata.

Anders Lindqvist's “Multiple Importance Sampling 101” (*Ray Tracing Gems II*,
chapter 20, sections 20.1.3–20.2) adds the explicit same-measure and matching-PDF
checks. Thanks to Lindqvist; choose and test a documented heuristic rather than
copying the sample's scene/light assumptions. The [Gems review](renderer-systems-gems-review.md#ray-and-path-tracing-follow-up)
records this revision. PBRT 4th edition, [section 13.4](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer),
provides a further check on emission weights and compensated path termination;
Ludus's RGB/bounded profile differs from its broader transport implementation.

Use deterministic random sample addressing by pixel, sample index and dimension,
with a capture seed and versioned sampler. Retries reproduce uncommitted samples;
the accepted-work boundary controls accumulation just as it controls other
histories. Store an accumulation key containing camera, viewport/resolution,
scene/pose, lights, materials/textures, origin, sampler and integrator versions.
Reset on any change that alters the sampled integral; changing only display
exposure need not reset unexposed data. Overlays never enter accumulated radiance.
Raw, independently sampled accumulation is the validation output; a denoised
preview is an additional image. The optional graph
[Denoise operation](../development/render-graph-authoring.md#denoise-ray-output-with-oidn)
uses OIDN on linear HDR color before display conversion and leaves the accumulation
unfiltered. Its initial native sharing profile is Metal on Apple silicon. Measure convergence across multiple seeds.
Nonfinite contributions or broken density invariants invalidate a reference
batch and report the offending sample; replacing them with black silently would
bias the result. `ReferenceCaptureInfo::Failure` identifies the rejecting validation
stage, while `FailurePixel`, `FailureSample` and `FailureDepth` locate the path.
These fields are meaningful when `InvalidContributions` is nonzero; a backend
failure can occur without a path-validation record. Previously committed tiles
remain available for partial diagnostic comparisons. CPU and GPU reference paths
bound interpolated vertex RGBA multipliers to their admitted `[0,1]` domain before
shading and opacity tests. A three-term FP32 barycentric sum can otherwise turn
constant white into `1.00000012` and falsely reject a valid BSDF. Nonfinite
interpolation still fails validation; radiance and throughput retain their
existing checks. Capture jobs reserve a bounded residency budget for their pinned
versions and support cancellation, releasing submitted uses through completion.

### Hybrid and real-time transport

Introduce effects in order: selected shadow visibility, then roughness-aware
reflections, then indirect diffuse, each with its own quality and fallback policy.
Raster provides primary visibility and the attributes needed to launch secondary
rays. Hit shading uses the same ray-scene tables; screen-only geometry/material
data is inadequate for an offscreen hit. Assign contributions explicitly so
reflection/probe blending and partial shadow replacement do not double light.

Trace into effect-specific linear signal buffers with validity, normal/depth,
roughness, motion and any required hit-distance guides. Define whether signals
contain albedo, their exposure domain, motion direction/units and guide spaces.
Keep reconstruction behind concrete pass functions with those contracts. Reproject
using actual previous camera/object/deformation data; reject disocclusions and
incompatible material/light/geometry versions. Reflection motion may require hit
surface motion in addition to the primary surface. A stable camera alone does not
make a lighting history valid. Bound history memory separately for each view.

Start with ordinary sampling plus a modest, inspectable temporal/spatial filter.
ReSTIR DI (direct light sampling), GI (indirect paths) and PT (path resampling)
are different upgrades. Reservoir reuse requires specified target densities,
weights, sample identity, visibility revalidation, age limits and bias policy;
do not substitute “reuse the last ray” for an estimator. The May 2026 paper
[ReSTIR PT Enhanced](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/)
by Daqi Lin, Markus Kettunen and Chris Wyman identifies contemporary work on
reconnection, correlation and reuse cost. Its abstract is discovery evidence,
not a completed algorithm review or a Ludus speedup. Review the full estimator
and validate it against the independent reference before adopting it. The
[many-area-light resampling review](many-area-light-sampling-reference-review.md)
proposes RIS over emissive triangles with projected solid angle/LTC sampling,
first as a `ris-projected-solid-angle` render graph template and then as an
explicit second RT1 light-sampling profile. Both are implemented; the RT1 profile
is [`ResampledProjectedSolidAngle`](#light-sampling-modes) in the CPU oracle and the
GPU-compute reference, and `UniformArea` remains the default reference oracle.

Keep a single loop kernel initially. A wavefront implementation can later split
intersection, hit shading and shadow work into compact queues when divergence
and occupancy measurements justify added dispatches/storage; PBRT's
[chapter 15](https://pbr-book.org/4ed/Wavefront_Rendering_on_GPUs) is a follow-up
source. Queue bounds are correctness contracts: process bounded tiles or return
an explicit incomplete result instead of dropping paths. Sorting, ray reordering,
adaptive sampling and reduced resolution need independent quality/cost evidence.
Fix sampling variance and AS build/traversal costs before adding a general GPU
work scheduler. No vendor denoiser or resampling SDK is a mandatory dependency.

### Ray milestones and evidence

These optional milestones depend on explicit compute/material/attachment and RHI
extensions; they do not claim any existing R1 backend already traces rays.

| Phase | Deliverable | Acceptance |
| --- | --- | --- |
| RT0: scene/query foundation | Optional native capability profile, static triangle BLAS/TLAS, retained tables, primary and visibility queries | Raster/ray instance, position, normal, UV and depth agreement; offscreen hits; build/read/lifetime stress; unsupported devices preserve raster |
| RT1: progressive reference | CPU oracle and GPU reference implementations of the same frozen RGB surface profile, light/environment/BSDF sampling, MIS, raw accumulation and HDR export | Matched raw CPU/GPU emission/direct/indirect/total comparisons; furnace/Cornell/emissive fixtures, sampling/PDF normalization, multiple seeds/depths, no double lighting; unsupported content and overflow are visible |
| RT2: hybrid effects | Bounded shadows, reflections and then indirect diffuse with reconstruction/fallbacks | Raw and reconstructed images compared to RT1; moving/disoccluded/reloaded scenes, per-view history, build/ray/filter median and tail times |
| RT3: dynamic scene scaling | Skinned/morphed BLAS, measured refit/rebuild, optional compaction and scheduling | Pose parity, changing topology/opacity, extreme/mirrored transforms, thin geometry, streaming/rebase/loss and peak overlap memory |
| RT4: high-end real-time PT | Reviewed resampling, reconstruction and optional wavefront/ray-pipeline variants | Scene suite at fixed resolution/quality budget; temporal artifacts and error versus RT1; measured target-GPU benefit and disabled-mode support |

Track AS resident/scratch/pending bytes, build/refit/compaction costs, traced
ray classes, path-depth distribution, opacity candidates, NaN/invalid-PDF counts,
sample counts, rejected histories and incomplete tiles. Actual hardware traversal
statistics may be unavailable; distinguish estimates from measured counters.
Inspect instance/primitive IDs, barycentrics, geometric/shading normals, ray offsets,
BSDF/light PDFs, MIS weights, direct/indirect/emission AOVs and raw/filtered error.
Selected-pixel path logging is bounded and opt-in. Reference scene fixtures and
statistical image tolerances matter more than reproducing one vendor's exact
floating-point image. No synchronous shipping-frame readback is required.

## Debug drawing, sprites, text and fonts

Debug submissions contain explicit view mask, space, depth mode, color domain,
width units and lifetime. Start with lines, boxes, axes, spheres and labels.
Reserve a frame arena; persistent commands use generation-checked handles and
bounded lifetime, while each worker has a private append buffer with deterministic
merge. Optional overflow drops complete commands and increments counters.

Portable thick lines expand into triangles. Clip world segments against the
near plane before perspective divide; handle zero-length segments, finite joins,
caps and minimum physical width. World-unit and physical-pixel width are distinct.
Depth-tested, always-on-top and two-pass x-ray modes are explicit variants.
Hardware wide lines are optional because API limits and raster rules differ.
Translucent overlays preserve order; opaque depth-tested primitives may batch
under the ordinary compatibility rules. Geometry is shared across views where
possible, but clip/width expansion can be per view. Debug work must not modify
scene visibility, lighting or temporal history.

Text shaping, script/direction runs, cluster identity, font bytes and rasterization
remain with the existing Text owner. UI/paragraph code owns bidi itemization,
fallback, line breaking and carets; renderer never maps one Unicode code point to
one quad. Submit shaped glyph IDs/positions with a font version and **physical**
raster size. Preserve original cluster provenance for inspection.

The first text adapter uses size-specific grayscale coverage, an append-only
bounded R8 atlas, transparent gutters and no mipmaps, plus adjacent instanced
quads in painter order. R8 sampling and streaming atlas writes are explicit RHI
prerequisites; RGBA replication is a declared transitional variant if necessary.
Atlas coordinates stay stable until a version reset; submitted pages/regions
remain leased. Missing/not-ready glyphs have a visible deterministic fallback,
and atlas pressure is reported. Reuse layouts and glyph cache entries; do not
reshape/rasterize static labels every frame. MSDF or analytic outline rendering
is a separate measured option for large/transformed text, preserving shaping and
the application contract. Never assume subpixel LCD rendering survives browser
composition or arbitrary display orientation.

Preserve the selected [GraphicsText ownership](../../.kiro/specs/text-font-rendering/design.md#1-decisions-and-dependency-direction):
GraphicsText owns glyph preparation/cache, CPU atlas shadows and ordered text
lists. Its Renderer adapter contributes generic leased quad packets to the frame
plan; Renderer owns composition, and RHI owns GPU resources. Do not create a second
glyph cache inside Renderer or make CPU Text depend on graphics. The standalone
text milestone and its narrow coverage interface remain a valid independent
consumer; moving its uploads into shared GDI services is an explicit integration
change with the same readiness and lifetime guarantees. Its pinned shader build
path need not be replaced merely to share packet submission.

Sprites and screen quads reuse the submission allocator but keep independent
material/texture semantics. Nine-slice and layout remain UI policy. Batching across
a different scissor, blend mode, atlas page or layer is forbidden. Filtering
gutter/mipmap policy belongs to the atlas's content type; font rules do not
automatically apply to sprites or lightmaps.

The Aurelio Reis chapter (*Game Programming Gems 8*, 1.1) supports separating
fixed quad geometry from glyph-instance data, but its constant-array/console
constraints are historical. Thanks to Reis: measure one-glyph, short-label and
large-overlay cases before choosing a packing/instance path. An optimization
must preserve painter order and the Text shaping contract.

Manny Ko's atlas chapter (*Game Engine Gems 3*, 9.4–9.5) sharpens the cook/runtime
boundary. Thanks to Ko: reserve filtering footprints **before** packing, report
occupancy including gutters, and validate chart seams. Use shelves for mutable
rectangular glyph pages and an offline chart packer for lightmaps; only measured
offline packing loss justifies irregular bitmask packing. Lightmap mip generation
must respect chart adjacency and mip-safe padding (or a clamped LOD policy).
Glyph coverage uses transparent borders; unrelated sprites use their own edge
extrusion. Diffusing unrelated glyphs or colors across atlas cells is invalid.

## Inspection, budgets and optimization gates

Expose counters for extraction/culling/sorting/recording time, visible/culled/LOD
counts, batches/instances, triangles, binding/pipeline switches, upload bytes,
pipeline misses, light-list/shadow overflow, atlas occupancy, peak frame arena,
GPU memory categories and pending retirement. GPU pass timings are optional and
report unavailable distinctly from zero; asynchronous readback never stalls a
shipping frame. Report median and tail frame times with resolution, scene,
adapter/driver/backend/tool pins, frames in flight and cold/warm state.

Give every pass/resource/view a bounded label and each packet a source ID.
Inspect why an object was culled, which pipeline/version bound, exact texture
ranges, light selection, shadow ownership, viewport mapping and history resets.
Provide modes for direct CPU draws, unsorted packets, disabled batching, frozen
LOD, no shadows, constant materials, overdraw, normals, depth, cluster occupancy,
lightmaps, atlas pages and linear/output color checks. Capture the immutable
frame plan and reproducible resource revisions; native GPU captures supplement
that record. Do not build a second command serializer to obtain basic tracing.

Optimization acceptance compares image correctness and memory/latency as well as
throughput. Turn on a new path only after representative scenes show an improvement
at acceptable tail latency and maintenance cost. Keep opt-outs for investigation.
GPU-driven geometry, async compute, virtual textures and virtual shadow maps need
separate proposals with residency, synchronization, capability fallback and
debugging evidence. Ray/path tracing follows the optional RT0–RT4 plan above.
These are modern candidates, not unconditional
definitions of a good low-level renderer.

### Modern technique selection

The [SIGGRAPH 2025 Advances course](https://advances.realtimerendering.com/s2025/)
includes production work on stochastic many-light rendering, ray-traced world
lighting and order-independent transparency. The program establishes relevant
research directions; it does not establish their suitability or cost for Ludus.
The following choices are engineering judgments, with future algorithm reviews
required before implementation.

| Option | Adopt when | Contract required first |
| --- | --- | --- |
| Clustered forward | Many local lights and varied materials make per-object lists expensive | Conservative 3D assignment, checked lists and transparent coverage |
| Deferred/visibility buffer | Expensive opaque shading/overdraw dominates and bandwidth/material indirection wins in comparison | Separate transparent path, material reconstruction, derivative/MSAA rules |
| GPU visibility/indirect | CPU submission/culling dominates after ordinary batching | Stable object IDs, output capacity, conservative occlusion, direct reference mode |
| Bindless/mesh shaders | Verified target limits and workload justify reduced binding or geometry work | Descriptor/version retirement and authored portable shader variants |
| Temporal upscaling/TAA | Shading cost or image quality warrants history complexity | Motion vectors for camera/object/deformation, disocclusion, exposure and jitter conventions |
| Hybrid ray tracing / real-time path tracing | Dynamic scenes need quality beyond bake/probe/direct-light approximations | RT0–RT4: shared ray scene, retained structures, independent reference, reviewed estimators and reconstruction budgets |
| Virtual textures/shadows | Measured working set exceeds conventional residency | Page-table versions, feedback latency, initialization, eviction proof and coarse fallback |
| Async compute | Actual queue overlap improves total time without excessive bandwidth contention | Cross-queue dependencies, ownership and completion proof in RHI |

Optional advanced features must preserve scene/view/material identities and expose
their own diagnostic data. A tiny scene, simple UI or limited GPU should not pay
mandatory constant costs for a technique chosen to scale a different workload.
The [contextualized research plan](renderer-systems-gems-review.md#review-order-and-completion-criteria)
orders architecture/lifetime and shader-debugging reviews before advanced scaling.
Record the consulted source sections and a concrete contract change or experiment
before promoting a program description into an implementation decision.

## Interactive World viewport

The primary editor viewport renders the selected project's current World/level
extraction and current camera through the ordinary renderer. World ownership,
entity-to-mesh/material extraction, selection and camera controls belong above
GraphicsRenderer; the renderer consumes `Snapshot` and `ViewDescription`, just
as a game runtime does. It must not substitute a fixed acceptance scene or an
offline capture for the selected World.

`RasterViewport` packages that existing raster path into GPU output textures.
Its `ViewportColorMode::Linear8` mode supports unlit textured snapshots using a
linear RGBA8 scene attachment; the default prefers HDR for lit snapshots when
enabled. Unlit textured material programs cannot draw into HDR attachments.
Initialize views/lighting on the renderer, then initialize the viewport with
physical dimensions and exposure. On a World edit, publish a complete replacement
snapshot; on a camera change, supply its current `WorldToClip` and camera values.
`Update` atomically prepares the latest snapshot/view and preserves the prior
view on rejection. `Render` draws that view and tone-maps it on GPU; it requires
no reference input retention, BVH, compute or ray-query capability and has no
sample target or convergence state. Poll Pending resource/view setup before
rendering. No driver shader compilation occurs in the per-frame draw path.

Browser consumers can initialize with `shouldPresentToSurface = true` to composite
the encoded display texture into the acquired surface in the same submission.
Surface and viewport extents must match. This uses the renderer's ordinary
composition shader and applies no second tone map or encoding. The default
texture-only mode retains the native Qt interop contract below.

Acquire a completed `ViewportImage` with a `ViewportFrame` lease. Two leased
images apply backpressure and neither may be overwritten. The image is top-left,
physical-pixel RGBA8Unorm carrying **sRGB-encoded tone-mapped RGB**, matching the optional PT image convention.
The consumer must not apply a second tone map or encoding. A Qt adapter must negotiate the exact logical
GPU device and retain an RHI interop lease through foreign sampling completion,
then return the renderer frame lease. There is no CPU image presentation fallback.
For resize prepare a candidate viewport, swap when Ready, and retire the old
viewport after its consumers finish. Reset the viewport before its borrowed
Renderer/device; RHI interop leases independently retain external backing.

Raster mode uses authored materials, direct lights, environment and admitted
bakes/shadows. The default clustered deferred graph evaluates unshadowed direct mesh area lighting
from emissive triangles, merged into convex polygons where coplanar, using polygon integrals and fitted LTC specular,
for the records a conservative world-space culling grid lists at each pixel. Other
raster profiles retain their existing emission behavior. Emissive indirect transport
is still a separate lighting capability; see the [default raster contract](../development/render-graph-authoring.md#default-clustered-deferred-lighting).
No editor lights or fake environment are added to hide that limitation. Choose
optional GPU path tracing explicitly when emissive/indirect transport is wanted;
do not make it the prerequisite for ordinary scene navigation.

Native acceptance changes a snapshot's material color and its camera while
retaining a prior GPU image, verifies visible content changes and culling,
checks two-image backpressure and stale identities, and uses no tracing setup.
This establishes the renderer output contract. Selected-project loading,
interactive UI controls and Qt GPU presentation require the editor integration
and RHI native-sharing bridge; renderer fixture acceptance alone does not claim
those end-to-end checks are complete.

## GPU-resident progressive preview

`Renderer::EnableGpuPreview` explicitly enables an optional native-ray-query GPU path before mesh
and material creation. It requires negotiated compute, storage buffers and
StorageVertex support. Cook `renderer_gpu_preview.slang` as compute and
`renderer_gpu_preview_image.slang` as raster with the installed shader helper;
pass their independently reflected descriptors. Native Metal cooking requires the
pinned SPIRV-Cross translator. WebGPU and WebGL2 reject this path with
`Unsupported`. The first acceptance covers native Metal; other backends require
their own pixel qualification. There is no CPU transport/readback fallback and
these native-query artifacts require native BLAS/TLAS traversal. Triangle graph
kernels may explicitly select the software compute ABI described in
[render-graph authoring](render-graph-authoring.md); that path creates no native
structures and does not use opacity ranges. No automatic traversal fallback is selected.

CPU setup freezes the existing snapshot, retained normals/UVs/materials and one
retained RGBA8 texture mip, builds the static binned-SAH BVH, and packs GPU buffers.
Shared texture content is packed once, in a separate read-only buffer so the
geometry/BVH and texel payload each respect the R4 storage-range limit. `BeginGpuPreview` then owns that immutable
scene independently of caller snapshot/mesh/material owners. Native world-triangle
BLAS construction groups triangles by authored opaque/masked alpha mode, with an
opaque prefix and masked suffix represented by native geometry ranges inside one
BLAS and one identity TLAS instance. Opaque-only and masked-only scenes retain one
native geometry range; empty Worlds use an empty TLAS. `RayGeometryDescription`'s
`OpaqueTriangleCount` declares that prefix; zero preserves ordinary alpha-filterable
geometry. Native query results still expose the full geometry-local triangle index,
independent of the driver's range-local primitive IDs. The matching SDK query shader
reads the prefix count from private metadata preceding the existing triangle-UV table.

The renderer's packed indirection preserves original shading, material, emitter and
visibility identities independently of the CPU oracle's unchanged BVH order.
Software GPU reference packing also retains that original BVH order. The native
indirection's first word stores the masked range's primitive base. Each indirection word's Z
component keeps the unpartitioned BVH leaf order, so compute traversal can share the packed scene
with native opacity ranges. This is private preview kernel ABI version 8; recook preview and graph
shaders with the matching runtime/SDK.
Native geometry opacity bypasses opaque candidates for first-accepted-hit visibility
queries. Mixed-scene closest-hit queries retain forced nonopaque traversal and
commit opaque-range candidates directly without material/alpha lookups; masked
candidates evaluate the same fixed-mip texture, tint, vertex alpha and inclusive
cutoff in both queries. Automatic opaque closest-hit acceptance failed the stronger
CPU-oracle image tolerance on native Metal, so it is not enabled for mixed scenes. One
native BLAS build admits or rejects the combined ranges under the existing budget;
its input, structure and scratch charges remain under the existing lifetime ledger.
Bindings publish after native builds reach Ready and retain exact versions through
GPU completion. No per-pixel CPU transport or accumulation arrays enter GPU setup.
The retained-input setup machinery is shared with RT1; selecting its CPU-oracle
input retention internally does not execute its CPU tracing loop.

A compute dispatch performs closest/visibility native acceleration-structure queries,
shared material opacity filtering,
RGB GGX/Lambert sampling, analytic-light/emissive-triangle/constant-environment sampling, MIS,
roulette and compensated accumulation. It uses RT1's addressed Philox dimensions
and world-space origin error allowance. A second ordered raster pass draws one
fullscreen quad; its fragment shader reads compute-written display colors by pixel
coordinate into an RHI attachment texture. Current preview admission retains the
2^24-pixel bound. The graph declares storage history, compute-to-fragment visibility
and the sampled texture export. No ray, radiance, counter or pixel readback occurs in these
renderer APIs. GPU numerical failure makes an error pixel magenta; CPU progress
reports completed dispatches, not observed valid sample counts or convergence.

The native opacity regression (`[opacity]` in `ludus_renderer_render_tests`)
compares empty, opaque-only, masked-only and mixed native-preview scenes against
raw linear CPU-oracle and software GPU-reference RGB and displayed pixels at
64×64 and 16 samples, with matched seed and camera.
Linear channel tolerance is 0.001 radiance units plus 0.5% of the reference channel;
display tolerance is two RGBA8 levels. A private test-build accessor reads completed
accumulation while retaining the frame; builds with tests disabled omit the accessor. Its mixed fixture contains both rejecting and accepting masked surfaces,
distinct emissive materials and directional shadows. `[opacity-budget]` exercises
combined-range budget rejection and resource cleanup. The
`[scene-shader][opacity]` unit test verifies stable original identities and that GPU
partitioning leaves the CPU BVH order unchanged.

To benchmark the production preview path, run the paired harness:

```sh
python3 scripts/python/benchmark_ray_opacity.py \
  --executable out/build/<native-build>/modules/graphics/renderer/ludus_renderer_render_tests \
  --output out/opacity/paired --runs 3
```

For each scene it alternates the original single nonopaque BLAS (CPU-BVH primitive
order and original uniform traversal flags) and partitioned layouts within the
same test executable and compiled shader.
The original layout control exists only in test builds; production always partitions.
`LUDUS_OPACITY_BASELINE=1` selects that control for a standalone `[opacity]` run.
`LUDUS_OPACITY_SCENE` optionally selects `empty`, `opaque`, `masked` or `mixed`.
The runner writes a JSON comparison of per-run/pooled timing medians and displayed
and raw linear differences. It reports differences without treating timing or
image similarity as independent CPU-oracle acceptance.

The benchmark uses 512×512 pixels, depth two, seed one and 48 accumulated samples
per scene; it discards the first eight timing samples. Each scene writes its final
`.rgba` image, `.linear.bin` raw readback (two binary32 float4 records per pixel:
mean RGB/sample count, then compensation/history tag), `.gpu.bin` device-clock frame intervals (binary64 nanoseconds),
`.wall.bin` submit-to-completion intervals (uint64 nanoseconds), and `.setup.bin`
CPU preparation interval (one uint64 nanosecond value). GPU intervals include the
preview compute and output raster pass; they are not isolated traversal timings.
Setup measures `BeginGpuPreview` through native readiness and is reported separately
from warm frames; device/renderer/material-program enable-time prewarming precedes it. Retain the unchanged executable for an additional original-build
comparison with `--baseline-executable <saved-unmodified-tests>`; that mode alternates
whole-scene batches for legacy harness compatibility. The in-process layout control
isolates partitioning from compiler changes. Each run records executable hashes.
Benchmark mode omits the
small CPU-oracle render; run the ordinary regression separately. GPU timings must
be available; the harness never substitutes CPU time for missing GPU measurements.

On 2026-10-26, an 8-core Apple M5 GPU on macOS 26.6, Clang 23.1.3 and the
Development profile produced the following original-executable comparison. The
unchanged executable was retained from base `8e35d219`, and the opacity implementation
was benchmarked at `9b94fc10` before integration with newer `develop` changes. Each row pools five
alternating runs of 40 warm frames. The editor viewport was closed. GPU intervals
include compute and the output raster pass. The mixed fixture has 6,144 triangles:
one opaque layer, one rejecting masked layer and one accepting masked patch,
with emissive materials, a directional light and two-bounce transport.

| Scene | Original executable GPU median (ms) | Opacity ranges GPU median (ms) | Original/ranges |
| --- | ---: | ---: | ---: |
| Empty | 0.956 | 0.986 | 0.970× |
| Opaque only | 3.529 | 4.120 | 0.857× |
| Masked only | 5.440 | 5.426 | 1.003× |
| Mixed | 6.152 | 5.461 | 1.127× |

These measurements do **not** establish a reliable speedup. A separate five-pair
same-kernel layout comparison gave mixed medians of 2.264 versus 4.919 ms, the
opposite direction. Its unchanged opaque control varied from 1.006 to 4.234 ms
between runs. Background GPU activity and frequency variation confound these
process-level measurements; more isolated device measurements are required before
claiming acceleration or a regression. Individual timings and setup/wall medians
remain in the runner's JSON report rather than being discarded.

After integration at `6a149003`, three further alternating layout pairs measured
mixed medians of 6.652 versus 6.305 ms (5.2% less GPU time). The unchanged masked
control measured 5.889 versus 5.058 ms, while opaque measured 3.441 versus 3.860 ms.
Those larger control changes leave the acceleration claim unresolved. The final
repetition retained bit-identical RGBA8 and the same raw RGB error bounds below.

All four scenes produced bit-identical RGBA8 output in both final comparisons.
The same-kernel comparison also retained raw linear readbacks: empty, opaque-only
and masked-only RGB were bit-identical; mixed maximum absolute RGB difference was
1.78814e-7, RMSE 6.72124e-10 and mean signed difference -9.09495e-13. Independent
CPU-oracle regressions passed the stated per-channel tolerance at 64×64 and 16
samples. An earlier automatic-opaque closest-hit variant failed that stronger
regression and was rejected; the final visibility optimization preserves the
existing closest-hit traversal mode. Thin/near-edge native FP32 precision and
arbitrary-scene convergence remain independent qualification limits below.
The harness retains raw readbacks and individual timing samples under its chosen
output directory; generated/machine-local evidence is not source-controlled.

Visibility queries use the same native traversal and candidate alpha acceptance as
closest-hit queries, but consume only committed hit/miss status. They skip committed
primitive/range remapping, distance and barycentric reads. The closest-hit result
and preview ABI remain unchanged. Ray-path capture variants retain full shadow-hit
extraction so diagnostic records still contain committed identity and distance;
normal rendering keeps the visibility-only fast path. Explicit software graph traversal retains its
existing hit/miss/failure contract. The diagnostic recipe
`renderer_gpu_preview_visibility_full` shares the production integrator but restores
full committed-hit extraction for shadows; it exists for reproducible A/B evidence.
Generated Metal has one set of committed primitive/geometry/distance/barycentric
reads in the production kernel (closest-hit), versus two sets in that diagnostic.

Run the frame-interleaved visibility benchmark with:

```sh
python3 scripts/python/benchmark_ray_visibility.py \
  --executable out/build/<native-build>/modules/graphics/renderer/ludus_renderer_render_tests \
  --output out/visibility/interleaved --runs 5
```

Both immutable pipelines and scenes are prepared on one device/queue/surface before
timing. Every sample pairs full-hit and visibility-only frames, alternates AB/BA,
and reverses the initial order between independent processes. No image readback or
file I/O occurs between timed pairs. Each scene uses 512×512 pixels, depth two,
seed one and 128 samples, discarding the first 16 pairs. A tessellated receiver and
opaque or accepting masked slats create blocked and unblocked shadow rays; masked
and mixed scenes add a fully rejecting masked layer. Materials are nonemissive,
with one directional light and constant environment. Opaque has 3,072 triangles;
masked and mixed each have 5,120. Empty is the no-geometry control.

The runner freezes and hashes its executable, preserves individual GPU/wall timings,
records AB/BA order and the CPU gap from the first completion observation to the
second submission, and checks complete per-pixel sample coverage. Its JSON reports
paired ratios, separate AB/BA ratios and run medians. The 95% percentile bootstrap
resamples independent process run medians, not correlated frames. GPU intervals
include compute and output raster and do not isolate traversal. Final RGBA8 and raw
linear readbacks happen after timing. The private little-endian artifact protocol
uses each scene's `.configuration.bin` (version, extent, target samples, warmup
samples as four uint32 values), `.order.bin` (uint8 baseline-first flags) and
`.gap.bin` (uint64 nanoseconds), with each variant's `.gpu.bin`, `.wall.bin`,
`.rgba` and `.linear.bin` using the formats described above. Existing output
directories are rejected to preserve evidence.

On 2026-10-26, two five-process repetitions on the same 8-core Apple M5 GPU,
macOS 26.6, Clang 23.1.3 Development build based on `0fbb4f29` gave the following
results. Each process contributes 112 balanced AB/BA pairs per scene (1,120 total
pairs per row). Times are medians of process GPU medians; reductions and intervals
use process median paired ratios, so they need not equal the ratio of those times.

| Scene | Full hit (ms) | Visibility only (ms) | Paired GPU time reduction | 95% run-bootstrap full/visibility ratio |
| --- | ---: | ---: | ---: | ---: |
| Empty | 0.2492 | 0.2515 | -1.13% | 0.9871–0.9923× |
| Opaque | 1.3071 | 1.2916 | 1.19% | 1.0108–1.0129× |
| Masked | 2.3816 | 2.3204 | 2.54% | 1.0256–1.0265× |
| Mixed | 2.5444 | 2.4810 | 2.47% | 1.0249–1.0256× |

Both submission orders retained gains on geometry scenes: median AB/BA ratios
were 1.0110/1.0130× opaque, 1.0241/1.0285× masked and 1.0231/1.0278× mixed.
The empty control was slower overall, with order sensitivity (0.9823/1.0003×).
The largest recorded CPU pair gap was 1 microsecond. This is a modest measured
improvement on these Metal fixtures, with an approximately 1% empty-scene cost;
it is not qualification of other scenes, GPUs or native backends.

Across all ten processes, every scene retained bit-identical normalized raw RGB
and RGBA8 output. The ordinary `[visibility]` regression separately compares both
kernels at 64×64 and 16 samples, verifies complete sample coverage, then compares
the fast path with independent CPU-oracle and software GPU-reference raw RGB at
the existing 0.001 absolute plus 0.5% relative channel tolerance. It also requires
zero invalid reference contributions and visibility-query coverage on geometry
scenes. Run this ordinary mode independently of benchmark mode, which omits oracle
renders during timing. Neither denoising nor temporal reuse establishes parity.

The opt-in `renderer_gpu_preview_hit_cached` recipe experiments with per-hit material
word-address and interpolated-UV reuse. The default preview, reference integral,
packed-scene ABI and candidate acceptance remain unchanged. `HitMaterial` is local
to one intersection; it introduces no persistent history, host allocation or GPU
buffer. Map transforms, fixed-mip filtering, decoding and normal mapping retain
the existing texture helper, including its out-of-line SPIR-V filter. The material
word constants are shared by the host packer and shader readers.

Generated-code inspection at base `44d7510a` found three separate primary-hit UV
interpolations for emission, base color and surface maps. The default artifact has
8,745 SPIR-V instructions, 907 static `OpLoad` and 354 `OpAccessChain` instructions;
the opt-in artifact has 8,571, 874 and 321 respectively. The default retains the
original counts. These are module-wide static instruction counts, including local
loads and branches, not measured device-memory traffic. Translated Metal source
is 160,150 versus 158,462 bytes. SPIRV-Cross still marks the texture helper
`always_inline`; downstream driver optimization can eliminate source redundancy.
Fewer intermediate instructions do not establish faster GPU execution.

Use the same interleaved harness with the cache experiment selected:

```sh
python3 scripts/python/benchmark_ray_visibility.py --experiment hit-cache \
  --executable out/build/<native-build>/modules/graphics/renderer/ludus_renderer_render_tests \
  --output out/hit-cache/interleaved --runs 5
```

The experiment prepares default/uncached and opt-in/cached pipelines together and
uses the same matched-sample AB/BA protocol, warmup, timing scope, executable hash,
complete sample-coverage checks and independent-process bootstrap described above.
Artifacts use `uncached/` and `cached/` variant directories. Four untextured controls
are joined by `textured-nearest` and `textured-linear` scenes with retained sRGB base
and emission maps, a linear normal/roughness/metallic map, explicit mesh UVs, negative
V scale, repeated U and clamped V. Each textured scene has 5,120 triangles. Its
emissive surfaces exercise mesh-light sampling too. The ordinary `[hit-cache]`
regression compares final RGBA8 A/B output, raw RGB, and the independent CPU oracle
and software GPU reference at 64×64/16 samples, with the same documented channel
tolerances. Benchmark mode omits oracle rendering during timing.

On 2026-10-27, five independent processes on an 8-core Apple M5 GPU,
macOS 26.6 and Clang 23.1.3 Development measured 112 balanced AB/BA pairs after
16 warmup pairs per process: 560 measured pairs per scene. Positive savings mean
the cached recipe used less GPU time. Each row uses the median of process median
matched-frame ratios; the bootstrap resamples those five process medians.

| Scene | Paired GPU time saved | 95% run-bootstrap uncached/cached ratio |
| --- | ---: | ---: |
| empty | 0.76% | 1.0009–1.0106× |
| opaque | -2.33% | 0.9760–0.9803× |
| masked | -0.39% | 0.9918–1.0015× |
| mixed | -0.74% | 0.9846–1.0003× |
| textured-nearest | -0.04% | 0.9948–1.0003× |
| textured-linear | 0.31% | 0.9985–1.0067× |

All six scenes retained bit-identical normalized raw RGB and RGBA8 across all five
processes. The ordinary regression also passed independent CPU/software-GPU parity.
The maximum recorded CPU gap within a pair was 2.754 ms (empty control); the
largest geometry-scene gap was 0.186 ms. Individual gaps and both order-specific
ratios remain in the JSON report; no additional timings were discarded. The
textured-linear median gain is small and its interval includes no gain; opaque
rendering has a repeatable regression. This does not establish a general speedup.

Two earlier variants were also measured and rejected. Resolving material addresses
through the texture filter reduced static loads from 907 to 805 but regressed the
opaque fixture by 2.40% over five runs. Adding lazy UV evaluation and per-filter
metadata caching regressed opaque by 4.21% and textured-linear by 1.77% over three
runs. The final implementation keeps the texture-filter boundary unchanged. A
separate three-run repetition of that narrow implementation also measured an
opaque regression (2.44%) and only 0.23% textured-linear savings. These results
motivate the opt-in status rather than a default performance claim.

The recipe remains an opt-in experiment. Native backend qualification and a
representative measured improvement are required before default activation.

The `pass-timings` benchmark experiment uses the existing RHI per-pass probes to
measure the named `GPU preview transport` compute operation and `GPU preview image`
raster operation separately. Both variants use the same production shader with
material caching disabled. `unprobed` keeps the frame probe alone; `profiled` also
arms `ArmGpuPassTimings` before submission and polls `GetGpuPassTimings` after
completion. Their matched AB/BA frame intervals expose the observed instrumentation
overhead, independently of the per-pass durations. Run:

```sh
python3 scripts/python/benchmark_ray_visibility.py --experiment pass-timings \
  --executable out/build/<native-build>/modules/graphics/renderer/ludus_renderer_render_tests \
  --output out/ray-pass/interleaved --runs 5
```

It reuses all six opaque/masked/textured fixtures, sample addresses, warmup and
independent-process repetitions above. The ordinary `[ray-pass-timings]` regression
checks the same shader with probes off/on, sample coverage and independent CPU/
software-GPU reference parity. Benchmark mode validates exactly one compute and
one raster interval per profiled frame, their distinct nonzero scope identities,
stable names/precision, zero dropped records, and explicit Available states with
finite nonnegative durations. A quantized zero is retained and counted. Unsupported
per-pass sampling skips the ordinary hardware test, but fails a requested benchmark;
unavailable, disjoint, missing or unexpected data never becomes a CPU-time fallback.

These intervals include GPU stalls. Metal raster timing is the elapsed envelope of
vertex/fragment stages, which can overlap other operation envelopes. The report
therefore retains each raw duration and, when the RHI supplies validated clock
alignment, their overlap and the output's end-time tail after compute. The latter
is `max(0, outputEnd - transportEnd)` on the calibrated clock, not raster shader
busy time. Overlap/tail statistics use only aligned samples and always report their
count and the largest clock-mapping uncertainty. Missing alignment leaves these
statistics absent while raw GPU durations remain available. Pass durations are
not an additive partition of frame time. The [profiling contract](profiling-final.md)
owns backend stage, clock-calibration and uncertainty details.

In each `profiled/` directory, `.transport.gpu.bin` and `.output.gpu.bin` contain one
little-endian binary64 nanosecond duration per measured sample. `.pass-info.bin`
contains seven explicit 64-bit words: version 1 (uint64), transport/output tick
nanoseconds (binary64), their valid-bit counts (uint64), and their scope identities
(uint64). Zero precision fields mean unknown; identities remain process/device
local. `.pass-names.bin` contains transport then output UTF-8 names, each prefixed
by its uint32 little-endian byte length, without a terminator. `.pass-intervals.bin`
has six uint64 little-endian words per sample: transport start/end, output start/end,
clock uncertainty in nanoseconds, and a 0/1 alignment-availability word. Unavailable
intervals contain zero timestamp/bound words and are not interpreted as zero overlap.
The JSON preserves per-run metadata, names, precision, zero-duration/alignment counts,
pass medians and process-bootstrap intervals, plus the original frame and image
checks. No image readback or file I/O occurs between paired submissions; counter
resolution happens after completion and its CPU cost is visible in the pair gap.

Measurement on 2026-10-27 used Apple M5 (8 GPU cores), macOS 26.6,
Clang 23.1.3 `RelWithDebInfo` (`-O2`), the then-default 64×1 visibility-only / uncached
shader, 512×512 pixels and 128 samples (16 warmup). Five independent processes
contributed 112 matched AB/BA pairs per scene each. The source base was
`02df0b37e3e2283f88a86ea1a012e2794a8905d6`; the frozen executable SHA-256 was
`7da4672c54616f898f58905a7f9145e64914aa5594b579c00eb3c0875cc30e91`. Values below are medians of
process medians; bracketed ranges are 95% process-bootstrap intervals for raw
pass durations. Durations and overlap/tail are in milliseconds.

| Scene | Transport [95% interval] | Output envelope [95% interval] | Overlap | Output tail | Aligned samples | Max clock uncertainty (µs) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| empty | 0.5345 [0.3561, 0.6879] | 0.5906 [0.3935, 0.7491] | 0.5075 | 0.0499 | 558/560 | 0.667 |
| opaque | 1.8432 [1.3666, 2.6971] | 1.8757 [1.3445, 2.7698] | 1.8361 | 0.0629 | 560/560 | 5.125 |
| masked | 3.4468 [2.6952, 6.2121] | 3.4984 [2.7276, 6.2898] | 3.4346 | 0.0664 | 560/560 | 9.000 |
| mixed | 3.6646 [3.2291, 6.2483] | 3.6408 [3.2203, 6.3440] | 3.5853 | 0.0670 | 559/560 | 17.730 |
| textured-nearest | 4.2184 [3.6288, 5.0066] | 4.2706 [3.6437, 5.2127] | 4.2063 | 0.0612 | 559/560 | 8.938 |
| textured-linear | 4.8813 [4.5703, 8.0433] | 4.8721 [4.5881, 8.0585] | 4.7998 | 0.0742 | 556/560 | 13.667 |

All 3360 profiled frames retained valid GPU pass durations; eight lacked validated
clock alignment and were excluded only from overlap/tail statistics. Tick precision
was 1 ns with 64 valid bits and no zero-duration samples. The large raster envelope
mostly overlaps transport; neither it nor the output tail identifies raster shader
busy time. Absolute times vary substantially between processes, so these are
observed workload intervals rather than uncontended hardware costs.

Matched frame overhead point estimates ranged from −0.71% to +0.60%; every
95% process-bootstrap unprobed/profiled ratio interval included 1. This does not
establish zero probe overhead. Median inter-frame pair gaps were 0.012–0.020 ms
across scene/process combinations; the JSON retains AB/BA splits and gaps for
order-bias inspection. Every displayed RGBA8 channel and raw linear RGB value
was identical between unprobed and profiled variants, with complete sample
coverage. The ordinary regression separately checks the independent CPU oracle
and software GPU-reference outputs at 64×64 / 16 samples using the tolerances above.

The standalone native-preview transport uses an 8×8 workgroup. Dispatch and image
admission use the supplied compute artifact's reflected X/Y dimensions, including
partial edge groups; Z must be one. Zero dimensions reject as `InvalidDescription`,
and workgroup volume/dimensions or dispatch counts exceeding device limits reject
as `Unsupported`. Authored render-graph callback entry points default to 64×1 and can opt into
8×8 through the scene compute state; see the authored-graph experiment below. This experiment qualifies the standalone preview workload, with
performance evidence from the M5 backend below.

Reproduce workgroup comparisons against the former 64×1 shape with:

```sh
python3 scripts/python/benchmark_ray_visibility.py --experiment workgroups \
  --workgroup 8x8 \
  --executable out/build/<native-build>/modules/graphics/renderer/ludus_renderer_render_tests \
  --output out/workgroups/confirmation --runs 5
```

`--workgroup` also accepts `32x1`, `128x1` and `64x1`; the last is an A/A control.
Both `baseline/` and `candidate/` use frame and pass probes. The same 512×512,
128-sample, 16-warmup fixtures alternate AB/BA at matched pixel/sample addresses;
processes reverse their initial order. The report retains both sets of raw pass
records and independently computes paired transport and full-frame ratios, order
splits and 95% process-bootstrap intervals. Quantized-zero transport durations
remain valid raw measurements; pairs containing one cannot form a ratio and are
counted explicitly. A run with no positive pairs has no transport ratio. Image
readback and file writing follow the timed sequence.

Generated-code inspection on the pinned Slang/SPIRV-Cross toolchain found identical
175,969-byte MSL for 32×1, 64×1, 128×1 and 8×8, including the proposed production
recipe (SHA-256 `da66fc86c67d5eda31ecd44ac3a409fb92fdd47cb00a897bf7b8a0623b754893`).
All SPIR-V artifacts had 9,377 instructions and 144,872 bytes; only the local-size
execution mode differed. Reflection matched each intended shape. There was no
threadgroup memory or barrier in the emitted MSL. The native pipeline receives the
reflected group size at dispatch; this change does not add a compilation occupancy
hint or change transport arithmetic. Native register/spill statistics were
unavailable because the local offline Metal compiler was not installed. Thanks to
**Apple**, [*Scale compute workloads across Apple GPUs*, WWDC22](https://developer.apple.com/videos/play/wwdc2022/10159/),
for the workload-specific threadgroup-size guidance used to select this experiment.

Measurement on 2026-10-27 used Apple M5 (8 GPU cores), macOS 26.6 and Clang
23.1.3 `RelWithDebInfo` (`-O2`). A three-process-block screen rotated 32×1, 128×1,
8×8 and a 64×1/64×1 control; 32×1 and 128×1 showed small/inconsistent changes,
while 8×8 improved transport and full-frame intervals across every nonempty fixture.
An independent five-block confirmation alternated 8×8 and the A/A control, reversing
initial AB/BA order per block: 560 matched measured pairs per scene and experiment.
The final source base was `94fa8ef9b26a5d3f081721baebebaa3402213b7a`; the frozen
executable SHA-256 was `75368d6d70c41d82097e1ecdf5b74c987edd703c329f4cfcc5e22124864f5f7a`.
No other builds or GPU tests from this experiment ran during timed submissions.
Raw duration columns are medians of process medians; percentage reductions are
medians of matched-frame ratios, with 95% process-bootstrap intervals in brackets.
Positive percentages mean less elapsed GPU time.

| Scene | 64×1 transport (ms) | 8×8 transport (ms) | Transport reduction [95% interval] | Full-frame reduction [95% interval] | A/A transport reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | 0.5271 | 0.5204 | 1.51% [1.22%, 1.53%] | 1.30% [1.08%, 1.37%] | 0.14% |
| opaque | 1.6577 | 1.5675 | 5.23% [4.92%, 5.50%] | 5.13% [4.69%, 5.41%] | -0.12% |
| masked | 2.9145 | 2.7124 | 7.06% [6.82%, 7.20%] | 6.78% [6.63%, 7.49%] | -0.28% |
| mixed | 2.9202 | 2.6693 | 8.99% [8.49%, 9.32%] | 8.77% [7.72%, 9.07%] | -0.26% |
| textured-nearest | 3.5355 | 3.0704 | 12.07% [12.03%, 13.32%] | 12.15% [11.81%, 13.37%] | 0.31% |
| textured-linear | 4.0648 | 3.5850 | 10.92% [10.59%, 11.08%] | 10.80% [10.64%, 10.94%] | -0.56% |

Every nonempty scene improved in all five independent runs and both frame-order
splits; transport and full-frame confidence intervals exclude no change. All A/A
control transport intervals include no change, with point estimates from −0.56% to
+0.31% for nonempty scenes. The confirmation retained 3360 pairs for each experiment
and complete sample coverage. Every raw accumulation file and displayed RGBA8 image
was byte-identical between its paired variants. The earlier screen showed occasional
one-ULP raw RGB drift (maximum 2.98×10⁻⁸), including in the A/A control; no displayed
channel changed. This observation is recorded rather than increasing any reference
comparison tolerance. GPU elapsed intervals include stalls and overlapping raster
envelopes as described above; these gains do not establish native shader busy time
or reduced register pressure. Performance on other adapters and authored graph
estimators remains unqualified.

At 64×64 / 16 samples, every tested shape passed the existing independent raw
CPU-oracle and software GPU-reference tolerances for all six fixtures. A separate
67×67 / 16-sample diagnostic exercises partial edge groups: native shape comparisons
and the software GPU reference agree, while the CPU oracle differs in nine channel
comparisons at pixel 1888 (x=12, y=28) across the nonempty fixtures, with maximum
absolute RGB difference about 0.0341. The same failure signature occurs in the
64×1/64×1 control and all candidate shapes; it is retained as a baseline reference
precision limitation, and tolerances are not increased. Set
`LUDUS_WORKGROUP_ODD_EXTENT=1` when running `[workgroups]` to reproduce that diagnostic.

The explicit quality profile admits at most four live jobs, 4096 requested
samples, depth eight, the reference secondary-distance/camera bounds and a fixed
texture mip. It has no denoising, motion history, transmission, spectral transport,
coating model. Native acceleration structures are mandatory. Up to 128 authored directional,
point and spot lights are admitted without dropping entries. Baked/shadow atlas
inputs reject with `Unsupported`; lighting comes from the admitted
emissive surfaces and caller-supplied constant environment. This is a finite-depth
preview of the prepared RGB profile, not general PBRT fidelity. FP32 BVH triangle
intersection remains subject to thin/near-edge precision qualification. The
existing native-query Staircase2 parity gap remains independent of this path.

The GPU budget also charges native structure inputs, BLAS/TLAS storage and build
scratch conservatively until the preview retires. Native ray capability rejection
preserves ordinary raster rendering. Mixed-hardware projects select an authored
raster graph on unsupported devices; see [renderer selection](../development/render-graph-authoring.md#support-developers-and-players-without-rt-hardware).

Enable-time CPU budget covers retained renderer geometry, preview state, BVH and
packing/zero-initialization scratch. RHI-retained source texels/registries and
driver staging are separate. GPU budget covers shared quad/index bytes, immutable
scene/uniform buffers, 32 accumulation bytes and 16 display-color bytes per pixel,
and two RGBA8/private-depth output allocations (16 bytes per pixel together).
It excludes RHI registries, staging, shaders/pipeline objects and driver padding.
Negotiated storage-range, dispatch and attachment limits can reject a scene
before resource creation. Jobs share the budget; cancelled jobs with acquired
frames or pending producers still count. PollGpuPreview/Begin reclaim cancelled
completed jobs; cancellation cannot bypass the in-flight GPU byte budget. Setup allocates and uploads; frame execution rebuilds a small
GDI plan and uploads no scene or pixel data.

Use `PollGpuPreview` for asynchronous enablement. After `BeginGpuPreview`, poll
`GetGpuPreviewInfo`, dispatch with `AdvanceGpuPreview`, and acquire an image with
`AcquireGpuPreviewFrame` when producer completion is known. `Pending` progress
may still expose the preceding complete image. The output is top-left physical
pixels, opaque RGBA8Unorm, exposure multiplied into linear radiance, Reinhard
compression then sRGB encoding. The consumer must not apply that conversion a
second time. Exposure/camera/content changes create a new job and restart
accumulation; resize uses the same replacement workflow.

Each job has two images. `GpuPreviewFrame` is an opaque ownership lease on one
complete texture incarnation. The renderer never overwrites an acquired image;
if both images are leased, dispatch returns `NotReady`. Acquire cannot issue a
second lease for the same image. Cancel the job with `Release(GpuPreview&)`;
acquired frames keep its GPU resources alive until they are returned. Stale
copies reject, and generations do not wrap. Reset/destruction invalidates all
renderer identities; RHI's accepted GPU uses retire through producer completion.

For Qt sharing, import this exact RHI texture on the same native logical device/backend.
Matching a physical adapter alone does not permit direct texture sharing.
Order Qt sampling after the returned producer token, and independently retain its
native backing with the RHI interoperability service. Return the renderer frame
only after every external GPU consumer has completed and released its RHI lease.
Renderer frame ownership is not a cross-device import, foreign-queue semaphore,
Qt completion token or permission to destroy/mutate the borrowed texture.
The RHI/Qt bridge remains a separate prerequisite; this renderer change alone
cannot claim an editor viewport is connected or teardown is cross-queue safe.

The native acceptance compares the small emissive/indirect fixture against the
CPU oracle at matched seed/depth/128 samples, checks opaque output and error
pixels, preserves a leased image across later accumulation, exercises two-image
backpressure and cancelled/stale ownership. The installed-SDK sample's `gpu` mode
exports a texture through public RHI readback **only for offline acceptance**;
interactive rendering/presentation must retain the GPU image instead. On native
Metal the full prepared Staircase2 scene (28,831 instanced triangles), 64×64,
32 samples, depth four, seed 871 produced 4,072 nonblack pixels and no magenta
error pixels. Its display comparison to the matched CPU capture had maximum
channel error 14/255, RMS of per-pixel maximum channel error 0.2923/255, with
3/4096 pixels above two display levels. This qualifies GPU execution and lighting
of the full scene; it does **not** qualify dense-scene pixel parity. The scene
fingerprint matches the CPU capture. The RGB output SHA-256 (including alpha) was
`e702b19b7f3f7235cbc6860e66c98c2279c1d81d743016d592679e2e974935dc`.
See the [scene workflow](../examples/reference-scene.md#gpu-preview-acceptance).

### Render-graph ray scene capacity

Each render-graph ray pass is one preview job. Passes of one graph update that
read the same scene data share the first pass's uploaded scene
(`GpuPreviewGraphInvocation::SceneSourceOrNull`): its packed triangles, BVH,
surfaces, lights, environment, texels and native geometry are stored once and
charged to the preview and RHI budgets once. The last job released frees the
copy. Each pass still keeps its own parameters, accumulation, images and TLAS.
Before this sharing, a template with three ray passes uploaded three copies, so
every byte ceiling admitted a third of the scene it could hold.

Measured on Intel Sponza's glTF triangles with Metal on October 29, 2026:

| Cost per triangle | Bytes | What it holds |
| --- | --- | --- |
| Packed GPU scene | about 322 | 16 float4 words per triangle, 3 per BVH node (about 1.03 nodes per triangle) and one order word |
| CPU scene estimate | 364 | `ReferenceTriangle`, two BVH nodes and three index words, reserved before the build |
| CPU fingerprint stream | 228 | Serialized triangle identity, checked against the scratch remaining after the scene |

The first 999,981 triangles pack to 321,543,168 bytes; 1,999,967 pack to
644,522,080 bytes. Independent byte ceilings bound scene preparation:

| Ceiling | Owner | Scene limit |
| --- | --- | --- |
| `CACHE_IDENTITY_BYTES`, 1 GiB on 64-bit hosts | Raster renderer (`render_graph_scene_resources.cpp`) | Current/pending/idle mesh and texture identity copies; 32-bit hosts retain 128 MiB |
| `RENDER_GRAPH_RAY_CPU_BYTES`, 4 GiB on 64-bit hosts | Renderer (`render_graph.hpp`) | Scene estimate plus fingerprint, packing and native-input scratch |
| `RENDER_GRAPH_RAY_GPU_BYTES`, 6 GiB on 64-bit hosts | Renderer (`render_graph.hpp`) | Current and replacement scenes, images, native structures and pending GPU holds |
| `rhi::StorageLimits::ResourceBytes` | The device creator | All logical RHI buffers and textures on the device. The 512 MiB default holds about 1.6 million triangles; the Editor viewport adds a separate 1 GiB raster/target allowance to the graph-ray GPU budget (7 GiB on 64-bit hosts) |

The former 2 GiB CPU ceiling rejected the full 3,747,018-triangle Sponza on
its first pass: `CpuScratch` needed 854,313,156 bytes with 783,538,976 left after
the 1,363,925,720-byte scene estimate. The 64-bit policy now admits that setup
and reserves GPU headroom for two complete scenes during replacement. On M5
Metal, one complete diagnostic scene charges 2,147,831,068 GPU bytes; two
exceed even 4 GiB before adding larger per-pass images or material texels. These
are ceilings, not eager allocations or measurements of free RAM/VRAM. 32-bit
hosts retain the 2 GiB CPU/GPU ceilings; device storage-buffer ranges and
physical allocation failures still apply independently. The viewport's shared
and offscreen devices use the same policy in `MakeViewportDeviceDescription`.
Native ray scenes also admit aligned opacity/UV tables up to 256 MiB, subject
to the device buffer range and byte budget. Float32 offsets remain exact because
UV offsets count float4 words and alpha scalar offsets are four-aligned. The
former 64 MiB scalar ceiling rejected full Sponza's 114 MiB UV table. The
viewport allows 512 MiB of RHI host metadata, including overlapping native UV
records and temporary opacity arrays during replacement. The raster identity
cache likewise needs more than its former 128 MiB for the complete mesh/texture
set; its independent 1 GiB native ceiling admits retained asset versions. The
raster emitter-record ceiling is 32,768: Sponza authors 8,098 emissive light-bulb
triangles, exceeding the former 4,096-record bound, and the Sample project's Bistro
needs 17,571 records, exceeding the later 8,192-record bound once the mesh-light
culling grid made that many affordable to shade. Admission retains the
authored emitters and their existing diffuse/LTC evaluation.
Renderer ray scene/scratch/preview byte rejections report the exceeded ceiling,
required bytes and available bytes in `RenderGraphReport::SceneDiagnostics`.
Native/RHI and raster cache failures can report only their status.

The `[scene-capacity]` regression in `render_graph_ray_tests.cpp` prepares a
generated 102,400-triangle scene through the three-pass `ComputeBvh`
line-sampling template. It checks that the graph's logical RHI bytes grow by
less than two packed copies. The hidden `[.line-traversal-performance]`
measurement selects an explicit device budget; full-scene viewport acceptance
uses the Editor's device policy.


The optional editor tests prepare the supplied glTF through the production
import/catalog path and require all 3,747,018 triangles. Use a **new**, isolated
output project, leaving supplied resources and existing projects unchanged:

```sh
QT_QPA_PLATFORM=offscreen \
LUDUS_FULL_SPONZA_PROJECT="$PWD/out/full-sponza" \
LUDUS_FULL_SPONZA_ENTRY=/absolute/path/to/main_sponza/NewSponza_Main_glTF_003.gltf \
  out/build/macos-clang-development/apps/editor/ludus_editor_tests '[.prepare-full-sponza]'

QT_QPA_PLATFORM=offscreen \
LUDUS_SPONZA_TEST_PROJECT="$PWD/out/full-sponza" \
LUDUS_SPONZA_TEST_RUNTIME=/absolute/path/to/authored-ray-graph.lurgrt \
  out/build/macos-clang-development/apps/editor/ludus_editor_tests '[.sponza-ray-project]'

LUDUS_SPONZA_TEST_PROJECT="$PWD/out/full-sponza" \
LUDUS_SPONZA_NATIVE_IMAGE="$PWD/out/sponza-native.png" \
  out/build/macos-clang-development/apps/editor/ludus_editor_tests '[.full-sponza-native]'
```

The authored-graph test checks camera navigation, a content replacement above
2 GiB of logical RHI resources, and preservation of the last frame while pending.
The native test requires a shared-device Qt presentation host. It presents the
ordinary raster profile first, then the declared flat-color diagnostic ray
profile, navigates and resizes, checks image variation/error pixels and verifies
that graph presentation performs no CPU image readback. The diagnostic ray
profile displays authored base-color averages; it adds no lights to compensate
for the supplied glTF's zero-intensity punctual sources. This is viewport and
capacity acceptance, not lighting fidelity or steady-state frame-rate evidence.


## Material and lighting fidelity

This implemented RGB surface milestone extends L4/L5 materials and both reference
paths. It preserves the baseline untextured-parameter programs and does not infer
new scene lights or convert raster transparency into glass transmission.

Prewarm `renderer_surface.slang` (bounded forward) or
`renderer_clustered_surface.slang` (all-light clustered forward), using
`VertexMain`, `FragmentMain` and `ShadowMain`. The lit/clustered baseline sources
also use those PascalCase entry names. Source builds supply the FoundationMath
and RHI shader include directories; installed SDK shader sources share one
directory. Both profiles retain the existing sixteen-attribute mesh ABI.

Set `LitMaterialDescription::SurfaceMap` to a ready sampled **linear
Rgba8Unorm** texture. The fixed packed layout uses RG for tangent normal XY
(`2*channel-1`, positive Z reconstructed), B for perceptual roughness multiplier,
and A for metallic multiplier. `NormalScale` in [0,8] scales reconstructed XY,
then renormalizes; zero disables normal detail while preserving parameter sampling.
Roughness retains the 0.045 floor. For a roughness/metallic-only map, use strength
zero. This is a separate data texture: base color still decodes its own sRGB
encoding and alpha remains opacity. Do not feed an ordinary RGB normal map or
ORM map without packing its channels to this contract.

The map shares the base UV transform, sampler and raster LOD. Ray paths instead
use the capture's fixed `TextureMip`; that initialized mip must exist and have
retained CPU content in **both** input textures. CPU retention supplies immutable
setup bytes, not a GPU transport/readback fallback. Mip filtering is the declared
filtered-channel model, without footprint-aware ray filtering or specular AA.
Creation retains an exact texture view and rejects sRGB/data-format mismatch,
attachments, unavailable resources and incompatible shader programs explicitly.
Null preserves constant parameters and the mesh normal. Failed creation preserves
material/report outputs; accepted asynchronous candidates retain the normal
poll/publish/release contract.

GPU normal-frame math belongs to FoundationMath; the renderer owns channel decoding
and BRDF/material evaluation. Raster derives the UV frame from fragment position/UV
derivatives; rays derive it from transformed triangle edges. Both normalize edge
magnitudes, reject relatively singular UV frames, orthogonalize tangent against the
shading normal and preserve mirrored UV handedness. Degenerate frames preserve
the original normal. This UV-derived basis is **not MikkTSpace**: tangent-baked
assets need a matching conversion/profile before claiming exact source fidelity.
Ray offsets and visibility remain geometric-normal operations, and continuations
below the viewer's side of the oriented geometric normal are null events. Shading-normal energy correction is still absent;
this is the declared RGB reflection profile, not unrestricted physical fidelity.

The forward profile extends the 1040-byte uniform to 1056 bytes and binds the map
at slot 6. The clustered profile extends 8800 to 8816 bytes, binds the map at 7,
and shares the existing nearest shadow sampler for cluster indexing. It stays
within RHI's eight-binding limit while retaining base, bake, shadow and cluster
inputs. Uniform names, offsets, shapes and shadow subsets are independently checked.
Older baseline layouts stay admitted and reject non-null maps with `Unsupported`.

GPU reference rendering now copies all normalized authored analytic lights into
its budgeted scene buffer, uniformly selects among them, finite triangle emitters
and a positive constant environment, and uses unit MIS weight for delta lights.
Within the emitter share, both GPU kernels pick triangles from the packed
power-weighted CDF (emitter word Y), matching the CPU proposal described above.
Point/spot shadow segments terminate before the light endpoint. Raster, CPU reference
and GPU reference use the same units, inverse-square floor, range window and
squared cone. No lighting selection truncates the reference integral. Existing
baked/shadow-atlas reference rejection remains explicit.

Pending raster viewport uploads keep the prior drawable view until preparation
succeeds; a failed candidate is discarded and reported once. Update reports describe
the accepted candidate, including Pending, rather than the last presented image.
A regression observes old packets during pending/failure and new packets only after
success. The closest-hit shader explicitly restricts primitive-index tie breaking
to equal distances, supplementing its existing closest-distance intersection bound.

The private GPU kernel uniform grows from 208 to 224 bytes, adding an integer
`SurfaceLayout` record for surface stride, analytic-light offset and kernel ABI 2.
Old cooked kernels are rejected by reflected uniform-size admission before scene
allocation; they must be recooked with the installed sources. A regression verifies
this rejection, so an old six-word surface decoder cannot silently consume the new
eight-word records. Unsupported runtime layout produces an error pixel.

Reference `ProfileVersion` is 2. LUHDR file schema remains 1, and so does the
addressed sampler version of the default `UniformArea` light sampling
(`ResampledProjectedSolidAngle` captures record sampler 2); the decoder retains
profile versions 1/2 as distinct metadata. Maps extend the
canonical scene fingerprint with normal strength, image layout and immutable bytes;
unmapped scene fingerprints remain stable. Comparisons require matching profile
versions. Changing a material/map creates a new snapshot/integral and restarts
accumulation; old accepted captures and leased viewport images retain their inputs.

Acceptance covers independent packed-channel decoding, rim/zero-strength and
invalid-output preservation; mirrored/tiny/degenerate UV frames; incompatible
program and sRGB-map rejection; complete native raster/clustered/CPU/GPU comparisons
for directional, point and off-axis spot illumination; and texture-owner destruction
before rendering/capture. Numerical pixel expectations are derived independently,
not merely compared between two shader variants. Emitted WGSL/GLES layout checks
cover all profile entries. These small fixtures establish the declared contract;
full Staircase2 native-query parity and source-PBRT fidelity remain unqualified.

The larger compute artifact is stored byte-for-byte in one generated `.cpp` when
its textual UTF-8 source reaches 60000 bytes, with only an external array declaration
in the generated header. This avoids C++'s giant-literal limit and repeated header
parsing while retaining exact source, line diagnostics and manifest hashes. The
installed CMake helper owns compilation of the generated companion; no runtime
concatenation, file loading or shader compiler is introduced.

Thanks to Google/Filament's authors, [*Physically Based Rendering in Filament*,
§4.4, §5.2.2 and §5.7](https://google.github.io/filament/main/filament.html), for the
reflection/photometric conventions and tangent-normal surface-detail model. The
packed layout, UV-derived frame and bounded immutable admission are Ludus choices.


### Typed diffuse and conductor reference materials

Reference profile 5 extends `ReferenceBsdf`, `LitMaterialDescription` and
`SceneShaderMaterial` with format-owned `MaterialScattering` parameters. The
legacy default retains GGX/Schlick plus Fresnel-weighted diffuse. `Diffuse`
evaluates Lambertian reflectance without an added dielectric specular lobe and
samples the cosine hemisphere. `Conductor` uses retained RGB complex-IOR Fresnel,
GGX with directly supplied alpha, correlated Smith masking and visible-normal
sampling with its matching PDF. Below-surface proposals remain null events;
there is no retry, energy clamp or hidden change to conductor alpha. Storage
preserves `AlphaU` and `AlphaV`; reference admission currently requires them to
match, and requires positive alpha rather than implementing delta transport.

CPU/native-query reference transport evaluates the CPU implementation;
GPU-compute reference transport evaluates the corresponding Slang implementation.
The authored graph's standard BSDF helpers use the same model selection. Existing
custom BRDFs remain author-owned. The GPU material records extend the existing
indexed read-only scene storage table, selected by each triangle's resolved
material binding. Eta/model, K/alpha and coating metadata have shared named word
addresses; the private scene-kernel ABI advances to 10. The table requires one
storage binding and does not require a descriptor array of constant buffers or
bindless textures. Existing resource retention and producer/consumer completion
rules continue to protect the complete accepted snapshot.

The source-preserving profile is an RGB proxy. Profile 5 retained coating parameters
with approximate shading; profile 6 adds the bounded non-scattering layered model
below. Raster lit shaders retain their approximation. Spectral PBRT equivalence is
not claimed.
References: thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys,
[*Physically Based Rendering*, fourth edition, §9.3](https://pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission),
for complex-IOR Fresnel, and the existing Heitz visible-GGX sampling reference.

Acceptance uses independent complex-amplitude Fresnel calculations, Lambertian
throughput identities, invalid-input output preservation, source-material asset
round trips and a mixed diffuse/conductor scene comparing raw CPU/GPU reference
AOVs. Full staircase2 source-PBRT fidelity and the previously recorded dense
native-query qualification gap remain separate acceptance items.

### Non-scattering coated-diffuse reference

Reference profile 6 evaluates staircase2's `Wood` and `FloorTiles` as an isotropic
rough dielectric interface above a textured Lambertian base. Their direct GGX alpha
values are 0.1 and 0.01 (`remaproughness=false`), with default coating IOR 1.5 and
optical thickness 0.01. The intervening medium has zero scattering albedo and unit
extinction in optical-depth coordinates: zero albedo still absorbs light. Every
internal crossing applies `exp(-thickness / abs(directionTangent.z))`.

CPU and GPU implement Fresnel reflection/refraction, total internal reflection,
correlated Smith GGX, visible-normal sampling, internal interface/base reflections
and absorption. The interfaces remain colocated at the shading point, as in PBRT's
layered BSDF approximation; this does not add geometric refraction or transmission
through the opaque base. The internal random walk has PBRT's default ten-event
limit and compensated roulette after event three. It does not consume scene path
depth or trace scene intersections. This finite limit is part of the declared
profile, not a claim of an infinite-depth layer solution.

The sampler returns a complete continuation `Weight` and a separate directional
`MisPdf`. A layered sample's proportional `Evaluation` retains a valid `f*cos/pdf`
ratio but its PDF must not drive external MIS. Direct-light evaluation estimates
the layered BSDF, with an independently addressed approximate directional PDF for
MIS. Both light-sampled and BSDF-hit contributions use those same semantics.
Legacy/diffuse/conductor sampling and their exact proposal PDFs remain available.

`ReferenceBsdfRandomContext` uses the caller's Philox key and logical receiver/path
address. `Scope` identifies the path, `Event` its sample and `Dimension` the scene
bounce/material event (0 through 255). Each event reserves 256 internal dimensions;
evaluation, PDF estimation and continuation use distinct key domains from
`coated_diffuse_layout.h`. The reference tracer supplies its pixel, committed sample
and scene bounce. Internal branching cannot shift the outer sampler's dimensions.
Null proposals, absorption and depth/roulette termination retain their probability
mass; there is no retry, energy clamp or roughness remapping.

LUSCN v7 and its existing indexed GPU table already carry alpha, IOR and thickness;
no format extension is needed for this restricted medium. The cooker resolves
explicit scalar `eta`/`thickness` and rejects scattering albedo, scattering textures,
anisotropy, smooth interfaces, IOR below one, and nondefault `maxdepth`/`nsamples`.
An explicit RGB zero albedo and default controls are admitted. Storage may retain
parameters outside the renderer's supported profile; reference material admission
rejects those inputs. Shader packages must be rebuilt for scene-kernel ABI 11 and
reference kernel ABI 5. Capture profile 6 separates these results from older
coating approximations and restarts accumulation under the new integral.

The authored shader interface exposes `CoatingEta` and `CoatingThickness`.
Use `EvaluateStandardBsdf` with `CoatingRandomContext` and
`SampleStandardBsdfTransport` together for layered reference transport. The latter
returns direction, complete throughput weight and the external MIS density.
The original direction-only helpers retain their compatibility approximation for
coatings; re-evaluating a sampled layered direction cannot reconstruct its internal
path weight. Ordinary raster and GPU preview shading remain approximate.

Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys,
[*Physically Based Rendering*, fourth edition, §9.7 and §14.3](https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/Scattering_from_Layered_Materials),
and pbrt-v4 revision `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`, for the adapted
dielectric and layered estimators. The implementation specializes their two-sided
layer model to a rough isotropic top interface, opaque diffuse base and zero
medium scattering, using RGB channels and Ludus Philox streams. The Apache-2.0
license is retained in `third_party/pbrt/LICENSE.txt` and installed with the SDK.

Validation includes independent black-base Fresnel and no-IOR-contrast absorption
limits, internal-path replay, output preservation, source-parameter round trips,
PBRT BSDF/continuation measurements, and matched raw CPU/GPU captures containing
textured coatings at both staircase2 alpha values. Full scene geometry/traversal
parity, spectral textures and texture footprint filtering remain separate limits.
The frozen PBRT fixture uses 131072 samples per alpha/reflectance pair; the Ludus
regression uses 32768 independent samples and accepts each BSDF, continuation and
MIS-density mean within six combined standard errors plus an absolute `1e-5`
rounding allowance. It also checks accepted-sample probability, mean reciprocity
and energy no greater than one within sampling error. The mixed textured scene
retains the existing raw linear CPU/GPU RMS tolerance of `1e-3`.

The independent measurement harness uses PBRT's own build/toolchain outside the
engine dependency graph. To reproduce the fixture in
`modules/graphics/renderer/tests/reference_material_tests.cpp`, construct
`CoatedDiffuseBxDF` with eta 1.5, alpha 0.1 or 0.01 on both axes, scalar base
reflectance 0.2, 0.6 or 0.9, thickness 0.01, albedo/g zero, maxDepth 10 and nSamples
one. Evaluate in radiance mode at `wo=(0.6,0,0.8)` and
`wi=(-0.4,0,sqrt(0.84))`. For each pair, vary `GetOptions().seed` from zero through
131071 and measure `f`, `PDF`, and `Sample_f`'s `f*abs(wi.z)/pdf` ratio. One external
PBRT `RNG(871)` stream supplies three uniforms per continuation sample and advances
across all six pairs in the fixture's order. Null proposals contribute zero to the
mean; divide by the total sample count. Record each mean and its standard error
`sqrt((mean(square)-square(mean))/count)`, plus the accepted-sample fraction.
CPU/GPU branch replay is checked separately from agreement of independent sample
means; numerical/statistical acceptance never uses tone mapping or denoising.

## Implementation phases and acceptance

Complete prepared-scene acceptance uses the existing installed-SDK reference
consumer and an independent serialized-bundle verifier. See the
[full prepared-scene renderer acceptance workflow](../examples/reference-scene.md#full-prepared-scene-renderer-acceptance)
for exact resource/triangle accounting, source-order versus optimized raster
pixels, detached-owner captures, optional native/CPU parity and provenance.
Passing that admitted RGB profile does not qualify arbitrary source materials,
scene-scale performance or the remaining L6 effects.

These phases complement RHI R0–R5; they do not rename or claim completion of them.

| Phase | Deliverable and prerequisites | Acceptance |
| --- | --- | --- |
| L0: portable scene packets (implemented) | R1 static flat/unlit meshes with fixed transforms, immutable snapshots, CPU visibility, ordered 2D overlays, error materials; remain within current limits and R1-compatible depth | Public-only SDK fixture on Vulkan/Metal/WebGPU/WebGL; invalid/stale inputs, capacity failure and pixels match an unsorted direct path |
| L1: views and depth (implemented) | GDI/RHI explicit attachments, clear/compare state and frame retention; orthographic/perspective, virtual screens, offscreen/split views | Reverse-Z ordering, clip/UV orientation, letterbox picking, DPI, zero extent, resize and neighboring view preservation |
| L2: mutable data and overlays (implemented) | Completion-scoped replacement uploads; dynamic instances, debug triangles, R8 text adapter | In-flight overwrite stress, line near-plane/degenerate cases, painter/scissor order, atlas pressure, CPU text fixtures and loss/restart |
| L3: material/content versions (implemented) | Cook/reflection/layout expansion, texture formats/mips and pipeline prewarm | Per-target layout fixtures; invalid reload preserves old image; mip/color semantics; bounded cache/upload overlap and deterministic fallback |
| L4: lighting baseline and RGB surface fidelity (implemented) | Lit material, opt-in packed normal/roughness/metallic map, bounded direct lists, environment, shadows, baked assets, linear intermediate/output | Reference BRDF/color tests; no double lighting, shadow invalidation/atlas borders, saturation/overflow and LDR/HDR capability variants |
| L5: measured scaling (portable implementation) | Opt-in CPU clustered forward, grouped culling and authored LOD; optional native parallel recording deferred | Dense-light and many-object fixtures; direct-path comparisons, overflow correctness, physical GPU median/tail/memory/latency evidence |
| L5.1: budgeted scene capacity (implemented) | Initialization-time registry and snapshot/view workspace reservations; full-order borrowed diagnostics; budgeted RT0 records and configurable static TLAS instance limits | Above-default capacity, partial groups, exact-budget rejection, detached/in-flight retention, stale handles and recovery |
| L6: optional advanced effects | Temporal/GPU-driven/virtual-residency plans and the RT0–RT4 ray plan | Capability probes and disabled-path support; image/temporal tests and workload-specific gains before default enablement |

Use empty, single triangle, Cornell box, transparent intersections, split-screen,
many small meshes, many lights, heavy overdraw, animated bounds, streamed textures,
multilingual overlays and repeated resize/reload/loss scenes. Separate software-GPU
CI from physical GPU performance evidence. Required implementation checks are
warning-clean pinned builds, meaningful unit/pixel/SDK tests, ASan/UBSan and pinned
format/tidy; documentation changes run documentation gates. Do not invent FPS
targets or buffer capacities without the supported workload and measured budget.

## Contemporary primary references

- Khronos, [Vulkan Pipeline Cache](https://docs.vulkan.org/guide/latest/pipeline_cache.html)
  and [Pipeline Management sample](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_cache/README.html):
  pipeline creation can remain costly; validate persistent cache compatibility
  and prewarm legal variants. Cache miss behavior must remain visible.
- Khronos, [Render Pass](https://docs.vulkan.org/spec/latest/chapters/renderpass.html)
  and [Synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html):
  explicit attachment preservation and declared hazards inform pass contracts;
  actual operations remain constrained by Ludus's pinned API/toolchain.
- W3C, [WebGPU specification](https://www.w3.org/TR/webgpu/): validate enabled
  features, usage scopes, limits and formats. Timestamp queries and relevant
  indirect features are capability paths, not assumptions about every browser.
- Google Filament authors, [Physically Based Rendering in Filament](https://github.com/google/filament/blob/main/docs/Filament.md.html):
  consult its energy-conserving BRDF and light/exposure conventions when specifying
  L4. The choice of a bounded forward baseline and staged features is Ludus's
  design judgment, not a performance result from Filament.

Thanks to these authors and organizations. Future implementation must cite the
specific consulted algorithm/section beside the code, preserve notices and explain
departures; this architecture's bibliography does not replace code attribution.

## Authored material inputs and HDR environments

The authored material recipes (`renderer_authored` and
`renderer_clustered_authored`, with matching shadow recipes) add mesh tangent
frames, linear vertex RGBA, a second material UV channel, textured emission and
HDR image-based lighting. Select `LitMaterialDescription::AreAuthoredInputsEnabled`
explicitly; older recipes reject the requested extension. `MaterialUvSet` selects
UV0 or UV1 for the base, packed surface and emission maps. These maps share their
UV scale/offset, sampler and LOD; independently transformed inputs require an
explicit cook/rebake. Bake UVs remain separate. Vertex color multiplies base RGBA,
including alpha-test coverage, but does not multiply emission.

`MeshDescription::Tangents` contains XYZ and handedness W. Creation orthogonalizes
and normalizes finite authored XYZ against the supplied normal. W must be +1 or
-1; W=0 explicitly declares an unavailable vertex tangent. Mirrored object
transforms adjust handedness. Missing/unavailable tangents use the existing
UV-derived fallback; Ludus does not regenerate or claim MikkTSpace tangents.
The authored recipes derive the inverse-transpose normal direction from the
world matrix, using a positive common rescaling before cofactors. This frees
three attribute locations for tangent, color and UV1 while preserving the
16-attribute interface. The common copied vertex record is now 80 bytes; legacy
recipes keep their original attribute offsets and semantics. Account for this
larger record when sizing mesh buffers.

The portable RHI binding address space now has 16 slots, shared by its public
metadata/dynamic-offset arrays, runtime and offline shader tooling. Physical
stage/descriptor limits remain separate admission constraints. Authored forward
uses ten bindings; clustered uses eleven, including independent environment
texture/sampler and emission texture. Baked lighting, shadow atlases and clustered
light lists coexist with HDR lighting. Rebuild SDK consumers after this public
metadata layout change.

`EnvironmentMap` is a move-only owner initialized from borrowed, tightly packed
linear HDR `Vector4` pixels. +Y is the north pole, longitude zero is +X and
increasing longitude turns toward +Z. RGB is finite radiance in [0,1e12]; alpha
is ignored. Width/height are bounded at 4096 each. Initialization copies the
source, builds a luminance/solid-angle CDF, integrates nine diffuse SH coefficients
with binary64 accumulation, and creates a deterministic GGX-filtered RGBA16F
latitude/longitude mip chain. Default filtering uses width 128 and 128 samples;
quality and a positive CPU byte budget are caller inputs. Preparation is cold
setup work, never part of a draw. Unsupported HDR devices reject explicitly.

The raster texture stores radiance divided by a recorded scale when the source
exceeds binary16's range, and restores that scale during sampling. This is storage
normalization, not exposure or clipping of the original source. Diffuse uses
cosine-convolved SH; specular uses roughness-selected prefiltered mips and the
analytic DFG approximation documented by Filament. These are finite-resolution,
single-scattering approximations, without local-probe visibility or energy
compensation. Environment lighting replaces the constant environment terms;
a valid bake still supplies diffuse irradiance. Background/sky composition is a
separate caller task.

```cpp
EnvironmentMap environment;
EnvironmentDescription source;
source.Pixels = hdrPixels;
source.Width = hdrWidth;
source.Height = hdrHeight;
source.CpuByteBudget = environmentPreparationBudget;
// Initialize returns Ready/Pending with ownership; other statuses preserve empty.
const rhi::RasterStatus status = environment.Initialize(device, source);
// Poll GetStatus until Ready before creating a material/capture.
LitMaterialDescription material;
material.AreAuthoredInputsEnabled = true;
material.Environment = &environment;
material.EmissionMap = emissionTexture;
```

All map operations use the device owner thread. Initialize, Reset, moves and
destruction require a closed frame. Accepted materials retain the immutable map
and sampled resources after its public owner resets; keep the device alive until
all retained uses finish. The map's source/CDF CPU storage has its own explicit
budget, separate from renderer/reference job budgets.

Reference captures select `ReferenceCaptureDescription::EnvironmentTexture`
explicitly. They evaluate the original source radiance and its importance PDF,
including miss and next-event MIS terms; raster SH/prefiltered mips are never added
to transport. CPU sampling uses exact row solid angles and a binary64 CDF;
GPU packing stores the normalized CDF in binary32 and evaluates the corresponding
quantized density. Both use piecewise-constant source radiance. Black maps have a
valid zero-radiance distribution. Tangents, vertex color, routed UVs and emission
maps are copied/fingerprinted with the integral. The reference lighting profile remains
version **4** (geometric emission sidedness); the packed GPU kernel ABI is version
**5** for opacity-range primitive identity. Recook matching preview, reference and
graph artifacts. Existing LUHDR container/sampler versions stay one.
Historical evidence retains its original profile identity.

CPU oracle and compute preview implement authored masked coverage. Native query
capture returns `Unsupported` for authored masked materials because the current
native opacity interface cannot evaluate routed UVs and vertex alpha. Other
native reflection inputs use the retained authored geometry. Dense-scene native
query numerical qualification remains a separate open item.

The implementation adapts Romain Guy and Mathias Agopian's *Physically Based
Rendering in Filament*, sections [5.3](https://google.github.io/filament/main/filament.html#lighting/imagebasedlights)
and [9.6](https://google.github.io/filament/main/filament.html#annex/sphericalharmonics),
including its documented Lazarov DFG approximation, and Matt Pharr, Wenzel Jakob
and Greg Humphreys's *Physically Based Rendering*, fourth edition,
[Infinite Area Lights](https://pbr-book.org/4ed/Light_Sources/Infinite_Area_Lights).
The adopted departures are latitude/longitude prefilter storage, exact row solid
angles, a flat CDF and the explicit finite RGB profile above. See
[full static resource acceptance](../examples/reference-scene.md#full-static-sponza-and-suntemple-acceptance)
for assets, commands and limitations.

### Authored input view and naming contract

`MeshDescription::Tangents`, `Colors` and `MaterialTexcoords1` use Foundation's
[`BorrowedView<const T>`](container-systems.md#borrowed-contiguous-views).
Each channel is empty (the documented fallback) or has exactly `VertexCount`
elements. `CreateMesh` rejects invalid or mismatched views before reading and
copies accepted data; the views do not extend caller storage lifetime. For an
array use `geometry.Colors = colors`; for dynamic storage use
`geometry.Colors = {colors.GetData(), colors.GetSize()}`.

Optional environment owner pointers are named `EnvironmentOrNull` for lit
materials and `EnvironmentTextureOrNull` for reference captures. Environment
source pixels are `PixelsOrNull`; successful initialization still requires a
nonnull pointer with the declared width/height extent. Move parameters are
named `other`, because they transfer ownership rather than output through an
lvalue reference. Shared/weak smart pointers are not currently a Foundation
facility; synchronous mesh borrowing does not require shared ownership.


### Authored static area lighting and worker diffuse transport

The [scene interchange contract](content-resources.md#authored-scene-lighting-luscn-v3)
retains analytic photometric lights, geometric emitter sidedness, per-placement
power and linear HDR source radiance. Version 1/2 decoder identities derive from
object/material order and retain two-sided emission. The renderer creates no
invented fill lights. Importers declare their fixed Rec.709 photometric proxy;
this does not establish PBRT spectral equivalence.

`BuildTriangleAreaLights` generates one centroid or three symmetric weighted
Lambertian samples. `BuildPlanarAreaLight` instead produces one area-weighted
centroid for a consistently oriented connected planar source, so triangulating a
disk or quad does not multiply its proxy lights. It validates nondegeneracy,
coplanarity and a centroid inside the supplied mesh. Connectivity is a producer
contract, not a topology proof. Curved sources or concave patches whose centroid
is outside fail explicitly. These are declared finite-source quadrature
approximations: near-field intensity, light shape and soft shadows are not exact.
Distances are metres; a weighted area sample stores RGB nits times square metres
and evaluates its emitting cosine over squared distance, with the existing
bounded range window. `AreaOneSided` clamps the cosine; `AreaTwoSided` uses its
absolute value. Material emission sidedness is also retained by raster and CPU/
compute reference transport, including mirrored placements.

The 80-region atlas admits thirteen six-face point/area proxies without reducing
the 128-entry clustered list. The authored clustered fragment uniform is 16272
bytes, within the 16 KiB portable profile. Use clustered recipes when more than
eight lights are present; forward selection still reports its dropped count.
Reflected parameter packing, light-selection reports and shadow-region staging
use reusable renderer-owned CPU scratch, allocated lazily on the device owner
thread and charged to `Limits::HostBytes`. They do not place the expanded layouts
on the browser's default 64 KiB call stack. Insufficient scratch budget returns
`CapacityExceeded` without publishing a material/atlas or changing its output
report. Switching between forward and clustered admissions resets report counters;
retained scratch is reused until renderer shutdown.
Each region owns a prepared view with conservative light-frustum culling and
compatible packet grouping; casters remain full-detail and independent of the
main camera. Configure at least 156 views plus ordinary
views for overlapping replacements of a 78-region atlas, and sufficient RHI
resource, binding and byte budgets (the reference acceptance uses 160 views and
256 resources per kind). `DrawShadowAtlas` runs before indexed draws in its frame
and preflights its aggregate packet count plus caller-reserved later draws against
the enabled per-frame draw budget. The direct pass loop does not consume GDI's
per-graph pass budget. Other backend failures can still partially encode a
candidate; discard it and keep the previous published image.

`StaticLightingBake` prepares CPU-only world-space geometry/BVH and immutable
surface terms. Serially, `Initialize` then `Step(maxPathSamples)` advance a bounded
number of complete paths on one thread. Scheduled, `Submit` runs preparation and
coarse disjoint triangle ranges on a caller-owned `JobSystem`; the owner's `Poll`
or `Finish` merges ranges in stable order and encodes the charts after the Wait
fence. Both modes produce bit-identical charts. `Cancel` stops scheduled ranges at
the next path sample; destroying an in-flight bake cancels and drains it. The jobs
carry the labels `StaticLightingPrepare` and `StaticLightingRange`, so a system
initialized with the profiler hooks shows them in captured traces. See the
[threading consumer contract](threading.md#first-engine-consumer-static-lighting-bake).
No renderer registry, native ray-query API, GPU upload, readback or Qt state is
touched there. Keep the input scene's texture storage immutable and alive until
the bake is destroyed. Composition must discard results with obsolete exact
revisions before publication.

Input flaws that cannot change the result are tolerated and reported, not fatal.
A zero-area (collapsed) triangle covers no surface: it is left out of the BVH,
emitter selection and probes, keeps its chart tile (zero irradiance) so layout
and bake coordinates are unchanged, and an emitter whose triangles all collapsed
emits nothing. `GetDiagnostics` and `GetSkippedTriangleCount` report the skipped
triangles as soon as preparation completes, while ranges still run, so a caller
can warn its user and offer `Cancel`; each Ready chart also carries its object's
count. Real triangles keep their indices and random streams, so their charts are
bit-identical to a scene without the collapsed triangles. Non-finite geometry,
including an area that overflows binary32, still fails preparation.

The virtual diffuse receiver uses cosine-hemisphere sampling plus the reference
integrator's same-measure next-event MIS, visibility, metallic/roughness transport
and compensated roulette, including its power-weighted emitter proposal. It
includes **all admitted emissive triangles** and
raw authored environment direct diffuse, plus subsequent indirect transport.
Analytic direct diffuse stays in raster. The declared path-depth/range bounds
introduce truncation bias, sample count controls variance, and per-triangle
centroid reconstruction is spatially coarse. There is no denoiser or general
lightmap unwrap. The private probe shares the CPU reference's BVH, opacity,
surface/emission sampling, random primitives, MIS and continuation code.

Ready charts are per placement and contain linear RGBA8 irradiance plus a
per-channel scale, fully replicated four-texel triangle tiles, stable SourceId,
exact scene/dependency revisions and mesh-to-world transform. GPU upload occurs
on the RHI owner after worker completion. Assign the SceneFormat helper UVs to
deindexed vertices (LUSCN v4 to v6 meshes are indexed and store no chart UVs, so
expand their corners first), keep source UVs separate, select no mipmaps, and enable
`BakedLighting::AreTriangleChartsEnabled` with the returned identity UV transform.
`HasStaticAreaDirect` suppresses the area proxy's diffuse term only when that
chart is valid. `DirectLightId=0` replaces no analytic direct term; area diffuse
is separately declared. A matching chart replaces the environment diffuse term
and retains the shadowed proxy specular term. A stale chart falls back to raster
area diffuse and increments the existing bake diagnostics.

Publish a ready shadowed direct-light candidate while worker indirect preparation
runs, with an explicit preparation status. Retain the previous drawable until
that first direct candidate is Ready. Publish the completed charts as a new
immutable material/snapshot version only when their provenance still matches the
selected World. Failed/pending replacements retain the current drawable. Camera
navigation never requires a progressive path-tracing preview or a finished bake.
Area specular uses the shadowed planar centroid proxy; it is approximate but must
include every admitted source rather than the brightest source alone.

Transport draws on Matt Pharr, Wenzel Jakob and Greg Humphreys, *Physically Based
Rendering*, 4th edition, sections [12.5 (area lights)](https://pbr-book.org/4ed/Light_Sources/Area_Lights)
and [13.4 (path tracing)](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer).
The implementation acknowledges these authors beside the shared transport code;
the deterministic raster quadrature and coarse triangle chart layout are explicit
Ludus departures.

For live World material edits, supply the optional per-object
`StaticLightingBakeDescription::Surfaces` overrides. They copy the current tint,
UV transform, metallic/roughness, resolved emission/sidedness and opaque/masked
coverage. Texture indices reference the immutable retained CPU scene array;
normal/surface maps and routed secondary material UVs are outside this interchange
bake profile and must be diagnosed by its adapter. Resolve authored power against
the current placement before supplying overridden emission. Overrides use the
raster material's white untextured base contract; an empty list retains the
legacy decoded scene's black untextured semantics. The override array is consumed
at Initialize, while the source texture bytes remain borrowed through destruction.
Use a bake revision key for physical geometry/material/light/environment changes;
camera movement and exposure do not change these view-independent irradiance charts.

### Material debug views

`LitMaterialDescription::DebugView` (`MaterialDebugView`) replaces a lit
material's shaded output with one inspection quantity per pixel: world-space
normal as RGB (`n * 0.5 + 0.5`), linear depth, perceptual roughness, metallic,
UV0 as red/green, or the number of lights whose shadowed contribution is nonzero
as a viridis ramp saturating at `MATERIAL_DEBUG_LIGHT_COUNT_SATURATION`.
Only the debug recipes honour it: `renderer_lit_debug` and
`renderer_clustered_authored_debug`, with matching shadow recipes. They compile
the production sources with `LUDUS_DEBUG_VIEW`, keep the production binding and
parameter layout, and read the mode from the reserved `Options.w`; the
production recipes are unchanged and ignore it (it stays zero). Creation rejects
values above `MaterialDebugView::LightCount`.

`ViewDescription::DebugDepthRange` supplies the linear-depth `{near, far}` in
world units: radial distance from the camera for perspective views and distance
along `CameraForward` for orthographic ones. It must be finite with
`0 <= near < far <= 1e7`, or `{0, 0}` when no view uses depth; anything else
fails `PrepareView` with `InvalidDescription`. The range rides in the unused
per-instance `Color.zw` of lit items.

Debug colours are display values. Each debug shader inverts the viewport output
pass (PBR Neutral, then sRGB encoding at exposure 1; `InvertPbrNeutral` is the CPU twin) so the
encoded pixel equals the intended colour. Inversion is capped just below 1, so pure white encodes
at about 99.5%. Compression desaturates toward white, so a saturated colour with a peak above about
0.95 (for example a pure blue) cannot keep its dark channels and shifts slightly toward white.
Render debug views into an HDR (`PreferHdr`) target: an RGBA8 target clips the inverted
values at one, which limits white to 0.87 display-linear. Overdraw is not a debug view.
Translucent painter layers draw in a separate overlay pass with their own
transform, and the world pass has no additive, depth-off state to accumulate
fragment counts.

### Authored graph workgroup experiment

The measurements below were collected before native ray-stage invocation was added,
using commit `44ade68e` and the generated-code hashes recorded below. They describe
the compute invocation on that revision; native ray-stage dispatch does not use the
scene-compute workgroup override. Runtime packages now use version 21 so versions
19 (custom traversal) and 20 (native ray stages) retain their existing layouts.

Scene-compute bundles can opt into 8×8 groups using
`"state": {"group_size": [8, 8, 1]}` on a scene compute node. All explicit
sizes across the bundle's configurations must agree; automatic members inherit
that choice. No override means 64×1. The editor exposes the same shapes through
**Pipeline State → Advanced overrides → Group size**, including the canvas form.
Runtime package version 21 carries reflected dimensions for every ray compute
stage; older packages retain 64×1. Device limits and edge bounds checks apply.

The `ludus_graph_workgroup_tests` target cooks the actual ComputeRayTracing and
VeachGuibasMis templates twice, using 64×1 and 8×8, without editing the source
templates. It compares generated-header/runtime dimensions and shader text,
then executes the runtime packages. The two-pass compute graph has three physical
operations: primary compute, shade compute and output raster. The four-pass MIS
graph has seven: primary compute, then MIS, light-only and BSDF-only compute/raster
pairs. The unused primary color is not rasterized. Physical operation position
identifies these intervals; the backend's retained scope name/index is also saved.
Scope names may cover the shared ordered sequence rather than identify each callback.

Reproduce with a native ray-query/timestamp-capable build and an idle viewport:

```sh
cmake --build out/build/<preset> --target ludus_graph_workgroup_tests
out/build/<preset>/modules/graphics/renderer/ludus_graph_workgroup_tests '[graph-workgroups]'
LUDUS_GRAPH_WORKGROUP_ODD_EXTENT=1 \
  out/build/<preset>/modules/graphics/renderer/ludus_graph_workgroup_tests '[graph-workgroups]'
python3 scripts/python/benchmark_graph_workgroups.py \
  --executable out/build/<preset>/modules/graphics/renderer/ludus_graph_workgroup_tests \
  --output out/graph-workgroups/confirmation --runs 5
```

Each independent process runs both graphs over opaque, geometric-cutout and
bilinearly textured planar fixtures under a constant environment. Cutouts omit
alternating geometry columns; these are **not alpha-masked materials**. The graph
material interface does not expose alpha coverage. The CPU oracle and all shaded
GPU outputs estimate the same first-hit direct-lighting integral, with no
indirect bounce, temporal reuse or denoising. Normal regressions compare 64×64
raw linear means within `0.002 + 0.02 * CPU mean` per channel and normalized RGB
RMSE ≤0.12, using 128 samples and matched camera/seed. Every pixel/pass must record
exactly 128 samples. Variant RGB differences must be ≤1e-5 after dividing by the
sample count, and every exported RGBA8 image must match exactly. The odd-extent
mode repeats those checks at 67×67, including partial groups.

Timing uses 512×512, 128 total samples, 16 warmups and 112 measured AB/BA frame
pairs, with both frame and pass probes armed on both variants. Readback and image
comparison happen after the paired sequence. Each block also runs a 64×1/64×1
A/A process. Blocks alternate experimental/control process order and reverse the
first frame pair. Confidence intervals bootstrap independent process medians;
AB/BA splits, all physical pass durations, timestamp precision and scope metadata
remain in the JSON/raw output. The tool rejects unavailable timings, incomplete
sample coverage, nonfinite RGB, missing exports and variant changes.

Generated code was checked before timing on Apple M5 / Metal, macOS 26.6,
Clang 23.1.3 `RelWithDebInfo` (`-O2`). ComputeRayTracing emitted identical
209,456-byte MSL for both shapes (SHA-256
`0905586217d0b369125770e0b2324c4def5c01b00d1c866055dcb40ffa3c7bb8`);
MIS emitted identical 510,545-byte MSL (SHA-256
`fdf3f5fda75e93d539df7692f4542ab37f3babd8da132664c9051bad876e12a1`).
Reflection changed only the dispatch shape. Offline Metal register/spill reports
were unavailable, so the evidence does not establish an occupancy explanation.
The three-block screen found a large benefit for geometric cutouts and little
change for opaque/textured fixtures; this supports an opt-in, not a new general
default. Performance qualification is specific to this adapter and these fixtures.

Five fresh confirmation blocks used executable SHA-256
`b51f3f555fa3ffe470250b7e551c3c21718deddec1a1fe01056a7f024a9c5c1a`,
from base `5d718cf6` plus this change (the base includes the merged editor test-link
fix and preserves the screened ray kernels). The paired full-frame results were:

| Graph / fixture | GPU time reduction [95% process interval] | A/A reduction |
| --- | ---: | ---: |
| compute-opaque | 0.68% [-0.19, 0.87] | 0.09% |
| compute-cutout | 27.98% [27.43, 28.35] | -0.17% |
| compute-textured | 0.73% [-0.69, 0.77] | 0.40% |
| mis-opaque | 0.24% [-0.85, 0.43] | 0.09% |
| mis-cutout | 31.18% [31.10, 31.48] | -0.05% |
| mis-textured | 0.23% [0.12, 0.28] | 0.02% |

Every confirmation raw buffer was bit-identical between variants, and all
exported RGBA8 images matched exactly, in both A/B and A/A. Only the cutout
workload establishes a material improvement. The opaque/textured changes are
small relative to controls/order effects; no general default changes.

For cutout geometry, the separately recorded physical GPU intervals were:

| Graph / physical operation | 64×1 (ms) | 8×8 (ms) | Paired reduction |
| --- | ---: | ---: | ---: |
| compute / frame | 1.4541 | 1.0494 | 27.98% |
| compute / primary | 0.4558 | 0.4100 | 10.49% |
| compute / shade | 0.9379 | 0.5777 | 37.76% |
| compute / shade_raster | 1.4350 | 1.0311 | 28.22% |
| mis / frame | 3.6816 | 2.5358 | 31.18% |
| mis / primary | 0.4549 | 0.4089 | 10.16% |
| mis / mis | 1.3609 | 0.8779 | 35.58% |
| mis / mis_raster | 2.0125 | 1.4676 | 27.08% |
| mis / light | 0.9430 | 0.6106 | 35.33% |
| mis / light_raster | 2.7861 | 1.9829 | 30.41% |
| mis / bsdf | 0.8780 | 0.5905 | 32.77% |
| mis / bsdf_raster | 3.6063 | 2.4598 | 31.89% |

Time columns are medians of independent process medians; percentage reductions
use matched per-frame ratios before taking process medians. They need not equal
the ratio of the displayed marginal medians. Metal raster probes span the earliest
vertex/fragment start through the latest end. They can overlap compute and include
dependency waits: the raster envelopes above are not isolated fragment costs,
and neither their durations nor their reductions are additive. Full-frame timing
independently establishes the improvement.

Native viewport acceptance uses the selected temporary starter World, an explicitly
authored point light, the cooked runtime and the free editor camera. Run the
`[viewport-native-input]` case in `ludus_editor_tests` on an awake, exposed Cocoa
desktop with `LUDUS_NATIVE_GRAPH_RUNTIME` set to the absolute cooked variant path;
`LUDUS_NATIVE_GRAPH_CAPTURE` optionally saves its final framebuffer. The fixture
checks native orbit/pan/fly/frame gestures, resize, a reloaded World transform edit,
GPU-frame retention and publication revision. Framebuffer capture follows interaction
so it does not perturb the graph texture leases during navigation.

The original MIS native acceptance attempt published the GPU generation but stalled
while waiting for Qt to paint it. The fixture polled `QTest::qWaitFor`, which manually
pumps events rather than entering Cocoa's normal `NSApp` run path. The native fixture
now uses a bounded `QEventLoop::exec()` wait, allowing display-link/main-queue delivery
while retaining the original deadlines and normal GPU texture leases. See The Qt
Company's [Qt 6.12 Cocoa event dispatcher](https://github.com/qt/qtbase/blob/v6.12.0/src/plugins/platforms/cocoa/qcocoaeventdispatcher.mm)
(`QCocoaEventDispatcher::processEvents`) for that distinction. Production presentation
and transport scheduling are unchanged. The ready-state overlay now recognizes GPU-only
snapshots, so an empty CPU readback image no longer leaves “Preparing renderer…” over
a completed native view. The fixture supplies cooked candidates directly,
so it disables the built-in document's debounced live build; a late default preview
must not overwrite the MIS runtime being tested.

The editor build cooks the canonical MIS template at 64×1 and 8×8 for the dedicated
native case:

```bash
cmake --build out/build/<preset> --target ludus_editor_tests
QT_QPA_PLATFORM=cocoa out/build/<preset>/apps/editor/ludus_editor_tests '[viewport-native-mis]'
```

This case checks native orbit, trackpad/middle-button pan, look/fly and framing;
each navigation wait requires a newer rendered camera revision as well as another
presented image. Resize changes both the embedded surface and graph extent. The
World edit moves `starter.cube` through the project's normal extraction boundary.
A valid runtime with deliberately broken Metal shader source exercises asynchronous
replacement failure while the prior GPU view remains drawable and navigable. A
subsequent valid replacement must retain that view until its new generation presents.
A runtime load can queue staging behind an export copy, so the test observes the new
candidate serial after the safe boundary rather than assuming the immediate activation
report already describes it. Every graph presentation wait checks the serial actually
sampled by Qt against the first snapshot published for the required revision/generation;
painting another sample of the old view cannot satisfy it.
Framebuffer capture remains optional and follows interaction, so it cannot drive the
paint waits itself. These checks qualify presentation separately from the historical
transport timings and CPU/GPU parity above.
