# Render graph authoring, shader builds and pipeline management

Status: RG0 contracts, RG1 bounded offline shader builds and RG2 static opaque
Renderer execution implemented, October 13, 2026; RG3a editing, mapped
diagnostics and runtime activation implemented, October 15, 2026; RG3b node canvas
and trusted in-editor preview implemented, October 16, 2026; execution profile widened
to per-draw uniform bindings and authored masked coverage, October 17, 2026; RG4a
exact-descriptor transient pooling with a serial control implemented, October 18,
2026; the remaining RG4 optimizations remain proposed.
Design update, October 19, 2026: GPU work uses graphics, compute and ray tracing
pipeline nodes with customizable interfaces and node-owned shaders/state. This
authoring records, editor controls and explicit raster conversion are implemented.
Graphics nodes lower through the existing qualified raster profile; compute/ray
execution and broader stage/state support remain capability-gated. The delivered
profiles listed below retain their documented limits.
This defines the renderer authoring/compiler contract; it does not claim
unrestricted shader composition or new RHI capability.
Baseline design: `5c44d19`. See [usage and source formats](../development/render-graph-authoring.md).
The existing [Renderer](renderer-systems.md), [RHI/GDI](rhi-gdi.md),
[editor](editor-architecture.md) and [game reload](project-live-reload.md) retain
their implementation and lifetime contracts.

## Decision and scope

Author rendering through reusable typed graphs. GPU work is authored through
three pipeline node types: Graphics Pipeline (raster), Compute Pipeline and
Ray Tracing Pipeline. Each node owns its customizable inputs, outputs, shader
assignments, pipeline state and invocation description. A shader function is the
smallest programmable unit inside that pipeline; graph connections describe
resource flow and dependencies between invocations.
Compile the authored documents into immutable shader packages and an inspectable
execution plan. The runtime executes that plan through the existing RHI instead
of traversing editor objects or generating shaders during a draw.

**Authors define the legal compilation space. Users select within it. Connecting
or disconnecting a port never creates a shader configuration.** The compiler can
optimize a chosen program without changing its observable behavior; it cannot
invent techniques, remove declared effects or expose new permutations.

Start with raster, explicit serial pass order, ordinary resource bindings and a
local incremental build. Keep async compute, native heap aliasing, bindless,
distributed cooking and a visual shader-expression language conditional on a
measured requirement and separately implemented backend contracts. Advanced ray
nodes use the [ray milestones](renderer-systems.md#ray-tracing-and-path-tracing)
and [RHI capability gates](rhi-gdi.md#resource-and-command-contracts); a visible
editor node does not establish platform support.

This is the owner of graph composition, compilation choices, shared shader
schemas, build provenance, camera/scene coverage and their editor workflow.
Backend synchronization, handles, completion and command legality stay with RHI;
scene extraction and shading policy stay with Renderer. Detailed rendering
algorithms remain in their existing owners.

Thanks to Donald Revie, "Designing a Data-Driven Renderer," *GPU Pro 3*, V.4,
sections 4.4-4.8, pp. 307-317, for separating shared representation from rendering
context, exposing techniques and documenting data-driven optimization/editing
costs. We retain complete immutable inputs and move interpretation into cooking;
his context-stack traversal is not our resource-hazard graph. The
[follow-up review](renderer-systems-gems-review.md#render-graph-and-shader-authoring-follow-up)
records source evidence and explicit departures.

## Owners and representations

| Owner | Responsibility | Boundary |
| --- | --- | --- |
| Project documents / editor core | Graph, schema and technique sources; stable IDs, versions, selection, undo and explicit save | Qt/browser panels project this data; widget state never defines rendering semantics |
| Offline authoring tools | Graph validation/lowering, schema generation, shader cooking, dependency keys and reports | Shared CLI and editor job path; no engine dependency on the GUI or compiler |
| Renderer | Immutable view/scene extraction, draw-item routing, material technique selection and coverage diagnostics | Receives typed values and leases; does not depend on GameplayWorld |
| GDI services / current RHI graph seam | Plan instantiation, imported resources, bounded uploads, pipeline requests and transient reuse | Use current implemented seams first; do not introduce a second handle/state registry |
| RHI/backend | Pipeline creation, bindings, actual resource state, barriers, command submission and completion | One authoritative submission/lifetime implementation |
| GameHost / reload coordinator | Version admission and frame-boundary activation | No callbacks or pointers into unloadable game code in persistent plans |

Keep four explicit representations: authored document, validated/lowered graph,
cooked package and frame instance. They have different lifetimes. A text editor
and the visual editor modify the same bounded, versioned source format; generated
headers, target shaders, caches and reports are build products, not parallel
authored truth. Start with canonical JSON records and stable IDs, using the
existing parsing/configuration owners; file extensions and SDK API spellings are
implementation decisions. Reject unknown schema versions rather than guess.

The lowering representation needs only typed tables, graph edges, pass records,
configuration IDs and source mappings. Use concrete functions and Ludus-owned
containers with explicit allocation/status contracts. Do not create a universal
runtime interpreter, a polymorphic object per shader instruction, or a public
pass class per effect. Bound nodes, ports, nesting, expansion, resources, source
sizes and build jobs before admitting a document.

## Node and connection contracts

### Three GPU pipeline node types

Creating a GPU work node means choosing Graphics Pipeline, Compute Pipeline or
Ray Tracing Pipeline. The new node starts as a draft with no authored inputs or
outputs; Scene, View, Color and Depth are additions, not mandatory built-in ports.
A template may populate the same fields explicitly. An incomplete node can be
edited and saved. It must satisfy shader, resource, invocation and capability
contracts when selected for execution. Unused draft branches do not block another
valid route; structural source and wire validation still covers the whole library.

| Pipeline node | Customizable inputs and outputs | Node-owned shaders, state and invocation |
| --- | --- | --- |
| Graphics Pipeline (raster) | Scene/draw selection, View, typed values and readable resources; color/depth attachments and other declared written resources | Vertex and fragment/pixel shader assignments, optional admitted geometry/tessellation stages; vertex interface, topology, rasterization, depth/stencil, per-attachment blending, write masks and multisampling; draw source and viewport/scissor policy |
| Compute Pipeline | Typed values, readable textures/buffers and writable storage resources | Compute shader assignment, resource interface and admitted specialization; thread-group size and dispatch dimensions or admitted indirect dispatch source |
| Ray Tracing Pipeline | Typed values, View, acceleration structures and readable/writable resources | Ray generation, miss, hit-group and optional callable shader assignments; payload/attribute interfaces, bounded recursion and shader-table configuration; launch dimensions |

All three types use the same port identity, schema, access, versioning,
configuration and readiness rules below. Outputs describe produced resource
versions, not shader return values: a compute or ray tracing node can publish an
image through storage writes without a fragment stage or a color attachment.
Resource read/update access declares both the previous version dependency and
the new written version. Raster attachment outputs additionally declare format,
aspect, extent, samples and clear/load/store/resolve intent.

Pipeline state belongs to the node's authored record and inspector. Named
configurations may select complete shader/state combinations within its bounded
manifest; reusable shader sources can be referenced without duplicating their
text. Attachment operations and draw/dispatch/launch parameters belong to the
same node's execution description, even where they are dynamic rather than part
of a device pipeline key. The compiler derives dependency ordering from declared
resource uses; the existing RHI boundary owns hazard lowering and barriers.
Blending and rasterization are fields for now. Separately connectable
pipeline-state blocks are deferred, as is stream output.

Shader rows create one registered `.slang` draft each. Renaming a generated row
renames its program and source; renaming the Program field to a new identity
moves the existing source and updates graph and technique references. Selecting
an existing Program reuses it. Stage edits update the same manifest entry and
replace an untouched template with the new stage template. Authored source text
is preserved and may need its function signature adjusted for the new stage.
A shared program's stage cannot change while other assignments or techniques
still require its previous stage. These edits share one undo/redo transaction;
Save checks external changes before publishing file moves.

Raster coverage family and shader-pair identity are separate. The historical
`DefaultStaticOpaque.ColorAndDepth` and other family/mode records retain their
numeric runtime IDs. Additional explicitly assigned pairs use separate manifest
records under that family, such as
`DefaultStaticOpaque.ColorAndDepth.Geometry.GBuffer`; the suffix is a stable
identifier and may retain an older name after program renaming. The builder
resolves each GraphicsPipeline by its exact stage program IDs and validates its
complete recipe. Different entry points in one source are distinct programs.
Legacy Raster nodes continue to select their unsuffixed records.

Numeric plan version 5 and runtime package version 7 carry bounded additional
raster pair/stage slots, preserving the earlier role IDs and record widths.
The existing eight-technique manifest budget still applies. Older native ray
packages (version 6) and supported older raster packages remain readable; older
versions cannot carry extended roles. Uniform bindings are keyed by the exact
pair, while material eligibility is checked against its coverage family. Built-in
color/depth geometry parity checks remain on their historical pairs; independent
pairs own their vertex functions and make no implicit depth-parity guarantee.
Distinct same-family/mode shader passes for one view explicitly permit repeated
scene coverage. Duplicate routes using the same pair retain duplicate-coverage
rejection. Driver preparation and generation publication keep their existing
Pending/Ready/failure transaction.

Unused, untouched generated templates left by earlier editor versions are
reclaimed during pipeline edits. Edited or shared sources are preserved.
The manifest is also the registry for unfinished shader drafts: programs need
not belong to a technique until selected for execution. Builds validate the
registry and compile only programs selected by explicit techniques. Incomplete
pipelines still report their missing stages, resources or unsupported state.

Scene and View are engine-level inputs, not implicit global shader variables.
Their declared schemas map scene extraction/draw selection and per-view values
to the node's shader resource interface. Inputs may be used by any legal stage;
adding Scene or View does not require a geometry shader. Geometry and other
optional stages must pass the selected backend profile's capability checks.
Likewise, adding Depth does not require a fragment shader to write depth:
ordinary rasterized depth is valid under the admitted profile. Custom fragment
depth or alpha coverage requires the corresponding authored shader behavior.
Color outputs must agree with fragment output locations/types and attachment
state. Port edits expose missing bindings and stage mismatches as diagnostics;
they do not synthesize shader bodies or silently choose another technique.

A typical raster authoring sequence is:

1. Create a Graphics Pipeline node, initially with no inputs or outputs.
2. Add Scene and View inputs, define their schemas/draw contract, and author or
   assign the vertex shader and any explicitly needed optional geometry stages.
3. Add Color and/or Depth outputs with attachment descriptors. Author or assign
   a matching fragment shader where required; retain the selected profile's
   depth-only and coverage rules.
4. Set rasterization, depth/stencil, blending and other pipeline state in the
   node, then connect providers and output consumers.
5. Validate and build a candidate, prepare its required device pipelines, and
   activate only when Ready. Pending or invalid edits retain the last drawable
   generation.

Compute and ray tracing follow the same interface/shader/state workflow, with
their own dispatch or launch contracts instead of raster stages/attachments.
Declaring either type does not admit it on every target. Unsupported stages,
resource kinds, shader tables or state combinations produce explicit capability
errors; no graphics fallback or ignored setting is invented.

### Upscaling operations and presets

`Upscale` is a version-2 supporting operation. Bilinear is an executable spatial
method; MetalFX Spatial executes through the native Metal RHI adapter.
Authoring-only presets cover DLSS SR, FSR 1/2/3 upscaling, MetalFX Temporal and XeSS SR.
Its fixed typed interface is chosen by `algorithm`; it owns no shader program or
pipeline-state record. Input color and output destination are distinct resource
ports. Spatial presets expose two texture inputs; temporal presets additionally
expose depth, motion, jitter, motion scale, exposure and reset values. The output
is the destination's written color version. Presets are small node records,
rather than duplicated full renderer configurations.

The remaining vendor presets admit source inspection and editing only. Selecting one for
execution reports an unavailable-backend diagnostic at the authored node,
including inside nested graphs. Unused drafts are culled from execution while their source shape
and wires still undergo structural validation. No placeholder dispatch, implicit
copy or automatic fallback may stand in for a vendor SDK. Backend availability,
texture precision, dynamic sizing and temporal history ownership require a
Renderer/RHI adapter before those presets can produce runtime work.
Bilinear lowers to an explicit image pass after its source producer. It samples
an initialized offscreen RGBA8 allocation into a distinct full-target destination,
with one propagated camera route, no scene coverage and no temporal state. The
private renderer-owned program reuses the installed composition shader recipe,
with an explicit linear-clamp sampler. Scene generations own immutable source
bindings while the graph retains reusable shader/pipeline/quad setup. RHI owns
resource hazards and completion leases. Resize preparation follows the existing
Pending/Ready publication transaction, retaining the previous drawable generation.
Numeric plan version 11 and runtime package version 15 add SourceColor and append
BilinearUpscale after all existing scene technique IDs; shader-stage IDs remain
unchanged. Older runtime versions decode with no image source, including version
14's independent explicit ToneMap policy. Both compiled headers and runtime
packages carry the same serialized source resource and authored pass name.
Numeric plan version 13 and runtime package version 17 append two MetalFX Spatial
roles (Linear and Perceptual) after BilinearUpscale, preserving scene/stage IDs and
the 19-field version-16 pass layout, including OIDN denoising quality. Versions 15/16 still decode bilinear; older
versions cannot select MetalFX roles. `color_processing` is optional only on
`metalfx-spatial` and defaults to Linear. The compiler records its interpretation
explicitly in the chosen image role. Both roles use the same distinct initialized
RGBA8 source/destination, camera routing and no-scene-coverage contract.

The RHI owns prepared spatial scalers in its existing generation registry and
submission ledger. Creation occurs outside frames on a supported Metal device;
encoding closes the preceding raster pass and records native work in the same
command buffer. The frame retains the effect and both textures until completion.
Only a successful operation defines destination color, invalidating its private
depth/stencil. A subsequent raster draw begins a new pass. Non-Metal backends
and unsupported devices report Unsupported without substituting another method.

Effects belong to attachment generations, independently of scene/camera snapshots.
Resize candidates prepare their own scalers and optional surface intermediate;
publication swaps these with the attachments only when the replacement scene is
Ready. Offscreen targets already satisfy MetalFX's private storage and declared
usage requirements. Presentation uses a private RGBA8 intermediate followed by
the shared installed composition recipe; swapchain usage is unchanged. Color
interpretation is explicit; neither mode inserts AA, tone mapping or encoding.
Vendor-private memory is opaque and is not fabricated into texture byte accounting;
the normal per-kind resource bound limits effect instances.

See [upscaling presets](../development/render-graph-authoring.md#upscaling-presets)
for the interface, references and editing workflow.

### Output and render size ownership

OutputSize owns final physical image dimensions, either the current drawable or
fixed offscreen dimensions. RenderSize can explicitly scale that output reference
with a bounded positive fraction. Native-resolution rendering uses 1/1; upscaling
presets wire scene attachments at 1/2 and destination storage at full output size.
Each provider's saved graph record is the settings owner, editable through the
same controls in Properties and the canvas dialog. Project/build descriptors and
machine preferences do not duplicate those values.

Both nodes expose the existing RenderSize graph value, including across subgraph
interfaces. Viewport policies lower to the current runtime view numerator and
denominator; fixed policies lower to fixed resource extents. Runtime resolution
uses physical drawable pixels, integer ceil and existing extent limits. No new
GPU pass or native texture representation is introduced. Existing legacy
RenderSize records retain their meaning. Output-derived rendering rejects a
scaled view as its output reference, avoiding hidden chains of rounding.

Upscaling presets add their providers and paired targets as one document/history
transaction, preserving existing execution paths and capacity-failure rollback.
They remain authoring-only. See [resolution settings](../development/render-graph-authoring.md#output-and-render-resolution)
for source examples and configuration controls.

### Connection domains and supporting operations

The graph retains different connection domains:

| Node/edge domain | Meaning | Compiler treatment |
| --- | --- | --- |
| Shader function / value | Typed computation, stage inputs/outputs and logical resource requirements | Compose within legal stage boundaries and generate entry wrappers |
| Pipeline execution | Graphics, compute or ray tracing pipeline node | Validates node-owned interface, shaders, state, invocation and profile; lowers declared uses and ordering |
| Supporting operation | Copy, clear, resolve or presentation | Explicit resource/target operation with its own legal uses; not a fourth programmable pipeline type |
| Resource | Texture/buffer versions, imported targets, persistent history and exported results | Validates descriptor, access, initialization and lifetime requirements |
| Scene/view | Scene snapshot, camera/view, query and draw-item stream | Renderer supplies CPU values and draw selection; not a GPU texture |
| Subgraph | Immutable referenced graph plus exposed interface and chosen configuration | Lower through the same rules, preserving the nested source path |

`VSMain`, `PSMain` and `RayGen` identify shader functions assigned within a
pipeline node. A future shader-expression view may expose function nodes inside
that composition; they are not separate GPU work nodes or independently
schedulable frame passes. Rasterization/interpolation connects
vertex outputs to fragment inputs; these edges do not allocate intermediate
textures. Stage inputs, derivatives, built-ins, payloads and invocation rules
need stage-aware validation. Ray function tables and recursion remain an
optional backend profile, not arbitrary ordinary-function links.

Each assigned shader function declares a signature, stage eligibility, parameter
schema, logical resources/access and effects. The shader author writes
computations and resource accesses against that interface. The graph binds actual resources;
generated wrappers assign physical bindings and target semantics. Handwritten
entry points may supply the equivalent manifest, checked against reflection.
Neither path requires hand-authored descriptor numbers, barriers or per-frame
resource wiring in shader code. Reflection checks declarations; it cannot prove
every dynamic resource index or hidden effect. Restrict access to declared
interfaces, reject unverifiable patterns or require conservative explicit uses.

Offer typed value-provider nodes for standard engine inputs (view transforms,
draw transforms, frame time and declared scene textures). A provider has a schema,
update frequency and explicit data/resource dependencies; generated bindings
resolve it before encoding. Cache derived CPU values once at their declared
view/draw frequency and invalidate on input revision, rather than hiding a mutable
global state behind convenient uniform names. Thanks to Patrick Cozzi,
"A Framework for GLSL Engine Uniforms," *Game Engine Gems 2*, chapter 6,
sections 6.2-6.6, pp. 83-92, for automatic inputs with explicit state and derived
value caching. We adapt that contract, not its string maps or per-uniform virtual
factories; see the [reading evidence](renderer-systems-gems-review.md#render-graph-and-shader-authoring-follow-up).

A port carries stable identity, direction/domain, type/schema version, required
or optional connection policy and configuration availability. GPU resource ports
add format/class, extent/layers/mips, samples, access and aspect/range constraints.
State color space and alpha convention where meaningful. Describe requested
resource usage as a checked union of legal producers/consumers; unsupported
attachment-plus-sampling/resolve combinations reject. Extents are data or bounded
expressions, not compile-time shader switches by default.

The current triangle ray profile supports target-derived launch sizing: its
`invocation.dimensions` may be `"ColorTarget"`, resolving to the connected writable
color target's `[width, height, 1]` for each graph instance. This derives execution
coverage without selecting another shader or pipeline configuration. Explicit
dimensions remain supported as a checked equality contract. See the [authoring guide](../development/render-graph-authoring.md#execute-a-triangle-ray-pipeline)
for the source representation and editor controls.

Version-2 `RenderSize` nodes own graph-only size values: fixed pixel dimensions or
the dimensions of a connected color/depth texture. Targets may consume those
values, and subgraph interfaces may pass them between caller instances. The
compiler resolves size dependencies before publishing nominal resource extents;
metadata-only target descriptions need no GPU allocation. The same explicit edge
and cycle rules apply to size dependencies. Fixed array extents remain compatible.
The [size workflow](../development/render-graph-authoring.md#share-render-sizes)
owns the source syntax and editor instructions. A view-relative RenderSize carries
an integer scale ratio into numeric plan version 3 and runtime package version 4.
The per-instance drawable reference comes from the viewport/export request, with
ceil rounding and bounded resolution. It never enters shader recipe identity.
Resource initialization resolves it before device admission. Extent-only resize
stages an owned attachment set and a fresh scene using the existing pipeline
contexts. They publish together only after readiness; the old scene/output remains
available while pending or on failure. A newer request discards the unactivated
set and its scene. GPU retirement and pooling remain in their existing owners.
Fresh ray jobs reset any explicitly authored color accumulation. Ordered ray sequences may bind graph-owned
persistent uint4 buffers. Fixed capacities and their contents survive scene/camera/size
replacement. Pixel-sized buffers follow a connected RenderSize policy and stage zeroed
replacements when capacity changes. Their shaders own history validation; external texture/history imports
remain outside the admitted profile. See the [ordered-ray contract](../development/render-graph-authoring.md#order-ray-passes-and-share-persistent-buffers). The editor supplies pre-render-scale physical viewport pixels,
keeps its last completed image across replacement, and updates inspection metadata
from the published resource descriptions.

Editor connection preflight resolves known target descriptors through paired
resource outputs and nested subgraph bindings, checking each supported operation's
attachment extent, format and sample contracts. It rejects newly provable
mismatches without mutating the document, while bounded/unknown inference defers
to concrete build validation. No global equal-size constraint is imposed on
sampled textures, and a texture connection never selects another shader recipe.

Each GPU resource output names a logical resource version. Writes produce a new
version; preserve/load requires a dependency on initialized previous contents.
External targets are imports supplied to the root graph, including an acquired
back buffer. An output edge exposes the written version, not an instruction to bind
an arbitrary GPU object after execution. Multiple writers must use explicit
version/order edges or a composition node. Clear/load/discard, store, depth,
stencil, blend and resolve intent are part of the execution contract.

Reject incompatible connections, multiple unresolved writers, uninitialized
reads, recursive imports and within-frame execution cycles. Previous/next-frame
history is an explicit persistent import/export with reset/invalidation policy;
it is not a cycle hidden inside the current frame graph. A subgraph exposes its
interface and effect summary, not permission to rewrite its internals.

## Cameras, reusable raster graphs and scene coverage

Every active presentation camera must have at least one valid graph route to its
designated presentation target and Present operation. Creating one assigns the
default raster graph. Inactive cameras need no executing route; texture cameras
terminate at an exported offscreen texture. Loading a saved broken assignment
reports it and preserves the document instead of silently replacing it.

The saved `RasterizeGraph` is a reusable subgraph/template containing a Graphics
Pipeline node, not a fourth pipeline type. It exposes scene, view/camera, material
policy, vertex contract and target requirements, plus Color and Depth outputs.
Its target is supplied/requested by the parent; its implementation does not hardcode a surface.
The default initially uses a documented default-material override on admitted
vertex layouts. It does not promise compatibility with arbitrary mesh layouts.
Later scene-material selection uses explicit compatible technique interfaces.

```text
Scene + View + material policy + compatible target
                         |
                         v
                 [RasterizeGraph]
                   |          |
                 Color      Depth
                   |
        Export texture, compose, or Present
```

Reuse a graph definition across cameras with separate view inputs, instance IDs
and resource/history ownership. Multiple cameras sharing a surface declare
viewport rectangles and clear/composition order. Two independent producers
connected to one back buffer do not implicitly blend or preserve one another.

Graph legality and scene coverage are separate validation results. Coverage is
per draw item (submesh/material slot/LOD representation), required rendering role
and view, rather than one Boolean per entity. Renderer adapters extract those
records from World/Scene/Level without a dependency from RHI to gameplay.

First determine which items the view intends to render from explicit visibility,
layer and authoring rules. Then find compatible active routes to the required
output. Keep intention separate from compatibility: a layout/material mismatch
must not disappear inside a query filter. Depth coverage alone does not satisfy
color/presentation coverage. Shadow and color participation are legitimate
distinct roles; unexpected duplication within a role is diagnosed unless the
author declares additive/compositional participation.

Report Covered, Unsupported (reason and rejected route), Conflicting, or
IntentionallyExcluded. Ordinary visibility/occlusion culling is an execution
outcome, not proof of an unsupported layout. Report against graph, scene, view,
material/schema and capability revisions; streaming/new content invalidates
affected coverage. Cache by compatible render signature and update changed
items, with a full reference scan for validation. Dynamic selection that cannot
be proved complete remains explicitly Unknown until evaluated.

An error route is an authored, ready editor technique for the same rendering
role. Use visible error geometry only when its position/layout path is valid;
otherwise draw a bounds/marker proxy and retain the original reason. It cannot
make required depth correct for unknown deformation/coverage, and cannot turn
missing capability or failed pipelines into success. Shipping policy explicitly
rejects required incomplete coverage or admits a named fallback; it never
silently hides required objects.

## Author-owned compilation choices

An asset declares an explicit finite list of legal configuration tuples, default
selection, admitted target profiles and child-configuration mappings. Users choose
only those entries. Runtime values, scene size, camera count and connection count
do not become shader configurations. Prefer a named mode over independent
Boolean switches; independent dimensions are allowed only with a bounded,
reviewable legal-tuple set. Specialization constants, generic/type substitutions
and link-time choices are configurations too, not ways around the manifest.

Keep connection policy, compilation choice and effects separate:

| Declaration | Result |
| --- | --- |
| Color may be unused in `ColorAndDepth` | Execute the authored program and retain its effects; unused result has no external consumer |
| Color must be consumed in `ColorAndDepth` | Missing consumer is a graph error |
| Author exposes `DepthOnly` | User can select that separately authored/validated technique |
| Author exposes an automatic selection rule | Rule may choose a listed configuration; the UI/build report displays that choice |
| No depth-only choice exists | Disconnecting Color never synthesizes a depth-only shader |

Example interface, conceptual rather than a new file-format promise:

```text
RasterizeGraph configurations:
  ColorAndDepth: Color optional-consumer; Depth optional-consumer
  DepthOnly:     Depth required-consumer; Color unavailable

Default: ColorAndDepth
Selection: explicit
DepthOnly technique: authored geometry + coverage/depth implementation
```

Unconsumed Color does not mean free color rendering. Allocate/store what the
chosen technique requires and show that cost. Removing a shader stage, changing
attachment topology or selecting another material technique requires an authored
configuration. Backend optimization of dead calculations or irrelevant stores
within the same observable contract does not expose an additional configuration.
Validate effect summaries when deciding whether whole unreachable passes can be
culled: presentation, exports, history writes, externally observable storage
writes, readbacks and declared required effects are roots. A write to an unused
transient resource is not independently observable, provided the declared effect
contract proves it has no other effects. Unknown effects retain work or reject
it. Debug
preview of an output is an explicit extra consumer and may change resource
retention/culling; it must never silently request a shader permutation.

`DepthOnly` must preserve deformation, alpha clipping/sample coverage and custom
depth where required. Transparent materials do not automatically enter a pre-Z
role. A depth-only pass is a pre-Z pass only when the later shading technique
declares matching geometry, coverage, samples and depth comparison/write state.
Authors may share geometry/coverage functions between techniques; the compiler
does not derive semantic equivalence from arbitrary `PSMain` code. Enforce this
with parity fixtures, not just matching signatures. `[[nodiscard]]`-like port
warnings cannot establish these semantics.

Nested graphs map parent choices to child choices explicitly. Composition
validates all selected tuples against a project/target budget; it does not take
an unrestricted Cartesian product. Count declared tuples, selected reachable
tuples, unique stage programs, emitted target artifacts and pipeline recipes
separately. Deduplicate identical canonical programs across graph instances;
never count deduplication as permission to expose an unbounded source space.
Budget failure reports the authoring nodes/choices responsible, without silently
dropping profiles, outputs or features. New budget values require measured review.

An exposed shader-function/material slot is an author-declared extension point,
with stage/schema/effect and technique constraints. List the concrete project
implementations and legal combinations in the package closure before cooking.
Implementing new shader code creates a new authored asset revision; connecting
an ordinary value, adding an instance or selecting an imported graph does not
create a new shader. An open interface is not permission to compile arbitrary
new per-object types at runtime. All nested/extension contributions enter the
same count and budget report.

Thanks to Arseny Kapoulkine, "Writing an Efficient Vulkan Renderer," *GPU Zen 2*,
IV.4, sections 4.5-4.6.3, pp. 239-245, for complete technique state and the warning
that shader plus fixed-state combinations multiply. Review whether a small
computation can remain runtime data/work, or a rare effect can have a separate
authored pass, before adding another static tuple. This is an author decision
with measured cost, not a compiler-created permutation or a promise that every
pipeline can cheaply remain resident. Detailed [source findings](renderer-systems-gems-review.md#render-graph-and-shader-authoring-follow-up)
record the departures.

## Shared schemas and CPU/GPU layout

The [shared scene, material and lighting shader contract](shared-scene-shader-contract.md)
describes typed scene/light access, geometric mesh-emitter sampling and bounded
intersection/visibility queries for authored raygen and closest-hit shaders.
The delivered scene-ray profile keeps NEE and bounce policy in user shader sources.
A full Scene connection selects every data group; expanded Geometry, Materials and
Lights wires retain one scene identity and select the pass's data mask. The
[authoring guide](../development/render-graph-authoring.md#connect-or-expand-a-scene-bundle)
owns the exact delivered workflow and limits.

Define fields, stable IDs, units/defaults, resource kinds and packing policy once
in an authoritative schema. The editor is a view/controller for that source; an
equivalent text source is accepted under the same rules. Generate C++ value
definitions, shader declarations, serialization/upload packing, inspector
metadata and interface/layout identities. Derived files are regenerated and
verified in CI. Do not manually maintain a second shader-side struct.

A restricted shared header is optional when both compilers accept it and every
supported target proves the layout. Single-source semantics do not promise
identical C++/shader bytes. Verify scalar representation, offsets, alignment,
array/matrix strides, matrix order and resource categories from each emitted
target. Use generated target packing where representations differ. Reject
unverified types, unbounded arrays, native pointers and accidental host `bool`
or object layouts at the GPU boundary. CPU generated code follows Ludus types,
include boundaries and exception-free explicit errors.

Separate logical schema identity from target binding/byte-layout identity.
Compatibility is a checked descriptor comparison, not a name/hash match or
equal total byte size. Stable field IDs support diagnostics and declared
migration; they do not authorize reinterpreting old bytes. Logical View/Material/
Draw grouping remains an update-frequency convention, lowered to the verified
Vulkan/Metal/WebGPU/WebGL profile rather than requiring identical descriptor sets.

Engine graph interfaces belong to versioned SDK schemas; game graphs may extend
them through declared material/function interfaces. Ship built-in graph templates
and required shader sources or pin-compatible modules in the authoring SDK so a
game can build a legal composition without recompiling the engine binary.
Shipping runtimes consume cooked packages and need no Slang compiler. Engine
ABI changes still require a host rebuild/restart; custom game schema changes
use a declared adapter/migration or reject activation.

## Deterministic shader and graph build

```text
Saved source snapshot: graph + dependencies + schemas + shader functions
  -> validate configurations/profiles and calculate count/budget report
  -> lower graph and generate schema/binding/entry wrappers + source map
  -> compile selected programs with the pinned shader chain
  -> reflect and validate each emitted target, stage interface and effect uses
  -> package shaders, graph plan, layouts, pipeline recipes and diagnostics
  -> prewarm device pipelines -> admit compatible immutable generation
```

Reuse [the existing pinned shader toolchain](../decisions/0010-shader-toolchain-feasibility.md)
and [GLSL ES lowering](../decisions/0013-browser-glsl-es-shader-toolchain.md).
Extend the current offline helper at its owning boundary; do not replace it with
a second editor compiler. A persistent Slang worker/module cache can later reduce
front-end repetition if measurements justify it and the pinned API is qualified.
Module/link-time specialization must still use the authored configuration list.
Do not assume current documentation or a newer compiler establishes support on
the project's pinned version.

Build keys include canonical semantic graph/configuration data, transitive source
and import hashes, schema/layout policy, entry functions, emitted binding maps,
target/profile/features, all code-generation flags, debug/optimization mode and
compiler/tool/generator versions. Keep editor positions/selection, timestamps,
absolute machine paths and camera/object counts out of shader identity. Separate
per-stage keys, graph-plan keys and pipeline keys so moving a node, editing a
runtime parameter or changing only pass order does not rebuild unrelated shader
code. Source maps/debug-path remapping have their own deterministic inputs.

Preserve source-to-intermediate-to-target provenance and expose Open Source from
generated diagnostics/selected artifacts. Generated-source views are read-only;
editing the owner regenerates intermediates instead of losing changes on the
next cook. Thanks to Rémi Arnaud, "The Game Asset Pipeline," *Game Engine Gems 1*,
chapter 2, sections 2.2-2.3, pp. 24-30, for readable intermediate data, tracked
dependencies and source-directed editing. We use a local shared build service;
COLLADA, DCC round trips and automatic source rewriting are not dependencies.

Use local content-addressed immutable outputs first. Concurrent jobs share only
validated immutable cache entries; each build owns its staging directory and
writable tool state. Hashes are checked against full identities at lookup and
artifact integrity at load. Publication is atomic after all dependencies match
the captured source snapshot. Cancellation/stale revision discards the candidate;
partial output cannot replace the last good package. A missing/corrupt cache
entry causes a rebuild, never a false ready state.

The package records configuration IDs and why selected, dependencies/provenance,
schema and target layout descriptors, shader entry artifacts, declared uses and
effects, pipeline recipes, required capabilities, resource/plan bounds and stable
authored-node mappings. Keep validation/source diagnostics and compilation time/
artifact bytes in a readable sidecar report. Runtime encoding uses resolved
numeric records; no source parser or string search per draw.

| Edit/action | Expected invalidation |
| --- | --- |
| Move a node or change panel layout | Presentation metadata only |
| Change a runtime material value or add a camera using the same graph/profile | Values/frame instance; no new shader configuration |
| Disconnect optional Color without changing the selected technique | Consumer/resource plan; same stage programs and authored pipeline attachment contract |
| Select declared DepthOnly | Select/build its listed programs/recipes; no additional inferred tuple |
| Change pass order only | Validate/rebuild affected plan; reuse unchanged stage programs |
| Connect shader value functions while authoring a new graph revision | Rebuild affected authored stage composition and verify its manifest/budget |
| Change a shared schema or imported shader source | Invalidate transitive affected programs, layouts and bindings; coordinate CPU interface if changed |
| Resize an ordinary target | Resource instance; pipeline only if format/samples/state actually changes |

Package every configuration that content may select during execution; explicit
configuration switching selects a cooked/prewarmed entry or waits outside drawing
for a newly admitted generation. Observed use can prioritize prewarm work, but
observations alone cannot prove the completeness of all runtime choices.

## Pipeline readiness and execution

Separate shader configurations from pipeline recipes and device pipeline objects.
Different vertex layouts, attachment formats, sample counts or fixed state can
require distinct pipelines without another shader program. Authors bound their
admitted pipeline state/profile combinations too. Pipeline keys include exact
shader versions, binding layouts, vertex interface, topology, raster/depth/blend
state, formats, samples and permitted specialization values; compare complete
canonical descriptions after hashing. Extent, camera ID, target handle and
ordinary uniform values normally do not belong in the key.

A portable recipe manifest supplies prewarm requests. Driver caches are optional
device/driver-qualified accelerators, not portable source truth or a promise of
zero cold compilation. Track Missing/Pending/Ready/Failed independently from
shader-package status. Required pipelines must be Ready before activating the
candidate route. Optional effects use an authored ready fallback or explicit
pending/unsupported status. The diagnostic error route itself must be prewarmed.
Draw encoding never blocks on compilation or constructs a new pipeline key.

Select observable execution dependencies before checking bound inputs or lowering
pipeline capabilities. Export/Present operations, root interface outputs and
explicit active-camera routing obligations are roots. Retain nested effects;
expand only consumed subgraph output branches while preserving the retained
subgraph's declared input contract. Required-output consumer checks count live
consumers. Pruning never edits source nodes or wires, and never changes a live
pipeline's declared attachment/shader contract. Structural cycles, invalid wires
and imports remain source errors even outside the execution selection.

Lower the selected dependency graph to a deterministic author-visible serial order first:
dependencies constrain order, explicit order edges express semantic order, stable
authored ordinals break remaining ties. Supply that ordered plan to existing
RHI/GDI graph validation. It owns hazard lowering and accepted-submission state;
the authoring compiler never commits speculative GPU state. Reflection and
declared uses must agree; widen conservative ranges instead of guessing safety.
Keep streaming outside the frame graph with explicit imported readiness.

Cache plans by topology/configuration/profile/resource compatibility. Bind fresh
scene/view data and imported resources each frame, retaining exact versions.
Extent-only resize updates checked resource descriptors/pools; shader rebuild is
unnecessary unless an authored specialization depends on it. Still validate
actual imported state, readiness, format/sample changes and completion obligations
on execution. Frame histories are per graph instance/view with explicit reset
conditions. Warm paths reuse reserved arenas and tables; capacity exhaustion
returns a bounded diagnostic rather than allocating without limit or dropping
required draws.

Use exact-descriptor transient pooling with completion proof first. Native memory
aliasing, pass merging/reordering, parallel recording and multiple GPU queues
are later independent optimizations with a serial/non-aliasing control, image/
effect parity, memory measurements and schedule diagnostics. WebGPU pooling
does not imply physical heap aliasing. Every optimization retains source mapping
and can be disabled in a diagnostic execution mode that preserves legal commands.

RG4a delivers that first slice in Renderer's `RenderGraphAttachmentPool`. A
transient backing is an allocation behind logical resources that are neither
exported nor the presentation surface. A graph given a pool leases each transient
backing from it and returns it on Reset; `RenderGraphActivation` owns one pool for
all of its generations. An idle allocation is reused only when its complete RHI
texture description is equal and the ordered submission of its last frame use
(`RasterTextureState::Dependency`) has completed; otherwise the acquisition
creates a new allocation and is reported as deferred. Exports never enter the
pool, because their consumers may retain views or bindings beyond the graph, and
surfaces allocate nothing. Idle capacity is explicit and evicts the least recently
released entry; idle entries keep their RHI records and bytes until reused, evicted
or trimmed. The null-pool `SerialControl` path is the pre-RG4 allocation behavior
and encodes the same commands.

The compatibility RGBA8 image of a depth-only backing is kept. RHI attachment
textures always pair a color image with private depth, `RasterPassDescription`
has no depth-only form, and every raster pipeline targets one to four color
formats. Removing it needs a depth-only RHI attachment and color-less pipelines
on every backend, which is RHI work outside this slice. The admitted RG2 profile
has no pass that samples a graph resource, and lowering never approves repeated
camera coverage, so no cooked graph has a transient backing today. Pooling is
therefore qualified on generations rewritten with unexported scratch backings, and
is idle for every cooked fixture. See
[RG4 usage, evidence and limits](../development/render-graph-authoring.md#pool-transient-attachments-rg4).

## Editor interaction, diagnostics and hot reload

Provide one Renderer workspace over the shared document/command core. The graph
canvas, node inspector, schemas and shader source editor use stable IDs and one
save/undo/conflict policy. Show function, execution and resource domains visually;
reject illegal links with actionable reasons. Double-click a subgraph to navigate
its source; edits create a draft/new asset version rather than secretly mutating
all imported runtime instances. Expose configuration choices and their count/cost
before build. Create GPU Work offers the three pipeline types. The selected node's
inspector owns Inputs, Outputs, Shaders, Pipeline State and Invocation editing;
adding/removing/retyping ports is a semantic edit with stable IDs, undo and
mapped validation diagnostics. Diagnostics carry stable node/port/direction or
field keys, independently of human-readable labels. The shared validator collects
bounded independent failures between validation phases and stops before dependent
analysis can produce cascades. Resource lowering remains transactional and
fail-fast. The editor displays each structured issue once with expected/actual
values and repair guidance, and navigation rings its port or focuses the shared
canvas field control without editing the document. See the
[mapped diagnostics contract](../development/render-graph-authoring.md#mapped-diagnostics)
for payload fields and limits. Removing a connected port reports the affected
links and requires an explicit draft edit rather than silently retargeting them.
Shader assignments open their source in the existing source editor. Stage
interfaces remain inside the pipeline node instead of appearing as independently
scheduled passes on the execution canvas. The editor provides explicit Apply
Pipeline and Convert to Graphics Pipeline actions; Save publishes the draft. Layout-only edits do not compile; parameter edits
follow declared runtime update paths; configuration/source edits build a candidate.

Configuration-interface editing prepares a bounded package snapshot before
publication. Existing row identities supply explicit input/output rename maps;
array order never implies a rename. Preparation rewrites both ends of affected
links before checking candidate endpoint types, including links between two
instances of the same child. Removed or incompatible uses disconnect; compatible
uses survive. Affected configuration output sources clear when incompatible,
while their declarations and required-consumer flags remain stable. Repairs do
not recursively change enclosing output contracts or alter pipeline declarations.
All direct callers in every loaded graph file/configuration participate in the
same document-history commit, undo/redo and multi-source save. Unaffected draft
links and unchanged file bytes are preserved. Malformed graph JSON, ambiguous
rename maps and package byte-limit failures reject without mutation. The dialog
previews the same preparation before Apply, with bounded details in a scrollable
area; Cancel makes no document edit. Canvas activation and the inspector use
the same dialog and document command. Raw Source edits and changes to an
instance's selected asset/configuration remain explicit, separately validated
edits. See the [interface editing guide](../development/render-graph-authoring.md#edit-configuration-inputs-and-outputs)
for the authoring workflow and identity restrictions.

Canvas annotations are a read-only projection of the displayed draft. A bounded
activity inspector mirrors observable-root selection (including nested effects,
active cameras and the retained-subgraph input contract), with compiler-selection
parity regressions. Standalone library inspection assumes every exposed output
is requested. Structural uncertainty and import recursion produce Unknown rather
than a false inactive claim; this projection never admits execution. Size labels
reuse connection-check resource inference through writable targets and nested
bindings. View-relative dimensions use the current World drawable reference,
runtime integer-ceil rounding and extent bounds, or stay symbolic when that
reference is unavailable. They do not describe the last presented generation.
Port hover during a drag calls the existing connection pre-check, caches its
result until the hovered endpoint changes, and displays its reason before drop.
Annotations and hover state own no document history, compiler jobs or GPU state.

Build, Preview and Apply to Session are explicit actions. Background compilation
is an opt-in preference and never implicitly saves drafts or applies stale output.
Jobs capture project epoch, document/dependency revisions, target and request ID;
the GUI thread never waits for a compiler, device or host shutdown. Unsaved preview
uses an explicit immutable draft snapshot visibly distinct from saved content.
The trusted authoring preview owns a separate scene/view/resource instance; live
gameplay rendering remains behind the existing out-of-process GameHost boundary.

Three inspectors make failures traceable:

| Inspector | Required information |
| --- | --- |
| Build/configurations | Declared/selected configurations, per-target shader and pipeline counts, cache hits/misses, rebuild reason, provenance and source diagnostics |
| Compiled plan/resources | Authored-to-lowered path, execution order, resource versions/ranges, initialization, clear/load/store/resolve, lifetimes/retained bytes and hazards |
| Coverage/frame | Camera/output routes, missing draw items and rejection reasons, chosen material/technique, pending pipelines, fallback usage, CPU/GPU timings and capture labels |

Generated code diagnostics map to authored function/file/line and nested node
path. Retain an inspectable generated-source view, configuration comparison and
machine-readable plan dump for regression reproduction. Label GPU captures with
asset/configuration/node IDs and generation; resource previews explicitly show
format/color/depth interpretation and any extra readback/retention cost. Keep
instrumentation bounded/opt-in and use existing logging/profiling rather than
another telemetry service. A diagnostic serial execution mode aids comparisons
but cannot conceal declared effects or invalid initialization.

Preview and live execution use the same cooked configuration, Renderer/RHI
contract and target profile, with explicit scene/view differences. Compare a
frozen fixture through both paths; a thumbnail-only approximation is not live
parity evidence. Share immutable shader packages and resource leases across
preview instances where the device permits; clone small parameter values, not
textures or GPU programs. Preserve intentional shared parameters through explicit
versioned bindings. Thanks to Curtiss Murphy, "Support Your Local Artist: Adding
Shaders to Your Engine," *Game Programming Gems 7*, section 7.4, pp. 541-554,
for in-engine iteration, program/parameter separation and the duplicate-texture
failure example. We use typed versions and existing host lifetimes instead of
his singleton/prototype class hierarchy and direct reload/reassignment.

Reload stages a consistent graph + shaders + layouts + bindings + pipeline set.
Require complete dependencies, profile/coverage admission and ready required
pipelines; publish at a frame boundary. Failure keeps the prior generation and
its diagnostics visible. Retire old GPU resources through RHI completion and old
game callbacks through GameHost quiescence separately: neither proves the other
is safe. Host-owned copied values/validated handles survive module unload;
persistent plans never retain pointers into game memory.

If a CPU schema changes, rebuild its generated game interface and stage a
compatible game generation/adapter before activation. If coordinated activation
is not available, return RestartRequired instead of publishing incompatible
halves. Built-in engine schemas stay compatible with the running SDK; loading a
new game shader package cannot mutate a compiled engine ABI. Browser dynamic
game-module reload remains subject to its existing documented limitations.

Parameter migration retains a value only when stable field identity, type,
meaning/units and declared conversion agree. A renamed label is metadata; a
changed semantic field requires explicit conversion/default and a visible diff.
Do not match by similar names or reinterpret bytes. Preserve the saved document
and last valid runtime values on conflict. Thanks to Christopher Maughan,
"The Design of FX Composer," *GPU Gems*, chapter 30, sections 30.3.1 and
30.6.2-30.6.4, for parameter reconciliation and separate device-independent
compilation/device instantiation. We retain strict migration and prewarm
admission instead of its COM plug-ins, separate device per panel and lazy
ApplyToDevice path; see the [review](renderer-systems-gems-review.md#render-graph-and-shader-authoring-follow-up).

## Contemporary cross-checks

These are focused documentation checks, not proof that another engine's complete
architecture fits Ludus or that current features exist on our pinned tools.

- Thanks to Epic Games, [Render Dependency Graph](https://dev.epicgames.com/documentation/unreal-engine/render-dependency-graph-in-unreal-engine),
  especially Debugging and Validation: declared resource dependencies, lifetime
  visualization and individual optimization controls inform our compiled-plan
  inspector and serial control. We keep checked lowering/RHI validation active;
  an optimization-disabled run can change memory/timing and mask concurrency
  failures, so repeat the optimized path separately. We do not import its
  immediate-mode C++ setup API or infer shader variants from graph liveness.
- Thanks to Epic Games, [PSO Precaching](https://dev.epicgames.com/documentation/en-us/unreal-engine/pso-precaching-for-unreal-engine),
  Global Shader PSO Precaching: authored compile eligibility and narrower precache
  eligibility are distinct. Ludus reports selected required recipes versus the
  full legal build manifest and measures first-use misses. Runtime observation
  audits manifest completeness; it cannot manufacture undeclared shader choices.
- Thanks to the Slang authors, [Using the Reflection API](https://shader-slang.org/slang/user-guide/reflection.html)
  and [Link-time Specialization and Module Precompilation](https://shader-slang.org/slang/user-guide/link-time-specialization):
  type versus target/context layout motivates generated packing; module reuse is
  a later build optimization under the same manifest. Keep our own compiler,
  options and dependency compatibility checks for distributed precompiled SDK
  modules; a source-absent module's freshness result is insufficient. Qualify
  these APIs on pinned Slang before adoption; no toolchain update is implied.
- Thanks to the Khronos Group, [Vulkan Fragment Operations](https://docs.vulkan.org/spec/latest/chapters/fragops.html),
  shader termination, depth replacement and multisample coverage: depth-only
  techniques must preserve relevant fragment work. Separately verify each
  admitted backend; Vulkan stage omission does not establish portable support.

## Delivery, optimization gates and acceptance

| Stage | Deliverable | Required acceptance |
| --- | --- | --- |
| RG0: contract vertical slice | Saved/imported raster graph, explicit ColorAndDepth/DepthOnly choices, camera target routes and CLI validator | Same graph to offscreen/Present; recursion/type/initialization failures; no implicit variant on disconnected Color; serial plan dump |
| RG1: build integration | Shared schemas, existing shader-helper extension, packages and local incremental cache | Target layout/entry fixtures; deterministic clean builds; correct transitive invalidation; corrupt/cancelled/stale build preserves prior output; configuration and pipeline bounds |
| RG2: Renderer integration | Scene matching, coverage reports, ready pipeline admission and error route | Mixed layouts/material slots; missing/conflicting roles; alpha/deformation/depth parity; capability rejection; no compile during draw; streaming/resize/history invalidation |
| RG3: editor and reload | Graph/schema/source editing, preview, undo/save, diagnostics and atomic version activation | CLI/editor equivalent result; mapped errors; draft/save separation; failed reload old pixels preserved; pending pipelines; CPU schema mismatch/migration and unload/completion lifetime fixtures |
| RG3a (delivered) | Typed document/undo/save, source editing, mapped diagnostics, runtime generations and native activation | All RG3 acceptance fixtures above, without visual canvas or in-editor GPU preview |
| RG3b (delivered) | Node canvas and trusted in-editor preview of the activated generation | Preview/live parity on a frozen fixture; pending/failed preview keeps the last drawable view |
| RG4: measured scheduling/memory work | Optional pooling improvements, merging/aliasing/parallel paths individually | Identical serial-control pixels/effects and actual retained memory; named native/browser profiles; repeated resize/reload and frames in flight; unsupported paths explicit |
| RG4a (delivered) | Exact-descriptor transient pooling with completion proof and a serial control | The RG4 acceptance above for pooling alone; no heap aliasing, pass merging, parallel recording or async queues |

RG0 is delivered in `ludus_tools.render_graph` and the installed `ludus
render-graph` CLI. It saves/imports a reusable raster asset, selects explicit
ColorAndDepth/DepthOnly configurations, validates typed connections, defined
resource versions and active camera routes, and emits an inspectable deterministic
serial plan. The same asset connects to offscreen Export or Present contract
roots; no implicit shader configuration arises from an unused Color output.
All closure interfaces are checked; resource initialization and camera routes
require a selected closed root. The plan reports ShaderBuild, DeviceCapabilities,
SceneCoverage and GpuExecution pending. ShaderBuild is discharged by the bounded RG1 package build; the other checks are RG2 integration gates,
not checks that an offline contract validator can establish. RG2 now maps logical
depth to RHI private depth allocations and qualifies completion-gated depth exports.

RG1 is delivered in `ludus_tools.render_graph_build`, the installed CLI and the
existing SDK shader-helper boundary. It generates scalar/vector shared schemas,
read-only uniform bindings and entry wrappers, independently validates target
packing/stage links, cooks explicitly enumerated raster techniques and bounded
portable pipeline recipes, and publishes content-addressed generations through
one atomic pointer. Program caches check full identities and artifact integrity;
source/tool snapshots, cancellation and failed jobs preserve prior publication.
See [source format, admitted profile and acceptance](../development/render-graph-authoring.md#build-explicit-shader-packages-rg1).
This initial profile rejects unqualified matrix/array/resource/stage extensions;
shader packages do not establish device pipeline readiness or GPU execution.

RG2 is delivered in Renderer’s `RenderGraph`, the neutral `RenderGraphFormat`
records and the shared `render_graph_execution` lowering service. It admits one
bounded static opaque configuration, prewarms the existing Renderer contexts,
scans every intended item/material slot per camera/role before visibility culling,
and serially encodes Ready views. Reports distinguish covered, unsupported,
conflicting and explicitly excluded items. The editor policy can use existing
prewarmed magenta overlay geometry or explicit bounds proxies; these remain
unsupported and never establish depth coverage. The shipping policy rejects
incomplete coverage while preserving the prior drawable extraction.

The execution profile uses RGBA8, Depth32, one sample, conventional Less with
depth writes and the Renderer’s six-location static instance interface. Within a
material family, ColorAndDepth and DepthOnly share the exact geometry stage;
opaque depth fixtures qualify the authored void coverage function. Transparency,
deformation, arbitrary vertex layouts, reverse-Z, imported images and
history remain explicit admission failures. Extent-only resize uses the staged
resource/scene replacement described in the size workflow. Thus an exported depth graph is not
automatically a compatible pre-Z for an arbitrary subsequent shading pass. No
error shader or output-driven permutation is synthesized. The installed CMake
helper and CLI emit identical typed plans with static shader storage, without a
runtime JSON parser or shader compiler.

Techniques may read RG1 scalar/vector uniform bindings. Lowering merges each
technique's per-stage reflected layouts into neutral binding, per-target block
and per-target field records; a slot shared by both stages must have one schema
and layout. Items supply logical float values laid out by uniform schema, and the
generated header names each field's offset. Renderer packs them with the selected
backend's target layout, deduplicates equal value sets into immutable uniform
buffers and binding snapshots, and keeps equal sets instanced. Values are runtime
data: changing them republishes an extraction, never a shader configuration.
Per-view values, matrices, arrays and raster texture/storage bindings remain unqualified.
Native ray storage uses the separate graph-owned interface described below.

An explicitly authored `DefaultMasked` raster material selects
`DefaultStaticMasked` techniques. Its DepthOnly fragment is a separately authored
coverage function. Build, lowering and Renderer admission all reject the opaque
void depth function in that role, and masked color and depth must share their
family's geometry stage. Coverage is matched per family: masked techniques cover
only `Masked` items, opaque techniques only `Opaque` items, and an item is never
redirected to the other family. Depth parity fixtures compare the two techniques'
exported depth byte for byte and their color coverage pixel for pixel. Equal-depth
pre-Z followed by shading still needs an `Equal`/`LessEqual` recipe profile, which
is not admitted.

Scene/camera replacements publish only when their retained resources are Ready.
Candidate reports and the last drawable revision are distinct, including when a
newer rejection arrives while an older candidate is pending. A view-relative extent
change stages replacement attachments and a fresh extraction in the same graph,
without preparing shader/pipeline contexts again. Caller-reported physical surface
extent is checked before acquisition and actual extent is checked again afterward. History/import modes
are rejected instead of retaining stale resources. RG2 bounds three retained
scene candidates (current, pending, preparation), plus RHI retirement of actual
GPU uses. It conservatively completion-gates reuse of export allocations; export
consumers must complete before another render, resize publication or reset.

Current RHI private depth is physically paired with color: depth-only offscreen
allocates a compatibility RGBA8 image alongside depth (kept by RG4a, see above). Renderer contexts also
prewarm their existing finite surface/offscreen, overlay, reverse-Z and composite
states; package recipe counts describe authored recipes, not those additional
physical Renderer PSOs. Export consumers can be prewarmed against allocation/aspect without claiming
valid pixels. Encode contributes to the caller’s existing frame; that caller
confirms the actual submission after composing the consumer. Whole-frame Render
uses existing RHI surface acquisition/clear/presentation rules. Distinct logical
depth images cannot share one physical depth, and a plan has one surface owner.
RG2 itself claims no aliasing, pooling, async scheduling, reduced allocation or
performance improvement; RG4a's measured pooling results are recorded separately. See
[RG2 usage, qualified scope and evidence](../development/render-graph-authoring.md#execute-cooked-static-graphs-rg2).

RG3a is delivered in the shared `render_graph_authoring` service, `render-graph
cook-runtime`/`schema-diff`/`migrate`, the neutral `runtime_package.h` layout,
Renderer's `RenderGraphPackage`/`RenderGraphActivation` and the editor's Renderer
tab. CLI JSON failures map graph errors to their package-relative source,
configuration, node and expanded instance, and shader-compiler lines to authored
function files; generated-wrapper lines map to their owning manifest program.
The editor edits typed drafts by stable ID with bounded undo and conflict-checked
explicit save, and runs the same CLI backend on immutable draft snapshots, so a
draft and its saved equivalent produce identical plans and errors.

A `.lurgrt` runtime generation carries the RG2 numeric plan, cooked stages and a
CPU interface identity; decoding copies the bytes, so an unloadable module can
release its buffer immediately. Activation stages a candidate beside the drawing
generation, prewarms it, requires it to prepare the latest accepted extraction,
and publishes it only inside `Poll` between frames with no unconfirmed encoding.
The CPU interface identity covers the vertex/instance schema, the static inputs,
and the per-draw uniform schemas with every target layout that packs them, so a
changed uniform schema or layout returns RestartRequired.
A pending, failed or coverage-rejecting candidate leaves the active generation,
its pixels and output revision unchanged; a CPU interface mismatch returns
RestartRequired and stages nothing. The replaced generation is Reset at the
boundary and its GPU uses retire through RHI completion; GameHost quiescence
remains a separate obligation. Parameter migration keeps values only for equal
field identity and type, requires explicit defaults for added or changed fields
and reports dropped values; it never writes sources. See
[RG3 usage and evidence](../development/render-graph-authoring.md#edit-and-reload-graphs-rg3).
Version 4 extends each resource with a view scale numerator/denominator; zero
numerator preserves fixed extents. Versions 2 and 3 decode as fixed-size plans.
Version 3 appends a bounded UTF-8 name for each executable authored pass, shared
by native GPU annotations and Perfetto counters. Version 2 remains readable with
fallback labels. Names are debug metadata, not source expressions; they are
validated and copied without invoking a runtime graph parser. The byte layout
and limits are owned by `render_graph_format/runtime_package.h`.

RG3b is delivered in the editor's Render Graph document, with its node inspector in
the Properties tool window and its preview in the Render Preview tool window. The
node canvas is another view
of the same typed draft: drags commit one layout edit per gesture, links are
pre-checked with the validator's Port/MultipleWriters/TypeMismatch/Cycle codes
before Connect, and double-clicking a Subgraph opens its source. The trusted
preview owns its own RenderGraphActivation on the World viewport's existing
device, fed by that World extraction; it stages a cooked `.lurgrt` only on an
explicit request and replaces its image only with a complete copy of an active
generation's frame, so pending, failed and restart-requiring candidates and a
stopped device keep the last drawable view. The editor host is compiled against
the RendererStatic offscreen fixture's interface; preview/live parity compares
that frozen fixture's cooked generation with its compiled header byte for byte.
Presentation-surface roots stay in the game host. See
[RG3b usage and evidence](../development/render-graph-authoring.md#node-canvas-and-preview-rg3b).

### Pipeline-node authoring migration and qualification

Version-2 graph documents and the editor support all three pipeline record types,
custom port names/types, shader program references, state and invocation editing.
Empty drafts can be saved; execution validation requires complete admitted inputs
and shaders. Compute/ray dispatch, optional graphics stages, procedural draws,
additional resources and arbitrary state remain outside the current graph build
profile and fail explicitly. Existing version-1 documents stay readable without
rewriting on load. The explicit conversion maps each raster execution record and
its selected technique/recipe to a Graphics Pipeline node, preserving stable identities,
connections, shaders, state and configuration choices. Keep `RasterizeGraph` as
its reusable subgraph wrapper. Unsupported or ambiguous conversions report an
error and preserve the source; do not guess defaults or rewrite assets on load.

The shared authored record, CLI validation and editor commands precede further
execution qualification per pipeline type/profile. Acceptance covers empty
saved drafts, adding/removing/retyping ports with undo/save, shader/interface
mismatches, complete state round trips, existing raster asset migration and
unchanged pixels, and failed/pending candidate preservation. Raster fixtures
include procedural input-free draws, Scene/View bindings, color/depth outputs
and per-attachment blending as those execution extensions are admitted. Current
compute/ray fixtures validate bounded draft settings and explicit capability
rejection. Future execution fixtures must verify storage read/write versions,
dispatch bounds, acceleration-structure readiness, shader-group/payload
compatibility and launch/recursion limits. Backend rejection
must be explicit, including targets without optional geometry or ray support.
Existing RG0-RG4 delivery evidence does not qualify these new capabilities.

Run representative native and browser raster fixtures under pinned tools, SDK
consumer checks, warning/format/tidy gates and sanitizers for implementation.
Ray nodes require their own independently negotiated tests. Measure cold/warm
shader builds, invalidation fan-out, unique programs/PSOs, driver creation time,
first-use misses, CPU plan/encoding time, allocations, GPU time and transient/
retained bytes, including p50/p95 and workload/tool/device metadata. Compare
against current Renderer output before claiming improvement. These are acceptance
metrics, not unsupported claims that this proposal is already faster.

The initial design above precedes the targeted article review. Source-informed
refinements and their exact evidence are recorded in the existing
[renderer reference review](renderer-systems-gems-review.md); detailed contracts
remain here rather than duplicated in an editor guide or wiki mirror.

## Persistent native compilation boundary

Runtime shader iteration is supported above RHI through one headless native Slang
worker, shared by editor and native application clients. Build-time and live
requests use the same compiler core and portable packages. Graph/schema lowering
and independent layout validation remain in host tooling; neither CI cooking nor
compiler initialization starts an editor, game session or graphics device.

The compiler owns one thread-confined global session and one bounded immutable
source snapshot. A byte-identical snapshot may reuse its module/session across
target requests. Any source edit replaces the session, preventing Slang's module
cache from returning stale source. A closed in-memory filesystem admits only
captured dependencies. Wire/version/bounds and request identities are validated;
source/compiler errors publish no artifacts. Native compiler identity and library
digest participate in graph cache identity. This follows the Slang authors'
[compilation API session guidance](https://docs.shader-slang.org/en/latest/compilation-api.html),
with a deliberately bounded latest-snapshot cache rather than an unbounded set of
live sessions.

Source preparation, portable artifact publication, runtime-package delivery,
device pipeline preparation and frame-boundary activation remain separate owners.
The native development-game `ReloadSession` owns asynchronous file delivery and
supersession only; Renderer still owns readiness, scene coverage and activation,
and RHI still owns GPU lifetime retirement. The editor applies a background draft
only if its captured semantic source/configuration identity is current. Existing
CPU-interface mismatch continues to mean RestartRequired. Portable shader
compilation does not guarantee device pipeline readiness or remove driver work.
See [native worker setup, limits, workflow and measurements](../development/render-graph-authoring.md#native-shader-worker-and-live-development-builds).

## Ordered ray sequences and persistent storage

The editor's ReSTIR configuration importer owns one unpublished package snapshot:
canonical renderer library data supplies its passes/ports; prefixed program/source
records and typed settings merge into the shared manifest. Root insertion also
creates the library and a Subgraph reference. The document owns the single history
commit and subsequent Save transaction. Existing callbacks remain byte-identical;
legacy signatures use the builder's adapters and a custom payload retains fields
while gaining the launch state required by ReSTIR. Collisions/nonempty destinations
fail before publication. Source-tree and canvas menus call this same operation.
The whole-package creator instantiates that same canonical library, avoiding a
second maintained copy of the three-pass wiring. See the
[editor workflow and ports](../development/render-graph-authoring.md#use-the-built-in-restir-di-template).

Numeric plan version 6 and runtime package version 9 append a callback-set index
and two persistent buffer-resource indices to each pass. Buffer resources use the
existing resource table with an explicit Buffer aspect: Width is byte capacity,
Height is one, and export/surface flags are forbidden. Runtime package version 11
and numeric plan version 8 add BytesPerPixel to each resource. Zero preserves fixed
capacity; a positive aligned stride with a view policy resolves byte capacity from
the scaled reference pixel count. Resizing stages buffers beside attachments and ray
jobs and publishes them together only at readiness. Versions 9 and 10 remain readable
with fixed-capacity buffers. Serialized
widths derive from the field schema, with no host-padding copies. Earlier hardware
package version 6 remains readable with callback zero and absent buffer slots.
The persistent compute stage role independently selects the extended eight-binding
ABI; legacy compute/image artifacts retain their existing ABI.

The offline builder captures the node-owned callback sets and links a bounded
selector into one compute kernel. Engine interface/payload types and reusable
headers are global; unique callback sources occupy distinct namespaces. A job's
immutable parameters select its callback set without changing shader artifacts
or descriptor layouts per frame. The extended kernel writes current-frame
radiance and advances a uint32 counter. Color averaging and its stop limit require
an explicit [Accumulate node](../development/render-graph-authoring.md#accumulate-ray-samples-explicitly).
Display conversion likewise requires an explicit
[ToneMap node](../development/render-graph-authoring.md#tone-map-ray-output-explicitly).
The logical plan retains both operations and their color resource versions;
execution fuses them into the ray producer in linear-mean, exposure, Reinhard,
sRGB order before the RGBA8 write. Without ToneMap, the graph clamps linear RGB
at that write. In-place version ownership rejects readers of an intermediate
raw version alongside a transformed version. A node authored with `"enabled": false`
is [disabled](../development/render-graph-authoring.md#disable-a-node): source
loading forwards its input's source to its consumers before validation of routes
and lowering, so the plan and runtime package contain no trace of it and need no
version change. Only Accumulate, Denoise and ToneMap admit the field, because each has one
input and one output of the same type and extent. This bounded profile accepts ray
color only; it does not promise a general HDR raster post-processing pipeline.

Numeric plan version 10 and runtime package version 14 append the float32
ToneMappingExposure field after AccumulationSamples. Zero bypasses conversion;
a finite positive multiplier in the format owner's range selects Reinhard and
sRGB. The existing shader parameter exposure component carries that policy,
without a shader ABI expansion. Older packages preserve scene-ray display
conversion at unit exposure and ordinary-ray clamping. Version 13's explicit
accumulation field remains independently decoded; earlier versions keep their
historical accumulation defaults.

RenderGraph owns zero-initialized storage allocations alongside its attachment
ledger. Scene candidates borrow those allocations during preparation, which does
not encode writes. GPU command assembly belongs to the private compiled
`gpu_preview_graph` component: one import/version ledger spans the consecutive
sequence, and only final versions become export/history roots. RHI retains the
actual resources and derives whole-buffer hazards; Renderer does not duplicate
backend barriers or native lifetime state. Discarded/failed candidate preparation
preserves the current generation and buffers. Encoded shader effects are not a
transaction that can roll back individual pixels.

The [authoring guide](../development/render-graph-authoring.md#order-ray-passes-and-share-persistent-buffers)
owns capacity, reset, ping-pong, editor and scheduling instructions. This change
provides ordered storage access without qualifying arbitrary authored reservoir
algorithms. The renderer-owned [ReSTIR DI template](../development/render-graph-authoring.md#use-the-built-in-restir-di-template)
uses that storage contract and manifest-owned typed shader constants. Template
creation uses the shared transactional backend; Properties and canvas controls
edit the same draft manifest and require Save/rebuild before activation.
### Structured storage in the native ray profile

Native triangle-ray jobs admit two named structured buffers in user slots 6/7.
Authored `StructuredBuffer` nodes own stride and fixed/per-color-pixel count;
pipeline input names match manifest storage bindings. Paired Load/Store outputs
declare in-place read/write versions; input-only ports declare reads. Offline
lowering checks unique allocations, complete wiring, reflected names, access and
identical element strides across carried targets. Numeric StorageBuffer records
remain separate from image resources and from CPU-visible per-draw uniforms.
Runtime package version 8 carries these records after stages and before pass names;
versions 6 and 7 remain readable without storage. Storage changes do not change the host
vertex/uniform interface identity, but activation creates a fresh job and history.

The per-job storage owner validates bounded checked sizes and device ranges,
uploads complete zeroed bytes and retains distinct physical allocations until
release. Its buffers participate in compute binding snapshots, in-place versions
and History roots of each RHI graph. Total GPU allocation and peak zero-upload
scratch obey existing budgets; the RHI additionally enforces resource limits.
Storage is private to current/pending scene jobs, so failed or pending replacement
cannot mutate the current job. Replacement scene/camera/extent/generation starts
fresh storage; RHI retirement protects outstanding producer uses on release.
Shader code owns bounds and races, including ping-pong choice and zero-history
handling. This adds no global dispatch synchronization, buffer exports or arbitrary
compute/raster execution. See the [authoring workflow](../development/render-graph-authoring.md#create-structured-reservoir-buffers).

## Forward scene generations

ForwardLit adds a bounded material-aware raster execution profile using the existing
scene submission boundary and renderer-authored shader recipes. Offline lowering
carries the shared flat setup stages and six explicit forward/lighting support
roles. These renderer-owned support stages do not change the caller's vertex/uniform
interface identity, so compatible flat and forward templates share the existing editor
extraction boundary. Runtime package version 12 appends those roles without changing older IDs.
Forward stage records additionally carry verified GLSL combined texture/sampler
names after their uniform-block names. Package versions 2 through 11 retain their
previous decoding contracts; pixel-derived storage remains a version-11 feature.
The compiler validates resource interfaces before publication, and native shader
reflection/material prewarming independently checks the fixed ABI at preparation.

`ForwardSceneResources` privately owns the exact-content cache, while each
`ForwardSceneGeneration` owns its pins, material candidates and prepared draw
inputs. Current/pending graph scenes retain distinct generations. Forward passes
share the existing opaque renderer context, and camera-only updates reuse its
snapshots and resources. Changed revision domains stage replacements; unchanged
meshes/textures can be reacquired without upload. Domain promises never substitute
for exact-content identity when admitting a new asset. General raster scene buffers,
GPU light assignment, arbitrary material interfaces and cross-device caches remain
separate extensions. See the [canonical workflow](../development/render-graph-authoring.md#use-the-forward-lit-template)
for limits, shader editing and installed-SDK usage.
