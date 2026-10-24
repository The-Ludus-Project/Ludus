# Compute ray tracing requirements

Status: proposed implementation, 2026-10-24. Baseline `origin/develop`
`dbbf7461`. Implement with AGENTS.md/steering, [design](design.md) and
[tasks](tasks.md). C01-C14 are normative. File paths, node shapes and APIs in
this package are deliverables, not existing capabilities.

## C01 Scope

A `ComputePipeline` node that declares a Scene input SHALL become an executable
*scene compute pass*: an authored compute function that traces the selected
World's static opaque triangles with the renderer's native inline ray queries.
A `ComputePipeline` without a Scene input SHALL keep returning the existing
`Capability` diagnostic. Generic compute, storage textures, indirect dispatch,
software BVH traversal and browser ray execution SHALL remain out of scope.

## C02 Hardware and selection

Scene compute passes SHALL require the same native ray profile as
`RayTracingPipeline` nodes (static triangles plus inline queries, Vulkan
`VK_KHR_ray_query` or Metal Apple GPU family 9+). A package containing one SHALL
be a ray package for `SelectRenderGraphRenderer`, `StageSelected` and the
editor's automatic/Force raster selection. No path SHALL add CPU or shader-based
software traversal.

## C03 Authored function

A scene compute program SHALL use the manifest stage `compute`, input
`RayLaunch` and output `Radiance`, with the authored signature
`float3 Name(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)`.
It SHALL have access to the complete `scene_shader.slang` API except
`TraceAndShade`, which has no hit/miss callbacks in a compute pass. A call to it
SHALL latch `queries.HasFailed`. Its bindings array SHALL stay empty; the
generated wrapper supplies scene resources and slots.

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

## C06 Sequencing

Scene compute and `RayTracingPipeline` passes SHALL share one ordered sequence
of at most four passes per configuration. They SHALL be consecutive in the
lowered plan, use one scene interface (query budgets, payload) and keep the
existing RAW/WAR/WAW hazard tracking for color and Buffer versions. Mixed
sequences SHALL be allowed. The rules for all-visibility versus all-ordinary
sequences SHALL be unchanged.

## C07 Post-processing

Accumulate, Denoise and ToneMap SHALL accept a scene compute pass as their
producer under the rules that apply to ray passes, including fusion into the
producing dispatch.

## C08 Package compatibility

Existing source graphs, cooked plans and runtime packages SHALL keep their bytes
and behavior. If a scene compute pass can be represented by the existing
numeric `Technique::RayTracing` record (design D1), the plan format and package
versions SHALL stay unchanged. Otherwise both SHALL be bumped, with explicit
rejection of the new records by older decoders.

## C09 Determinism and sampling

Two passes in one sequence SHALL NOT consume correlated random dimensions
unless the author asks for it explicitly. The template SHALL use explicit
per-pass dimension offsets (design D4), and the authoring docs SHALL state the
seeding contract.

## C10 Template

A built-in `compute-ray-tracing` graph template and snippet SHALL ship. They
SHALL consist of a primary-hits scene compute pass, which writes a per-pixel hit
record into a render-size `Buffer`, and a trace & shade scene compute pass,
which reconstructs the surface and computes direct lighting. The chain ends in
explicit Accumulate → ToneMap → Export. The template SHALL carry the
`native-ray-tracing` requirement and the `ray-traced` graph category.

## C11 Editor authoring

The editor SHALL author scene compute nodes without hand-editing JSON. That
covers adding Scene/View/Buffer ports, the Launch size control, Automatic group
size, scene-aware shader creation with the C03 stub, and the shader interface
panel. Quick Add, New Graph and Apply Configuration Template SHALL offer the
template from the registry.

## C12 Failure and lifetime

Pending or failed candidates SHALL preserve the last drawable view. Invalid hit
records, budget exhaustion and failed queries SHALL produce the existing magenta
error pixels. Buffer zero-initialization SHALL never decode as a valid hit.

## C13 Evidence

There SHALL be Python builder tests, editor tests, a GPU pixel test that skips
only on `Unsupported` ray capability, raster-fallback selection tests, and an
installed SDK consumer fixture. All of these run in addition to the existing
ray suites, which SHALL keep passing unchanged.

## C14 Documentation and attribution

The canonical guide `docs/development/render-graph-authoring.md` SHALL document
scene compute passes and the template in the same changes that add them. It
SHALL replace the "ComputePipeline is not executable" statements and keep the
public API Doxygen contracts current. Shader sources SHALL credit the papers
they adopt.
