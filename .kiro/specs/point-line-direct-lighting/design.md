# Point and line direct lighting design

Status: proposed, 2026-10-27. Baseline `origin/develop` `02df0b37`. PL0
evidence recorded 2026-10-27 on `431613a2` (section 4). D5 revised 2026-10-29 for
PL3 on `c08ffec7`. Large-scene equal-time measurement recorded 2026-10-29 on
`d00cd9eb` (section 5). This describes work to
build. Read [requirements](requirements.md) and
[tasks](tasks.md). AGENTS.md, steering, ADR 0003, the
[compute ray-tracing spec](../compute-ray-tracing/design.md) and the canonical
[render graph authoring guide](../../../docs/development/render-graph-authoring.md)
remain authoritative. Line references drift; symbol names are the stable anchors.

## 1. What already exists

- **Scene compute passes.** A `ComputePipeline` with a Scene input runs one
  authored kernel, or a shared callback set, per pixel. Up to four passes share
  one ordered sequence, `GraphBuffer0/1` and one scene interface (compute
  ray-tracing D1-D3, C06).
- **Traversal boundary.** `TraceClosest`, `TestVisibility` and `TraceAndShade`
  call `Trace` in `renderer_gpu_preview.slang`. It dispatches to
  `TraceNativeQuery` (inline `RayQuery`) or `TraceTriangleLoop`
  (`ray_triangle_loop.slang`) according to the pass's `RayTraversal`
  (compute ray-tracing D8). The query functions own budgets and failure
  latching. Callbacks never see the strategy.
- **Scene data a kernel can read.** `GetTriangle`, `GetEmitterTriangle`,
  `GetEmitter`, `GetLightInfo`, `SelectLight`, `EvaluateLightSelectionPmf`
  (uniform), `SampleIncidentLight`, `EvaluateLightPdfSolidAngle`,
  `EvaluateEnvironmentPdfSolidAngle`, `EvaluateEmission`, `EvaluateMaterial`,
  `EvaluateStandardBsdf`, `SampleStandardBsdf`, `SpawnSurfaceRay` and the
  explicit-address `SampleSceneRandom` overloads (`scene_shader.slang`).
- **Packed BVH.** `PackGpuScene` (`gpu_preview_services.h`) writes the
  `ReferenceBvhNode` array at `Offsets.x` (three `float4` words per node: lower
  bound and one child index, upper bound and the other, triangle range) and `Order`
  at `Offsets.y`. `TraceScene` in `renderer_reference.slang` traverses it with a
  bounded stack and reports `HasExceededStack`. Render-graph ray passes do not
  traverse it today. (Since #676, `ray_bvh_traversal.slang` holds that walk,
  `ComputeBvh` graph passes use it, and order word Z keeps the BVH leaf order;
  see D5.)
- **Opacity.** `LUDUS_GPU_PREVIEW_OPAQUE_FLAG` in `Sampling.z` marks a scene
  whose instances are all opaque. Otherwise a triangle is alpha-tested when its
  `LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD` `.z` is nonzero. `TraceTriangleLoop`
  then compares texture, material and vertex alpha with the cutoff per hit.
- **Visibility tolerances.** `SpawnSurfaceRay` offsets the receiver with
  `OffsetPositionWorld` for the ray direction. `TestVisibility` offsets a light
  endpoint on its triangle toward the receiver and shortens the ray by one ulp,
  so light geometry does not occlude its own emission.
- **MIS template.** `veach_guibas_mis` (compute ray-tracing D9) runs a primary
  pass and three shading passes, with a two-strategy `ComputeMisWeight` in
  `main_shaders/multiple_importance.slang` under balance, power, cutoff and
  maximum heuristics.
- **Query budgets.** `scene_interface` declares `surface_queries` and
  `visibility_queries`. `render_graph_ray.py` substitutes them into the wrapper
  as `RayQueryContext.Maximum*Queries` and bounds them by `SCENE_QUERY_LIMITS`.
  They are not plan or package fields.

No line visibility query, line strategy or triangle-shaped traversal exists.

## 2. The method as adopted

Salesin and Jarosz (2019) write direct lighting from a light as a 2D integral
over the light's surface. A line sample chooses a line offset `v` with a
marginal density `p(v)`, then a point `u` along that line with a conditional
density `p(u | v)`. Their key observation (section 4) is that this is ordinary
point sampling with the joint density `p(u, v) = p(v) p(u | v)`. So line samples
can be combined by MIS with BSDF point samples, and with lines in another
direction, in one sampling space.

A line sample is useful because its conditional density covers only the
visible parts of the line. The shadow triangle formed by the shading point and
the line is intersected with the scene (Billen and Dutré 2016). Points are then
drawn only where visibility is one, which removes visibility discontinuities
from the conditional integrand. The paper's surface-area strategy (their
equations 14-15) draws `v` proportional to chord length and `u` uniformly over
the visible intervals. In the unoccluded case that reduces to uniform area
sampling. Evaluating a line strategy's density at a point drawn by another
strategy needs a line query through that point (their section 4.2). The paper
reports a line query costing 1.2-55 times a shadow ray on its scenes, and
implements the method in PBRT on the CPU.

Ludus adopts:

- the joint-density formulation and MIS between BSDF points and two line
  directions (section 4.2);
- the surface-area line strategy (section 4.1, equations 14-15);
- Billen and Dutré's shadow-triangle intersection, restated in D2.

Ludus departs as follows:

- **Triangle emitters.** The paper's lights are quads. Here a line is a chord
  of one emitter triangle (D6), and planar chords across triangles are deferred.
- **Area measure only.** The solid-angle line strategy, which adapts Ureña,
  Fajardo and King, "An Area-Preserving Parametrization for Spherical
  Rectangles", *Computer Graphics Forum* 32(4) (EGSR 2013), needs
  parallelogram emitters and is deferred. Near the light and on glossy
  surfaces this costs variance (the paper's figure 3). MIS with BSDF sampling
  covers the glossy case.
- **Masked occluders.** The paper's scenes are opaque. D4 adds a stochastic
  rule for alpha-tested triangles.
- **Bounded intervals.** The paper's interval lists are unbounded CPU
  containers. D3 bounds them and fails explicitly.
- **No smoothing heuristic.** Section 4.3's discontinuity-smoothing balance
  heuristic only improves the convergence rate under stratified sampling. The
  scene random stream is not stratified (PL15).
- **GPU compute.** The query runs in compute shaders, first as a linear scan,
  then over the packed BVH (D5).

## 3. Decisions

### D1 Query at the traversal boundary

A new private source, `ray_line_visibility.slang`, implements the traversal.
`scene_shader.slang` declares the public API (PL02) and owns budgets and failure
latching, as `TestVisibility` does around `Trace`:

```slang
struct LineSegment
{
    float3 StartWorld;
    float3 EndWorld;
    uint TriangleIndex; // carrying emitter triangle, or SCENE_INVALID_INDEX
};

struct LineVisibility
{
    float2 Intervals[SCENE_LINE_INTERVAL_CAPACITY]; // visible [start, end) in [0, 1], sorted
    uint IntervalCount;
    float VisibleLengthWorld;
    bool HasMaskedOccluders;
};

uint FindVisibleLineIntervals(SceneContext scene,
                              inout RayQueryContext queries,
                              SurfaceHit receiver,
                              LineSegment segment,
                              out LineVisibility outVisibility);
```

- `LUDUS_SCENE_LINE_INTERVAL_CAPACITY` and
  `LUDUS_SCENE_LINE_INTERVAL_MERGE_TOLERANCE` are defined in
  `scene_shader_layout.h` and mirrored by `SCENE_LINE_INTERVAL_CAPACITY` and
  `SCENE_LINE_INTERVAL_MERGE_TOLERANCE` in Slang and C++ (`scene_shader.hpp`).
  The C++ side lets tests and the SDK name the same bound. PL0 chose 32
  intervals and 1e-6 of the segment (sections 4.2 and 4.3).
- `RayQueryContext` gains `LineQueries` and `MaximumLineQueries`. The wrapper
  initializes the maximum from `line_queries` (D9).
- The function name says what it returns: visible intervals, not occluders.

Alternatives considered:

- An author helper in each template. Rejected: compute ray-tracing D7 places new
  traversal at the shared boundary, and budgets and failure latching belong to
  the query layer.
- Returning occluded intervals. Rejected: every caller needs the visible set and
  its length.

### D2 Shadow-triangle clipping

Notation: `x` is the offset apex (PL03), `a` and `b` the offset segment
endpoints. A point `y` in the plane of `S = (x, a, b)` has coordinates
`y = x + alpha (a - x) + beta (b - x)`. The interior of `S` is `alpha >= 0`,
`beta >= 0`, `alpha + beta <= 1`. Along the ray from `x` through `y`, `s =
alpha + beta` is the fraction of the way to the segment, and
`t = beta / (alpha + beta)` is where the ray meets the segment.

For each occluder triangle `T` not excluded by index:

1. Compute the signed distances of `T`'s vertices to the plane of `S`. If all
   three have one strict sign, `T` does not cross the plane. If all three lie
   within the parallel tolerance, `T` is coplanar and does not occlude (PL03).
2. Otherwise intersect `T`'s edges with the plane to get the segment `p q` of
   `T` in the plane. A vertex on the plane contributes itself. Each edge is
   intersected from its lexicographically smaller endpoint (compare `x`, then
   `y`, then `z`), so two triangles that share an edge compute a bitwise-identical
   point. PL0 found that intersecting in each triangle's winding order leaves
   cracks that no tolerance up to 1e-3 closes (section 4.3).
3. Express `p` and `q` in `(alpha, beta)` and clip `p q` to
   `alpha >= 0`, `beta >= 0`, `s >= s_near`, `s <= 1 - s_far`. `s_near`
   excludes surfaces at the apex and `s_far` the light plane; both come from the
   PL03 offsets, not new constants.
4. The clipped segment maps to `t` through a projective map whose denominator
   `s` stays positive after clipping, so `t` is monotonic along it. The occluded
   interval is `[min(t_p, t_q), max(t_p, t_q)]`, clamped to `[0, 1]`.
5. If `T` is alpha-tested (section 1), set `HasMaskedOccluders` instead of
   inserting the interval (D4).

This restates Billen and Dutré's routine (their section 4) for the packed
triangle layout. Computing in `(alpha, beta)` keeps one clip region for every
occluder. The receiver and carrying triangles are excluded by index because a
ray from a point of a plane meets that plane only once.

### D3 Interval set

The query keeps a sorted array of disjoint occluded intervals in registers and
inserts each new interval with merging:

- Two intervals merge when they overlap or their gap is at most
  `SCENE_LINE_INTERVAL_MERGE_TOLERANCE`.
- When an insertion would make the count exceed the capacity, the query fails
  (PL04). The visible complement has at most one more interval than the
  occluded set, so the occluded set's capacity is
  `SCENE_LINE_INTERVAL_CAPACITY - 1`.
- After the scan, the visible intervals are the complement in `[0, 1]`.
  Visible intervals no longer than the tolerance are dropped.
  `VisibleLengthWorld` is the remaining parameter length times
  `length(b - a)`.

The tolerance rule only removes visible length, deterministically. A strategy
whose support shrinks by a fixed, deterministic set is still a valid density, so
MIS outputs stay unbiased wherever another strategy covers that set: BSDF
sampling in `color`, the other line direction in `lineSampling`. A line-only
estimate loses at most the dropped length: 2.1e-7 m at most on the PL0 fixtures
(section 4.3). Canonical edge order (D2 step 2) removes cracks along shared
edges by itself. The rule guards abutting geometry that does not share vertex
bits, where a crack would make line samples drawn there assume visibility one:
a light leak.

Overflow fails rather than approximates:

- Merging the closest occluded intervals would mark visible length occluded.
  That is deterministic only for one insertion order, so the loop (PL1) and the
  BVH (PL3) would disagree.
- Treating overflow as fully visible would leak light.

Insertion order still decides when overflow happens, because the running set
can hold more intervals than the final union. PL3 compares the loop and the BVH
only on fixtures that do not overflow.

PL1 storage. The query keeps the running occluded set in
`LineVisibility.Intervals` itself and complements it in place, from the last gap
down, then drops short gaps in order. With a separate occluded array, the
line-sampling template's merged three-pass kernel lost its interval stores on
Metal (Apple M5, macOS 26.6) at capacity 32, though not at 8 or 16. The count and
length survived but every interval read back as zero, so every line sample used
the chord's first endpoint and the image was 2.6% too bright in red. The same
kernel also needed `ClearLineVisibility`, a field-by-field reset, in place of
`(LineVisibility)0`. After merging later `develop` ray changes the template
regressed again while passing a `LineVisibility` between functions, so it now
reads the intervals only in the function that ran the query and keeps scalars
(visible length, drawn parameter) in what it passes on. Its mean then matches
light sampling within 0.03%. The emitted MSL was logically correct, so this is a
Metal compiler problem. PL2 and PL3 should follow the same pattern, and PL3's BVH
stack adds thread memory that should be checked against it.

PL3 check. The BVH walk adds a 64-entry `uint` stack to every line query site and
keeps the single interval array. Both templates' merged kernels still pass the PL1
and PL2 convergence tests, and their images match the scan's byte for byte
(tasks PL3).

### D4 Alpha-tested occluders

An exact interval set through an alpha-tested triangle would need the alpha
texture integrated along a curve, which no existing contract provides.

Rule: clip only opaque occluders. Report any alpha-tested candidate through
`HasMaskedOccluders`, and make the line strategy confirm its drawn point with
`TestVisibility`.

Why this is unbiased. The line strategy's density is positive only where no
opaque occluder blocks the path. The confirmed point's integrand includes the
true visibility, evaluated by the same traversal as every other shadow ray.
Wherever the true visibility is one, no opaque occluder blocks the path, so the
density is positive there. The estimator therefore covers the integrand's whole
support, and the masked occluders' visibility is estimated stochastically.
Density evaluation (PL09) uses the same opaque-only intervals, so MIS weights
match the densities actually sampled. The cost is one visibility query per line
sample that reports masked candidates, and none in an opaque scene.

PL1 finding. Render-graph scenes cannot hold alpha-tested triangles today:
`SceneShaderMaterial` has no alpha mode, and scene preparation rejects any item
whose coverage is not opaque. So every graph ray pass sees an opaque scene, and the
masked path is implemented but cannot be exercised through the public API. Its
GPU test, and PL2's and PL3's masked tests, wait for a scene-material alpha mode.

Alternatives considered:

- **Treat masked triangles as opaque.** Biased: foliage and fences would block
  all light.
- **Reject scenes with masked materials.** Too restrictive, and unnecessary
  given the rule above.
- **Skip masked triangles silently.** Biased: light leaks through opaque texels.

### D5 Traversal

**PL1, linear scan.** Loop over `GetSceneInfo(scene).TriangleCount` packed
triangles, as `TraceTriangleLoop` does: cost linear in triangle count per query,
for small scenes and comparison. It reads the packed triangle words directly,
not through `GetTriangle`, to avoid copying texture coordinates per triangle.

**PL3, BVH traversal** (revised 2026-10-29, after #676). The first version of
this decision planned a private walk of the packed `ReferenceBvhNode` array
modeled on the RT1 kernel. Since then #676 moved that walk into
`ray_bvh_traversal.slang` and made it a graph ray strategy, `ComputeBvh`. The
line query now shares its boundary instead of duplicating it:

- **Shared node reader.** A new private source, `ray_bvh_nodes.slang`, holds
  what both walks need: `BVH_NODE_WORDS`, `BVH_MAXIMUM_PENDING`
  (`LUDUS_REFERENCE_STACK_SIZE - 2`), `LoadBvhNode` (bounds, `Left`/`Right`,
  `First`, `Count`, `SplitAxis`) and `GetBvhLeafTriangle`. `TraceBvh` and the
  line walk both use it, so the node layout, the leaf order and the stack bound
  have one owner.
- **Leaf order.** #676 keeps the BVH leaf order in order word Z whatever the
  native opacity partition in word X (preview kernel ABI 8,
  `PackGpuPreviewRayOrder`). `GetBvhLeafTriangle` reads word Z. That settles the
  PL0 finding (section 4.4) with no packing change and no extra permutation:
  leaf ranges address the unpartitioned order in every plan, including plans
  with a `NativeQuery` pass and scenes with masked triangles.
- **Separate loop body.** `TraceBvh` orders children by the ray direction,
  culls by the closest distance and stops at the first visibility hit. The line
  walk has no direction or distance. It visits every leaf whose box overlaps the
  shadow triangle and runs the D2 clip on each leaf triangle. A generic walk with
  a visitor interface was considered and rejected: the shared part is the node
  decode and the stack bound, about ten lines, and a visitor would pass the
  interval array between functions, the pattern that lost stores on Metal (D3,
  "PL1 storage").
- **Node test.** A node is visited when its box overlaps the shadow triangle
  `(x, a, b)`, by the separating-axis test of Tomas Akenine-Möller, "Fast 3D
  Triangle-Box Overlap Testing", *Journal of Graphics Tools* 6(1), 2001,
  pp. 29-33: the three box axes, the shadow plane's normal and the nine edge
  cross products. The test is exact in real arithmetic. In float the box's half
  extents grow by a margin of `64 * 2^-24 * scale / sine`, where `scale` is the
  largest coordinate magnitude among the apex, the endpoints and the box, and
  `sine` is the apex angle's sine that the query already bounds below by
  `TRIANGLE_PARALLEL_COSINE_TOLERANCE`. The clip's own rounding grows as
  `1 / sine` in the shadow plane's coordinates, so a thin shadow triangle gets a
  wider margin; a wider margin only visits more nodes. The node boxes are float
  bounds of the packed float vertices, so every leaf box contains its triangles.
- **Stack exhaustion fails** (PL12) with the `TraceBvh` bound: pushing two
  children with more than `BVH_MAXIMUM_PENDING` entries pending fails the query.
  The walk keeps at most one deferred sibling per level, and `BuildReferenceBvh`
  bounds the depth to `REFERENCE_BVH_MAX_DEPTH = 48`, so a packed scene cannot
  reach it. The check guards a malformed pack and has no GPU test, since the
  public API cannot build a deeper tree. A pack with triangles but no nodes also
  fails.
- **Full occlusion stops the walk.** Once the occluded set is one interval
  whose gaps to 0 and 1 are both within the merge tolerance, no visible length
  can remain and later insertions only merge into it. Both traversals then stop.
  `HasMaskedOccluders` is false whenever no length is visible, because no line
  point is drawn, so the early stop does not depend on visiting order.

Why results equal the loop's. The node test is conservative, so the BVH visits
a superset of the triangles that the loop clips, and each candidate runs the same
`ClipLineOccluder` arithmetic. The merged occluded set does not depend on
insertion order: two intervals end up in one entry exactly when a chain of
intervals, each within the tolerance of the next, joins them, and `min`/`max` are
exact. Its complement, the status and `VisibleLengthWorld` are therefore
bitwise equal. Two cases may differ, both from insertion order (D3): overflow,
because the running set can exceed the final union; and a nonfinite interval,
which fails only if it is met before the segment is fully occluded.

**Selection.** The scene interface gains an optional `line_traversal`,
`"ComputeBvh"` (the default) or `"TriangleLoop"`. Like `line_queries` it is a
builder and wrapper input, not a plan or package field (D9, PL07). The wrapper
defines `LUDUS_SCENE_LINE_TRIANGLE_LOOP` for `"TriangleLoop"`. It is independent
of each pass's `invocation.traversal`, because the line query is software in
every pass (PL06). Alternatives considered:

- **Follow the pass strategy** (`TriangleLoop` scans, `ComputeBvh` and
  `NativeQuery` walk the BVH). Rejected: the shipped templates select
  `TriangleLoop` for their rays (D8) and would keep scanning, and a BVH
  comparison would also change their ray tests (Möller-Trumbore against the
  watertight test), so their images could not be compared byte for byte.
- **BVH only, loop removed.** Rejected: the loop is the oracle for the parity
  tests and for authors checking a scene of their own.

PL3 notes. On an Apple M5 the walk and the scan gave byte-identical readouts and
images on every PL1 and PL2 fixture and on a 500,000-triangle part of Intel's
Sponza. There a line-sampling pass with one query per pixel took 2.2 ms with the
BVH against 427-452 ms with the scan at 96×64. On the 14-triangle Cornell room the
walk was 13-28% slower than the scan. At equal GPU time on that room, PL2's
penumbra comparison reverses: point sampling with about 6.5 times the samples has
about a third of `color`'s error. These are measurements (tasks PL3), not claims.

**Native.** Not provided. Inline `RayQuery` and Metal intersection queries
accept rays only, and the native acceleration structure is opaque, so its nodes
cannot be walked with a triangle. Line queries in a `NativeQuery` pass use the
software traversal (PL06). The software clip and hardware ray-triangle tests
can then disagree within float tolerance at triangle edges. The template
avoids this by selecting `TriangleLoop` for every pass (D8).

### D6 Line strategies on a triangle emitter

For a selected mesh light with total emitting area `A_light` (emitter `.z`):

1. `SampleIncidentLight(scene, selected, receiver, shapeSample)` returns an
   area-uniform point `l0` on triangle `T`, chosen with probability
   `A_T / A_light`.
2. Direction `U = normalize(P1 - P0)` of `T`; direction `V = normalize(cross(n,
   U))`, with `n` the geometric normal. The chord is the intersection of `T`
   with the line through `l0` in the chosen direction, giving endpoints `a`
   and `b` and length `c`. Because `l0` is area-uniform, the chord's offset has
   density `c / A_T`, which is Billen and Dutré's length-proportional offset
   (Salesin and Jarosz equation 14) with no extra inversion.
3. `FindVisibleLineIntervals` with `segment = {a, b, T}`.
4. Zero visible intervals contribute zero. Otherwise draw `xi` and map it
   linearly across the concatenated visible intervals to a point `l`, so that
   `p(u | v) = 1 / L_visible` in metres (equation 15).
5. If `HasMaskedOccluders`, build a `LightSample` for `l` and call
   `TestVisibility` (D4). Contribute zero on `SCENE_QUERY_SURFACE`.
6. Joint area density `p_A = P(light) * (A_T / A_light) * (c / A_T) /
   L_visible = P(light) * c / (A_light * L_visible)`. Convert it to solid angle
   with `distance^2 / |cos_light|`, as `EvaluateLightPdfSolidAngle` does,
   respecting emitter sidedness through `EvaluateEmission`. With no occluder
   this equals the existing area-light density, a useful test invariant.

Emission is evaluated at `l`, so textured emission is integrated but not
importance sampled, which matches the paper's figure 11 discussion.

Degenerate chords (added after section 5.1). Where the line through `l0` leaves
`T` at once, the chord has zero length: at a vertex whose interior angle excludes
the direction, or, through rounding, at a point on an edge whose chord runs
along that edge. `FindVisibleLineIntervals` rejects a segment shorter than
`SCENE_UNIT_TOLERANCE` (PL12). The template therefore does not query such a
chord. `IsEmitterChordQueryable` compares the chord's length, computed as the
query computes it, with that tolerance. Below it the strategy proposes nothing
and its D7 density is zero, as for a receiver in the emitter's plane. The
rejected points have zero area in exact arithmetic, so the strategy's density
elsewhere is unchanged. `color` stays unbiased, because BSDF sampling covers
them. `lineSampling` loses only points that both line directions reject, and
`line-sampling`'s `color` the points that `U` rejects. Those points lie within
`SCENE_UNIT_TOLERANCE` of a vertex, or on an edge parallel to the line, so the
lost fraction of a triangle's area is about `(1e-5 m)^2 / A_T`, apart from the
discrete draws below. With 24-bit draws the area point lands exactly on `P0`
when its radial draw is zero, a probability of `2^-24`. `U` from `P0` runs along
the first edge, but `V` there has zero length whenever the angle at `P0` is
acute. A `V` sample therefore proposes nothing with probability `2^-24` on such
a triangle. `TryMakeEmitterChord` still fails for a degenerate triangle or
nonfinite bounds.

A selected non-mesh light (directional, point, spot, environment) uses the
existing point proposal and its density, with `TestVisibility`. Two line
strategies then propose the same density for such a source, so under balance or
power weighting each takes half of it. BSDF sampling never reaches a delta
light, and reaches the environment with the environment density.

### D7 Density of a line strategy at a given point

For a point `l` on emitter triangle `T` reached by a BSDF sample or drawn by the
other line direction:

1. Build the chord of `T` through `l` in the strategy's direction.
2. Run `FindVisibleLineIntervals`. If `l`'s chord parameter lies in no visible
   interval, the density is zero.
3. Otherwise apply D6 step 6 with the chord's `c` and `L_visible`.

A BSDF sample that reaches an emitter costs two line queries (`U` and `V`). A
`U` sample costs its own query and one `V` evaluation, and likewise for `V`.

### D8 Templates

Each template directory under `modules/graphics/renderer/render_graphs/`
follows the `veach_guibas_mis` layout: `library.lurender.json`,
`main.lurender.json`, `main.lushader.json.in` and `main_shaders/` with its own
`hit_record.slang` copy of the compute ray-tracing D5 record. Every pass sets
`invocation.traversal` to `TriangleLoop`, so each package is a
`ComputeRayTracing` package and needs no native ray profile.

#### D8a `line-sampling` (PL1)

Directory `line_sampling/`, program IDs prefixed `LineSampling`. It ships with
the query so that the query has an end-to-end, editable example from its first
change.

```text
library configuration LineSampling (hits = Buffer, 32 bytes per pixel of RenderSize):
  primary              [ComputePipeline: scene, view, color rw, hits rw]
  shade                [ComputePipeline: one line U sample]      -> output color
  shadeVisibleFraction [ComputePipeline: L_visible / c, gray]    -> output visibleFraction (optional)
root graph: each output -> Accumulate -> ToneMap -> Export
```

- `shade` is the D6 strategy in direction `U` alone: Billen and Dutré's
  surface-area line estimator, with the sampled point evaluated instead of an
  analytic line integral. Non-mesh lights use the point fallback with weight 1.
- `shadeVisibleFraction` repeats the same chord (same dimensions) and returns
  `float3(L_visible / c)` for mesh lights and `float3(0)` otherwise. It shows
  where the query finds penumbra and umbra, and gives PL1's tests an image-level
  check. Accumulated, it is the mean visible fraction, not radiance; the guide
  says so.
- Random dimensions: the D8b layout's line `U` entries, 2-6. Budgets: surface
  0, visibility 1 (masked confirmation or a non-mesh light's shadow ray), line 1.

#### D8b `salesin-jarosz-lines` (PL2)

Directory `salesin_jarosz_lines/`, program IDs prefixed `PointLine`.

```text
library configuration PointLineMis (hits = Buffer, 32 bytes per pixel of RenderSize):
  primary     [ComputePipeline: scene, view, color rw, hits rw]
  shade       [ComputePipeline: BSDF + line U + line V]   -> output color
  shadeLines  [ComputePipeline: line U + line V]          -> output lineSampling (optional)
  shadePoints [ComputePipeline: BSDF + area light point]  -> output pointSampling (optional)
root graph: each output -> Accumulate -> ToneMap -> Export
```

Random dimensions. The primary pass keeps `[0, 2)` (compute ray-tracing D4).
All three shading passes use one layout:

| Dimensions | Draws |
| --- | --- |
| 2 | line `U` light selection |
| 3-5 | line `U` shape sample (`SampleIncidentLight`) |
| 6 | line `U` position along the visible intervals |
| 7 | line `V` light selection |
| 8-10 | line `V` shape sample |
| 11 | line `V` position along the visible intervals |
| 12-14 | `SampleStandardBsdf` lobe, radius, angle |

`shadePoints` draws its light point from dimensions 2-5. Its point therefore
equals the area point that seeds the `U` chord, which makes the point and line
images directly comparable per sample. PL0 checks that 15 dimensions are within
`RANDOM_MAX_DIMENSION`.

MIS. `main_shaders/multiple_importance.slang` generalizes the `veach_guibas_mis`
helper (compute ray-tracing D9) to three
strategies, `ComputeMisWeight(float sampledDensity, float otherDensity0, float
otherDensity1)`, for the balance and power heuristics only. A zero other density
drops that strategy. For finite positive densities the three weights sum to one,
which a GPU test checks numerically. `shadeLines` and `shadePoints` pass zero
for the absent strategy. Graph settings: `MIS_HEURISTIC` (`0` balance, `1`
power, default `0`, the paper's choice) and `MIS_POWER_EXPONENT`.

Budgets. Passes in one sequence share a scene interface (compute ray-tracing
C06), so it declares the maximum over passes:

| Pass | Surface | Visibility | Line |
| --- | --- | --- | --- |
| `shade` | 1 | 2 | 6 |
| `shadeLines` | 0 | 2 | 4 |
| `shadePoints` | 1 | 1 | 0 |
| declared | 1 | 2 | 6 |

The two visibility queries cover a masked confirmation (D4) or a non-mesh
light's shadow ray, one per line strategy.

PL2 notes. `line_sampling.slang` is a copy of PL1's, extended, because a template
directory imports only its own include closure. Its line queries run in
`[noinline]` functions (`ProposeLineOutOfLine`, `EvaluateLineDensityOutOfLine`)
that take `RayQueryContext` by value and return it through an `out` parameter.
Inlined at every call path, the ten line queries of the three shade passes made the
merged Metal stage 2.10 MB, over the builder's 2 MiB artifact limit; out of line it
is 1.09 MB. Slang 2026.1.2 inlines a `[noinline]` function that has an `inout`
parameter, hence the copy in and out. SPIRV-Cross still marks the MSL functions
`always_inline`, so Metal compiles the same kernel, and each function still reads
its `LineVisibility` only where it ran the query (D3, "PL1 storage").

PL2 seam finding. A receiver exactly on the seam where the floor meets a wall lies in
the wall's plane. The line query then clips that wall at grazing incidence, and a
line strategy's density there can drop to zero while a shadow ray from the same
receiver misses the wall. The PL2 density fixture first met this with pixel-center
camera rays, which the Cornell room's symmetry lands exactly on the seams, and now
traces off-center rays. Seams have zero area, so jittered rendering does not meet
them, and the convergence tests show no bias. A robust treatment, such as excluding
occluders that contain the apex, belongs with PL3's traversal work.

#### D8c Registration

Each template is registered everywhere `veach-guibas-mis` is:

- `scripts/python/ludus_tools/render_graph_templates.py` `TEMPLATES`, with the
  third field `True` (Metal or Vulkan profiles only), and the `cli.py`
  `--template` choices;
- `apps/editor/resources/render_graph_templates.json`: one `graph` entry
  (category `ray-traced`) and one `snippet` entry (category `lighting`, with
  `defaultConfiguration`);
- the `apps/editor/CMakeLists.txt` template embedding list;
- a `Prepare…Configuration` function in its own editor source, dispatched from
  `PrepareConfigurationTemplate` beside
  `PrepareComputeRayTracingConfiguration`, following compute ray-tracing D6;
- the renderer's installed SDK template files, as the existing templates.

Registry requirement. The vocabulary today is `native-ray-tracing`,
`denoiser` and `metalfx` (`render_graph_template_registry.cpp`). The existing
`simple-compute-ray-tracing` template uses the triangle loop yet declares
`native-ray-tracing`. These templates must not repeat that, because they run on
GPUs without ray hardware. PL1 adds `compute-ray-tracing` with the label
"Metal or Vulkan compute" to the registry, its label function and the
registry tests, and both templates declare it. Correcting the older template's
declaration is a separate change, noted in the PL1 pull request.

The CLI's profile check for these templates is the existing Metal-or-Vulkan
rule, but its repair text says "this template requires native ray tracing".
PL1 makes the text follow the template's registry requirement, so a
`line-sampling` failure names Metal or Vulkan compute instead.

### D9 Builder and wrapper

- `render_graph_build.py` accepts optional `line_queries` in
  `scene_interface`, bounded by new `SCENE_QUERY_LIMITS` entries
  `MIN_LINE_QUERIES = 0` and `MAX_LINE_QUERIES = 8`. An absent field means 0
  and generates the same wrapper bytes as today.
- `render_graph_ray.py` substitutes `MAX_LINE_QUERIES` into
  `queries.MaximumLineQueries` beside the other limits.
- PL3: `render_graph_build.py` accepts optional `line_traversal` in
  `scene_interface`, `"ComputeBvh"` or `"TriangleLoop"`, and rejects anything
  else with a repair naming both. `render_graph_ray.py` defines
  `LUDUS_SCENE_LINE_TRIANGLE_LOOP` for `"TriangleLoop"`. An absent field and
  `"ComputeBvh"` generate the same wrapper bytes (D5, "Selection").
- The plan and package formats are unchanged (PL07). PL1 confirms this with
  byte-identical cooked outputs for the existing ray fixtures and templates.

### D10 Deferred work

These are deferred, each with its own reason:

- **Solid-angle lines.** Need parallelogram emitters (section 2).
- **Planar chords.** Need emitter-level topology.
- **Discontinuity smoothing.** Needs stratified sampling.
- **Path capture.** Line queries are not recorded by the ray path capture.
- **Native and browser.** No native line query exists, and browser ray
  execution is not provided.
- **Seam receivers** (PL2 seam finding, D8b). Excluding occluders that contain
  the apex would change intervals, and PL3 must keep them bitwise equal to the
  loop's, so it is left for its own change with its own evidence.

## 4. PL0 evidence

PL0 makes no product change. It was asked to record: visible-interval counts
and Metal register/occupancy cost for capacities 8, 16 and 32 (choose the
smallest capacity that covers the fixtures with margin); the smallest crack-free
merge tolerance on a tessellated sphere and a two-triangle quad occluder, with
the visible length it drops; whether render-graph scene packs carry
`ReferenceBvhNode` data under both strategies; whether `RANDOM_MAX_DIMENSION`
covers dimension 14; and the prototype query's cost against `TestVisibility`
with `TriangleLoop`.

Recorded 2026-10-27 on `origin/develop` `431613a2`.

- **Device.** Apple M5 (MacBook Air, 16 GB), macOS 26.6 (25G72), Metal 4.
- **Toolchain.** `macos-clang-development` preset, Homebrew clang 23.1.3,
  pinned Slang 2026.1.2 and SPIRV-Cross. MSL is compiled by Metal at pipeline
  creation.
- **Vulkan.** Not run. There is no Linux or Vulkan host here, so every number
  below is Metal only.

**Prototype.** A throwaway scene compute kernel, not committed. It implements D2
and D3 as written, as a linear scan over `Counts.x` packed triangles:

- The apex and endpoints are offset with `OffsetPositionWorld` (PL03), and the
  receiver and carrying triangles are excluded by index.
- The clip region is `s` in `[0, 1]` (`s_near = s_far = 0`), with clipped
  points at `s <= 0` skipped.
- Chords are the D6 `U` and `V` chords through `SampleIncidentLight` area
  points.

Three local fixtures ran one kernel per pass with `invocation.traversal:
TriangleLoop`; only the node probe also ran as `NativeQuery`. A local Catch2
driver read results back as RGBA8 holding 24-bit fixed point, so readout
quantization is 6e-8.

Scenes: a `[-1, 1]` room (10 triangles) with a 0.7 m two-triangle quad light at
`y = 0.98`, plus one of the following:

- a two-triangle block top (the Cornell penumbra case, standing in for the PL2
  room);
- a UV sphere, radius 0.3 m, 64×32 (3968 triangles);
- a 0.6 m quad rotated 30°;
- blinds: `K` slats at `y = 0.85` spanning the light, pitch `0.7 / K` m, half
  open.

A 640×480 pinhole camera gives about 190,000 receivers. Each receiver tests 8
area points, with a `U` and a `V` chord through each.

### 4.1 Prototype check

Receiver at the origin, segment `(-1, 2, 0)` to `(1, 2, 0)` (2 m), no offsets,
capacity 16, tolerance 0. Intervals are segment parameters.

| Case | Expected visible | Measured | Max error |
| --- | --- | --- | --- |
| Occluders only behind the receiver (`y = -1`) and beyond the segment (`y = 3`) | `[0, 1]` | `[0, 1]`, nothing inserted | 0 |
| One occluder, cross-section `x` in `[-0.25, 0.25]` at `y = 1` | `[0, .25] [.75, 1]` | same | 4.5e-8 |
| Two overlapping (`[.25, .75]`, `[.6, .9]`) and a disjoint `[.05, .15]` | `[0, .05] [.15, .25] [.9, 1]` | same, peak running set 2 | 8.9e-8 |
| Coplanar triangle in the shadow plane | `[0, 1]` | `[0, 1]`, rejected as coplanar | 0 |
| Abutting two-triangle quad, diagonal crossing at a non-dyadic point | `[0, .25] [.75, 1]` | same | 4.5e-8 |

Every case is in the scene alongside the behind and beyond occluders, and gives
the same result with either edge-intersection order (section 4.3). All errors
are within readout quantization.

### 4.2 Capacity

Counts use capacity 64, tolerance 1e-6 and canonical edge order, over every
chord of every receiver:

| Fixture | Triangles | Max visible | 99th percentile | Max running occluded set |
| --- | --- | --- | --- | --- |
| Room + block (penumbra) | 14 | 2 | 1 | 1 |
| Room + sphere 64×32 | 3980 | 2 | 1 | 3 |
| Room + rotated quad | 14 | 2 | 2 | 2 |
| Blinds 8 | 28 | 8 | 8 | 8 |
| Blinds 12 | 36 | 12 | 12 | 12 |
| Blinds 16 | 44 | 16 | 16 | 16 |
| Blinds 24 | 60 | 23 | 23 | 23 |
| Blinds 32 | 76 | 31 | 30 | 31 |

The sphere's running set reaches 3 before its pieces merge into one interval.
That is D3's insertion-order effect, measured.

Register use is not measured. Metal exposes no per-thread register count, and
Xcode's GPU profiler is not installed on this machine. As the occupancy proxy,
each pipeline was compiled alone from its generated MSL and
`MTLComputePipelineState` queried:

- the base, shadow-ray and line pipelines at capacities 8, 16 and 32 all report
  `maxTotalThreadsPerThreadgroup = 1024`, `threadExecutionWidth = 32` and no
  threadgroup memory;
- GPU time does not order by capacity (section 4.6). On the sphere the three
  capacities are within ±5%, inside run-to-run variance.

The insertion indexes its arrays dynamically. The Metal compiler therefore
probably keeps them in thread memory, not registers. That is an inference;
nothing here measured it.

**Chosen `LUDUS_SCENE_LINE_INTERVAL_CAPACITY = 32`.** It is the smallest tested
capacity that covers the fixtures with margin. The PL2-style single-occluder
fixtures need 2 visible intervals (3 running), which 8 already covers. Dense
blinds of 16 slats across the 0.7 m light reach 16 running occluded intervals,
which overflows 16, whose occluded capacity is 15 (D3). At 32, the occluded
capacity of 31 covers 24 slats with a margin of 8, and 32 slats exactly. The
cost on Metal is none measurable. On Vulkan it is unmeasured, so PL1 reports
the llvmpipe run, which measures no GPU registers either.

### 4.3 Merge tolerance

A sweep over `tau` (segment parameter) with the sphere and the rotated quad.
"Cracked" counts receivers, out of the shadowed ones, with a visible interval
inside the hull of the inserted occluded intervals; for a convex occluder that
is a crack. "Dropped" is the analytic visible length minus the reported visible
length, with canonical order, in metres; chords are at most 0.99 m long.

Naive order intersects each edge in its triangle's winding order. Canonical
order intersects each edge from its lexicographically smaller endpoint
(compare `x`, then `y`, then `z`).

| `tau` | Sphere: cracked naive / canonical (of 2308) | Sphere: dropped max, mean (m) | Quad: cracked naive / canonical (of 5583) | Quad: dropped max, mean (m) |
| --- | --- | --- | --- | --- |
| 0 | 634 / 0 | 2.8e-8, 5.4e-11 | 1637 / 0 | 5.9e-8, 4.9e-10 |
| 1e-7 | 522 / 0 | 6.0e-8, 5.8e-9 | 770 / 0 | 9.2e-8, 1.5e-8 |
| 3e-7 | 247 / 0 | 7.6e-8, 6.0e-9 | 128 / 0 | 1.5e-7, 2.5e-8 |
| 1e-6 | 54 / 0 | 2.1e-7, 6.4e-9 | 9 / 0 | 1.5e-7, 2.6e-8 |
| 3e-6 | 4 / 0 | 1.0e-6, 7.1e-9 | 2 / 0 | 9.7e-7, 2.6e-8 |
| 1e-5 | 1 / 0 | 1.1e-5, 2.0e-8 | 2 / 0 | 6.9e-6, 3.0e-8 |
| 1e-4 | 0 / 0 | 5.6e-5, 1.4e-7 | 2 / 0 | 7.6e-5, 9.3e-8 |
| 1e-3 | 0 / 0 | 4.6e-4, 1.1e-6 | 2 / 0 | 9.0e-4, 1.9e-6 |

The `tau = 0` dropped values are float rounding in the comparison itself.

Two findings follow from the table:

- **Tolerance is not the fix.** With naive order, two quad receivers still
  crack at `tau = 1e-3`. An edge that grazes the shadow plane makes the
  intersection ill-conditioned, so the two triangles' points can differ by more
  than any small tolerance.
- **Canonical order is the fix.** It makes the shared-edge point bitwise
  identical in both triangles. Neither fixture has a crack at any `tau`,
  including 0. D2 step 2 now requires it.

**Chosen `LUDUS_SCENE_LINE_INTERVAL_MERGE_TOLERANCE = 1e-6`** (a fraction of the
segment), with canonical edge order. The smallest crack-free tolerance is 0. The
1e-6 margin, about 8 ulp at 1, guards abutting geometry that does not share
vertex bits, such as instances placed edge to edge. That case was not measured.
At 1e-6 the dropped visible length is at most 2.1e-7 m (mean 6.4e-9 m) on the
sphere and 1.5e-7 m (mean 2.6e-8 m) on the quad.

### 4.4 Packed nodes

A probe kernel read `Counts.y`, and compared node 0's lower bound at
`Offsets.x` with `BoundsMinimumWorld`, once in a `TriangleLoop` pass and once in
a `NativeQuery` pass. Both strategies gave the same node counts, and every root
matched the scene bounds:

| Fixture | Triangles | Nodes |
| --- | --- | --- |
| Room + block | 14 | 7 |
| Room + sphere | 3980 | 2047 |
| Room + rotated quad | 14 | 7 |
| Blinds 8 / 12 / 16 / 24 / 32 | 28 / 36 / 44 / 60 / 76 | 15 / 23 / 31 / 31 / 55 |

The code agrees. The graph ray scene (`BuildGraphRayScene`), material graph
scene and reference job paths all call `BuildReferenceBvh`, and `PackGpuScene`
packs the nodes unconditionally.

PL3 must still handle one case. `render_graph_activation.cpp` sets
`HasNativeRayQueries` when any pass in the plan selects `NativeQuery`.
`PackGpuScene` then partitions `Order` by opacity through
`PackGpuPreviewRayOrder`: an opaque prefix, then a masked suffix. In that case
a node's `[First, First + Count)` range no longer addresses `Order` whenever the
scene contains masked triangles. The fixtures here are all opaque, so the
partition preserved order. PL3 therefore adds an unpartitioned leaf order, or
its own permutation, for plans that contain a `NativeQuery` pass, with a
masked-scene parity test under both strategies (see D5).

### 4.5 Random dimensions

`RANDOM_MAX_DIMENSION = 0xffff` (FoundationMath `math.slang`, mirrored by C++
`RandomMaxDimension`). `TryAdvanceSceneRandomDimension` fails only above it,
and the explicit-address `SampleSceneRandom` overloads require dimensions no
greater than it. D8b uses dimensions 0-14, so the layout stands unchanged.

### 4.6 Line query cost

Each kernel takes one camera hit and draws 8 area samples per pixel at 640×480.
"Shadow" then adds 8 `TestVisibility` calls under `TriangleLoop`; "line" adds 8
`U`-chord line queries. Each cell is the median GPU pass time over 64 frames,
for two runs. The ratio is `(line - base) / (shadow - base)`.

| Fixture | Base (ms) | Shadow (ms) | Line 8 (ms) | Line 16 (ms) | Line 32 (ms) | Line/shadow ratio |
| --- | --- | --- | --- | --- | --- | --- |
| Room + sphere (3980 triangles) | 60.5 / 74.2 | 484.3 / 512.6 | 470.6 / 491.7 | 492.5 / 480.4 | 487.4 / 469.9 | 0.90-1.02 |
| Room + blinds 16 (44) | 1.31 / 1.21 | 11.79 / 11.90 | 16.64 / 16.17 (overflows) | 19.07 / 18.90 | 17.50 / 16.32 | 1.40-1.69 |
| Room + block (14) | 1.53 / 1.34 | 2.43 / 2.97 | 5.57 / 4.84 | 8.87 / 12.91 | 5.12 / 9.25 | 2.1-8.2 |

On the sphere, one query costs about 172 ns per pixel as a shadow ray and
176 ns as a line query at capacity 16. Both scan every triangle; the
`TriangleLoop` shadow ray exits early only when it is occluded. The small
scenes are dominated by per-query overhead. They also vary up to 2× from run to
run on this machine, so the capacity columns do not order there. Capacity 8
overflows on blinds 16 and stops early, so its time there is not comparable.

For comparison, the paper reports 1.2-55× with a CPU BVH. These linear-scan
ratios are context, not an acceptance threshold, and support no speed claim.

## 5. Large-scene equal-time measurement

PL3 compared `color` with `pointSampling` at equal GPU time only on the
14-triangle Cornell room ([tasks](tasks.md), PL3). This section repeats the
comparison on a large scene with real occluders and soft shadows. It records
measurements of one scene on one device and makes no general claim.

Recorded 2026-10-29 on `origin/develop` `d00cd9eb` plus this change (runs A and
B). Run C repeated the measurement after merging `develop` `f3bdd943`, which
includes #745's shared scene upload.

- **Device.** Apple M5 (MacBook Air, Mac17,3, 16 GB), macOS 26.6 (25G72),
  Metal 4, `macos-clang-development` preset. Other agents' builds shared the
  machine, with load averages from 9 to 66 during the runs.
- **Vulkan.** Not run. Every number below is Metal only.

**Setup.** The hidden test `[.point-line-large-scene-performance]` in
`render_graph_ray_tests.cpp`:

- **Scene.** The 500,000-triangle Sponza export from PL3's line traversal
  measurement: the first 500,000 triangles of Intel's
  `NewSponza_Main_glTF_003.gltf` (274,307 vertices), named by
  `LUDUS_LINE_TRAVERSAL_SCENE`. The camera is the same, at
  `(10.94, 1.69, 0.50)` m looking along -X, with 96×64 pixels.
- **Light.** A 6 m square emitter facing down at `y = 10.14` m with emission 50,
  above the scene's horizontal center. PL3 used a 2 m light. The wider light
  casts wide penumbrae: a fifth of the image is penumbra.
- **Templates.** `line-sampling` and `salesin-jarosz-lines`, each cooked with
  every `ComputePipeline` on `ComputeBvh`. Line queries use PL3's BVH walk. A
  `TriangleLoop` pass would scan all 500,000 triangles per ray.
- **Device budget.** `rhi::StorageLimits::ResourceBytes` is 2 GiB, the Editor
  viewport's budget. On `d00cd9eb` the default 512 MiB rejected the four-pass
  graph with `CapacityExceeded`. `SceneMemoryBudget::RhiResources` needed
  160,481,248 bytes with 55,083,060 left, because each ray pass uploaded its own
  copy of the scene. `develop` #745 now shares one copy.
- **Masks.** The penumbra mask is PL2's. It uses the line-sampling template's
  `visibleFraction` after 256 samples. A pixel is in the penumbra when it lies in
  the lower half of the image (below eye height) and its fraction is strictly
  between 0.1 and 0.9. That gives 1,278 penumbra pixels. The other 1,794
  lower-half pixels form the "outside" mask, of 6,144 pixels in all.
- **Equal time.** The point-and-line graph renders 64 timed frames, which give a
  mean GPU time for each pass. As in PL3, a `color` sample costs the primary pass
  plus `shade`, and a `pointSampling` sample costs the primary pass plus
  `shadePoints`. For budgets of 16 and 64 `color` samples, point sampling gets
  `round(budget × color cost / point cost)` samples. The cap is 4,096, and no
  budget reached it.
- **Estimates.** One graph at scene revision 1 accumulates up to the largest
  point count. `color` and `pointSampling` are read from it at each count.
- **Reference.** `color` averaged over 8 graphs at scene revisions 2-9, each
  accumulated to 4,096 samples, for 32,768 samples per pixel. A single graph
  cannot give more, because accumulation stops at
  `format::MAX_ACCUMULATION_SAMPLES` (4,096). The reference noise estimate is the
  mean, over masked values, of the variance across the 8 references divided
  by 8. It is 1.45e-6 in the penumbra and 2.52e-7 outside. Those are 3.3% and 4.8%
  of the smallest MSE measured in each mask. The expected MSE against the
  reference is the estimator's MSE plus this noise. Five earlier runs used a
  single 4,096-sample reference, whose penumbra noise of 1.35e-5 was 27% of the
  smallest MSE. Those runs were discarded.
- **Error.** The MSE is computed per channel on decoded linear radiance (the
  inverse of the tone map) against the reference mean, over the masked pixels.

**Failure pixels.** Across the 8 references, 12 `color` and 12 `lineSampling`
pixels showed the failure color, summed over the references. No `pointSampling`
pixel did. The estimate graph, at up to 1,906 samples, had none. Failures appear
only in outputs that make line queries. A pixel that fails in any reference or
estimate leaves both masks. That removed 2 penumbra and 8 outside pixels,
leaving 1,276 and 1,786. Section 5.1 traces them to zero-length `V` chords,
which the template no longer treats as failures. The measurements above
predate that fix.

**GPU time per frame** in ms, the mean over 64 frames for each run:

| Run | Start (load) | Primary | `shade` | `shadeLines` | `shadePoints` | `color` sample | `pointSampling` sample | Ratio |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A | 13:40 (9.5) | 0.200 | 15.43 | 14.69 | 0.325 | 15.63 | 0.525 | 29.8 |
| B | 14:24 (60.5) | 0.364 | 16.80 | 15.71 | 0.391 | 17.17 | 0.755 | 22.7 |
| C | 16:50 (29.6) | 0.220 | 15.35 | 14.58 | 0.325 | 15.57 | 0.544 | 28.6 |

At equal time, point sampling got 477 and 1,906 samples in run A, 364 and 1,455
in run B and 458 and 1,831 in run C, against 16 and 64 `color` samples. From A to B, the primary pass
slowed 1.82×, a point sample 1.44× and a `color` sample 1.10×. Eight timed runs
of this test that day gave a primary pass of 0.195-0.527 ms (2.7×),
`shadePoints` of 0.313-0.473 ms, a `color` sample of 15.3-17.7 ms and a cost
ratio of 17.7-30.2. Those runs include the five discarded ones, whose timings
are valid. The `color` cost is almost entirely line queries: `shadeLines` alone
takes 93-95% of `shade`.

**Mean squared error.** Images are deterministic for each scene revision. All
runs therefore give the same `color` errors, and the same point errors at a
given sample count. The runs differ only in the point sample counts their
timings produced. Run C, after the merge, reproduced the reference, its noise
estimate, the failure counts and the `color` errors bit for bit.

| Mask | `color` samples | `color` MSE | `pointSampling` MSE at equal time, A / B / C | Point/`color`, A / B / C | `pointSampling` MSE at equal samples |
| --- | --- | --- | --- | --- | --- |
| Penumbra | 16 | 2.73e-3 | 1.94e-4 / 2.41e-4 / 1.91e-4 | 0.071 / 0.088 / 0.070 | 4.69e-3 |
| Penumbra | 64 | 6.62e-4 | 4.42e-5 / 5.62e-5 / 4.50e-5 | 0.067 / 0.085 / 0.068 | 1.21e-3 |
| Outside | 16 | 2.86e-4 | 1.89e-5 / 2.70e-5 / 2.01e-5 | 0.066 / 0.095 / 0.070 | 6.39e-4 |
| Outside | 64 | 6.45e-5 | 5.22e-6 / 6.73e-6 / 5.68e-6 | 0.081 / 0.104 / 0.088 | 1.37e-4 |

At equal sample count, `color` has 0.58 and 0.55 times `pointSampling`'s
penumbra MSE at 16 and 64 samples. Outside the penumbra the ratios are 0.45 and
0.47. A `color` sample cost 23-30 times a `pointSampling` sample in these runs, against
6.4-6.6 on the room. At equal GPU time, in this scene on this device,
`pointSampling` reached 7-9% of `color`'s MSE in the penumbra and 7-10% outside
it. The measurement covers one scene, one light, one camera and one device. It
supports no claim about other scenes or GPUs.

### 5.1 Failure pixels traced

Recorded 2026-10-29 on `origin/develop` `3c60f713`, on the same device and scene as
section 5, with the same 8 reference graphs (scene revisions 2-9, 4,096 samples
each). Vulkan was not run.

**Readout.** A local diagnostic build, not part of the change, gave
`RayQueryContext` a failure code and a call site. Every failure path of
`FindVisibleLineIntervals` and of the template's shading passes set it, with
distinct codes for an exhausted budget, an invalid triangle, a short or
nonfinite segment, a receiver in the segment's line, interval overflow, a
nonfinite interval, an exhausted BVH stack and a pack without nodes. Call sites
distinguished the `U` and `V` proposals, the other direction's density and the
two BSDF densities. The latching accumulation kept the code of the first failed
sample in the pixel's blue channel instead of magenta.

**Result.** Each of the 24 failure pixels hit the same status: the `V` proposal's
`TryMakeEmitterChord` failed (`maximum <= minimum`) before any line query ran.
No pixel hit a line-query status. That rules out interval overflow, BVH stack
exhaustion, a short segment, a receiver in the segment's line, budget exhaustion
and a nonfinite interval. `color` and `lineSampling` failed at the same pixels
in the same graphs, because `shade` and `shadeLines` read the same `V` draws
(D8b). `shadePoints` builds no chord.

| Revision | Pixels (x, y) |
| --- | --- |
| 2 | (58, 53), (30, 59) |
| 3 | (88, 39) |
| 4 | (84, 44) |
| 5 | (84, 13), (94, 44) |
| 6 | (42, 44) |
| 7 | (67, 56) |
| 8 | (95, 10), (12, 40), (27, 57) |
| 9 | (64, 32) |

**Cause.** `SampleRandomFloat01` returns the top 24 bits of a Philox word times
`2^-24`, so it returns exactly 0 with probability `2^-24`. `SampleIncidentLight`
maps the radial draw `y` to the barycentric `sqrt(y) * (1 - z, z)`, so `y = 0`
places the area point on vertex `P0`. Both triangles of the square light have
acute (45°) angles at `P0`. The `V` line, perpendicular to the first edge,
leaves the triangle there at once, and the chord has zero length. `U` from `P0`
is the first edge itself. With 6,144 pixels, one `V` proposal per pixel and
sample, and 32,768 samples per pixel, the expected count is
`32,768 × 6,144 × 2^-24 = 12.0`, which matches the 12 measured. At 1,906 samples
the estimate graph expected 0.70 and had none. With the fix in place, the same
readout marked each rejected chord's point. All 24 pixels again proposed a
degenerate `V` chord, and each one's area point was exactly `(0, 0)`.

This is a template bug, not a documented line-query failure. A zero-length chord
has no visible length to estimate, but the template latched `HasFailed`, as for a
degenerate emitter triangle. The fix (D6, "Degenerate chords") makes it propose
nothing and gives it zero density, without a line query. The same applies to any
chord shorter than the query's minimum segment. The point-line densities fixture
gained a row of vertex chords. It renders with no failure pixel, rejects the
first-edge chord through vertex 2, which always has zero length, and gives every
rejected chord zero density. After the fix, `[.point-line-large-scene-performance]`
reported no failure pixel in any output, in the eight references or in the
estimate graph. The masks then kept all 1,278 penumbra and 1,794 outside pixels.
The `color` MSE at 16 and 64 samples became 2.72e-3 and 6.61e-4 in the penumbra
and 2.85e-4 and 6.43e-5 outside. Before, it was 2.73e-3, 6.62e-4, 2.86e-4 and
6.45e-5. The section 5 tables keep the earlier measurements. The `color` and
`lineSampling` images change only where a failed `V` sample had latched them.
The merged Metal stage of the `ComputeBvh` variant is 1,686,273 bytes, under
the 2 MiB artifact limit.

**Not covered.** The query still fails, as designed (PL12), for a receiver whose
apex angle to a queryable chord has sine at most `1e-6`. A chord just longer
than `SCENE_UNIT_TOLERANCE`, seen from 10 m or more, reaches that bound, so a
small emitter seen from far could still fail near a vertex. On Sponza's 6 m
light the nearest nonzero draw, `y = 2^-24`, gives a `V` chord of 1.5 mm, with a
sine of about 1e-4 from 10 m. A small-emitter scene would need its own
measurement.

## 6. Risks

- **Linear cost.** Six line queries per pixel, each scanning every triangle,
  limited PL1/PL2 to small scenes. PL3's BVH walk removes the scan from line
  queries; the templates' `TriangleLoop` rays still scan, so large scenes need
  their passes on `ComputeBvh`.
- **Register pressure.** A 32-entry `float2` array per thread can spill. On
  Metal, PL0 found no occupancy change and no capacity-ordered time difference
  (section 4.2). Register counts and Vulkan were not measured.
- **Geometric disagreement.** The D2 clip and the Möller-Trumbore loop treat
  edges differently within float tolerance. With `TriangleLoop` on every pass
  the effect is confined to near-edge slivers. The PL2 convergence test bounds
  it with the same 2% tolerance as the MIS template's same-integral check.
- **Equal-sample, not equal-time.** Line samples cost more per sample. The PL2
  error comparison is at equal sample count and reports timing separately, so
  it supports no speed claim. The equal-time measurements found point sampling
  with lower error on both scenes measured: about a third of `color`'s penumbra
  MSE on the 14-triangle room (PL3) and 7-9% on the 500,000-triangle Sponza
  export (section 5), on one Metal device. They measure two scenes and support
  no general claim.
