# Ludus::FoundationGeometry

Prepared spatial paths, scalar tracks, bounded distance maps, rotation-minimizing
frames, closest-point projection, sweep meshes, prepared patch surfaces and
crack-free surface meshes above the FoundationMath cubic and bicubic kernel.
These are phases G2, G3 and G4 of the
[curves and surfaces architecture](../../architecture/curves-surfaces.md). Names,
statuses, domains and guarantee classes follow
[ADR 0024](../../decisions/0024-curves-surfaces-contracts.md). Validation is
recorded in the evidence ledger for
[G2](../../architecture/math-evidence/curves-surfaces.md#g2-prepared-paths-tracks-and-distance-maps),
[G3](../../architecture/math-evidence/curves-surfaces.md#g3-frames-projection-and-sweeps)
and [G4](../../architecture/math-evidence/curves-surfaces.md#g4-patch-topology-and-cpu-surface-meshes).

The module depends on `Ludus::FoundationBase`, `Ludus::FoundationMath` and
`Ludus::FoundationContainers`. It is not in `core.h` or the foundational PCH.
Include the header you use:

| Header | Contents |
| --- | --- |
| `status.hpp` | `GeometryStatus`, `GuaranteeClass`, domain values, `SpanSide`, `BoundaryPolicy`, `ToleranceProfile`, `WorkLimits`, `ComputePositionFloor`, `GeometryDiagnostic`; includes only Base |
| `path.hpp` | `PathSource`, `TryPreparePath`, `PreparedPath`, `PathView`, sampling, tangents, control bounds, joint continuity, Hermite and centripetal Catmull-Rom conversion |
| `distance.hpp` | `TryBuildDistanceMap`, `PreparedDistanceMap`, `TryBoundDistance`, `TryResolveDistance` |
| `track.hpp` | `TrackKey`, `TryPrepareTrack`, `PreparedTrack`, `TryEvaluateTrack` |
| `frame.hpp` | `FramePreparation`, `TryBuildFrames`, `PreparedFrames`, `TryEvaluateFrame`, seed, singularity, corner and closed-loop policies |
| `query.hpp` | `ProjectionScratch`, `TryProjectPoint`, `ProjectPointApproximate` |
| `sweep.hpp` | `SweepRequest`, `TryPlanSweepMesh`, `SweepMeshPlan`, `TryEmitSweepMesh` |
| `surface.hpp` | `SurfaceSource`, `TryPrepareSurface`, `PreparedSurface`, `SurfaceView`, patch/edge/corner records, surface evaluation and normals, `TryMapEdgeParameter` |
| `tessellation.hpp` | `SurfaceMeshProfile`, `TryBuildSurfaceMesh`, `SurfaceMesh`, `SurfaceMeshView`, `SurfaceMeshReport` |

## Contracts

Every function is `noexcept`; fallible ones return `[[nodiscard]] GeometryStatus`
and leave outputs unchanged on failure. Preparation builds a complete candidate
and moves it into the output owner only on success, so a failed rebuild keeps the
previous snapshot. Preparation reports the first failure through an optional
`GeometryDiagnostic`. Sampling, inversion and conversion into caller-owned ranges
never allocate. Owners are move-only and single-owner; views borrow them and are
invalidated by moving or destroying the owner.

- **Domains.** `SegmentParameter` is a span index and u in [0,1].
  `PathDistance` is prefix arc length in asset units. `TrackTime` is seconds.
  They are separate types. Kernel derivatives are per normalized u; track
  derivatives are per second.
- **Span sides.** An exact boundary selects the outgoing span at u = 0. The
  terminal endpoint selects the last span at u = 1. `SpanSide::Incoming` selects
  the previous span at u = 1; at the first endpoint both sides select the first
  span. A Step span holds its left value until u = 1.
- **Boundary policies.** Policies act only outside the closed domain. Paths
  accept `Reject`, `Clamp` and, when closed, `Wrap`. Tracks accept `Reject`,
  `Clamp`, `Hold`, `Loop` and `PingPong`. Every other combination is
  `InvalidArgument`, and nothing extrapolates.
- **Precision.** Source controls are float64. Preparation subtracts the chunk
  origin in float64, then narrows to float32. The effective position error,
  max(`PositionError`, `RelativePositionError` x `AssetScale`), must be at least
  the float32 position floor `2 * ulp32(E)` of the narrowed controls and their
  narrowing error. Otherwise preparation returns `UnsupportedPrecision`; split
  the asset into chunks or choose a nearer origin.
- **Connectivity.** Consecutive spans, and the closing joint of a closed path,
  must meet within `ContinuityPosition` (`InvalidTopology` otherwise). Each
  joint's C0/G1/C1/C2 classification compares derivatives scaled by each span's
  `DomainWidth` and stores the measured differences. It is a threshold decision
  on Rounded values.
- **No defaults.** `ToleranceProfile` and `WorkLimits` have no engine
  defaults. Every tolerance field is validated against its ADR 0024 range, and a
  zero work limit allows no work.

## Guarantees

| Result | Class |
| --- | --- |
| Stored endpoints, key values, span control boxes, position floor | Exact |
| Interior positions, derivatives, tangents, conversions, continuity values | Rounded |
| Leaf and total length bounds, prefix bounds, inversion residual | Certified |
| `TotalEstimate`, `PrefixEstimate`, `DistanceBound::Estimate` | Estimated, inside the Certified interval |
| Frame regularity decision (derivative box versus `MinimumSpeed`) | Certified |
| Frame vectors and positions | Rounded |
| `FramesView::AngularErrorEstimate`, residual twist | Estimated |
| Strict projection `Distance`, `LowerBound` and `Gap` | Certified |
| `ProjectPointApproximate` result | Approximate |
| Sweep ring frames and vertices | Rounded |
| Sweep longitudinal and cross-section error, self-intersection risk | Estimated |
| Shared-edge matching, corner identity, mesh corner positions | Exact |
| Surface positions, partials, normals, edge continuity values | Rounded |
| Mesh cell bounds (`SurfaceMeshPatch::PositionBound`), emitted position error bounds | Certified |
| Mesh normal-angle estimate | Estimated |

Certified bounds come from FoundationMath's [outward-rounded enclosures](math.md#outward-rounded-enclosures).
A strict `DistancePreparation` (the default) accepts them; there is no Estimated
fallback mode yet. Linear table lookup (an Approximate mode) is not provided.

## Paths

```cpp
#include <ludus/foundation/geometry/distance.hpp>
#include <ludus/foundation/geometry/path.hpp>
using namespace ludus::foundation::geometry;
using ludus::foundation::BorrowedView;
using ludus::foundation::math::Vector3d;

// Waypoints with uneven spacing: centripetal Catmull-Rom uses sqrt(chord) knots.
const Vector3d points[] = {{0, 0, 0}, {1, 0, 0}, {1, 4, 0}, {10, 4, 0}};
PathSpanSource spans[3];
usize spanCount = 0;
GeometryStatus status = TryConvertCentripetalCatmullRom(
    BorrowedView<const Vector3d>(points), false, 1e-6, BorrowedView<PathSpanSource>(spans), spanCount);

PreparedPath path;
if (IsSuccess(status))
{
    const PathSource source{.Spans = BorrowedView<const PathSpanSource>(spans, spanCount), .Revision = 1};
    status = TryPreparePath(source, PathPreparation{.Tolerance = tolerance, .Limits = limits}, path, nullptr);
}
```

`TryConvertCentripetalCatmullRom` uses the nonuniform Barry-Goldman tangent at
interior waypoints and the one-sided chord tangent at open endpoints. It rejects
consecutive waypoints within `coincidenceDistance` as `InvalidDomain`; editors
should offer a visible merge. A closed source wraps neighbors. It must not repeat
its first waypoint at the end: the repeated pair is coincident, so it is rejected.
`TryConvertHermite` applies `B1 = P0 + h*T0/3` and `B2 = P1 - h*T1/3` with each
waypoint's explicit `OutgoingWidth`. Both conversions check output capacity
before writing (`CapacityExceeded`).

`TryEvaluatePath` returns a chunk-relative position, or a `CubicSample3` with
derivatives per u. Add `PathView::Origin` in float64 for asset coordinates.
`TryEvaluatePathTangent` returns `Degenerate` where |dP/du| is at or below a
caller-supplied speed, for example at a cusp, a zero-speed ease endpoint or a
stationary span. Position at those points remains valid.

## Distance maps

`TryBuildDistanceMap` brackets each leaf's arc length between its chord and its
control-polygon length, using outward rounding. It splits the leaf with the
largest gap at its parameter midpoint, with ties broken by earlier span and
parameter, until the whole-path gap `TotalUpper - TotalLower` is at most the
path's `DistanceError`. The gap is rechecked in path order before success.
`MaxLeaves`, `MaxDepth`, `MaxSubdivisions` and `MaxPreparedBytes` bound the work.
Exhausting a limit returns `BudgetExceeded`, reports the achieved Certified gap in
the diagnostic and keeps the previous map.

```cpp
PreparedDistanceMap map;
TryBuildDistanceMap(path.GetView(), DistancePreparation{.Limits = limits}, map, nullptr);

DistanceResolution at;
const DistanceQuery query{.Distance = PathDistance{.Value = 2.5}, .Policy = BoundaryPolicy::Clamp,
                          .MaxIterations = 128};
if (IsSuccess(TryResolveDistance(path.GetView(), map.GetView(), query, at)))
{
    // |L(at.Parameter) - 2.5| <= at.ResidualBound <= DistanceError (Certified).
}
```

The `PathDistance` domain is [0, `TotalEstimate`]. `TryResolveDistance` finds
the leaf by monotone lookup over prefix estimates, then bisects partial-span
estimates for the earliest parameter that reaches the target. It accepts only
when the Certified residual, which includes every preceding leaf's uncertainty,
fits `DistanceError`. A target equal to a stored boundary estimate resolves to
that exact boundary. Zero-length runs resolve to their earliest parameter, and
the path end resolves to the last span at u = 1. Near zero speed, u(s) is
ill-conditioned even though its residual is bounded; request a tangent to detect
that case. Allow at least 64 iterations so bisection can reach float64
resolution. `TryBoundDistance` returns the Certified prefix enclosure at any
parameter.

A map records its path's `Revision` and span count. Queries through an unbuilt
map return `MissingCache`, and queries through a map of another revision return
`StaleRevision`. Sampling never builds or repairs a cache. Distance uses the
asset's local metric; nonuniform scale needs a separate world-metric cache.

## Tracks

```cpp
#include <ludus/foundation/geometry/track.hpp>

const TrackKey keys[] = {
    {.Time = 0.0, .Value = 0.0F, .Interpolation = TrackInterpolation::Linear},
    {.Time = 1.0, .Value = 1.0F, .OutSlope = 2.0F, .Interpolation = TrackInterpolation::Hermite},
    {.Time = 3.0, .Value = 1.0F, .Interpolation = TrackInterpolation::Step},
    {.Time = 3.5, .Value = 0.0F},
};
PreparedTrack track;
TrackSample sample;
if (IsSuccess(TryPrepareTrack(BorrowedView<const TrackKey>(keys), limits, track, nullptr)))
{
    TryEvaluateTrack(track.GetView(), TrackTime{.Seconds = 4.5}, BoundaryPolicy::PingPong, SpanSide::Outgoing,
                     sample);
    // Reflected to 2.5 s: Value 1.1875, DerivativePerSecond +0.625 (negated on the reflected half).
}
```

Key times must be strictly increasing; equal times are `InvalidDomain`, and the
runtime never resolves a tie. Each key names the interpolation of the span that
starts at it. Hermite spans use the left key's `OutSlope` and the right key's
`InSlope`, in value units per second. They may overshoot, and evaluation never
clamps the result. Author a timed hold as equal values over a positive interval.
`Hold` returns the endpoint value with zero derivative. `Clamp` returns the
endpoint value with the one-sided derivative from inside the domain.

## Transported frames

```cpp
#include <ludus/foundation/geometry/frame.hpp>

FramePreparation preparation{
    .SeedUp = Vector3d{0, 1, 0},
    .SeedPolicy = FrameSeedPolicy::UseFallbackAxis,
    .FallbackAxis = Vector3d{0, 0, 1},
    .SingularityPolicy = FrameSingularityPolicy::SplitRuns,
    .Limits = limits,
};
PreparedFrames frames;
if (IsSuccess(TryBuildFrames(path.GetView(), map.GetView(), preparation, frames, nullptr)))
{
    TransportedFrame frame;
    TryEvaluateFrame(path.GetView(), frames.GetView(), SegmentParameter{.SpanIndex = 0, .U = 0.4}, roll, frame);
    // frame.Tangent, frame.Normal and frame.Binormal = Tangent x Normal are right-handed and orthonormal.
}
```

`TryBuildFrames` bakes rotation-minimizing frames with the double-reflection
method of Wang, Juttler, Zheng and Liu. It needs the path's distance map, which
supplies sample distances and the closed-loop twist distribution. Each span is
stepped over dyadic intervals. An interval is regular when an outward-rounded
box of its derivative stays farther than `MinimumSpeed` from zero. A regular
interval is accepted when one reflection step and two half steps differ by at
most `TangentAngleError` times its share of the path. The summed differences,
plus the largest one, form `AngularErrorEstimate`. That value is Estimated, so a
`Strict` frame preparation returns `UnsupportedPrecision`.

The first run starts from `SeedUp` projected perpendicular to the first tangent.
When that projection is degenerate (its sine is at or below `MinimumSinAngle`),
`RejectDegenerateSeed` fails with `DegenerateFrame`. `UseFallbackAxis` projects
`FallbackAxis` instead and records `FrameRun::IsFallbackSeeded`.

| Case | Policy | Result |
| --- | --- | --- |
| Zero speed, cusp or reversal inside a span | `FrameSingularityPolicy::Reject` | `DegenerateFrame` with the offending parameter range |
| | `SplitRuns` | The run ends before the range; a new run is seeded after it |
| Joint that is not G1 under `ContinuityAngle` | `FrameCornerPolicy::Reject` | `DegenerateFrame` |
| | `RotateMinimally` | Rotates about the incoming x outgoing tangent; an exact reversal fails |
| | `SplitRuns` | A new run is seeded on the outgoing span |
| Closed path | `ClosedFramePolicy::DistributeTwist` | Adds `-ResidualTwist + 2 pi WindingTurns` of roll in proportion to estimated distance; the seam becomes periodic |
| | `PreserveHolonomy` | Keeps the transported frames and reports `ResidualTwist` |

A range that cannot be shown regular at the depth limit is 2^-`MaxDepth` wide.
`TryEvaluateFrame` returns `Degenerate` inside it and never reuses a frame
across it. Evaluation transports the preceding baked sample of the same run to
the analytic tangent with one reflection step, so its result depends only on the
cache and the request, not on earlier queries. It then applies the interpolated
twist compensation and the caller's roll. A joint has an incoming sample at
u = 1 and an outgoing sample at u = 0.

## Projection

```cpp
#include <ludus/foundation/geometry/query.hpp>

usize capacity = 0;
ComputeProjectionScratchCapacity(path.GetView(), 4096, capacity);
ProjectionScratch scratch;
scratch.TryReserve(capacity); // Once; queries never allocate.

ProjectionResult nearest;
const ProjectionQuery query{.Point = Vector3d{1.5, 0, 0}, .MaxIterations = 4096};
if (IsSuccess(TryProjectPoint(path.GetView(), query, scratch, nearest)))
{
    // Exact distance to nearest.Parameter <= nearest.Distance <= global minimum + ProjectionGap (Certified).
}
```

`TryProjectPoint` runs a branch-and-bound over control-hull boxes. Each span
starts as one interval; span starts and an open path's terminal endpoint are
candidates. Every iteration splits the interval with the smallest lower bound at
its midpoint and evaluates the midpoint. The search stops when the best
candidate minus the smallest live lower bound fits the path's `ProjectionGap`.
All bounds use outward rounding, and the query point's origin subtraction is
enclosed too. Any candidate within the gap of the global lower bound is an
acceptable answer; the result is the earliest by span, then parameter. Symmetric
minima therefore return the earlier one, and a minimum at a joint is named by
the outgoing span at u = 0.

Strict projection leaves its result unchanged on `BudgetExceeded` or
`UnsupportedPrecision`. `ProjectPointApproximate` runs the same search but
returns its earliest best candidate when work runs out, with `IsGapMet` false and
`Guarantee` Approximate. Scratch must hold `span count + 1 + MaxIterations`
entries; a smaller reservation is `CapacityExceeded` before any work.

## Sweeps

```cpp
#include <ludus/foundation/geometry/sweep.hpp>

const SweepProfileKey profile[] = {{.Distance = PathDistance{0.0}, .Scale = 0.1}};
SweepRequest request{
    .Section = SweepSection::Tube,
    .Profile = BorrowedView<const SweepProfileKey>(profile),
    .Caps = SweepCaps::Flat,
    .LongitudinalError = 1e-3,
    .CrossSectionError = 1e-3,
    .MaxSectionSegments = 64,
    .MaxVertexCount = 65536,
    .MaxIndexCount = 393216,
    .Limits = limits,
};
SweepMeshPlan plan;
if (IsSuccess(TryPlanSweepMesh(path.GetView(), map.GetView(), frames.GetView(), request, plan, nullptr)))
{
    // Size caller storage from plan.GetView().Report, then emit without further failure points.
    TryEmitSweepMesh(path.GetView(), plan.GetView(), vertices, indices);
}
```

Planning chooses rings and validates every count, limit and numeric range
before emission writes anything. It needs frames with one run over the whole
path and G1 joints; split a path at singularities or corners and sweep each
piece. Each span starts a ring and is stepped over dyadic intervals. An interval
is accepted when its control hull's distance from the chord plus the midpoint
deviation of every section point fits `LongitudinalError`. A tube uses the
fewest polygon segments whose error at the largest profile scale fits
`CrossSectionError`. Both errors are Estimated. The width or radius (`Scale`)
and roll are linear in estimated path distance between profile keys and clamped
outside them. A zero or negative scale is rejected, not normalized away.

A ribbon ring emits its -Binormal then +Binormal edge, faces the frame normal and
has no caps. A tube ring repeats its first vertex with U = 1 as an explicit UV
seam. `SweepCaps::Flat` closes an open tube with fans whose rim vertices copy
the ring positions bitwise but carry the cap normal. V is estimated path
distance in asset units. A closed path needs periodic frames
(`DistributeTwist`); its last ring copies the first ring's positions and normals
with V equal to the total length. Triangles are counter-clockwise from the side
their normals face unless `SweepWinding::Clockwise` is requested.

`TryEmitSweepMesh` checks the pinned path revision (`StaleRevision`) and both
capacities (`CapacityExceeded`) before writing. The report's
`SelfIntersectionRiskCount` counts rings whose section half-extent times path
curvature reaches 1. This sampled local check is Estimated: it can flag a risk
but never proves the surface simple.

## Surfaces

A `SurfaceSource` lists untrimmed bicubic patches, each with a unique author
`Id` and float64 controls `Control[u][v]`, plus explicit
`SurfaceAdjacencySource` records that join two whole patch sides. Each side has
an along-edge parameter s: v for `U0` and `U1`, u for `V0` and `V1`. A join
declares whether the second side runs opposite to the first (`IsReversed`).

```cpp
#include <ludus/foundation/geometry/surface.hpp>

SurfacePatchSource patches[2] = {/* Id = 10 and Id = 20, float64 controls */};
// The first patch's u = 1 side is the second patch's u = 0 side, with v reversed.
const SurfaceAdjacencySource join{.FirstPatch = 0, .FirstSide = PatchSide::U1,
                                  .SecondPatch = 1, .SecondSide = PatchSide::U0, .IsReversed = true};
PreparedSurface surface;
const GeometryStatus status = TryPrepareSurface(
    SurfaceSource{.Patches = BorrowedView<const SurfacePatchSource>(patches),
                  .Adjacency = BorrowedView<const SurfaceAdjacencySource>(&join, 1), .Revision = 1},
    SurfacePreparation{.Tolerance = tolerance, .Limits = limits}, surface, nullptr);
```

`TryPrepareSurface` narrows controls as `TryPreparePath` does, then builds the
topology. V1 accepts only matching whole sides with at most two incident patches
per edge. The two narrowed control curves must be equal after the declared
mapping. A mismatched curve, a partial join, a third incidence on a side, a side
joined to itself, an unknown patch index or a duplicate patch ID is
`InvalidTopology`; nothing is welded by a distance threshold. A side may join
another side of the same patch, which closes it into a tube.

The view holds one `SurfaceEdge` per edge and one `SurfaceCorner` per corner.
An edge's owner is the incidence with the smallest (patch ID, side). Its
`SurfaceEdgeId` and canonical control curve come from that owner, so they do not
depend on patch order. `TryMapEdgeParameter` converts a canonical edge
parameter into each incident patch's (u, v). Corners joined through shared edges
form one record with one exact position and every incident patch corner.
Corners that only coincide geometrically remain separate records.

Shared edges also report two properties, without guessing smoothness.
`IsOrientationConsistent` says whether the two patches traverse the edge in
opposite directions along their counterclockwise (u, v) boundaries, so their
`Cross(DU, DV)` normals agree in orientation. `IsC1` compares the outward
cross-boundary derivative controls; it is a threshold decision on Rounded
values. G1 tangent-plane classification is not provided.

`TryEvaluateSurface` returns positions or all partials per normalized U and V.
`TryEvaluateSurfaceNormal` uses the surface's `NormalMinDerivativeLength` and
`NormalMinSinAngle`. It returns `Degenerate` for zero or parallel partials and
`IllConditioned` at or below the thresholds; the position remains valid.

## Surface meshes

`TryBuildSurfaceMesh` tessellates every patch as a dyadic grid of
2^`LevelU` x 2^`LevelV` cells. Each cell becomes two triangles along its
(u0, v0)-(u1, v1) diagonal, counterclockwise in (u, v). The function builds a
complete `SurfaceMesh` staging owner and moves it into the output only on
success.

```cpp
#include <ludus/foundation/geometry/tessellation.hpp>

const SurfaceMeshProfile profile{
    .Precision = PrecisionProfile::Strict, .PositionError = 0.01,
    .Degeneracy = SurfaceDegeneracyPolicy::Report,
    .Limits = {.MaxLevel = 10, .MaxCellTests = 1 << 20, .MaxVertices = 1 << 20,
               .MaxTriangles = 1 << 20, .MaxOutputBytes = 64 << 20}};
SurfaceMesh mesh;
if (IsSuccess(TryBuildSurfaceMesh(surface.GetView(), profile, mesh, nullptr)))
{
    const SurfaceMeshView view = mesh.GetView();
    // view.Report.PositionBound <= 0.01 (Certified); upload view.Vertices and view.Indices.
}
```

The cell bound adds three terms. The first is the largest distance between the
subpatch controls and the degree-elevated bilinear interpolant of its corners.
The second is the twisted bilinear quad's distance to its two triangles,
`Length(P00 - P10 - P01 + P11)/4`. The third is the rounding error of the
emitted vertices. Each term uses outward-rounded enclosures, so
`PositionBound` is Certified for the parametric distance between the prepared
surface and the mesh.

Every patch starts at level (0, 0). While a cell misses the profile, the
direction whose refinement gives the clearly smaller normalized bound is
refined. Near-ties refine the coarser direction, then u. Each shared edge then
takes the larger of its two along-edge levels. Patches whose levels changed are
rechecked until the network agrees. This can over-tessellate; for example, a
planar patch beside a curved edge inherits that edge's level.

`SurfaceMeshView::Positions` emits each corner and each edge sample once. Edge
samples are evaluated on the canonical curve, interior samples on their patch.
Every `SurfaceMeshVertex` copies its shared position bitwise and names its
`PositionIndex`, while its normal and `Parameter` belong to its own patch. Two
patches therefore meet without cracks or T-junctions, and their normal or
parameter seams stay visible. A surface parameter is never a texture coordinate.

Failure behavior is explicit:

- `Strict` accepts only Certified position bounds. A nonzero
  `NormalAngleError` is `UnsupportedPrecision`, because only an Estimated
  normal bound exists. `IsCoarserMeshAccepted` is `InvalidArgument`.
- `Visual` may request `NormalAngleError`. Refinement then also samples analytic
  normals at cell corners and centres, skipping singular samples, and reports an
  Estimated angle. With `IsCoarserMeshAccepted`, a patch that `MaxLevel` stops
  keeps its finest grid. The mesh then reports `IsWithinPositionError = false`
  and the achieved Certified bound instead of failing.
- `MaxCellTests`, `MaxVertices`, `MaxTriangles` and `MaxOutputBytes` always fail
  with `BudgetExceeded`. Counts are checked before allocation. More than 2^32
  vertices is `OutOfRange`.
- `SurfaceDegeneracyPolicy::Reject` fails on a singular vertex normal or a
  triangle with two equal positions. `Report` emits a zero normal with its
  status and counts both cases in the report.

`PositionError` must be at least the surface's position floor. Errors are
measured against the prepared float32 patches; `TryPrepareSurface` checks source
narrowing separately. Every failure keeps the previous mesh.

## Convex polygon partition

`TryPartitionConvexPolygons` turns a triangle mesh, such as an emissive light
mesh, into the convex planar polygons that FoundationMath's
[projected solid angle sampling](math.md#projected-solid-angle-sampling) expects:
at most seven vertices, no three collinear, as Peters 2021 requires in section 3.1
and supplement A.2.

```cpp
#include <ludus/foundation/geometry/convex_polygon.hpp>

ConvexPolygonPartition partition;
if (IsSuccess(TryPartitionConvexPolygons({positions, positionCount}, {indices, indexCount}, partition)))
{
    for (const ConvexPolygon& polygon : partition.Polygons)
    {
        // polygon.PositionsMesh[0 .. VertexCount) wind counterclockwise about polygon.NormalMesh;
        // partition.Triangles[FirstTriangle .. FirstTriangle + TriangleCount) are its source triangles.
    }
}
```

Triangles are matched by equal positions, so split vertices along normal or UV
seams still share edges. Two pieces merge across a shared edge when the edge has
exactly two triangles of opposite direction and the union stays coplanar, convex,
simple and within seven vertices once collinear vertices are removed. Merging
follows the whole chain of edges the pieces share, so a fan around a center
vertex or a tessellated grid can collapse into one polygon. Non-convex patches and
patches with holes become several convex polygons, as the paper asks, instead of
being rejected. This adapts the inessential-diagonal removal of Hertel and
Mehlhorn, "Fast triangulation of simple polygons", FCT 1983, LNCS 158,
pp. 207-218; the greedy order is deterministic but the partition is not minimal.

Every nondegenerate triangle belongs to exactly one polygon, and a lone triangle
is a polygon of three vertices. Output positions are bitwise source positions.
Triangles with coincident or collinear corners are counted in
`DegenerateTriangleCount` and left out; edges with more than two triangles or
inconsistent winding are counted in `UnmergeableEdgeCount` and never crossed.
Coplanarity and collinearity use tolerances relative to the polygon's extent with
a floor relative to float32 coordinate magnitude; far from the origin, rounding
can keep a fan's center vertex visibly off its chord, leaving several convex
pieces. Invalid views or index counts, out-of-range indices and nonfinite
positions fail without changing the output. The partition is CPU preparation
work; it allocates its scratch and output arrays.

## Not yet provided

Path cursors for coherent traversal, a Visual-only Estimated distance build,
Approximate table lookup, certified frame angular error, mitred or split sweeps,
G1 surface classification, camera-dependent and displaced meshes, quadtree
tessellation, cooked formats and editor adapters are later work. Performance
and memory budgets are unmeasured.
