# Ludus::FoundationGeometry

Prepared spatial paths, scalar tracks, bounded distance maps, prepared patch
surfaces and crack-free surface meshes above the FoundationMath cubic and
bicubic kernel. These are phases G2 and G4 of the
[curves and surfaces architecture](../../architecture/curves-surfaces.md). Names,
statuses, domains and guarantee classes follow
[ADR 0024](../../decisions/0024-curves-surfaces-contracts.md). Validation is
recorded in the evidence ledger for
[G2](../../architecture/math-evidence/curves-surfaces.md#g2-prepared-paths-tracks-and-distance-maps)
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
| `surface.hpp` | `SurfaceSource`, `TryPrepareSurface`, `PreparedSurface`, `SurfaceView`, patch/edge/corner records, surface evaluation and normals, `TryMapEdgeParameter` |
| `tessellation.hpp` | `SurfaceMeshProfile`, `TryBuildSurfaceMesh`, `SurfaceMesh`, `SurfaceMeshView`, `SurfaceMeshReport` |

Frames and projection (`frame.hpp`, `query.hpp`) belong to phase G3.

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

## Not yet provided

Path cursors for coherent traversal, a Visual-only Estimated distance build,
Approximate table lookup, transported frames, projection, sweeps, G1 surface
classification, camera-dependent and displaced meshes, quadtree tessellation,
cooked formats and editor adapters are later phases. Performance and memory
budgets are unmeasured.
