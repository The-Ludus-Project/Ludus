# Point and line direct lighting requirements

Status: proposed, 2026-10-27. Baseline `origin/develop` `02df0b37`. Implement
with AGENTS.md/steering, [design](design.md) and [tasks](tasks.md). The
[compute ray-tracing spec](../compute-ray-tracing/requirements.md) and the
canonical [render graph authoring guide](../../../docs/development/render-graph-authoring.md)
remain authoritative for everything this package does not change. PL01-PL15 are
normative. File paths, functions, constants and templates named here are
deliverables, not existing capabilities.

This package adapts Katherine Salesin and Wojciech Jarosz, "Combining Point and
Line Samples for Direct Illumination", *Computer Graphics Forum* 38(4)
(Proceedings of EGSR 2019), pp. 159-169, https://doi.org/10.1111/cgf.13779,
and the shadow-triangle visibility routine of Niels Billen and Philip Dutré,
"Line Sampling for Direct Illumination", *Computer Graphics Forum* 35(4)
(Proceedings of EGSR 2016), pp. 93-102, https://doi.org/10/f84z2h (the short
DOI given in the 2019 paper's references). Design section 2 records what is adopted
and where Ludus departs.

## PL01 Scope

Scene compute and ray passes SHALL be able to estimate first-hit direct lighting
from emissive triangles with line samples, and to combine line samples with BSDF
point samples and with each other through multiple importance sampling (MIS).

- Line visibility SHALL be a scene query at the shared traversal boundary
  (PL02), not code that each author copies into callbacks.
- Line samples SHALL apply to mesh emitters only. Directional, point, spot and
  environment sources keep the existing point light proposal (PL08).
- Every technique this package delivers SHALL ship as a built-in render graph
  template (PL10): a line-sampling template with the query (PL1) and a
  point-and-line MIS template with the combined estimator and comparison
  outputs (PL2).

Out of scope (PL15): native or hardware line traversal, browser execution, CPU
tracing, indirect bounces, solid-angle line sampling, chords across several
coplanar emitter triangles, the discontinuity-smoothing heuristic until a
stratified sampler exists, and any change to the RT1 reference path tracer.

The templates are render graph direct-lighting estimators, not a reference path
tracer. They do not change the RT1 `CpuOracle` or GPU-compute reference, so the
AGENTS.md CPU and GPU reference path tracing rule does not apply to this
package. It applies in full to the PL15 reference item.

## PL02 Line visibility query

`scene_shader.slang` SHALL expose:

```slang
uint FindVisibleLineIntervals(SceneContext scene,
                              inout RayQueryContext queries,
                              SurfaceHit receiver,
                              LineSegment segment,
                              out LineVisibility outVisibility);
```

- `LineSegment` names world-space `StartWorld` and `EndWorld` and the packed
  `TriangleIndex` of the emitter triangle that carries the segment, or
  `SCENE_INVALID_INDEX` for a segment carried by no scene triangle.
- `LineVisibility` holds the visible intervals of the segment as parameters in
  `[0, 1]` from `StartWorld` to `EndWorld`, their count, the total visible
  length in metres, and `HasMaskedOccluders` (PL05).
- The result is `SCENE_QUERY_MISS` when no opaque occluder removes any part of
  the segment, `SCENE_QUERY_SURFACE` when at least one does, and
  `SCENE_QUERY_FAILURE` otherwise (PL12). A fully occluded segment returns
  `SCENE_QUERY_SURFACE` with zero intervals.
- On failure `outVisibility` SHALL be zero-initialized and `queries.HasFailed`
  SHALL be latched, as for `TraceClosest` and `TestVisibility`.
- The query SHALL be available to kernel passes and to every callback of a
  shared callback set, under either traversal strategy (PL06).

## PL03 Geometric semantics

The query SHALL find, for every point `l` of the segment, whether the open path
from the receiver to `l` crosses an opaque scene triangle. Its geometry SHALL
match `TestVisibility` as follows:

- The apex is the receiver position offset with `OffsetPositionWorld` exactly
  as `SpawnSurfaceRay` does for the direction to the segment midpoint. Segment
  endpoints are offset toward the receiver as `TestVisibility` offsets a light
  endpoint on `segment.TriangleIndex`.
- The receiver triangle and `segment.TriangleIndex` SHALL be excluded by index.
  A ray leaving a planar triangle cannot cross that plane again, so the
  exclusion is exact.
- An occluder triangle whose plane coincides with the shadow-triangle plane
  within the triangle loop's parallel tolerance
  (`TRIANGLE_PARALLEL_COSINE_TOLERANCE`) SHALL NOT occlude, matching the loop's
  rejection of rays parallel to a triangle.
- Occluders behind the receiver or beyond the segment SHALL NOT occlude.
- Degenerate inputs SHALL fail (PL12): a segment shorter than the scene unit
  tolerance, nonfinite positions, or a receiver in the segment's line.

## PL04 Interval bounds

- `SCENE_LINE_INTERVAL_CAPACITY` SHALL bound the visible intervals in one
  result. It is a named constant beside the scene shader layout and shared with
  C++ through `scene_shader_layout.h`. Its value is decided by PL0 evidence:
  32 ([design section 4.2](design.md#42-capacity)).
- Intervals SHALL be sorted, disjoint and nonempty.
- Occluded intervals separated by a gap no larger than
  `SCENE_LINE_INTERVAL_MERGE_TOLERANCE` (a fraction of the segment, chosen in
  PL0) SHALL merge, and visible intervals no longer than it SHALL be dropped.
  This rule only shrinks the visible set, so abutting occluder triangles never
  leak light through a numerical crack.
- When the running set of occluded intervals would exceed the capacity, the
  query SHALL fail. It SHALL NOT drop, merge or otherwise approximate intervals
  beyond the tolerance rule.

## PL05 Alpha policy

- The interval set SHALL contain only opaque occluders. A triangle whose
  material is alpha-tested SHALL NOT be clipped.
- When such a triangle intersects the shadow triangle between the receiver and
  the segment, the result SHALL set `HasMaskedOccluders`.
- A line strategy that draws a point from a result with `HasMaskedOccluders`
  SHALL confirm that point with `TestVisibility` and use its result as the
  point's visibility. PDF evaluation (PL09) SHALL use the opaque-only intervals
  in every case, so sampling and evaluation agree.
- When the scene's opaque flag is set, `HasMaskedOccluders` SHALL be false
  without examining materials.
- Treating masked triangles as opaque, or rejecting scenes that contain them,
  SHALL NOT be the policy. Design D4 explains why.

## PL06 Traversal and selection

- The line query SHALL use software traversal of the packed World triangles in
  every pass. A `NativeQuery` pass uses native rays for `TraceClosest` and
  `TestVisibility`, and the same software line traversal.
- Calling the line query SHALL NOT change whether a package requires hardware
  ray tracing. A configuration whose ray passes all select `TriangleLoop`
  remains a `ComputeRayTracing` package.
- The first traversal SHALL scan all triangles. A bounding volume hierarchy
  (BVH) traversal over the packed nodes MAY replace it (PL3) only with
  identical results on fixtures that do not overflow. When it does, the scan
  SHALL remain selectable for comparison through an optional scene-interface
  input that is not a plan or package field (design D5, D9).
- No path SHALL add CPU tracing or a native line query.

## PL07 Budgets and package compatibility

- The scene interface SHALL accept an optional `line_queries` budget beside
  `surface_queries` and `visibility_queries`, with an absent value meaning 0
  and a bounded maximum declared with the other query limits. A query beyond
  the budget SHALL fail (PL12).
- Existing source graphs, cooked plans and runtime packages SHALL keep their
  bytes and behavior. The budget is compiled into the generated wrapper, so no
  plan `VERSION` or `PACKAGE_VERSION` change is expected. If PL1 finds one is
  required, it SHALL be a separate, documented version bump.
- `SCENE_SHADER_INTERFACE_VERSION` SHALL stay 1 unless a change alters an
  existing declaration. Added declarations follow the additive precedent of
  earlier scene API extensions.

## PL08 Line strategies on triangle emitters

For a selected mesh light, a line strategy SHALL:

1. draw an area-uniform point on the emitter with the existing
   `SampleIncidentLight` proposal (triangle by area, then barycentric);
2. form the chord of that point's triangle through the point, in the strategy's
   direction: `U` along the triangle's first edge, `V` perpendicular to it in
   the triangle's plane. A chord shorter than the scene unit tolerance, such as
   the zero-length chord through a vertex whose angle excludes the direction,
   SHALL contribute zero without a line query and without failing (design D6,
   section 5.1);
3. find the chord's visible intervals with `FindVisibleLineIntervals`;
4. draw a point uniformly by length over the visible intervals, or contribute
   zero when none remain;
5. estimate with the joint area density
   `p_A(l) = P(light) * c(l) / (A_light * L_visible(l))`, where `c` is the
   chord length, `A_light` the emitter area and `L_visible` the chord's visible
   length, converted to solid angle by `distance^2 / |cos_light|`.

For a selected non-mesh light, a line strategy SHALL use the existing point
proposal (`SampleIncidentLight`, `TestVisibility`) and its density. Emission is
evaluated at the drawn point, so textured emission stays unbiased, though not
importance sampled.

## PL09 Line density evaluation for MIS

Evaluating a line strategy's density at a point `l` on emitter triangle `T`
SHALL form the chord of `T` through `l` in that strategy's direction, run the
line query and apply PL08 step 5. If `l` lies in no visible interval of that
chord, or the chord is shorter than the scene unit tolerance (PL08 step 2), the
density SHALL be zero. Each evaluation with a line query counts it against the
budget.

## PL10 Templates

Two built-in render graph templates SHALL ship. Each SHALL reuse the
[compute ray-tracing template's](../compute-ray-tracing/design.md#d5-template-two-passes)
primary pass and 32-byte hit record, and give each output its own
Accumulate → ToneMap → Export chain.

**`line-sampling`** (library configuration `LineSampling`, PL1). It is the
query's end-to-end example, and the surface-area line estimator of Billen and
Dutré in one direction:

| Pass | Estimate | Output |
| --- | --- | --- |
| `shade` | Emission plus one line `U` sample | `color` (required) |
| `shadeVisibleFraction` | Visible fraction `L_visible / c` of the same chord, as gray radiance | `visibleFraction` (optional) |

**`salesin-jarosz-lines`** (library configuration `PointLineMis`, PL2). Three
shading passes, four passes in total (`MAX_RAY_PASSES`):

| Pass | Strategies | Output |
| --- | --- | --- |
| `shade` | BSDF + line `U` + line `V`, MIS-weighted | `color` (required) |
| `shadeLines` | line `U` + line `V`, MIS-weighted | `lineSampling` (optional) |
| `shadePoints` | BSDF + area point light sample, MIS-weighted | `pointSampling` (optional) |

Its heuristic is a Graph setting admitting balance and power with its exponent.
All weights SHALL sum to one for every sampled point where the integrand is
nonzero.

Both templates:

- SHALL select `TriangleLoop` on every pass, so they need Metal or Vulkan
  compute but no hardware ray tracing, and get Vulkan evidence on llvmpipe;
- SHALL be available everywhere existing ray templates are: as a graph template
  (New Render Graph → Template and `ludus render-graph new --template`), as a
  snippet and configuration template (Quick Add and Apply Configuration
  Template), and as installed SDK template files;
- SHALL declare their requirement accurately in the template registry. They
  SHALL NOT claim `native-ray-tracing`. If the registry vocabulary cannot say
  "Metal or Vulkan compute", PL1 SHALL add that requirement value and label.

## PL11 Determinism and sampling

The shading passes SHALL draw from explicit, documented dimensions following
compute ray-tracing D4, with one shared layout for all three passes, so each
output's samples are comparable. The line query SHALL draw no random
dimensions. Its result SHALL be a deterministic function of the receiver, the
segment and the packed scene.

## PL12 Failure and lifetime

Invalid input, a degenerate segment, capacity overflow, budget exhaustion, a
nonfinite intermediate value and an exceeded BVH stack (PL3) SHALL latch
`queries.HasFailed` and produce the existing magenta error pixel. Pending or
failed candidates SHALL preserve the last drawable view, as for every ray
package.

## PL13 Evidence

There SHALL be:

- Python builder tests for the `line_queries` budget and both templates;
- CLI tests creating each template with `ludus render-graph new --template`,
  and editor tests creating each from New Render Graph, Quick Add and Apply
  Configuration Template. Graph settings edits SHALL be tested from the canvas
  with Properties hidden, as AGENTS.md requires of every Properties operation;
- `line-sampling` GPU tests: `color` converges to the existing
  `veach-guibas-mis` `lightSampling` mean on the same scene, and
  `visibleFraction` is 1 where unoccluded and 0 in umbra;
- GPU tests of the line query against analytic intervals: one occluder, two
  overlapping occluders, an occluder mesh whose triangles abut (no crack), a
  coplanar occluder, occluders behind the receiver and beyond the segment, a
  masked occluder, capacity overflow, budget exhaustion;
- GPU template tests: all three outputs converge to one mean, and after a fixed
  sample count the `color` output has lower mean squared error than
  `pointSampling` in a penumbra region, against an independently seeded
  reference;
- an installed SDK consumer fixture that cooks a pass calling the line query;
- PL3: identical intervals from the triangle loop and the BVH traversal on the
  same fixtures.

GPU tests use `TriangleLoop` passes and SHALL NOT skip for a missing native ray
profile. Results SHALL report the device. Timing is reported as measurement,
not as a claim that line samples are faster.

## PL14 Documentation and attribution

The canonical guide SHALL document the line query, its bounds and alpha policy,
the budget and each template in the same changes that add them, beside the
existing scene API and MIS template sections. Each template SHALL have a guide
section like "Compare light and BSDF sampling with MIS": creation commands,
passes, outputs, settings, evidence and limits. Shader sources SHALL credit
Salesin and Jarosz (2019) for the line strategy and MIS framing, Billen and Dutré
(2016) for the shadow-triangle intersection, and Veach and Guibas (1995) for the
heuristics, with the departures listed in design section 2.

## PL15 Deferred work

Each item needs its own reviewed change:

- **Solid-angle line sampling.** Ureña et al.'s spherical-rectangle method
  needs parallelogram emitters; the paper limits it to edge-aligned lines.
- **Planar chords.** Chords across all coplanar triangles of one emitter.
- **Discontinuity smoothing.** The paper's smoothing heuristic (its section
  4.3) improves convergence rate only under stratified sampling, which the
  scene random stream does not provide.
- **Native and browser.** A hardware line query (no API exposes one) and
  browser execution.
- **Path capture.** Recording line queries in ray path capture.
- **Reference path tracer.** Line sampling in RT1 needs the CPU oracle and the
  GPU-compute reference to change together, with matched-fixture parity
  evidence, as AGENTS.md and the
  [renderer contract](../../../docs/architecture/renderer-systems.md#cpu-and-gpu-reference-implementation-requirement)
  require. A CPU oracle line query would also give PL1 an independent check.
