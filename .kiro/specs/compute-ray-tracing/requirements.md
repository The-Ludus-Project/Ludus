# Compute ray tracing requirements

Status: proposed implementation, 2026-10-24. Baseline `origin/develop`
`dbbf7461`. Revised 2026-10-26 against `origin/develop` `c3312612` for the
traversal strategies added after C4 by #619 and #621 (software compute
traversal); see [design D8](design.md#d8-traversal-strategy-added-after-c4).
Revised again 2026-10-27 against `bf909fe0`: #626 had put MIS into the C10
template, `01a50759` reverted that, and #635 added MIS as the separate
`veach-guibas-mis` template (design D9). Implement with
AGENTS.md/steering, [design](design.md) and [tasks](tasks.md). C01-C14 are
normative. File paths, node shapes and APIs in this package are deliverables,
not existing capabilities.

## C01 Scope

A `ComputePipeline` node that declares a Scene input SHALL become an executable
*scene compute pass*. It traces the selected World's static opaque triangles
through the pass's traversal strategy (C02): native inline ray queries, or the
renderer's `TriangleLoop` scan in ordinary GPU compute. A `ComputePipeline`
without a Scene input SHALL keep returning the existing `Capability`
diagnostic. Generic compute, storage textures, indirect dispatch, a software
acceleration structure (BVH) traversal, CPU tracing and browser ray execution
SHALL remain out of scope.

## C02 Traversal, hardware and selection

Each ray pass SHALL carry an explicit traversal requirement, `NativeQuery` or
`TriangleLoop`.

- A `NativeQuery` pass SHALL require the native ray profile: static triangles
  plus inline queries, Vulkan `VK_KHR_ray_query` or Metal Apple GPU family 9+.
- A configuration whose ray passes all select `TriangleLoop` SHALL run on
  native Metal or Vulkan devices that meet the ordinary compute/storage
  admission, including GPUs without RT hardware and Vulkan software adapters.
  It SHALL create no acceleration structures and emit no ray-query instructions.
- `RequiresHardwareRayTracing` SHALL be true exactly when a plan contains a
  `NativeQuery` ray pass. Activation SHALL report `HardwareRayTracing` for such
  a package and `ComputeRayTracing` for a software-only one.
- `SelectRenderGraphRenderer`, `StageSelected` and the editor's
  Automatic/Force raster selection SHALL fall back to the raster package only
  when a required native profile is unavailable, or under Force raster.
  Selection SHALL NOT change an authored traversal strategy. An explicit
  `NativeQuery` on a device without native rays SHALL return `Unsupported`, not
  silently become `TriangleLoop`.
- No path SHALL add CPU ray tracing.

## C03 Authored functions

A scene compute node SHALL hold either of these:

- **One kernel.** The manifest stage is `compute`, the input `RayLaunch` and the
  output `Radiance`, with the authored signature
  `float3 Name(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)`.
  It has access to the complete `scene_shader.slang` API except
  `TraceAndShade`, which has no hit/miss callbacks in a kernel pass. A call to
  it SHALL latch `queries.HasFailed`.
- **A shared callback set.** The complete `ray_generation`, `miss` and
  `closest_hit` set, and no `compute` shader. The same manifest programs MAY
  also be assigned to a `RayTracingPipeline`. `TraceAndShade` then dispatches
  these callbacks.

Bindings arrays SHALL stay empty; the generated wrapper supplies scene resources
and slots. Callback sources, `ScenePayload` and query budgets SHALL NOT depend
on the traversal strategy.

## C04 Node contract

A scene compute node SHALL declare exactly one Scene and one View input, exactly
one RGBA8Unorm ColorTexture writable with Clear/Store, up to two `Buffer`
writables with Load/Store, and optionally the visibility/depth input pair. The
shapes and limits SHALL match `RayTracingPipeline`. `StructuredBuffer` inputs
SHALL be rejected with a repair message naming `Buffer`.

## C05 Invocation and state

Scene compute nodes SHALL accept `"dimensions": "ColorTarget"` and an exact
explicit `[width, height, 1]`, with the same diagnostics as ray nodes. New scene
compute nodes SHALL use `"state": {}`. The group size is the wrapper's fixed
64×1×1. An authored `group_size` other than `[64, 1, 1]` SHALL be rejected with a
repair that clears it. Existing generic compute drafts SHALL be read and saved
unchanged.

Ray and scene compute nodes SHALL accept an optional `invocation.traversal`
of `TriangleLoop` or `NativeQuery`. Omitting it means Automatic:

- a shared callback set in a ComputePipeline resolves to `TriangleLoop`;
- a kernel pass and a `RayTracingPipeline` resolve to `NativeQuery`.

`traversal` on any other node SHALL be a `Capability` error. The editor SHALL
expose it as Invocation → Ray traversal in both Properties and the canvas
pipeline editor.

## C06 Sequencing

Scene compute and `RayTracingPipeline` passes SHALL share one ordered sequence
of at most four passes per configuration. They SHALL be consecutive in the
lowered plan, use one scene interface (query budgets, payload) and keep the
existing RAW/WAR/WAW hazard tracking for color and Buffer versions. Mixed
sequences SHALL be allowed, including passes that select different traversal
strategies. The strategy SHALL be part of the cooked callback variant, so passes
sharing program IDs cannot overwrite each other's strategy. The rules for
all-visibility versus all-ordinary sequences SHALL be unchanged.

## C07 Post-processing

Accumulate, Denoise and ToneMap SHALL accept a scene compute pass as their
producer under the rules that apply to ray passes, including fusion into the
producing dispatch, whatever its traversal strategy.

## C08 Package compatibility

Existing source graphs, cooked plans and runtime packages SHALL keep their bytes
and behavior.

- Scene compute passes themselves are represented by the existing numeric
  `Technique::RayTracing` record (design D1) and needed no version change.
- The traversal requirement is a per-pass field. It is carried by plan format
  `VERSION` 14 (`PASS_FIELD_COUNT` 20) and runtime package
  `SOFTWARE_RAY_PACKAGE_VERSION` 18.
- Packages before version 18 SHALL decode every ray pass as `NativeQuery`.
- A traversal value other than the two strategies, or a nonzero value on a
  non-ray pass, SHALL decode as `Malformed`.

## C09 Determinism and sampling

Two passes in one sequence SHALL NOT consume correlated random dimensions
unless the author asks for it explicitly. The template SHALL use explicit,
disjoint per-pass dimension ranges (design D4), and the authoring docs SHALL
state the seeding contract. Changing a pass's traversal SHALL NOT change which
random dimensions it draws.

## C10 Template

A built-in `compute-ray-tracing` graph template and snippet SHALL ship. They
SHALL consist of two scene compute passes, then explicit
Accumulate → ToneMap → Export:

- a primary-hits pass, which writes a per-pixel hit record into a render-size
  `Buffer`;
- a trace & shade pass, which reconstructs the surface and shades emission
  plus one shadowed light sample.

The template's kernels SHALL keep the native default traversal, so it carries
the `native-ray-tracing` requirement and the `ray-traced` graph category. A
`TriangleLoop` selection on both passes is an author choice and is not required
of the template.

Other shading policies SHALL ship as separate templates that reuse the hit
record, not as changes to this one. The first is `veach-guibas-mis` (#635,
design D9): a primary-hits pass and three shading passes for MIS, light sampling
alone and BSDF sampling alone.

## C11 Editor authoring

The editor SHALL author scene compute nodes without hand-editing JSON. That
covers:

- adding Scene/View/Buffer ports;
- the Launch size control and Automatic group size;
- the Ray traversal choice;
- scene-aware shader creation with the C03 stub, or assigning a shared
  callback set;
- the shader interface panel.

Quick Add, New Graph and Apply Configuration Template SHALL offer the template
from the registry.

## C12 Failure and lifetime

Pending or failed candidates SHALL preserve the last drawable view. Invalid hit
records, budget exhaustion and failed queries SHALL produce the existing magenta
error pixels under either traversal strategy. Buffer zero-initialization SHALL
never decode as a valid hit.

## C13 Evidence

There SHALL be:

- Python builder tests;
- editor tests;
- GPU pixel tests;
- raster-fallback selection tests;
- an installed SDK consumer fixture.

A native-query GPU pixel test SHALL skip only on `Unsupported` ray capability. A
`TriangleLoop` GPU test SHALL NOT skip for a missing native ray profile, and
SHALL run on a device without one where available. All of these run in addition
to the existing ray suites, which SHALL keep passing unchanged.

## C14 Documentation and attribution

The canonical guide `docs/development/render-graph-authoring.md` SHALL document
scene compute passes, traversal strategies and the template in the same changes
that add them. It SHALL replace the "ComputePipeline is not executable"
statements and keep the public API Doxygen contracts current. Shader sources
SHALL credit the papers they adopt, including Möller and Trumbore (1997) for the
triangle test, and Veach and Guibas (1995) for MIS in the `veach-guibas-mis`
template.
