# Save, validate and build render graphs

RG0 implements the offline graph contract in the installable `ludus` host tools.
It saves reusable graph assets, validates explicit configurations and camera
output routes, and prints a deterministic serial plan. The same Python backend
is available to future editor jobs; no Qt or engine checkout is required after
installation. See [host tools](host-tools.md) for installation and the
[architecture](../architecture/render-graph-authoring.md) for ownership and later
stages. RG1 adds explicit offline shader/schema builds and immutable packages,
using the same CLI/backend. RG2 adds bounded static opaque Renderer admission and
execution through cooked headers. RG3a adds the editor's Render Graph document, mapped
diagnostics, runtime generations and atomic activation; the node canvas and live
in-editor preview are RG3b. RG4a adds measured exact-descriptor pooling of
transient attachments with a serial control. To begin in the editor, see
[Start customizing the renderer](#start-customizing-the-renderer).

**RG0 plans are contract diagnostics, not executable renderer packages.** They
do not compile shaders, evaluate scene draw-item coverage, negotiate device
capabilities or render pixels. Those checks are reported as pending. RG1 builds shader packages; RG2 owns
Renderer/RHI integration. `DefaultOpaque`, `DefaultMasked` and `RendererStatic`
identify the qualified static profile described below; arbitrary mesh/material
compatibility is never inferred from those labels.

## Author pipeline nodes

The node type picker offers **GraphicsPipeline**, **ComputePipeline** and
**RayTracingPipeline** for GPU work. Scene, Camera, Target, Subgraph, Export and
Present remain supporting nodes. A new pipeline has no inputs, outputs or shader
assignments. It is an editable draft, not a ready rendering pass.

Select the node in Properties. **Read Inputs** declares resources and values the
pass consumes. **Writable Resources** declares paired input/output ports: each
row owns the input receiving the texture or buffer and the output exposing its
updated contents. Add one row instead of declaring those ports separately. New
pairs start with the same name; renaming the output also renames its input while
the names match. Existing graphs with different port names retain both names, shown in the
resource name tooltip. A missing or incompatible old target association is rebuilt
as a same-name pair on the next completed edit; opening the inspector does not rewrite sources.
The editor writes the serialized `target` association automatically; there is no
editable target field. Connect a compatible resource to the pair's input, then
connect its output to the next pass or configuration output. A writable input
needs a connection even when its Load operation is Clear or Discard. Paired ports
line up horizontally on the canvas. Changing the resource type changes both
port types and selects its corresponding format.

**Shaders**, **Pipeline State** and **Invocation** edit the rest of the node.
Completed edits enter the undoable draft directly in Properties or the canvas node editor.
There is no **Apply Pipeline** button. **Save** publishes the draft and updates Live Preview;
unsaved edits do not build shaders. Removing a
writable row removes both ports. Disconnect affected links before removing,
renaming or changing the type of a connected port. A rejected edit preserves
the draft and identifies the affected endpoint and repair. Duplicate or invalid
port names are explained beside the controls; graph validation names missing
connections and incompatible output/input pairs with concrete repair actions.
Undo restores the complete record and document version. Double-click the pipeline
to open a mapped shader source through its explicit program reference in the
build manifest.

Pipeline records use document `version: 2`; adding one upgrades the draft in the
same undo step. Version-1 graphs remain readable and are never rewritten on load.
Select a legacy Raster node and choose **Convert to Graphics Pipeline** to retain
its ID, ports, load/store operations, shaders and selected recipe. Conversion
requires exactly one explicit recipe for that technique; an ambiguous or missing
mapping preserves the draft. Convert each legacy node explicitly, including
nodes in referenced subgraphs. The CLI can print a converted document for review:

```bash
ludus render-graph migrate-pipelines graphs/Main/rasterize.lurender.json \
  --manifest graphs/Main/main.lushader.json
```

The command writes only stdout (`--json` returns a `document` object). It does
not replace files or convert imported assets automatically.

In **Properties → Shaders** or the canvas node editor, add a row, choose its stage
and set its shader ID. Completed edits automatically create a `.slang` template and registers a program in the root
`.lushader.json` for each new program name. The initial `Program` value
receives a unique name from the node and shader IDs. To share a shader, enter an existing
program name with a matching stage. New sources appear immediately in **Source**;
**Save** writes them beside the manifest. Creation, manifest registration and node
assignments form one undo step. Name/file conflicts or incompatible references
preserve the draft. Templates are starting points: vertex/fragment interfaces use
the manifest schemas; ray-generation, miss and closest-hit functions use the
renderer’s triangle ray interfaces. Other stages remain authoring drafts subject
to the execution limits below. Changing stage creates a new matching program and
retains the previous source for reuse. Removing a shader row retains its program and source for reuse.

The version-2 node fields are:

| Field | Contract |
| --- | --- |
| `inputs` | Up to eight `{id, type, required}` records; stable port IDs and explicit connection requirements |
| `outputs` | Up to eight `{id, type, target, format, load, store}` records; `target` names a same-type input carrying the previous resource version |
| `shaders` | Up to sixteen `{id, stage, program}` records; `program` references an explicitly declared build-manifest program, not a guessed source filename |
| `state` | Graphics topology/cull/front/depth comparison/depth writes/blending/samples; compute `group_size`; ray tracing recursion/payload/attribute bounds |
| `invocation` | Graphics mode, vertex layout, material family and Scene/View input names; compute dispatch or ray launch `dimensions` |

Input types include Scene, View, ColorTexture, DepthTexture, Buffer,
AccelerationStructure, Float32, Float2, Float3, Float4 and UInt32. Outputs are
resource versions (ColorTexture, DepthTexture or Buffer). Texture output formats
are RGBA8Unorm/Depth32Float; Buffer uses Bytes. These declarations do not establish
execution support or create a buffer/acceleration-structure provider. Outputs
still need compatible resources connected through their target inputs.

**Execution remains qualified by the current graph profile.** A complete graphics
node supports custom Scene/View and attachment port names while lowering to the
existing RendererStatic ColorAndDepth/DepthOnly implementation. The build checks
its exact vertex/fragment assignments and complete state against the authored
technique/recipe; settings are never silently ignored. Each admitted output's
format/sample count must match its connected target. Additional resources,
optional geometry/tessellation stages and unsupported state produce an explicit
capability error. Compute settings can be authored and saved, but compute-node execution remains
outside this graph profile. Triangle ray pipelines execute through the
[authored ray profile](#execute-a-triangle-ray-pipeline) below. Device capabilities
and the complete authored interface are checked before activation. Missing required shader assignments
produce IncompletePipeline; an invalid/pending candidate leaves the current
viewport generation intact. Blending/rasterization remain fields inside the
node; separate state blocks and stream output are deferred.

## Execute a triangle ray pipeline

A `RayTracingPipeline` can run authored ray-generation, miss and closest-hit
functions over the selected World's static opaque triangles. The renderer freezes
an immutable World extraction, builds its BVH during scene preparation, and uploads
geometry and camera inputs. GPU compute performs traversal and shading; a
renderer-owned image pass transfers GPU-produced colors to the graph's attachment.
There is no CPU tracing or pixel readback in this path. The editor's World/camera
updates create replacement inputs, retaining the last drawable view until they
are ready, just as raster graph updates do.

This is a portable GPU triangle profile, including Metal on macOS. It does not
request dedicated Vulkan/DXR ray stages or hardware acceleration structures, and
makes no claim of hardware ray-traversal acceleration. WebGPU compute artifacts
can be carried by the same package. WebGL2/GLSL ES is unsupported. An unavailable
compute/storage capability rejects activation while preserving the prior view.

Author three manifest programs with these built-in function interfaces:

| Stage | Manifest input / output | Authored function |
| --- | --- | --- |
| `ray_generation` | `RayLaunch` / `Ray` | `Ray GenerateRay(RayLaunch launch)` |
| `miss` | `Ray` / `Color` | `float3 ShadeMiss(Ray ray)` |
| `closest_hit` | `RayHit` / `Color` | `float3 ShadeClosest(RayHit hit)` |

Each program still declares `id`, `stage`, package-relative `source`, `function`,
`input`, `output` and an empty `bindings` array. The builder supplies these input
types. `Ray` contains `OriginWorld`, `MinDistanceWorld`, `DirectionWorld` and
`MaxDistanceWorld`, in world meters; directions must be finite unit vectors and
distances finite, nonnegative and ordered. `RayLaunch` supplies `PositionPixel`,
`ExtentPixel` and `CameraRay`, a center ray clipped to the current camera's near/far
planes. Return that ray or author another one. `RayHit` supplies `IncomingRay`,
`PositionWorld`, `DistanceWorld`, geometric `NormalWorld`, packed world triangle
`Primitive`, vertex-one/two `Barycentric` weights and `Color` (RGB material tint;
its W component is reserved). Hit/miss functions return linear RGB, clamped into
RGBA8 with opaque alpha. Invalid/nonfinite/negative colors or invalid rays show
magenta error pixels. This profile performs exactly one trace per pixel; it adds
no lights or automatic transport model.

Declare the linked technique alongside the manifest's existing static opaque
setup technique:

```json
{
  "id": "RendererTriangles.RayTracing",
  "configuration": "RayTracing",
  "ray_generation": "Generate",
  "miss": "Miss",
  "closest_hit": "Closest",
  "recipes": [
    {"id": "Triangles", "max_recursion": 1, "payload_bytes": 0, "attribute_bytes": 8}
  ]
}
```

Select those exact program IDs in the node's Shaders table. Its state must match
the recipe above: the eight attribute bytes represent triangle barycentrics.
Declare exactly one input each of type Scene, View and ColorTexture, and one
RGBA8Unorm color output naming that color input as its `target`, with `Clear` and
`Store`. In the editor, add the color through **Writable Resources**; its paired
input and target association are automatic. Port names are authored, not fixed. Set invocation `dimensions` to the
color target's exact `[width, height, 1]`. Feed Scene and Camera into those inputs,
the target into both Camera's designated target and the ray node, and the ray
color output into Export. Use a single-sample offscreen target. Raster and ray
versions of a renderer can be separate configurations; do not branch physical
writes from the same attachment version.

The profile admits one ray invocation per configuration and the existing graph
mesh/item limits. It rejects payloads, recursive tracing, additional hit groups,
any-hit/intersection/callable stages, extra resources, masked/transparent or
deformed geometry, reverse-Z and infinite-far cameras. Color is the only produced
aspect; no ray depth output is inferred. Ray preparation has renderer-owned
256 MiB CPU and 256 MiB GPU limits across retained jobs; device storage limits can
be tighter. The existing static opaque setup technique remains in the manifest
for shared mesh/snapshot ownership and is never used to rasterize ray scene
geometry. A complete source example is the
[SDK ray fixture](../../tests/sdk_consumer/rg2/raytracing/main.lurender.json) and its
[manifest](../../tests/sdk_consumer/rg2/raytracing/main.lushader.json.in).

Build, cook and activate it through the same graph commands as a raster package.
SDK callers using `RenderGraph::Render` need no separate ray job. Callers composing
an acquired frame call `PrepareEncoding()` between frames before `Encode()`, then
`ConfirmSubmission()` with their actual `EndFrame` token. Preparation and failed
candidates preserve the last drawable scene; an exported color is valid only
after its tracked producer completes. Shader changes continue through immutable
packages and atomic graph activation.

## Start customizing the renderer

You don't need to create a render graph first. The engine ships a default one, and
the editor uses it for any project that has no graph of its own.

1. **Look at the default.** Open a project and open the **Render Graph** document
   from the **Window** menu (or a render graph in Project Explorer). If the project has
   no `graphs/Main/main.lurender.json`, the document shows the **Engine default
   renderer**. This is the static opaque `RendererStatic` graph: Scene and Camera feed
   `RasterizeGraph` into 640×360 color and depth targets, and the color target is
   exported. Its sources live in
   [`modules/graphics/renderer/render_graphs/default`](../../modules/graphics/renderer/render_graphs/default/main.lurender.json).
   The SDK installs a read-only copy to `share/Ludus/render-graphs/default`, and
   the editor embeds the same copy. Showing the default writes nothing to the
   project.
2. **Open a World.** Its viewport renders through the project's graph automatically. When
   there is no project graph, it uses the engine default generation cooked with the editor;
   opening the project writes nothing. The **Render Preview** panel mirrors the same graph
   output and is optional. Camera navigation and scene edits feed that graph directly.
3. **Edit it.** Change anything: move a node on the canvas, edit a selected node's
   field in **Properties**, or apply shader text in the **Source** view. The first edit turns the
   default into a project draft:
   - The path label changes to `graphs/Main/main.lurender.json (new, unsaved)`, and the
     status line names the folder that Save will create.
   - Undo works as usual. Undoing every edit returns to the unedited default.
   - **Save** (Ctrl+S or Command-S) creates the whole package under `graphs/Main/`,
     beside graphs made with **New Render Graph...**, and reports where it went. After that,
     the project always opens its own graph.

   To copy the default without editing it, right-click the source tree and choose
   **Customize in Project**. Neither path ever replaces a file. If `graphs/Main/`
   already holds any of the package's files, nothing is written; open that graph with
   **File → Open Render Graph...** instead. A linked `graphs` folder is refused, as for
   New Render Graph.
4. **Iterate.** Pipeline inspector edits go directly into the undoable draft; there is no
   **Apply Pipeline** step. Finish a field with Enter, Tab or focus loss; dropdowns,
   checkboxes and row additions/removals update the draft immediately. **Save** (Ctrl+S or
   Command-S) finishes the active inspector field, writes the draft, then schedules a
   background preview update when **Live Preview** is enabled. Unsaved edits, Undo and Redo
   do not compile shaders. Layout-only saves do not rebuild. Several saves during a build
   coalesce into the newest saved version; later unsaved edits stay out of that snapshot.
   Saved external shader edits are watched too, including editors that replace files on save.
   Validation or compilation failure keeps the last working World preview and reports the
   failing source in the Error List; invalid drafts can still be saved and corrected.
   No worker socket or shader paths need to be entered when the project's SDK is configured.
   **Build & Preview** explicitly builds the current draft without saving; **Live Preview**
   pauses/resumes automatic builds of saved sources. Changing the preview configuration
   selects that configuration of the saved sources.

**File → New Render Graph...** creates an editable barebone graph and its shaders in
`graphs/<name>/`, then makes it active in the World viewport. **Customize in Project**
copies the engine default into `graphs/Main/`. At project opening, `graphs/Main/main.lurender.json`
has priority; otherwise the first `graphs/<name>/main.lurender.json` in name order is used.
With no such graph, the engine default is used. A malformed project graph reports an error
rather than silently substituting the default. Opening a library in an existing package keeps
its `main.lurender.json` as the execution root while editing the library.

The viewport's output selector lists **Final output** and every attachment of the active
runtime generation, including intermediate colour and depth targets. It does not require
an Export node for each diagnostic target. Selection follows expanded resource names across
rebuilds; a removed target returns to Final output. Discarded/undefined attachments report
that their contents cannot be shown. Depth is displayed with an automatic value range.
The separate texture inspector has an independent selection.

The adjacent configuration selector lists configurations authored in the root graph. Use
those for custom debug permutations or open another project graph. The editor does not
invent graph passes or infer a normal/UV buffer from a target's name. A normal/UV debug
program must receive those attributes through an admitted runtime interface; the current
`RendererStatic` interface carries position, transform and tint only. Authored normal/UV,
textured material and lighting interfaces remain outside that profile and are diagnosed,
not replaced by the old editor shading modes. The engine default is a static opaque tint
renderer, not the former Lit material preview.

Limits:

- This World integration is native-only. Browser authoring retains its existing viewport;
  it has no local shader compiler or native runtime-graph preview.
- The current runtime profile and CPU interface identity restrictions below still apply.
  The default uses 640×360 targets. Target extents come from the graph; resizing the World
  widget scales that output and preserves its aspect ratio, rather than rewriting project data.
- Graph output uses the existing asynchronous GPU readback path. Navigation remains live,
  but this is not the Qt shared-texture presentation path; no zero-copy performance is claimed.
- Opening or saving an unedited default writes nothing. Project loading never bootstraps
  tools or repairs an SDK. Missing tools are build diagnostics; use project setup to repair them.

## Create a saved graph and import it

In the native editor, open your game project and choose **File → New Render Graph...**.
(Without any graph, the Render Graph document already shows the
[engine default](#start-customizing-the-renderer).) Enter a graph name and
choose **Create**. The editor creates `graphs/<name>/` inside the selected game
project, including `main.lurender.json`, the reusable `rasterize.lurender.json`,
`main.lushader.json` and editable `main_shaders/*.slang` functions, then opens the
graph on the canvas. The starter exports a color image and uses the
RendererStatic interface required by the editor preview. Target profiles are
Metal/WGSL on macOS and SPIR-V/WGSL on Linux. Creation needs no shader compiler or
running viewport and never overwrites an existing graph folder. A failed creation
keeps the previously open draft. Graphs and shader sources belong to the game
project; engine/SDK files are read-only inputs.

Next, open a World viewport and choose **Build & Preview**. The result appears in the
**Render Preview** tool window. The editor detects the
selected project's SDK shader helper and prepared shader compiler automatically;
SPIR-V/GLSL dependencies are detected only when the graph declares those targets. See
[editing and building](#edit-and-reload-graphs-rg3) and the
[preview's supported rendering profile](#preview-a-cooked-generation).
Creation and building do not change the game's selected runtime renderer.

From a checkout, use `./scripts/ludus`; after installation use `ludus`:

```bash
./scripts/ludus render-graph new out/my-render-graphs
./scripts/ludus render-graph validate out/my-render-graphs/main.lurender.json
./scripts/ludus render-graph dump out/my-render-graphs/main.lurender.json
./scripts/ludus --json render-graph dump out/my-render-graphs/main.lurender.json
```

`new` creates a new directory containing `rasterize.lurender.json` and
`main.lurender.json`. Existing directories/files/symlinks are never replaced.
Sources are staged and validated before publication; failure cleans the owned
staging directory. This creates graph sources, not a game project, SDK, build
tree or shader package. No command downloads tools or starts the renderer.

The root has Scene, Camera, color/depth Target, imported Subgraph and Present
nodes. The camera's target input names its designated presentation texture;
its view and the scene feed the imported graph. The raster result, rather than
the unwritten target, feeds Present. Its import is an ordinary editable source
record:

```json
{
  "id": "raster",
  "kind": "Subgraph",
  "asset": "rasterize.lurender.json",
  "configuration": "ColorAndDepth"
}
```

The asset path is relative to the graph package root, not to the importing file
or current working directory. By default the package root is the root graph's
directory; `--root path/to/package` supports root documents in a subdirectory.
Imports cannot use absolute paths, `..`, URLs or symlinks escaping that root.
Each file owns a distinct asset ID. Repeated imports of the same file are
separate instances with shared asset/configuration identity and scoped node IDs.

`validate` and `dump` only read files. They validate the entire declared import
closure's interfaces and reject bad unselected configurations too. Initialization
and active camera routing are evaluated for the selected root configuration.
Source bytes are rechecked before a result is returned; a changed source rejects
the candidate. A caller can retain its previously validated plan unchanged.

## Offscreen and depth-only roots

```bash
./scripts/ludus render-graph new out/offscreen-graphs --output offscreen
./scripts/ludus render-graph dump out/offscreen-graphs/main.lurender.json
./scripts/ludus render-graph new out/depth-graphs --output depth
./scripts/ludus render-graph dump out/depth-graphs/main.lurender.json
```

All three creation modes save the identical `RasterizeGraph` source asset. The
presentation and offscreen-color roots import its `ColorAndDepth` configuration;
only their targets and terminal operations differ. The depth root explicitly
imports `DepthOnly` and exports a depth texture, with no color port/attachment.

Consuming only Depth from a `ColorAndDepth` import is legal: Color's consumer is
optional, but the authored color work/attachment remain in the plan. It never
selects or creates `DepthOnly`. Authors must expose another configuration and
users must select it explicitly. Changing a selected configuration does not
repair incompatible connections automatically. Unknown configurations or
connections to unavailable ports fail validation.

Inspect a reusable library without pretending its unbound inputs are a closed
execution graph:

```bash
./scripts/ludus render-graph validate out/my-render-graphs/rasterize.lurender.json --library
```

This returns `InterfacesOnly`, listing all declared configurations and ports.
It does not prove concrete resource initialization or camera routes. For a closed
root with additional authored configurations, use `--configuration Name` on
`validate` or `dump`; omission chooses the asset's declared default. Imports
always contain an explicit configuration name. There is no connection-driven
automatic selection in RG0.

## Version 1 source format

Sources are bounded UTF-8 JSON. Duplicate fields, nonfinite constants, unknown
fields/versions, duplicate identities and unsupported node kinds are errors.
Text editors modify the saved source directly; future editor UI will use this
same document representation. Generated plans are not a second authored source.

| Record | Required fields and contract |
| --- | --- |
| Asset | `version: 1`, `id`, `default`, `configurations`; default names a declared entry |
| Configuration | `id`, `inputs`, `outputs`, `nodes`, `edges`; optional `layout` |
| Interface input | `id`, `type`; each required input is bound by the containing Subgraph node |
| Interface output | `id`, `type`, `source`, `required`; source is `node.port`; required means the imported output must have a consumer |
| Connection | `from`, `to`, each `node.port`; `input.port` denotes a configuration input and cannot be a destination |
| Layout | `positions`, containing unique `{node, x, y}` records for existing configuration nodes; coordinates are bounded integers |

IDs are 1..63 ASCII characters: first a letter, then letters, digits, `_` or `-`.
`input` is reserved as a node ID. Node/input/output IDs are unique within their
respective configuration tables; configuration IDs are unique within an asset.
Texture outputs of a root configuration are explicit Export roots. Other library
outputs become roots only when a containing graph consumes or exports them.
Root execution configurations cannot contain unbound interface inputs.

| Node kind | Additional fields | Inputs / outputs |
| --- | --- | --- |
| Scene | None | Outputs `scene: Scene`; declares a future scene-snapshot binding |
| Camera | `active`, `role` (`Presentation` or `Offscreen`), `target_type` | Input `target` of the specified texture type; output `view: View`. An inactive camera may leave target unbound and needs no route, but cannot drive a raster pass |
| Target | `type`, `format`, `extent: [width, height]`, `samples`, `imported`, `initialized`, `presentation` | Outputs `texture` of the specified texture type; names physical resource version 0 |
| Raster | `mode`, `vertex_layout: RendererStatic`, `material` (`DefaultOpaque` or `DefaultMasked`), `depth_load`, `depth_store`; also `color_load`, `color_store` for ColorAndDepth | Inputs Scene/View/DepthTexture, plus ColorTexture for ColorAndDepth; outputs written depth/color versions |
| Subgraph | `asset`, `configuration` | Selected asset interface defines its input/output ports |
| Export | `type` | Input `texture` matching ColorTexture or DepthTexture; external output root |
| Present | None | Input `color: ColorTexture`; root for the written version of an imported presentation target |

The Raster input names are `scene`, `view`, `depth`, and (when available) `color`.
Outputs are `depth` and `color`. Types are `Scene`, `View`, `ColorTexture` and
`DepthTexture`; interface outputs and Export nodes accept only texture types.
Color targets use `RGBA8Unorm`; depth targets use `Depth32Float`. The initial
profile admits one sample and width/height each in 1..16384 pixels. These bounds
describe RG0's contract profile, not a hardware-capability guarantee. Unsupported
formats/samples/layouts/material families reject rather than invent variants.

All Boolean fields require JSON `true`/`false`; Boolean values are not integers.
Only imported targets may declare initialized previous contents. A presentation
target must be an imported color texture. Active presentation cameras require a
color target flagged for presentation; offscreen cameras require a target without
that flag. A valid active route must carry the camera's rendered result to Present
or Export on its designated physical resource. Presenting an already initialized
target without rendering the camera does not satisfy that requirement.

## Initialization, versions and order

Raster attachment load is `Clear`, `Load` or `Discard`; store is `Store` or
`Discard`. Clear initializes the complete attachment; Load requires defined prior
contents. Arbitrary scene geometry after Discard does not establish complete
initialization. Store preserves defined contents; Discard invalidates them even
after a clear. Undefined outputs cannot be exported/presented. Color/depth extents
and sample counts must match in a ColorAndDepth pass.

Writes produce a new logical version of the same physical target. Two passes
cannot independently write from the same preceding version: connect the first
pass's output to the second pass's input. Exports/Present must consume the final
physical version. A texture connection does not copy previous contents to another
physical allocation. Copies, history, resolve, stencil and sampled-texture/compute
nodes are outside RG0's profile and are rejected as unknown kinds/fields.

Dependencies determine order; the earliest ready node in the authored node array
breaks ties. Imported nodes expand at that position through their own deterministic
order. Within-frame cycles and recursive imports are errors. The dump preserves
nested instance paths, source asset/configuration, scene/view identity, technique,
attachment versions and initialization, dependencies and observable roots.

Two cameras contributing to one target need explicit ordering and preservation.
A later full Clear removes the earlier camera's contribution and its final route
fails. An explicitly authored Load technique receiving the previous version can
preserve both. Viewport rectangles/compositing remain later Renderer integration;
RG0 never infers merging or blending from two output connections.

Repeated instances reuse the same authored technique identity; the plan reports
the selected unique technique IDs. This is not a shader binary or device pipeline
count. Moving nodes through `layout` changes source hashes but not the semantic
digest or serial plan. Plans and source labels are deterministic after relocation
under the same package-relative paths.

## Bounds, errors and verification

Each asset is at most 128 KiB; a closure contains at most 32 files, 2 MiB of
source bytes and eight levels including the root. Each asset has at most eight
configurations; each configuration admits eight input/output ports, 128 nodes
and 512 edges. Expansion admits at most 1024 nodes and 256 serial operations.
The validator rejects capacity excess before publishing a plan.

CLI success exits 0; contract/source failures exit 1. `--json` returns schema 1
with `plan`, `library` or `created`; failure returns `error.code` and
`error.message`. Stable graph error codes have prefix `RenderGraph` and suffix
`Schema`, `Version`, `Capacity`, `Source`, `SourceChanged`, `Configuration`,
`DuplicateIdentity`, `Port`, `TypeMismatch`, `UnboundInput`, `UnboundOutput`,
`UnusedRequiredOutput`, `Recursion`, `Cycle`, `Initialization`,
`AttachmentMismatch`, `MultipleWriters`, `StaleVersion`, `CameraTarget` or
`CameraRouteMissing`. Messages identify the source/configuration or nested node
path; diagnostics are capped at 4 KiB of UTF-8. Directory-creation conflicts use
the existing host-tooling errors.

Run the source and CLI regressions with:

```bash
PYTHONPATH=scripts/python python3 -m unittest test_render_graph test_ludus_cli -v
```

The project-SDK CI job also installs the wheel and exercises saved/imported
presentation and depth roots outside the checkout. The macOS host-tooling suite
runs the graph regressions. RG0 assertions cover contracts and provenance; GPU
pixel, shader compilation and scene-coverage evidence must come from their later
stages and cannot be inferred from this validator's success.


## Build explicit shader packages (RG1)

RG1 delivers the bounded scalar/vector uniform raster profile of the
[build contract](../architecture/render-graph-authoring.md#deterministic-shader-and-graph-build).
It generates shared CPU/schema/shader declarations, resource bindings and entry
wrappers; calls the existing SDK shader helper; verifies target layouts and stage
links; and publishes all authored techniques/profiles/recipes as one immutable
package. It does not create device pipelines or execute the diagnostic graph.

Start with an RG0 closed root and create its sibling build source:

```bash
ludus render-graph new out/raster-sources
ludus render-graph new-build out/raster-sources/main.lurender.json --profiles metal wgsl
ludus --json render-graph build out/raster-sources/main.lushader.json out/raster-build \
  --helper /path/to/sdk/lib/cmake/Ludus/shaders \
  --compiler /path/to/pinned/slangc
```

Use the SDK built from this revision or later: its `shaders` directory includes
`compile_graph_stage.py`. The helper remains the owner of target compilation;
the editor/CLI share the build service, rather than introducing another compiler.
The host tools wheel owns the editable starter graph/functions. The source
checkout's `cmake/shaders` directory by itself lacks installed toolchain locks;
use an installed SDK. No command downloads tools, repairs game projects or
changes engine capabilities. See [shader toolchain setup](../decisions/0010-shader-toolchain-feasibility.md).

For the Linux Vulkan/browser chain, explicitly admit its three targets:

```bash
ludus render-graph new out/browser-raster
ludus render-graph new-build out/browser-raster/main.lurender.json --profiles spirv wgsl glsl
ludus render-graph build out/browser-raster/main.lushader.json out/browser-build \
  --helper /path/to/sdk/lib/cmake/Ludus/shaders \
  --compiler /path/to/pinned/slangc \
  --validator /path/to/pinned/spirv-val \
  --spirv-cross /path/to/verified/spirv-cross
```

Targets are `metal`, `spirv`, `wgsl` and `glsl` (ES 3.00). Slang must match the
SDK pin, SPIR-V uses the pinned validator, and GLSL additionally verifies the
SPIRV-Cross source/build identity and binary digest. Metal and WGSL can build on
macOS without a Linux SPIR-V validator. The initial build profile has vertex and
fragment functions, float scalar/vector fields, read-only uniform bindings in
set/group zero, and one color plus depth or depth alone. Matrices, arrays,
texture/storage bindings, compute/ray stages, custom fragment-depth outputs,
MSAA and broader attachment profiles reject; extend them with independently
qualified schema/target fixtures. Transform rows can use explicit `Float4` fields.
The illustrative starter vertex takes clip-space positions; it does not wire an
engine scene or implement scene-material/deformation/coverage parity.

### Source ownership and finite choices

The closed version-1 `.lushader.json` source has these fields:

| Field | Contract |
| --- | --- |
| `version`, `id` | Version 1 and an ASCII identifier |
| `profiles` | Nonempty, unique list of admitted targets; all are built |
| `schemas` | At most 16 named `Uniform`, `Vertex` or `Varying` schemas; at most 16 fields each |
| `programs` | Named stage/function/source records; input/output schemas and explicit uniform bindings |
| `techniques` | Explicit `DefaultStaticOpaque.ColorAndDepth`/`.DepthOnly` and/or `DefaultStaticMasked.ColorAndDepth`/`.DepthOnly` mappings and legal pipeline recipes |
| `budgets` | Author bounds for configurations, programs, emitted artifacts and pipeline recipes |

Identifiers use 1–63 ASCII letters/digits/underscores and start with a letter;
reserved C++/Slang/wrapper names reject. A field has `id` and `type`
(`Float32`, `Float2`, `Float3`, `Float4`). Vertex/varying fields also declare
`semantic` and `location` in 0–15; ordinary varying locations are unique. Vertex inputs use Float2/3/4.
Vertex output has exactly one Float4 `SV_Position`; other supported semantics
are POSITION, NORMAL, TEXCOORD0–9 and COLOR0–9. System position is not a user
varying link. Field names are stable schema identities in this initial format;
a rename is a schema change, rather than an implicit migration.

A program declares `id`, `stage`, `source`, `function`, `input`, `output` and
`bindings`. Vertex input/output name Vertex/Varying schemas; fragment input names
a Varying schema and output is `Color` or `Depth`. Color functions return float4;
Depth functions return void and may implement authored coverage/discard. Entry
wrappers add stage and target semantics. Uniform bindings declare `id`, `schema`
and a unique `slot` in 0–7. Shader authors write functions using those generated
values, with no manually duplicated structs or resource register declarations:

```slang
float4 PSMain(RasterVarying value)
{
    return float4(material.tint * material.gain, 1);
}
```

Each technique declares `id`, `configuration`, `vertex`, `fragment` and nonempty
`recipes`. A graph raster with `material: DefaultMasked` selects the
`DefaultStaticMasked` technique of its mode. When both depth-only techniques are
declared, `DefaultStaticMasked.DepthOnly` must name its own authored coverage
program rather than the opaque void depth program. The starter manifest shows the entire source format. Every program
belongs to a technique; unknown, implicit or wrong-stage selections fail.
All root graph configurations and their import interfaces are validated, and
selected graph techniques must appear in the build manifest. Both authored
techniques are cooked even if the current root only selects one. Unused Color
never manufactures a depth-only program.

A recipe declares `id`, `color`, `depth`, `samples`, `topology`, `cull`, `front`,
`depth_compare`, `depth_write` and `blend`. The profile admits RGBA8Unorm or None,
Depth32Float, one sample, TriangleList, None/Back/Front culling,
Clockwise/CounterClockwise front face, Less/LessEqual/Equal/Always comparison,
explicit Boolean depth writes and Opaque blend. This describes portable prewarm
requests; device capability and Ready status remain pending. Load/store belongs
to the graph plan, rather than the shader program. Recipe keys compare complete
canonical state, stage identities, vertex schema and target binding layouts;
recipe/node names and camera/extent values do not create extra shader programs.

Budget ceilings are 8 configurations, 32 programs, 128 target artifacts and
64 target pipeline recipes. The author may choose smaller bounds. Root and
import-closure graph configurations also fit the configuration bound, counted
separately from technique configurations. Reports distinguish declared programs,
unique stage identities, target artifacts and unique recipe keys. Deduplication
never permits the authored count to exceed its bound or silently drops a target.

Shader sources and literal quoted includes must remain inside the package root.
Includes are relative to their shader file; graph imports are relative to the
package root. Module imports, dynamic/continued includes, source remapping and
recursive includes reject. Conditional includes are conservatively tracked.
The compiler reads staged captured sources only, and its dependency file is
checked against that closure. Sources are bounded to 64 files/4 MiB, each at
most 128 KiB, with include depth 16; generated artifacts are at most 2 MiB each
and a generation is at most 64 MiB/1024 files. Use `--root` for a containing
package when the sibling root/manifest lives in a subdirectory; program source
paths are relative to that package root, not the manifest directory.

### Generated layouts and publication

`current.json` names a content-addressed directory under `generations/`.
`build-report.json` contains candidate job timing and cache hit/miss counts outside
the immutable generation. Only `current.json` determines activation; a failure
after reporting can leave an unactivated candidate report. Inspect the report's package path:

```bash
ludus --json render-graph verify-package /path/to/raster-build/generations/GENERATION_DIGEST
```

| File/directory | Purpose |
| --- | --- |
| `package.json` | Plans, explicit selections, counts/bounds, program descriptors, complete recipe keys, routes, pending runtime checks and provenance |
| `schemas.json`, `schemas.h` | Logical schema source and generated C++ values/per-program/target uniform packers |
| `programs/KEY/` | Target artifact, raw reflection, verified stage descriptor and generated wrapper |
| `sources/` | Captured graph/build/function/include sources for source-directed diagnostics |
| `integrity.json` | Complete bounded file inventory and SHA-256 identities |

Generated C++ uses Ludus primitive aliases and has no owning runtime containers,
allocation or exceptions. Include `schemas.h`, fill logical values, and call the
appropriate `ludus::render_graph_generated::Pack_PROGRAM_TARGET_BINDING` with
caller-owned native-endian upload bytes. Null/short output returns false and
preserves output; input/output must not overlap. Padding is zeroed, and bytes
beyond the target's occupied size remain untouched. The packer is usable on any
thread when callers provide independent storage; device uploads retain their
own RHI ownership/thread rules. A native C++ struct is never reinterpreted as the
shader buffer: `Float3` followed by `Float32` places the scalar at 16 in Metal
and at 12 in the qualified SPIR-V/WGSL/GLSL profile. Target reflection and emitted
Metal/WGSL/GLSL declarations are checked independently, including field identities
and offsets; inter-stage consumers must match a typed subset of vertex outputs.

Stage cache identities include captured transitive sources, generated wrappers,
used schemas, target/profile and the actual helper/generator/tool/library/lock
identities. Graph layout, camera count, extent and recipe state do not enter stage
identity. Pipeline state changes rebuild packages/recipes and reuse stages;
a changed included function rebuilds only its affected stage/targets. Cached
entries compare full identity and every artifact digest. Corrupt/missing entries
rebuild; a corrupt cooked generation is rejected, never reported ready. Metal
source debug paths are remapped to package-relative owners for clean-build
reproducibility, and saved wrappers/source snapshots retain provenance.

One nonblocking cooperative writer owns a build tree; a second CLI/editor job
reports Busy. Different trees do not share writable compiler state. Cancellation
is supported through the shared API callback and CLI interruption; compiler
process groups are terminated/reaped and private staging is removed. Source/tool
snapshots are rechecked before publication. Compiler/reflection/budget failures,
cancellation, stale sources and failed publication preserve the previous
`current.json`. Cache entries completed by a failed job may be reused later;
they cannot activate a partial package. A completed generation is published
before the pointer is atomically replaced, and old generations remain available.
This guarantees process-level publication, not power-loss durability across all
filesystems; raw edits to cache/generation/build trees are unsupported.

### Acceptance evidence

`test_render_graph_build` covers deterministic clean/warm builds, transitive
invalidation, independent target packing, budgets, strict source boundaries,
corrupt artifacts/descriptors, cancellation/staleness/compiler failures, Busy,
CLI/backend equivalence and unchanged prior publication. Pinned Slang acceptance
can be enabled with `LUDUS_RG1_SLANG`; optional `LUDUS_RG1_CXX` compiles/runs the
generated header. `tests/sdk_consumer/verify_render_graph_build.py` exercises the
installed wheel/SDK helper outside the checkout, clean/warm builds, real include
invalidation, compiler failure, relocated package verification and generated CPU
packing under ASan/UBSan. `render-graph-browser-test.mjs` validates emitted WGSL
and GLSL stages on pinned Chromium's software adapter; this is shader admission,
not a rendering or physical-GPU performance claim.

Local macOS evidence covers pinned Slang Metal/WGSL, pinned Chromium WGSL
admission and Clang 18 generated packing. On macOS 26.6 the local sanitizer
packing executable and an empty Clang 18 sanitizer control both stall at startup;
local ASan/UBSan acceptance is unavailable. CI runs the packing fixture with
sanitizers on its supported macOS/Linux hosts.
CI includes installed macOS Metal/WGSL and Linux SPIR-V/WGSL/GLSL acceptance.
Linux SPIR-V/GLSL evidence must pass that job; it is not inferred from macOS.
DeviceCapabilities, SceneCoverage, PipelineReadiness and GpuExecution remain
explicitly pending even after successful RG1 publication.


## Execute cooked static graphs (RG2)

Use an explicitly authored Renderer instance interface when creating build sources:

```bash
ludus render-graph new render-graphs --output offscreen
ludus render-graph new-build render-graphs/main.lurender.json --renderer-static --profiles metal wgsl
ludus render-graph build render-graphs/main.lushader.json out/raster-package \
  --helper /path/to/sdk/lib/cmake/Ludus/shaders --compiler /path/to/pinned/slangc
ludus render-graph emit-runtime out/raster-package out/RasterGraph.h --name RasterGraph
```

For a depth export, use `new ... --output depth`: the saved graph selects the
already declared DepthOnly technique. Neither disconnecting color nor
`emit-runtime` invents shader programs. Both declared techniques still enter the
bounded offline build; the generated execution header includes only selected
stages. A nonvoid depth function rejects at shader compilation instead of silently
discarding its return value. Generic `new-build` manifests (clip-space starter
inputs) still reject; executed techniques keep the `RendererStatic` instance
interface and may add the per-draw uniform bindings described below.

The source and installed SDK expose the same build helper:

```cmake
find_package(Ludus REQUIRED COMPONENTS GraphicsRenderer RenderGraphFormat)
add_executable(my_viewer main.cpp)
target_link_libraries(my_viewer PRIVATE Ludus::GraphicsRenderer)
ludus_compile_renderer_shaders(TARGET my_viewer SHADERS renderer_composite)
ludus_compile_render_graph(TARGET my_viewer NAME RasterGraph
    MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/render-graphs/main.lushader.json")
```

`LUDUS_SLANG_COMPILER`, and for declared SPIR-V/GLSL profiles
`LUDUS_SPIRV_VALIDATOR`/`LUDUS_SPIRV_CROSS`, identify prepared pinned host tools.
No configure/build step downloads tools. `ROOT` defaults to the manifest's
directory; use a dedicated source directory outside generated output. Nested
graph/shader paths are package relative; `new-build --root` preserves this owner.
`CONFIGURATION` defaults to Default. Target-local build outputs contain immutable
RG1 generations, a stage cache and the generated `RasterGraph.h`. Source and
installed helpers reuse the exact Python services; the SDK includes their stdlib
modules, so CMake generation requires Python but no installed wheel or checkout.

At setup, include `RasterGraph.h` and `renderer_composite.h`, call
`RasterGraph::MakePlan()` and `RasterGraph::MakePrograms()` in namespace
`ludus::render_graph_packages`. Pass the installed
`ludus::shaders::renderer_composite::Vertex()` and `Fragment()` descriptions to
`MakePrograms`; built-in recipe paths/entry points stay with Renderer. Initialize
`ludus::graphics::renderer::RenderGraph` with those values and the owned RHI
device. Ready permits Update; Pending requires Poll between frames. Generated
shader strings/reflection arrays have static storage. Pipeline/device/capability
failure never becomes a request to compile at draw time.

The World/extraction adapter supplies borrowed tables of `RenderGraphMesh`,
`RenderGraphItem` and generation-local `ViewDescription`, then a nonzero revision
and explicit coverage policy to Update. Emit one item for each intended
submesh/material slot, with a unique stable source identity; do not filter
incompatible items out beforehand. Static geometry and runtime transform/color
values are copied before Update returns. Camera input order and required target
indices are emitted into the plan. Each active camera must reach its designated
surface color or exported image, independently of another camera’s route.
View matrices must use the authored conventional [0,1] depth convention.

`RejectIncomplete` is the shipping policy. `DiagnosticColor` can admit an editor
marker route for unsupported material/layout inputs. Known static positions use
magenta overlay geometry; an unknown vertex layout requires explicit finite
mesh bounds and uses a bounds proxy without reading unknown vertex data.
Diagnostic overlay draws never write required depth, and the coverage record
stays Unsupported. Transparent, masked and deformation contracts remain
unsupported even when invisible or rendered diagnostically. Explicit exclusions
are reported separately. Duplicate source identities or unapproved repeated
camera/role coverage remain conflicting under either policy.

`GetReport()` describes the latest candidate; `GetDrawableSceneRevision()`
identifies the last Ready extraction. `GetOutputSceneRevision()` identifies a
valid completed producer’s pixels; it returns zero while that producer is pending
or invalid and never relabels old pixels after a new scene becomes Ready. Actual
platform presentation/external consumer completion remain the caller’s ownership.
Pending or failed replacements preserve
that extraction. A rejected newer report remains rejected if an older pending
candidate becomes drawable. Coverage records belong to the graph until the next
Update/Reset. Mesh streaming and camera changes cause a fresh scan/preparation;
there is no coverage cache that can hide changed inputs.

For an export-only graph, prewarm its consumers with GetExportAllocation after
initialization. This borrows the allocation/aspect without claiming valid pixels.
Begin one caller-owned RHI frame, call Encode, then draw the prepared consumer
(e.g. Renderer composition) in that same frame. Encode does not acquire or present
a surface. End that frame with an actual submission token and pass it to
ConfirmSubmission whenever HasUnconfirmedEncoding is true, including after a
partial encoding failure. A missing/failed submission invalidates output; a
foreign token rejects. The actual token must belong to the encoded frame.
Update/Poll/Render cannot replace that unconfirmed encoding. Completion and
GetOutputSceneRevision/GetExport remain separate from submission admission.
This path lets offscreen rendering and presentation share one submission.

Render is a whole-frame convenience that acquires, clears and ends the carrier
surface under existing RHI rules, including for export-only roots; use Encode
when the caller owns frame composition. Call Render between frames with the
surface owner’s current drawable pixel width
and height and an empty completion token. It submits retained Ready views in
authored serial order and creates no shaders or pipelines. Resize preflight,
pending producer or device failure preserves the drawable extraction. The
acquired dimensions are rechecked; if a resize races acquisition, ending that
frame may clear/present the surface. Failed partial encoding returns actual
completion when available and invalidates export correctness; it does not roll
back accepted GPU work or promise unchanged surface pixels.

After producer completion, GetExport returns the graph-owned texture allocation
and Color/Depth aspect. Create the matching sampled RHI view; finish every
consumer before another Render or Reset. Export ownership is borrowed, not
transferred. Reuse is conservatively completion gated. Distinct logical depth images cannot
share one private color/depth allocation, and one plan has at most one acquired
surface owner; such topology is rejected before GPU setup. Depth is the allocation’s
private depth aspect; a depth-only graph currently pays for a compatibility RGBA8
allocation as well. Resize, foreign image import and history need another
qualified profile/generation; they reject here instead of silently changing
allocation identity. Reset before destroying the device.

The profile is bounded to 16 passes, 32 logical resources, 8 cameras, 128 draw
items and 32 meshes. It qualifies RGBA8/Depth32, single-sample static geometry,
TriangleList, no culling, counter-clockwise front and conventional
Less/depth-write, for the opaque and explicitly authored masked families. A depth
export does not establish pre-Z equivalence for later shading with another
comparison, transparency, custom depth or deformation path. Arbitrary programs
with those semantics require explicit author qualification and parity fixtures.

### Per-draw uniform values

Add RG1 uniform bindings to the techniques' programs in a `--renderer-static`
manifest. Fields are `Float32`–`Float4` in slots 0–7, as in RG1. The emitted
header exports `UNIFORM_VALUE_COUNT` and one `UNIFORM_VALUE_<SCHEMA>_<FIELD>`
offset per schema field, for example:

```cpp
ludus::foundation::StaticArray<ludus::foundation::float32, RasterGraph::UNIFORM_VALUE_COUNT> values{};
values[RasterGraph::UNIFORM_VALUE_MATERIAL_TINT + 0] = 1; // Tint is Float3.
values[RasterGraph::UNIFORM_VALUE_MATERIAL_GAIN] = .5F;
item.Uniforms = {values.GetData(), values.GetSize()};
```

Values are grouped by uniform schema in identifier order; techniques reading one
schema share its values. Every item that is not intentionally excluded must supply
exactly `UNIFORM_VALUE_COUNT` finite values. An unbound plan requires none. Values
are copied before Update returns. Renderer packs them with the selected backend's
reflected layout: Metal, SPIR-V, WGSL or GLSL ES, never by reinterpreting a host
struct. Equal value sets share one immutable uniform buffer and binding snapshot
and stay instanced. Each bound technique admits at most 16 distinct sets per
extraction; more return CapacityExceeded and preserve the drawable scene. Each
item carries at most 64 logical values, a plan at most 16 bindings, and each block
at most 1024 bytes. A vertex and fragment stage may share a slot only with an
identical schema and layout. Changing values republishes the extraction and never
creates a shader or pipeline. The diagnostic magenta route is unavailable when the
opaque color technique binds uniforms, because it cannot honor the instance color.
Coverage stays Unsupported. Per-view values, matrices, arrays, textures and storage
bindings remain unqualified.

### Masked coverage

Declare `material: DefaultMasked` on the graph raster and author both
`DefaultStaticMasked` techniques. Share the coverage function between their
fragments through an include, and give the depth-only fragment its own program.
The checked-in [masked fixtures](../../tests/sdk_consumer/rg2/masked_color/main.lushader.json.in)
discard by a per-draw mesh-space threshold:

```slang
void ApplyCoverage(RasterVarying value)
{
    if (value.PositionMesh.x < mask.Threshold)
    {
        discard;
    }
}
```

Masked techniques cover only items declared `RenderGraphMaterial::Masked`, and
opaque techniques cover only `Opaque` items. A graph with both families serves
mixed scenes, each item drawn only by its own family's passes. `Transparent`,
`Unsupported` and deformation contracts are still rejected under either family.
Renderer rejects a masked depth-only fragment identical to the opaque depth
fragment, or a masked geometry stage that differs between the masked techniques. Renderer prewarms additional
finite built-in state combinations; these physical PSOs are separate from RG1’s
authored recipe budget. RG2 makes no scheduling/memory/performance improvement
claim. Interactive editor adoption, graph editing and reload are RG3.

`test_render_graph_execution` checks lowering, rejection, deterministic header
publication, profile bounds and the RG1 nested-root/packer-name regressions. It
also checks uniform table lowering, merged stage slots, the interface identity
changing with uniform schemas and layouts, masked technique lowering, masked
coverage needing its own depth function, and still-rejected transparent
materials, deformation layouts and masked plans without masked techniques.
Renderer `[render-graph]` unit tests cover per-draw packing for the selected
target, set deduplication and its bound, incomplete or nonfinite values,
mismatched stage interfaces, malformed and missing target layouts, masked family
coverage, mixed opaque/masked views, and masked depth/geometry admission. Native
GPU tests render the `bound` fixture's per-draw values, compare its cooked
`.lurgrt` uniform tables and pixels with the compiled header, and require the
`masked_color` and `masked_depth` exports to agree byte for byte in depth and
pixel for pixel in coverage.
Renderer’s deterministic tests cover mixed material slots, visibility-independent
coverage, missing/conflicting routes, streaming readiness/failure and preserved
revisions, resize preflight and export initialization/completion. Native GPU tests
compile the actual graph package via the shared CMake helper, render changing
camera/instance inputs, compare direct/offscreen color byte for byte within one caller-owned frame and sample
DepthOnly output at independently expected geometry/background depths. The
installed `verify_render_graph_execution.py` fixture checks relocated CLI/CMake
header equivalence and rejects a nonvoid depth function with real Slang.

Local qualification uses warning-clean Clang 18 production code, pinned Slang
Metal/WGSL and Metal pixel regressions. CI additionally compiles/renders the
SPIR-V/Vulkan profile and qualifies installed SPIR-V/WGSL/GLSL headers. WGSL
stage admission alone does not establish browser runtime graph execution. The
installed RG3 runtime fixture (see [RG3 acceptance evidence](#rg3-acceptance-evidence))
also executes a compiled-header graph on WebGPU in the browser CI job. The RG2
Renderer unit suite also passes UBSan locally; ASan still stalls before
main in an empty control executable on this host. RG2 WGSL stages pass pinned
Chromium software-adapter admission.


## Edit and reload graphs (RG3)

RG3a delivers the editing, diagnostics and activation half of the
[RG3 contract](../architecture/render-graph-authoring.md#editor-interaction-diagnostics-and-hot-reload):
a tree, field inspector, connection table and source editor, and runtime
generations with atomic activation. RG3b adds the node canvas and the trusted
in-editor preview; see [Node canvas and preview (RG3b)](#node-canvas-and-preview-rg3b).

### Edit in the Render Graph document

Open a project and open the **Render Graph** document. It shows the project's
`graphs/Main/main.lurender.json`, or else the
[engine default](#start-customizing-the-renderer). To edit another graph, choose
**File → Open Render Graph...** and select a root `.lurender.json` inside the
project, or double-click a render graph or shader in Project Explorer. Its
directory is the package root. The document
loads every `.lurender.json`, `.lushader.json` and `.slang` file below it (at
most 64 files, 128 KiB each and 4 MiB in total; hidden and `out` directories and
symlinks are skipped).

For a project without graph sources, the document shows the engine default, and build
actions work on it right away. Use **File → New Render Graph...**, as described in
[graph creation](#create-a-saved-graph-and-import-it), to start another graph.

![The Render Graph document at 1280×800: source tree and node canvas in the centre, the node inspector in Properties on the right](images/editor-render-graph-document-1280.png)

The document is the package's source tree beside three views, switched by the tabs
below them: **Canvas**, **Source** and **Plan**. A bar above them shows the open
graph's path and the root **Configuration** to validate and cook. The document has
no command buttons of its own; its commands are in the **File** and **Build** menus
and on the Render Graph toolbar (see the
[editor workspace layout](editor-workspace.md#workspace-layout)).

- **Properties** (the tool window, F4) edits the node selected on the canvas, in the
  source tree or by Error List navigation: its authored fields by stable ID, its
  layout position, removal (which also drops its connections and layout position),
  new nodes, and `node.port` connections in the configuration. Layout x/y edits
  only change the `layout` record; consecutive moves of one node coalesce into one
  undo step.
- **Source** edits graph, build-manifest/schema and shader text; **Apply Text to
  Draft** makes it one undoable document edit. Generated wrappers and packages
  are build products and never editable sources.
- **Plan** shows the deterministic serial plan of the last validation, or the last
  build's report.
- Edit > Undo/Redo and Save act on the render-graph draft while the Render Graph is
  the active document, including while you edit it in Properties.
  Save writes only changed files, each through an atomic replacement, and first
  checks that disk still holds the bytes that were loaded or last saved. An
  external change returns a conflict and writes nothing. Graph documents are
  saved in the same canonical form as `ludus render-graph new`.

Closing the project, opening another one or launching a build asks to save or
discard a dirty render-graph draft.

### Validate, build and cook a draft

**Build → Validate Render Graph** (Ctrl+F7), **Build Render Graph** (F7) and **Cook
Render Graph Runtime** run the installed
`ludus --json render-graph` backend as a separate process; the GUI never waits
for it; **Cancel Render Graph Job** (Ctrl+Break) stops the running job. Results and
diagnostics appear in the **Error List**; double-click a row to select its graph
node or open its source line. Validate and Build first write an immutable copy of the current draft to
`out/render-graph/drafts/` in the project and delete it afterwards, so unsaved
edits can be checked without touching saved sources. The same files give the
same result through the CLI:

```bash
ludus --json render-graph dump graphs/main.lurender.json --root graphs
```

A result names the draft revision it checked; a later edit marks it as describing
an older revision. Results from a cancelled job, an earlier request or another
project are discarded. Build resolves the saved project's current native profile
through the same SDK resolver as ordinary project builds: a saved local SDK
selection takes precedence over an installed locked SDK in the shared store.
Its installed `lib/cmake/Ludus/shaders` directory supplies the shader helper and
toolchain pins. Ordinary offline compilation does not require the optional
`native_worker.py` client, so SDKs that predate native-worker support remain usable.
That client is required and included in tool identity only when a native worker
is selected. Slang and, when needed, the SPIR-V validator and SPIRV-Cross come
from the selected local setup preset or the editor's prepared shader tools.
No path entry is needed for **New Graph → Build & Preview**. Loading and building
do not download tools, repair the project or write into the engine/SDK.

Build publishes to `out/render-graph/<graph>/build`. Cook Runtime writes
`out/render-graph/<graph>.lurgrt` from that build tree. These paths are resolved
from the project's canonical location, so a project opened through a symlink
still builds inside its own folder. Build and Cook Runtime refuse a graph whose
package lies outside the project.

### Build settings

**Tools → Options... → Render Graph** holds the render-graph build settings and
shows the paths the last automatic build resolved. Its **Override detected shader
tools (advanced)** checkbox explicitly enables manual paths for an exceptional tool
setup; an override needs at least the SDK shader helper and the Slang compiler, and
the dialog refuses OK until both are set. Turning the override off keeps the paths
for later but builds detect tools again. Old editor-wide path preferences do not
override project detection, and switching projects clears these overrides.
Each automatic build resolves selection again, so SDK switches and repairs take
effect without restarting the editor. Missing/incomplete SDKs or tools report a
setup error; use **Project → Repair Project Setup**. A selection change during
compilation/cooking rejects publication and retains the previous runtime.
**Native shader worker** is an editor preference that stays across projects (see
[editor and game clients](#editor-and-game-clients)).

The same automatic path is available to tooling without the editor:

```bash
ludus render-graph build graphs/Main/main.lushader.json out/graph-build \
  --project /path/to/game --tooling-root /path/to/prepared/ludus \
  --runtime-package out/graph.lurgrt
```

`--tooling-root` is unnecessary when the selected local preset records all required
shader-tool paths. Explicit `--helper`/`--compiler` arguments remain supported for
standalone packages. Pin/version, reflection and publication checks still apply.

### Mapped diagnostics

With `--json`, a failure's `error.location` names its authored owner:

| Failure | Location fields | Editor action |
| --- | --- | --- |
| Source/document error | `source`, optional `configuration` and `node` | Selects the configuration or node |
| Expanded-plan error | `source`, `configuration`, `node`, `instance` (nested path such as `Main/raster/draw`) | Selects the node in its own library |
| Shader compiler error | `source`, `line`, optional `column`; `diagnostics` lists up to 32 compiler messages | Opens the function file at that line |
| Generated wrapper error | `generated: wrapper.slang`, `program`, `line` | Opens the program record in the build manifest |

Locations are package-relative and never contain machine paths. Activate a
diagnostic (double-click or Enter) to navigate to it.

Shader failure messages are portable too. The helper's own command echoes
(`+ <tool> <arguments>`, with absolute tool and temporary paths) are dropped,
because the stage descriptor and build manifest already identify tools and
inputs, and they would otherwise use up the 4 KiB message bound before the
compiler's diagnostics. In the remaining text, captured sources appear
package-relative, stage outputs as `artifact/`, helper files as `helper/` and the
compiler by its file name.

The shader helper (`compile_shader.py`) reports a compiler failure as one stderr
line prefixed `Ludus shader build: `, so the compiler's first diagnostic arrives as
`Ludus shader build: main_shaders/color.slang(3): error 30015: ...`. The mapper
accepts that prefix; later compiler lines arrive unprefixed. The installed CLI
consumer and the editor tests assert the location from the pinned compiler's real
output.

### Runtime generations and activation

```bash
ludus render-graph cook-runtime out/raster-package out/main.lurgrt --configuration Default
```

`cook-runtime` accepts the same verified `--renderer-static` packages as
`emit-runtime` and writes the same plan records, stages and CPU interface
identity into a little-endian file described by
`ludus/content/render_graph_format/runtime_package.h` (layout version 2, with
uniform binding/block/field records and GLSL ES block names). `emit-runtime`
headers also export `INTERFACE_IDENTITY`, which covers the vertex/instance
interface and the per-draw uniform schemas with their target layouts. With CMake, add `RUNTIME_PACKAGE` to
`ludus_compile_render_graph`; it sets `<NAME>_RUNTIME_PACKAGE` to the cooked file.

At runtime, decode the file into a `RenderGraphPackage` (bytes are copied, so the
source buffer may be released immediately) and pass it to a
`RenderGraphActivation` initialized with the host's `INTERFACE_IDENTITY` and the
installed composite recipes:

1. `Stage` prewarms the candidate beside the drawing generation. A different
   CPU interface returns Unsupported with `RestartRequired` and stages nothing.
2. Call `Poll` between frames. While pipelines are pending the active generation
   keeps drawing. When the report says `NeedsUpdate`, call `Update` with the
   current extraction; the candidate must prepare it before activation.
3. `Poll` publishes the candidate once it is Ready, only when the active graph has
   no unconfirmed encoding. `ActiveGeneration` changes, so re-query
   `GetActiveOrNull` and exports. The new graph reports no output revision until it
   renders, so it never relabels the previous generation's pixels.
4. A failed setup, failed pipeline or a candidate that rejects an extraction the
   active generation accepts reports `Failed` with `LastFailure`; the active
   generation, its pixels and output revision are unchanged. A newer `Stage`
   supersedes an unactivated candidate.

Export consumers of the active generation must finish before a `Poll` that may
activate, as before `Render`/`Reset`. The replaced graph's GPU uses retire through
RHI completion; unloading game code is a separate GameHost obligation and never
implied by activation.

### Schema changes and parameter migration

```bash
ludus --json render-graph schema-diff out/old-build out/new-build
ludus --json render-graph migrate out/old-build out/new-build --values values.json --defaults defaults.json
```

`schema-diff` compares logical schemas field by field and every program/target
binding layout. Any changed Uniform or Vertex schema or layout makes
`cpu_interface` `RestartRequired`: rebuild the host/game interface before
activating that generation. `migrate` maps `schema -> field -> numbers` (a
`Float32` uses one number). A value is kept only when the field identity and type
are unchanged. An added field or a changed type needs an explicit default, or the
result is `Rejected` with no values and exit status 1. Removed values are reported
as dropped. Field IDs are stable identities, so a rename is a removal plus an
addition. Neither command writes sources, values or packages.

### RG3 acceptance evidence

What each fixture establishes:

- `test_render_graph_authoring` covers mapped graph/shader locations, CLI JSON
  locations, draft-snapshot plan equality, layout-only digest stability,
  migration and restart verdicts, deterministic runtime packages and header/package
  identity agreement.
- Renderer `[activation]` tests cover decoding and malformed packages, first
  activation, pending replacements that keep drawing without creating pipelines,
  failed reloads that preserve output revisions, `RestartRequired`, activation
  waiting for a confirmed submission, completion-gated retirement and superseded
  candidates. They also check that caller bytes can be released after decoding.
- SPIR-V decoding: a generation carrying Metal, SPIR-V, WGSL and GLSL ES stages
  decodes each SPIR-V word little-endian, from a source buffer at an odd address,
  beside the text targets and their entries. A SPIR-V artifact that is not a whole
  number of words, or a vertex stage without its fragment stage for that target, is
  rejected as malformed. A role the generation does not carry stays empty, as it
  does in a compiled header.
- The cooked-fixture `[render-graph][activation]` test decodes the CMake-cooked
  `rg2_surface.lurgrt` and compares it with its compiled header: plan records,
  CPU interface identity and every stage artifact (SPIR-V words, Metal/WGSL/GLSL
  text, entries and inputs). The host's native target (Metal on macOS, SPIR-V
  elsewhere) and WGSL must be present.
- The `[render-graph][gpu][activation]` test loads the same `.lurgrt`, activates
  it twice and compares its pixels byte for byte with the compiled-header graph.
- The installed-SDK check (`tests/sdk_consumer/verify_render_graph_execution.py`)
  requires identical `.lurgrt` bytes from three cooks of one package: the installed
  CLI's `cook-runtime`, the relocated CMake helper's `RUNTIME_PACKAGE`, and the
  source-tree tool. The generation's identity must equal the emitted header's
  `INTERFACE_IDENTITY`. The installed CLI also runs `schema-diff`.
- The installed runtime consumer (`tests/sdk_consumer/render_graph_runtime.cpp`)
  is built against the installed SDK for the host's single target: Metal, SPIR-V,
  or WGSL in the browser. It qualifies two cooked offscreen graphs in turn: the
  opaque static graph, and the `masked_color` graph, whose authored masked
  coverage reads a per-draw uniform value. For each graph it renders the compiled
  header's frame, then decodes the embedded `.lurgrt` and activates it twice. After
  each activation it reads the exported color back through the RHI and compares
  every byte with the compiled-header frame. The masked graph must also keep its
  covered half and leave the discarded half at the cleared background.
- Editor `[render-graph]` tests cover canonical JSON bytes, draft/save separation,
  undo coalescing, save conflicts, snapshots and diagnostic navigation. They run
  the real CLI to check that the editor and CLI produce the same plan and the same
  mapped failure.

`render_graph_authoring.py`, which provides `schema-diff` and `migrate`, ships in
the `ludus-tools` package behind the installed `ludus` CLI. The relocated CMake
helper installs only the build and execution modules. `cook_runtime` lives in
`render_graph_execution.py`, so the helper never imports the authoring module.

Platform status:

| Platform | Established | Evidence |
| --- | --- | --- |
| macOS / Metal | Decode, artifact equivalence, activation pixel parity, installed CLI/CMake/source byte equivalence, installed runtime consumer | Local Clang 18 runs; CI macOS jobs run the same suites |
| Linux / Vulkan (Mesa llvmpipe) | Activation pixel parity under the Khronos validation layer | The October 16, 2026 batch validation of `develop` commit `c2d299d0` ran the 20 non-hidden `ludus_renderer_render_tests` cases with none skipped, including this parity case, and reported no validation messages |
| Linux / Vulkan, added checks | SPIR-V decode and artifact equivalence; a dedicated `[render-graph]` step that fails on any skipped case; installed `.lurgrt` equivalence for SPIR-V/WGSL/GLSL; installed runtime consumer under the validation layer | CI "Development and SDK" job; not runnable on the macOS development host |
| Browser / WebGPU | `GraphicsRenderer`, including `RenderGraphPackage` and `RenderGraphActivation`, is part of the web SDK. A WGSL-only `.lurgrt` is decoded and activated twice in Chromium on the SwiftShader WebGPU adapter, with byte-identical exported pixels | `render-graph-runtime-browser-test.mjs` in the "Renderer browser tests (baseline)" job. First passed on October 17, 2026 (Chromium 140): two generations activated, zero mismatched bytes. Browser builds need the Linux-pinned SPIR-V validator, so this fixture has no local run |

Remaining gaps:

- WebGL 2 / GLSL ES: the browser fixture cooks only WGSL and requests WebGPU; no
  runtime generation is qualified on the WebGL 2 fallback.
- Browser delivery: the fixture embeds the `.lurgrt` bytes in the module. Fetching
  a generation at run time, and browser editor preview, are not established.
- Physical GPUs: Linux and browser evidence uses software adapters (llvmpipe and
  SwiftShader).
- RG3b editor preview on Linux/Vulkan is not covered by this evidence.
- No scheduling, memory or performance claim; RG4 owns measured pooling, aliasing
  and scheduling (see [RG4a pooling](#pool-transient-attachments-rg4)). The editor renders runtime generations only through the RG3b
  preview below.

## Node canvas and preview (RG3b)

RG3b completes the editor half of the
[RG3 contract](../architecture/render-graph-authoring.md#editor-interaction-diagnostics-and-hot-reload):
a visual node canvas over the same draft, and a trusted preview of the cooked
runtime generation on the editor's existing viewport device.

### Edit shader sources in a code editor

On native desktop, select a file in the **Source** view and click **Open in Code
Editor**. The first use opens a native application picker; **Choose Code
Editor…** changes that saved preference. On macOS, select the editor's `.app`
from Applications. No command-line launcher or shell command is required.
The selected application receives the saved project file; completion and
formatting come from that application's language support. This first launcher
opens the file without a diagnostic line argument. External application launch
is unavailable in the browser editor.

If the graph or text pane has unsaved changes, opening asks to save them first.
For the engine default, opening asks to create an editable copy in the project;
the engine sources stay unchanged. Cancelling retains the current text/draft.

Save in the code editor, then use **Build & Preview**. Validation and builds
refresh external saves when the draft is clean; **Reload Sources** performs the
same refresh without starting a job. Refresh uses the existing package limits
and creates one undoable revision. When local and external changes conflict,
the current draft is retained and the job does not start. Undo local edits or
reopen the graph to resolve the conflict. Apply or revert unapplied text before
reloading; source refresh never silently replaces that text.

The Source view shows an **Inputs / Outputs** reference beside each Slang file.
It reads the current draft build manifest and expands input and output schemas
into field names, types, semantics and locations, with read-only uniform bindings
and their slots. Every program sharing the source is listed by function and stage.
Fragment color output is labeled `Color : float4 → SV_Target0`; depth-only
functions return `void`, with depth supplied by rasterization and writes governed
by the pipeline recipe. These shader interfaces differ from the canvas's pass
resource ports (`scene`, `view`, `color`, `depth`). The reference describes authored
metadata, not compiler reflection or validation of unapplied shader text. Edit
schemas and program contracts in the manifest and apply the text to refresh it;
undo and external source reload also refresh it. Missing or invalid manifests and
unregistered sources show an unavailable message. The reference is separate from
the editable source, preserving saved bytes and compiler diagnostic line numbers.

### Create a subgraph without leaving the editor

Select the containing configuration in **Graph sources**, then choose **Create
Subgraph...** above the canvas or from the source-tree/canvas right-click menu.
Enter a unique name starting with an ASCII letter, followed by letters, digits,
`_` or `-` (up to 63 characters). The action creates `<name>.lurender.json` in
this graph package as an unsaved draft, adds a Subgraph reference to the selected
configuration and opens the new library's empty `Default` configuration.

When a Subgraph node is selected, **Create Subgraph...** instead points that node
to the new library. Its previous source remains intact; connections are retained
for explicit interface editing. Existing files and asset IDs are never replaced.
Cancel leaves the draft unchanged. Creation and reference changes are one undo
step; **Save** publishes the new file. Undoing a saved creation and saving again
removes the created file if its disk contents still match the editor's saved copy.
An external change reports a conflict and is preserved.

Add nodes using Properties or **Create Node** in the right-click menu. Choose
**Edit Inputs / Outputs...** above the canvas or in its right-click menu to define
the library interface. The empty library has no ports or rendering work until
authored. It does not add ray-tracing or compute
support; the supported node kinds above still apply. The original root remains
the execution root when editing the library.

Right-click a node in **Graph sources** or on the **Canvas** to use its menu:

- **Create Node** lists Scene, Camera, Target, Raster, Subgraph Reference, Export
  and Present. It is also available on empty canvas space and configuration/file
  rows in Graph sources. Choose a type to add it to the displayed configuration
  with a unique ID, selected and scrolled into view. This uses the same undoable
  operation as Properties' **Add Node**. Creation is disabled while a job runs or
  Source text needs applying/reverting. **Subgraph Reference** adds a reference
  node; **Create Subgraph...** creates a new library source as well.
- A selected Subgraph node's **asset** and **configuration** are dropdowns in
  Properties. **asset** lists the package's other `.lurender.json` graphs (never the
  graph that contains the node). **configuration** lists the selected asset's
  configurations. Choosing an asset that lacks the current configuration
  switches to that asset's `default`, or to its first configuration if it has no
  default. A missing asset or configuration is still listed, so you can see it
  and replace it. Validation still decides whether the reference is legal,
  including whether it creates a cycle.
- **Open Subgraph** opens the referenced configuration; a missing source disables
  this item. **Create Subgraph...** can supply a new source for that node.
- **View Shaders** on other nodes lists the source files mapped by the root build
  manifest to the Raster pass's material/mode technique. Choose a file to open it
  in the in-app Source editor. Scene, camera, target and output nodes have no
  shader sources, so this item is disabled for them.
- **Remove Node** removes that node and its connections as one undoable edit.
  Referenced subgraph files are retained.

Double-click a Subgraph to open it, or a Raster node to open its first mapped
shader source. The source selector and **View Shaders** menu give access to the
other stage. Apply or revert pending Source text before navigating or creating.

### Edit on the canvas

The **Canvas** view sits beside the source tree and shows the selected
configuration (a selected file shows its `default`); Properties shows the selected
node. It is another view of the same typed draft: every canvas edit is a
`RenderGraphDocument` operation, so undo, explicit save, validation and Properties
stay in step.

Every graph-authoring operation in Properties also has a canvas gesture or
context-menu entry. Right-click the background to create nodes and subgraphs or
edit the configuration interface; right-click a node to edit its fields or remove
it; right-click wires and ports to manage connections. Properties remains an
additional way to perform those same document edits.

Choose a node type in Properties and click **Add Node**. It uses the configuration
currently shown on the canvas, including a selected file's default; selecting a
configuration row or entering an ID is unnecessary. IDs are generated from the
type (`scene`, `scene2`, `scene3`, and so on), avoiding existing IDs in that
configuration. The new node is selected and scrolled into view. Any failure is
explained beside **Add Node** and leaves the draft unchanged. Addition is one
undoable edit; **Save** writes it to the project.

| Gesture | Draft operation |
| --- | --- |
| Drag nodes (Ctrl-click adds to the selection) | One `MoveNodes` edit per gesture, applied on release: one undo step however far the drag went. A drag that returns to its start commits nothing. |
| Drag from an output port to an input port | `Connect`, after the pre-check below |
| Select a link and press Delete, or right-click a wire/port and choose Disconnect | `Disconnect` |
| Drag an output onto an occupied input | Replace its source as one undoable edit; invalid links preserve the existing source. |
| Right-click a port → Connect from / Connect to | Choose a compatible endpoint, replacing an occupied input in one undo step. |
| Right-click a node → Edit Node... | Edit the same authored fields, pipeline controls and layout coordinates as Properties, without opening that panel. |
| Select a node and press Delete | `RemoveNode` (also drops its links and layout position) |
| Double-click a Subgraph node | Opens its asset's configuration on the canvas, in the tree and in the Source view; not an edit |
| Double-click a Raster node | Opens its first mapped shader source; choose other stages with View Shaders or the Source selector |
| Pinch apart/together on a supported trackpad | Zoom in/out around the gesture position; does not edit the draft |
| Wheel or vertical two-finger scroll | Zoom around the pointer, including smooth pixel scrolling |
| Drag empty space | Pan the view only |

Header colours show the connection domain: blue scene/view (Scene, Camera),
orange execution (Raster, Present), green resource (Target, Export), purple
Subgraph and grey for a configuration's `input` ports and outputs. Port colours
show the value type. Nodes without a `layout` position are arranged by
dependency order for display only; nothing is written until you move them.
Configuration output links can be selected and deleted, just like pass connections.
They edit the configuration output's source rather than adding an ordinary edge.

### Edit configuration inputs and outputs

Choose **Edit Inputs / Outputs...** above the canvas or in the source-tree/canvas
right-click menu. It edits the configuration currently displayed on the canvas,
including a selected file's default. Double-click an existing grey interface box
to open the same dialog.

- On **Inputs**, choose **Add Input**, enter a unique name and select its type.
- On **Outputs**, choose **Add Output**, enter a unique name and select its resource
  type. Choose a compatible **Source** from the dropdown, or leave it **Unconnected**
  and wire it on the canvas after applying. **Requires consumer** means every
  containing Subgraph instance must consume that output.
- Select rows and choose **Remove Selected** to remove ports. Names must begin
  with an ASCII letter and contain at most 63 letters, digits, `_` or `-`.
  Each direction supports up to eight ports. Types follow the graph's version.
- Choose **Apply** to commit both tables as one undoable draft edit; **Cancel**
  discards the dialog's edits. **Save** publishes the draft. Rejected edits keep
  the dialog open with an explanation and preserve the document.

The grey input/output boxes appear as soon as their first ports are applied.
Drag configuration input ports to pass inputs; drag pass outputs to configuration
output ports. Compatible resource inputs can also pass directly to configuration
outputs. Select a link and press Delete, or use a wire/port's right-click menu,
to disconnect it. Each input and configuration output has one source; dropping a
new wire onto it replaces that source in one undo step. Type or cycle rejection
preserves the previous source. Unconnected outputs are incomplete
drafts and must be connected before validation/build succeeds.

Disconnect affected internal and containing-subgraph links before removing,
renaming or changing the type of a connected port. The dialog checks those
references across the loaded package. Removing a producing node retains its
configuration output declarations as unconnected ports. Editing interfaces does
not change which GPU pipeline kinds the runtime profile can execute.

Before a link reaches the draft, the canvas checks it against the configuration
and rejects it with the shared validator's error name and a reason in the status
line:

| Code | Rejected link |
| --- | --- |
| `Port` | Unknown output or input, or a link that starts at an input or ends at an output |
| `MultipleWriters` | The input already has a connection; disconnect it first |
| `TypeMismatch` | The output and input types differ, for example `DepthTexture` into a color input |
| `Cycle` | The link would make a node depend on its own result within a frame |

The pre-check gives immediate feedback; **Validate Render Graph** still runs the shared
CLI backend, which owns legality and reports the same codes as
`RenderGraph<Code>` for drafts edited another way.

### Preview a cooked generation

1. Build and **Cook Runtime** as above, so `out/render-graph/<graph>.lurgrt`
   exists. **Build & Preview** does both, then previews the result. An unedited
   engine default skips this step and uses the generation cooked with the editor.
2. Open a World and start its viewport; the preview borrows that device and the
   World extraction. It never starts a device of its own. The preview follows
   the viewport the World shows: the scene viewport of an imported-scene World
   (such as a converted `.lulevel` or a World with a `scene` section), otherwise
   the schema-bound render panel. Switching Worlds re-attaches the preview and
   re-stages the requested generation on the newly shown viewport's device.
3. The native World viewport shows that graph automatically. The optional **Render Preview**
   tool window (**View → Render Preview**) mirrors it; **Preview Runtime** explicitly reloads
   the last cooked file when needed.

Each accepted build reads the cooked file and stages it in a `RenderGraphActivation`
beside the generation already shown. Live Preview rebuilds saved graph/shader sources
only; unsaved changes remain in the draft. A result for an older saved version never
replaces the current request. Validation, compilation and activation failures preserve
the last drawable view; successful replacements publish only after readiness. By default the viewport shows the configuration's first exported
`ColorTexture`, using the World's current camera or orbit. Its output selector can
instead show any attachment after producer completion. Each World object becomes one static
item with its flat tint (object tint times the surface's flat tint); a
straight tint with alpha below one is declared `Transparent`, and the editor's
`DiagnosticColor` coverage policy draws unsupported items in magenta instead of
hiding them. Textures, lights and other materials are outside the current static
profile. An authored surface's flat tint is its schema tint.

Imported and merged Worlds are cooked: their geometry lives only in the decoded
scene definition, which the extraction borrows (and keeps alive) without copying.
Their base color lives only in textures, which the static profile does not sample.
The preview approximates each cooked material by one flat color instead:

- A textured material's flat tint is the mean of its base-color texture's mip-zero
  texels in linear space. Each sRGB byte is decoded before averaging; every texel
  has equal weight. White and black texels average to linear one half, not to the
  decode of sRGB code 128.
- Alpha is one. The World viewport draws cooked materials `Opaque` and ignores
  sampled alpha, so a texture with partial alpha previews opaque rather than as a
  magenta `Transparent` item.
- An untextured cooked material stays black, as the cooked format defines it.
- The mean is computed once per texture when the World is decoded and stored on
  each material that uses it. Decoding has already applied the scene format's
  texture limits (4096 texels per axis, 16 MiB per texture, 32 MiB per scene), so
  the work is linear in a bounded byte count.

This is a coverage-free average, not a lit or textured image: a texture with
strong detail or high contrast previews as its overall color. Merged Worlds bake
each authored object's schema tint into its cooked texture, so authored objects in
a merged World render in their schema color. This static graph does not sample the
texture or provide the former viewport's Lit material path.

The runtime profile admits only conventional depth (near 0, far 1, `Less`, clear
1). The World viewport's editor camera is an infinite reverse-Z perspective, so the
preview asks for a finite conventional projection of the same camera instead:

- An infinite perspective lens becomes a finite one whose far plane is 1% beyond
  the farthest corner of the World's geometry bounds from the camera (at least
  twice the near plane). Finite authored perspective and orthographic lenses keep
  their planes.
- The finite reverse-Z clip z is rewritten as `w - z`. This is exact only for a
  finite projection; applied to an infinite one it would leave a degenerate far
  plane at depth 1.
- Position, orientation, field of view, near plane and the clip x, y and w rows
  are unchanged, so the image matches the viewport framing. Conventional depth
  has less precision than reverse-Z far from the camera; distant coplanar
  surfaces can z-fight in the preview where the viewport does not.
- A World without finite geometry bounds reports that in the preview status
  instead of drawing.

Reverse-Z graphs are not admitted: that would need an authored and qualified graph
configuration (depth compare `Greater`, clear 0), never an implicit variant of a
conventional graph.

The image changes only when a complete copy of a frame from the **active**
generation arrives:

- While a candidate prepares (`Preparing` or `NeedsUpdate`), the previous frame
  stays; the active generation keeps rendering World changes.
- A rejected file (unreadable, larger than 16 MiB, malformed or an unsupported
  version), a candidate that fails setup, coverage or device admission, and a
  `RestartRequired` interface mismatch all keep the previous frame and its
  generation number. The status line names the failure.
- Configurations that render to a presentation surface are not previewed; they
  run in the game host. A configuration without an exported color texture is
  reported instead of shown.
- When the World viewport stops, the preview releases every generation before the
  device is destroyed and keeps showing the last presented frame, labelled as such.
  Starting the viewport again re-stages the last requested file.

The status line distinguishes the shown frame (`Showing generation N at World
revision R`) from the candidate's state. The copy to the CPU is asynchronous and
always finishes before the preview polls for activation or renders again, so a
replaced generation is never reset while its export is being read.

The editor is built against the RendererStatic interface of the
[engine default graph](#start-customizing-the-renderer). CMake cooks it into the
editor, and its interface identity is the one every previewed generation must
match. A compile-time check keeps the checked-in
[offscreen fixture](../../tests/sdk_consumer/rg2/offscreen/main.lurender.json),
which the preview parity tests cook, on the same interface. Graphs scaffolded with
`ludus render-graph new-build --renderer-static` use that interface. Hot-swapping
a candidate whose camera count differs from the active generation's fails, because
one extraction feeds both; preview it after a viewport restart instead.

### Texture viewer

The Renderer tab's **Textures** page shows any texture of the previewed generation,
not only its export: every Target (colour and depth), including transient
attachments that are never exported. It needs the native GPU preview, the same as
the Preview page.

- **Select** a texture from the list, or click a node on the canvas:
  - A Target node selects its texture.
  - A Raster node, or a Subgraph node containing Raster passes, selects the
    attachments the pass writes. Clicking it again cycles from colour to depth.
  - An Export or Present node selects the texture it publishes.
  - A Camera node selects its destination target.
  - A Raster node in a subgraph document opened on its own matches every
    instance of that subgraph.
- **Channel** offers RGBA, R, G, B, A, or Rec. 709 luminance. Single channels and
  luminance show as grey, and alpha is never composited. A depth texture has one
  channel.
- **Range** is either *Manual* (minimum and maximum) or *Auto min/max*.
  - Auto measures the finite shown values. For depth, it leaves out the cleared
    far plane (exactly 1), so a scene's depth spans black to white. Switching to
    Manual starts from the measured range.
  - RGBA8 colour starts on Manual [0, 1]. Depth and float data start on Auto.
  - NaN and infinite values show in magenta.
- **Exposure** (in stops) scales HDR formats before the range is applied, and is
  hidden for other formats. The cooker currently admits only `RGBA8Unorm` and
  `Depth32Float`, so it is hidden for every graph texture today. The viewer's
  `RGBA16Float` decoding is unit tested for when HDR targets are admitted.
- **Mip and layer:** graph attachments are single-mip, single-layer 2D textures
  (the cooker's admission rule), so there is nothing to pick. The info line says
  "1 mip, 1 layer".
- **Zoom and pan** use nearest-neighbour sampling, so texels stay square. The
  wheel zooms about the cursor, dragging pans, and a double-click fits the view.
- **Pixel probe:** hovering shows the texel under the cursor, and clicking pins
  it. The probe shows the coordinates and the raw stored value: RGBA8 bytes with
  their normalized values, or the binary32 depth with its bits (for example
  `depth 0.5 (0x3f000000)`).
- **Info line:** the expanded resource name, resolution, format, and each use
  with its load and store operations, for example
  `colour attachment of Main/raster/draw (Clear → Store)`. It also notes
  `exported by …`, the shared allocation for a depth texture, or `transient`.

Readback never stalls the GUI thread:

- The preview reads the selected texture after each presented frame. This happens
  only between frames, once the export copy finishes and before the next Poll or
  Render. While the read is outstanding, the graph's attachments are not
  rewritten or released.
- Colour textures use the same banded RGBA8 copy as the preview image.
- RHI readback is RGBA8 only. So an editor shader
  (`apps/editor/shaders/texture_depth_pack.slang`) first draws the depth aspect
  into an RGBA8 attachment, packing each texel's IEEE bits into its four bytes.
  The editor then copies that attachment and rebuilds the depth bit-exactly.
- Every step is a request polled on later ticks. Nothing waits on a fence.
- The textures come from `RenderGraph::GetResourceAllocation`. It hands out a
  graph's non-surface allocation once the producer has completed, under the same
  rules as an export lease, and it is meant for diagnostics only.

What the viewer shows is the allocation's contents at the end of the frame:

- A depth resource shares its colour resource's allocation (in the private
  depth aspect).
- A pooled transient shows its last write in this graph.
- A subresource whose last pass discards it (`Store = Discard`) or that nothing
  writes has no stored contents. The viewer says so instead of showing stale
  data.

With **Live Preview**, the selection is kept by name:

- Every new World revision re-reads it.
- Every rebuilt generation re-reads it as soon as that generation's first frame
  is presented.
- If a rebuild removes the texture, the viewer reports that it is not part of
  the active generation.

**Resource names:** the runtime package is numeric, so `ludus render-graph`
cooking writes `<name>.lurgrt.resources.json` beside it. The file holds the
generation identity, each resource's expanded ID, type, format and extent in
record order, the expanded operations with their attachments, and the camera
targets.

- The editor uses the file only when its generation and every record's extent and
  kind match the package.
- Otherwise the textures are listed as `Resource N`, uses are described per pass,
  and canvas selection is unavailable. The info line says why.
- Runtime activation never reads the file.

**Browser editor:** the texture viewer is disabled there. The render-graph
preview it reads from is native only (`LUDUS_EDITOR_NATIVE_PREVIEW`): the browser
editor has no RenderGraph preview device, so there is nothing to inspect. The
packing shader is portable Slang, and the readback uses only RGBA8 texture
transfers, so enabling it in the browser needs a browser render-graph preview
first.

### RG3b acceptance evidence

- Editor `[render-graph][canvas]` tests cover domain and port display, dependency
  placement without edits, a ten-step drag committing one undo step (and undo
  restoring every moved node), separate steps for separate gestures, a real mouse
  drag, Delete removal with undo, each rejected link code with an unchanged draft,
  a legal link as one undoable `Connect`, the CLI reporting `RenderGraphTypeMismatch`
  for the edge the canvas rejects, and double-click Subgraph navigation.
- Editor `[render-graph][preview]` tests run on a headless RHI device (skipped
  without a GPU adapter). They cover the World extraction mapping; a pending first
  candidate showing nothing; the first successful frame; truncated, foreign-interface
  and missing generations keeping the previous frame and generation; a pending
  replacement keeping the previous frame until it presents; and the last
  frame surviving a device stop. A cooked (definition-backed) scene composed by
  `RenderScene::Merge` checks that the extraction borrows the definition's geometry,
  that the reverse-Z editor camera yields a finite conventional view with the same
  x, y and w rows and in-range depths, and that the scene renders through the
  graph from the editor camera in its red base-color texture's flat color. Merged
  scenes with a non-white 1×1 texture (straight alpha 128) and a white/black 2×1
  texture check the linear mean, opaque coverage and the untouched lit tint. A World test checks that the Render Graph document's preview source
  follows a schema World's render panel, an imported World's scene viewport and
  back. A Renderer test builds a broken `color.slang` with the pinned compiler and
  opens line 3 from the diagnostic. Preview/live parity renders the frozen fixture
  through the preview (cooked `.lurgrt`, activation, World extraction adapter) and
  through the live runtime path (the compiled header and a directly owned
  `RenderGraph` with independently written inputs) and requires identical bytes.
- Editor `[texture-viewer]` tests cover:
  - binary16 and binary32 decoding, and probe text per format (RGBA8, RGBA16F,
    Depth32F);
  - channel selection, luminance, remapping (including empty, NaN and infinite
    inputs), automatic ranges that leave out the depth background, display images
    with exposure and magenta non-finite texels, and the zoom, pan and probe
    mapping;
  - the fixture's names sidecar, with uses, canvas resolution for Target, Subgraph,
    Export and Camera nodes and for a subgraph document, and refusal of a stale
    sidecar;
  - on a headless device, the transient depth target read bit-exactly (plane depth
    0.5, background 1) and the colour target byte-identical to the presented frame;
  - a reader that reports never-written attachments as undefined and returns
    Pending before completing over later polls;
  - Live Preview refresh across a World revision and a rebuilt generation, keeping
    the selection by name.

  The renderer's `[render-graph]` tests cover `GetResourceAllocation` admission
  (out of range, before and after producer completion, and the transient depth
  that GetExport refuses).

Local qualification ran these suites with Clang 18 on macOS (Metal) under the
offscreen Qt platform. A native (cocoa) editor run on a copy of an imported
staircase World (17 objects, 6 meshes) attached the preview to the World's scene
viewport, presented the offscreen fixture's generation from the editor camera, and
opened `main_shaders/color.slang` line 3 for an undefined identifier while keeping
the presented generation. They prove the preview's renderer output and publication
rules, not native Qt presentation of the Render Preview tool window or a live GameHost
session.
Linux/Vulkan and browser execution are not established by these local runs, and
RG3b makes no scheduling, memory or performance claim.

## Pool transient attachments (RG4)

RG4 is delivered as individually measured optimizations, each with a serial
control that issues legal commands. RG4a is the first: exact-descriptor pooling
of transient attachments, with proof of RHI completion before reuse. Physical heap
aliasing, pass merging, parallel recording and async queues are later, separate
slices; none of them is part of RG4a. The contract is in the
[architecture](../architecture/render-graph-authoring.md#pipeline-readiness-and-execution).

### Choose the allocation mode

`RenderGraphActivation::Initialize` takes a `RenderGraphAllocation`:

- `ExactPool` (the default) gives every staged generation one shared
  `RenderGraphAttachmentPool`.
- `SerialControl` is the diagnostic control. Each generation creates all of its
  attachments and destroys them on Reset, exactly as before RG4.

Both modes encode the same commands. `GetPoolReport` copies the pool counters, and
`TrimPool` destroys idle allocations between frames, for example under memory
pressure. A directly owned `RenderGraph` takes an optional pool in `Initialize`.
Pass null for the control; otherwise pass a pool initialized on the same device
that outlives the graph and is Reset before the device is destroyed.
`GetAllocationReport` reports a graph's export and transient allocations and how
many it created or reused.

### What is pooled

A transient backing is an allocation behind logical resources that are neither
exported nor the presentation surface. On Reset the graph returns it to the pool.
A later acquisition reuses an idle allocation only when both of these hold:

- Its complete RHI texture description (format, extent, sample count, mips,
  layers, dimension, attachment usage and private depth/stencil) is equal.
- `RasterTextureState::Dependency`, the ordered submission of its last frame use,
  has completed. A completed later submission also covers earlier ones.

If every exact match is still in flight, the acquisition creates an allocation and
counts as `DeferredAcquisitions`. An entry whose proof fails (failed submission or
lost device) is destroyed instead of reused. Exports are never pooled, because
their consumers may keep views or bindings beyond the graph. Surfaces allocate
nothing. A pool keeps at most its `idleCapacity` idle entries (default and maximum
`RENDER_GRAPH_POOL_CAPACITY`, 32) and evicts the least recently released one. Idle
entries keep their RHI texture records and bytes, which count against
`StorageLimits::ResourcesPerKind` (default 64) and `ResourceBytes`. Choose a
smaller capacity, or trim, when those budgets are tight.

A depth-only backing keeps its compatibility RGBA8 image, at 4 logical bytes per
pixel. The RHI private-depth contract cannot express a depth-only attachment:

- Attachment textures always pair a color image with private depth.
- `RasterPassDescription` has no depth-only form.
- Every raster pipeline targets one to four color formats.

Removing the image needs RHI support for depth-only attachments and color-less
pipelines on every backend, which is outside this slice.

### Which graphs benefit

None of the admitted cooked graphs. In the RG2 profile no pass samples a graph
resource, and lowering never approves repeated camera coverage. A scene pass into
an unexported target is therefore `Conflicting`, and every cooked fixture's
backings are exports or the surface. A GPU test checks that each cooked RG2
fixture leases nothing from the pool.

To qualify the mechanism, `tests/sdk_consumer/render_graph_scratch.h` rewrites
the cooked offscreen `.lurgrt` into a numeric plan the Renderer does admit:

- every pass approves repeated coverage;
- unexported scratch color + depth backings follow, each drawn by one more pass;
- all resources take a chosen extent, so a resize is a reload of a generation with
  another extent, as a re-cooked graph would be.

Pooling becomes useful for cooked graphs only once a later profile admits sampled
graph reads or approved pre-Z passes.

### RG4a acceptance evidence

What each fixture establishes:

- Renderer `[render-graph][pool]` unit tests (deterministic reference RHI, where
  submissions complete only when the test says so):
  - reuse only after the last frame's completion, with two frames in flight;
  - exact descriptors under alternating extents;
  - exports, of either aspect, never pooled;
  - least-recent eviction, trimming, and an unbound or stale-device pool;
  - identical draw and pass counts in the pooled and control paths;
  - generation reloads through `RenderGraphActivation`, including reuse deferred
    while a retired generation's frame is in flight, a superseded candidate
    returning its transients, alternating-extent reloads, and the serial control.
- `[render-graph][gpu][pool]` runs the scratch generation through
  `RenderGraphActivation` in both modes on the host GPU: six captured reloads
  alternating two extents, four reloads that each retire a generation with its
  frame in flight, and a final captured reload. Every exported RGBA8 byte of the
  pooled run must equal the control's, over an image that differs from the clear
  color.
- The installed measurement consumer
  (`tests/sdk_consumer/render_graph_pooling.cpp`) is built against the installed
  SDK for the host's single target: Metal, SPIR-V, or WGSL in the browser. Run it
  natively, or in Chromium with `render-graph-pooling-browser-test.mjs`. It runs
  two workloads at the named 640×360 profile: export-only (the cooked offscreen
  generation) and scratch-2 (two scratch backings). Each workload runs first with
  the serial control, then pooled. A run has six reloads, then six alternating
  360×640 and 640×360, then six in flight, then one final reload, and then times
  120 `Encode` calls. Every captured export of the pooled run is compared with the
  control's byte for byte, and the run fails on any mismatch or on reuse where none
  is expected.

The consumer logs one `RG4 run` line per run. Its fields are:

- attachments created and reused in each phase (counts include exports);
- the pool's deferred acquisitions;
- RHI `GetAllocationTelemetry` logical bytes and, where the backend exposes them,
  API allocation bytes: sampled after each completed capture (steady) and the
  maximum sampled after staging (peak);
- idle pool bytes, measured as the telemetry drop on `TrimPool`;
- p50/p95 CPU microseconds of `Stage` (graph initialization, including pipeline
  prewarm and attachment allocation or reuse) and of `Encode`.

Measured on macOS 26.6 / Metal, Apple M5 (10 cores), Clang 23.1.3, installed
SDK built RelWithDebInfo, five consecutive runs on October 18, 2026. Counts and
bytes were identical in all five runs except where a range is shown.

| Workload | Mode | Created (reload / resize / in flight) | Reused | Deferred | Logical bytes steady / peak | Metal API bytes steady / peak | Idle pool bytes (logical / API) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| export-only | serial control | 6 / 6 / 7 | 0 | 0 | 1,843,668 / 3,686,892–5,530,536 | 1,982,932 / 3,965,420–5,948,328 | 0 / 0 |
| export-only | exact pool | 6 / 6 / 7 | 0 | 0 | 1,843,668 / 3,686,892 | 1,982,932 / 3,965,420 | 0 / 0 |
| scratch-2 | serial control | 18 / 18 / 21 | 0 | 0 | 5,530,452 / 11,060,076 | 5,948,244 / 11,895,660 | 0 / 0 |
| scratch-2 | exact pool | 10 / 8 / 7 | 8 / 10 / 14 | 0 | 12,903,252 / 14,746,476 | 13,878,100 / 15,860,588 | 7,372,800 / 7,929,856 |

Medians of the per-run p50 / p95 CPU times (µs), with the range of the per-run
p50 over the five runs in parentheses:

| Workload | Mode | Stage p50 / p95 | Encode p50 / p95 |
| --- | --- | --- | --- |
| export-only | serial control | 558 (551–578) / 750 | 17.0 (16.6–17.6) / 20.0 |
| export-only | exact pool | 564 (545–577) / 635 | 17.1 (16.5–17.8) / 18.8 |
| scratch-2 | serial control | 604 (598–618) / 641 | 38.8 (38.7–39.1) / 43.1 |
| scratch-2 | exact pool | 592 (590–603) / 658 | 38.7 (38.6–39.1) / 40.2 |

What these numbers show:

- Allocations: pooling cuts scratch-2's attachment creations from 57 to 25 over
  the 19 generations. After warm-up, each reload creates only its export.
- Memory: the cost is retained memory. Steady logical bytes rise from 5.5 MB to
  12.9 MB because both extents' scratch sets stay idle, and the sampled peak rises
  from 11.1 MB to 14.7 MB. Trimming returns the 7.4 MB idle pool.
- CPU time: there is no CPU-time claim. `Stage` is dominated by pipeline prewarm,
  and the export-only pair, which does identical work in both modes, differs by as
  much as the scratch pair. `Encode` is unchanged.
- In flight: no deferred acquisition was observed locally, because this small
  workload's frames completed before the next staging. Deferral is covered
  deterministically by the reference-backend tests, and was observed in CI below.

One run of the same consumer in the CI macOS job (GitHub `macos-15` runner,
"Apple Paravirtual device", October 18, 2026, ci run 39144503034):

| Workload | Mode | Created (reload / resize / in flight) | Reused | Deferred | Logical bytes steady / peak | Idle pool bytes (logical) | Stage p50 / p95 µs | Encode p50 / p95 µs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| export-only | serial control | 6 / 6 / 7 | 0 | 0 | 1,843,668 / 5,530,536 | 0 | 1458 / 2127 | 30.1 / 104.6 |
| export-only | exact pool | 6 / 6 / 7 | 0 | 0 | 1,843,668 / 5,530,536 | 0 | 1513 / 2457 | 35.7 / 129.0 |
| scratch-2 | serial control | 18 / 18 / 21 | 0 | 0 | 5,530,452 / 16,590,504 | 0 | 2621 / 4563 | 68.9 / 311.3 |
| scratch-2 | exact pool | 10 / 8 / 9 | 8 / 10 / 12 | 2 | 16,589,652 / 20,276,904 | 11,059,200 | 1644 / 4484 | 73.8 / 316.2 |

On this virtual device two in-flight acquisitions found only unfinished exact
matches, created new attachments instead, and left six entries idle. Exports
stayed byte identical. The single virtualized run's CPU times vary too much to
support a timing claim either way.

The same consumer also ran once in Chromium WebGPU on October 18, 2026, in the
"Renderer browser tests (pooling)" job of browser run 39166437459. It used the
pinned headless Chromium 140.0.7339.186 on a GitHub `ubuntu-24.04` runner, with
the SwiftShader adapter, which reports itself as software and exposes no adapter
name or IDs. The run built `develop` 9de35f00 plus the
one-line OBJ reader wasm32 tidy fix ([#380](https://github.com/The-Ludus-Project/Ludus/pull/380)), which does not touch the renderer.
The page was cross-origin isolated, and the smallest observed `performance.now()`
step was 5 µs, so the CPU times are quantized to 5 µs. WebGPU telemetry exposes
no API allocation bytes:

| Workload | Mode | Created (reload / resize / in flight) | Reused | Deferred | Logical bytes steady / peak | Idle pool bytes (logical) | Stage p50 / p95 µs | Encode p50 / p95 µs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| export-only | serial control | 6 / 6 / 7 | 0 | 0 | 1,843,668 / 3,686,868 | 0 | 805 / 1135 | 145 / 185 |
| export-only | exact pool | 6 / 6 / 7 | 0 | 0 | 1,843,668 / 3,686,868 | 0 | 800 / 835 | 145 / 195 |
| scratch-2 | serial control | 18 / 18 / 21 | 0 | 0 | 5,530,452 / 11,060,052 | 0 | 950 / 1040 | 280 / 365 |
| scratch-2 | exact pool | 10 / 8 / 7 | 8 / 10 / 14 | 0 | 12,903,252 / 14,746,452 | 7,372,800 | 850 / 935 | 275 / 340 |

The allocation counts and the 7.4 MB idle pool match the local Metal runs, and
every captured export of the pooled runs was byte identical to the control's. Every
peak is 24 bytes below the local Metal one. No deferred acquisition was observed: the
SwiftShader frames completed before the next staging. The scratch-2 `Stage` p50 is
100 µs lower pooled, but this is a single run on a software adapter, and the
export-only pair, which does identical work in both modes, differs by 300 µs at
p95. That supports no CPU-time claim. `Encode` is unchanged.

Platform status:

| Platform | Established | Evidence |
| --- | --- | --- |
| macOS / Metal (Apple M5; GitHub paravirtual device) | Unit and GPU byte parity, ASan/UBSan, and the measurement consumer above | Local runs, October 18, 2026; the CI macOS job runs the consumer and uploads `rg4-pooling-metal` (ci run 39144503034) |
| Linux / Vulkan (Mesa llvmpipe) | Not yet established. The GPU byte-parity cases are part of the existing `[render-graph]` validation-layer step; the measurement consumer step is pending | The consumer step and its `rg4-pooling-vulkan` artifact exist only in `develop`'s `ci.yml`. Batches and the nightly run use `main`'s workflow files, and `main` (5d8d2090 on October 18, 2026) predates RG4a, so no run has produced the artifact. A branch dispatch skips "Development and SDK", because its source-policy step compares against `main`. The numbers arrive with the first batch or nightly run after promotion |
| Browser / WebGPU (Chromium 140 SwiftShader) | Byte parity between the pooled and control runs, and the measurement consumer above, in the "Renderer browser tests (pooling)" suite | Browser run 39166437459, October 18, 2026; the suite uploads `report.json` in `renderer-browser-pooling-evidence` |

Unsupported or not established:

- No admitted cooked graph benefits (see above). Exports are never pooled.
- The depth-only compatibility RGBA8 image remains.
- Physical heap aliasing, pass merging, parallel recording and async queues are
  separate RG4 slices with their own controls.
- The Vulkan measurement is not established yet (see the platform table).
  WebGL 2 / GLSL ES is not measured. Linux and browser CI use software
  adapters, so no physical-GPU numbers will exist for Vulkan or WebGPU.
- Idle entries are bounded by count, not bytes: a large-extent workload retains
  correspondingly more memory until it is trimmed.

## Native shader worker and live development builds

Shader iteration is a development capability shared by the editor, standalone
applications and headless tooling. Slang runs in a persistent native worker;
Python retains graph/schema generation, independent layout validation and package
cooking. Compilation never requires starting the editor, GameHost or an RHI device.
The portable artifact and runtime-generation formats remain the shared boundary.

### Build and run the headless compiler

After preparing the pinned tools, select the host's native preset. For example:

```bash
cmake --preset macos-clang-development -B out/build/shader-worker \
  -DLUDUS_SHADER_WORKER_ONLY=ON -DLUDUS_BUILD_TESTS=OFF
cmake --build out/build/shader-worker --target ludus-shader-worker
cmake --install out/build/shader-worker --prefix "$PWD/out/shader-sdk"
```

Use `linux-clang-development` on Linux. `LUDUS_SLANG_ROOT` selects an already
verified Slang distribution; default is `out/shader-tools/slang`. Configuration
and compilation perform no tool downloads. The tools-only closure contains Base,
Memory, Hash, Strings, Containers, Filesystem and Parsing, plus the compiler and
client; it does not configure Qt, Platform, RHI, audio or game modules. A full SDK
can instead enable `LUDUS_BUILD_SHADER_WORKER=ON`. Native editor builds with 3D
preview enable the worker automatically.

The worker/API library supports native Linux and macOS. The pinned upstream macOS
Slang API library requires macOS 15; the compiler target advertises that deployment
minimum. Browser builds can consume cooked generations, but cannot run this native
worker or Unix-socket client. Existing offline helpers remain available, including
the older multi-entry fullscreen compatibility path.

Create a private directory for the socket, then keep the worker running:

```bash
worker_directory=$(mktemp -d)
out/shader-sdk/bin/ludus-shader-worker --serve "$worker_directory/compiler.sock"
```

The standalone prefix contains the worker and native libraries. Install the
`ludus-tools` Python CLI separately; `$SDK` below names an existing full SDK
containing the graph/schema validation helpers, and `$SLANG` its verified compiler
distribution. These are build inputs and do not initialize the engine.

In another terminal, use that endpoint explicitly (or set
`LUDUS_SHADER_WORKER_SOCKET` for the graph CLI):

```bash
ludus --json render-graph build graphs/main.lushader.json out/graph-build \
  --helper "$SDK/lib/cmake/Ludus/shaders" --compiler "$SLANG/bin/slangc" \
  --worker-socket "$worker_directory/compiler.sock" \
  --runtime-package out/render-graph/main.lurgrt --configuration Default
```

`worker_directory` must name the same directory in both terminals. CI and
packaging use the same one-shot `build` command and its exit status. Add the
pinned validator/translator arguments when the manifest declares SPIR-V or GLSL.
CMake's graph and renderer raster/compute helpers accept
`LUDUS_SHADER_WORKER_SOCKET`; for an external installed worker, also set
`LUDUS_SHADER_WORKER_EXECUTABLE` to make its binary an explicit build dependency.
When the worker target is part of the build, that dependency is automatic.
Built-in recipe selection continues to belong to
`ludus_compile_renderer_shaders` and its manifest.

For continuous source iteration, replace `build` with `watch`. It observes the
explicit source root, debounces changes, and emits JSON only for completed builds
or errors. It requires a native worker and `--runtime-package`. Stop with Ctrl-C.
The watcher preserves the last runtime file on source, compile, validation or cook
failure, and rejects source changes detected before publication. The source tree
must be separate from generated output; `out/` and hidden directories are ignored.
An unchanged failure is retried after the next source edit or a restarted command.

The native executable also accepts `--stdio`: each request/reply has a four-byte
little-endian byte length followed by versioned UTF-8 JSON, bounded to 16 MiB.
It supports multiple requests until EOF and needs no Unix socket in this mode.
`--version` reports the pinned Slang version. An occupied socket is refused;
normal termination removes the worker's socket. After a crash, use a new private
endpoint or explicitly remove the abandoned socket after confirming no worker
owns it. Clients never silently switch to an offline compiler after worker failure.

### Editor and game clients

The editor starts and connects its bundled native shader worker automatically.
For an advanced shared-worker setup, optionally set **Native shader worker** in
**Tools → Options... → Render Graph** to its endpoint. **Build & Preview**
builds/cooks an immutable draft and previews a
successful result only if its project/request and semantic source/configuration
identity still match. Native World rendering enables **Live Preview** automatically for
background builds after graph/shader saves;
layout-only moves do not schedule a build. Saving is independent. **Cancel Render
Graph Job** disables live building; failures retain the last drawable preview. The existing preview's
static-material/profile restrictions still apply.

The World viewport follows project graph sources through **Live Preview**. It does
not require users to distinguish engine recipe reload from project shader reload.
The engine's rendered editor setup includes its worker automatically; endpoint
preferences remain available for advanced shared-worker setups. See
[Start customizing the renderer](#start-customizing-the-renderer).

Standalone native games can link `Ludus::ShaderDevelopment` and create a
`ludus::runtime::shader_development::ReloadSession` over their producer's final
`.lurgrt` path and existing `RenderGraphActivation`. `Initialize` pins the output
directory and starts one dedicated file-read worker. Call `Service` between frames
with a monotonic timestamp, then drive the activation's existing `Poll` and
`Update` protocol. Metadata observation/open can access disk; shader payload reads
are asynchronous. A newer publication cancels/drains the old read before its
buffer is reused. Failed reads/decodes never stage a partial generation. Inspect
`ReloadReport` separately from the activation report and presented image.

Applications needing direct source compilation can link
`Ludus::ShaderCompilerClient`. `Client::Submit` copies an immutable set of relative
source paths/texts plus target/stage/entry and a caller-owned request identity;
`Poll` advances nonblocking IPC. Cancel discards the reply locally. Callers validate
the echoed identity, reflection and their own interface contract before creating
replacement resources. This lower-level API does not bypass graph admission or
pipeline readiness. `Ludus::ShaderCompiler` is the optional in-process API library
for tools or applications that explicitly choose to embed the compiler. Its
synchronous `Compile` belongs on a dedicated worker thread; one compiler instance
is confined to one thread. Neither client library initializes an engine/editor.
The standalone install exposes these components through `LudusShaderTools`:

```cmake
find_package(LudusShaderTools REQUIRED COMPONENTS ShaderCompilerClient)
target_link_libraries(my_development_tool PRIVATE Ludus::ShaderCompilerClient)
```

Request `ShaderCompiler` and link `Ludus::ShaderCompiler` when embedding the API.
Target reflection includes `nativeEntryPointResourceUsage`, a map from global
parameter names to booleans queried from Slang's generated-code metadata. Slang's
API JSON alone omits the CLI's per-entry usage records; consumers must retain this
metadata when deriving live resource bindings. The graph and recipe adapters
validate it before comparing emitted target layouts.

The worker copies at most 64 source files and 4 MiB of source bytes into a closed
in-memory Slang filesystem. It cannot fall back to source files on disk. It retains
one global Slang session and reuses its latest module/session only while the entire
snapshot and root source are identical. Any changed file creates a fresh session;
objects from the old session are released first. Sequential requests avoid sharing
non-reentrant Slang state between threads. Outputs are bounded to 2 MiB each for
artifact/reflection; diagnostics to 64 KiB. The graph cache includes native worker
implementation identity and the actual API-library digest alongside existing
source/schema/tool identities. Native and command-line compiler cache entries are
distinct; a worker replacement during a graph build is rejected.

### Portable artifacts and device compilation

SPIR-V and DXIL are intermediate representations themselves. Slang's serialized
modules are an earlier, compiler-specific IR that can preserve frontend work;
they are not device machine code. This worker emits SPIR-V, WGSL or MSL as selected
by the caller. Existing RHI shader/pipeline creation still lets the graphics
implementation compile for the actual adapter and driver before activation.
Measure that device preparation separately from source compilation.

A future server can distribute portable artifacts or compiler modules, and may
also distribute validated device-cache blobs for matching hardware/driver keys.
Vendor name alone is insufficient: Vulkan's pipeline-cache header includes vendor
ID, device ID and a pipeline-cache UUID. Retain portable-artifact fallback when
opaque cache data is rejected. No device-cache server or portable vendor-machine-
code format is introduced here. See the [Vulkan pipeline cache contract](https://docs.vulkan.org/spec/latest/chapters/pipelines.html#pipelines-cache)
and [Slang's compilation API and sessions](https://docs.shader-slang.org/en/latest/compilation-api.html).

### Benchmark and acceptance

`tools/shader-bench/benchmark.py` measures real CLI builds without a device. Run
it once without `--worker` and once with `--worker /path/to/ludus-shader-worker`,
using different empty `--output` directories and the same pinned `--compiler`.
It writes raw JSON samples under the selected ignored output directory. The
native run starts one worker and retains it for all samples; compiler startup
is outside per-request times. Cold refers to an empty artifact cache, not flushed
OS caches. Both runs use a fresh Python CLI process per sample.

On October 18, 2026, macOS arm64, pinned Slang 2026.1.2, seven samples per case,
three static-profile programs and Metal/WGSL targets, the pre-implementation
baseline measured 1402.90 ms for an empty artifact cache and 735.72 ms for one
shader edit. Repeating both paths after implementation produced:

| Case | Existing CLI median | Persistent native median |
| --- | ---: | ---: |
| Empty artifact cache | 1595.25 ms | 888.27 ms |
| Cached artifacts | 399.29 ms | 390.52 ms |
| Single shader edit | 734.33 ms | 562.72 ms |
| Shared include edit | 741.80 ms | 601.58 ms |
| Invalid shader edit | 389.79 ms | 270.37 ms |

The final native run's one-time worker startup was 522.60 ms, measured
separately. An installed, relocated tools-only consumer also compiled WGSL using
only installed headers/libraries. ASan/UBSan passed the compiler and socket tests.

These small-fixture wall times do not establish large-project throughput or GPU
pipeline latency. Graph preparation still invokes Python validation helpers, and
the first worker has one compilation lane. Cache misses are currently sequential.

The native API tests cover deterministic output, source/include invalidation,
all three targets, closed inputs and diagnostics. The worker integration test
uses the real native client and compiler to cover cancellation, malformed and
abandoned connections, cached graph builds, changed shaders, compile failures,
occupied endpoints and worker exit. The dedicated `ludus_shader_reload_gpu` test
passes worker-produced generations through the native game loader, driver
pipeline preparation and real pixel readback; it checks changed pixels, failed
publication preservation, superseded reads and interface-mismatch preservation.
Before asynchronous driver preparation, the first local Metal run's
changed-generation read/preparation/activation took 65.3 ms, and its largest
delivery-service call, including initial driver preparation, took 251.3 ms.

Activation now explicitly queues native shader compilation, reflection checks and
graphics/compute pipeline creation on a dedicated preparation lane. The owner
thread consumes completions through normal RHI polling; it never waits for a
replacement's driver compilation. The old generation stays active until every
replacement pipeline is Ready and the latest accepted scene extraction is ready.
Superseding a candidate detaches it immediately; its native dependencies remain
retained until late completions can be retired safely. CPU interface mismatch,
queue exhaustion, allocation failure and native compile failure leave the active
view intact. RHI shutdown drains preparation before releasing device resources.
Browser backend policy is preserved; see the
[native RHI preparation contract](fullscreen-rendering.md#portable-raster-r1).

The strengthened Metal regression pauses actual pipeline work on its worker,
draws eight frames from the previous generation, supersedes that blocked
candidate, and then admits the successful replacement. It also corrupts a valid
MSL artifact to trigger a native asynchronous compiler failure and verifies the
active green image survives. In the first local run of that regression, the
largest delivery-service call took 0.371 ms, and replacement processing took
24.67 ms including deliberately paused frames and supersession. These are single
observations with different workloads, not percentile estimates or a comparable
end-to-end speedup against the earlier synchronous run. Owner-side file observation,
validation, allocation, completion collection and scene updates still have costs;
no universal frame-time bound is claimed. The test reports service/pipeline timing
separately from source compilation and refuses a skipped GPU case. Linux, browser
and physical-device qualification remain distinct from any local macOS result.
