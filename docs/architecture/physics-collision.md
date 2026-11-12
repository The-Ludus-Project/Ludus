# Physics collision module

`Ludus::PhysicsCollision` (`modules/physics/collision/`) is the first slice of
physics gate P1 in the [physics systems architecture](physics-systems.md#delivery-gates).
It exports an immutable static triangle mesh with a bounded bounding volume
hierarchy (BVH), a closest raycast and a closest translation-only cast of an
axis-aligned box. It depends only on FoundationBase, FoundationContainers and
FoundationMath, contains no third-party physics code, and builds for native
and Emscripten targets. The installed SDK exports it as a component.

Public headers:

| Header | Contents |
| --- | --- |
| `<ludus/physics/collision/status.h>` | `Status`, the coordinate envelope and `INVALID_TRIANGLE_INDEX` |
| `<ludus/physics/collision/queries.h>` | Raycast and box-cast inputs, results and policies |
| `<ludus/physics/collision/triangle_mesh.h>` | Cook source, settings and result, and `TriangleMesh` |

The Doxygen comments beside each declaration are the API contract. This page
explains the model, its limits and how to use it.

## Cooking a mesh

```cpp
#include <ludus/physics/collision/triangle_mesh.h>

using namespace ludus::physics::collision;

TriangleMesh mesh;
const TriangleMeshSource source{
    .PositionsMesh = BorrowedView<const Vector3>(positions, positionCount),
    .Indices = BorrowedView<const uint32>(indices, indexCount),
};
const TriangleMeshCookResult cooked = TriangleMesh::TryCook(source, {}, mesh);
if (cooked.Result != Status::Success)
{
    // cooked.TriangleIndex names the first offending triangle when one is to blame.
}
```

Cooking validates before it allocates, in source order:

| Check | Failure |
| --- | --- |
| Both views valid, index count a multiple of three, known policy | `InvalidArgument` |
| At least one triangle | `NoGeometry` |
| At most `MAX_TRIANGLE_MESH_TRIANGLES` (8 000 000) triangles | `CapacityExceeded` |
| Every index refers to a position | `IndexOutOfRange` with the triangle |
| Referenced coordinates finite | `NonFiniteInput` with the triangle |
| Referenced coordinates within `MAX_COORDINATE_METERS` (100 km) | `OutOfRange` with the triangle |
| Twice the area above `MIN_TRIANGLE_SHAPE_RATIO` (1e-7) times the longest edge squared | `DegenerateTriangle` with the triangle, or skipped |

The degeneracy test is scale free: the same shape is admitted or rejected at
any size. `DegenerateTrianglePolicy::Reject` is the default, so unusable
geometry is reported. `Skip` leaves degenerate triangles out and counts them in
`GetSkippedTriangleCount()`; the remaining triangles keep their source indices.
A source whose triangles are all skipped fails with `NoGeometry`.

Cooking copies the admitted triangles, so the source views need to stay alive
only for the call. It then reserves storage for one triangle record (40 bytes)
and at most one node (36 bytes) per triangle. Those two allocations are the
only ones; if either fails the result is `AllocationFailure` and the output
mesh keeps whatever it held before. Any failure leaves the output unchanged.

The hierarchy is built in preorder with an explicit stack. Each node splits at
the centroid midpoint of its longest axis, preferring the vertical axis when it
is the longest; a split leaving either side under a quarter of the node falls
back to a split by count. Leaves hold at most eight triangles. Every child keeps
at most three quarters of its parent, so the largest admitted mesh needs at most
50 levels, below `MAX_TRIANGLE_MESH_HIERARCHY_DEPTH` (64). Cooking is
deterministic: the same source and settings produce the same hierarchy.

A cooked mesh never changes. Queries are `const`, never allocate and keep no
state, so several threads may query one mesh while no thread moves or destroys
it. The mesh is static environment geometry: it has no mass properties and is
not a dynamic shape.

## Queries

Queries run in the mesh's local space, in meters, with `float64` origins and
results. Both return `[[nodiscard]] Status`: a miss is `Success` with
`HasHit == false` and default fields; every failure leaves the result
unchanged. An uncooked or moved-from mesh returns `NoGeometry`.

### Closest raycast

`RaycastClosest` takes a finite origin within the coordinate envelope, a unit
direction (squared length within 1e-6 of one) and a distance limit in
(0, `MAX_QUERY_DISTANCE_METERS`]. A zero direction or zero distance is a
zero-length ray and returns `InvalidArgument`.

- Hits at distance zero and at exactly the limit are admitted. A ray that starts
  on a triangle reports distance zero with that triangle's normal.
- The ray/triangle test is watertight: a ray through a shared edge or vertex of
  adjacent triangles hits at least one of them. A ray parallel to a triangle's
  plane, including one lying in it, misses that triangle.
- `NormalMesh` is the unit geometric normal from the winding,
  `Cross(second - first, third - first)`; it is not turned toward the ray.
  `IsFrontFace` reports whether the ray points against it.
- `TriangleFacePolicy` selects two-sided, front-face-only or back-face-only hits.
- Equal distances resolve to the lowest source triangle index.

### Closest box cast

`CastBoxClosest` translates an axis-aligned box (no rotation) from
`CenterMesh` by `TranslationMesh`. Half extents must be positive; extents and
translation components are bounded by `MAX_QUERY_DISTANCE_METERS`. Triangles are
two-sided for casts.

- `Fraction` lies in [0, 1). A contact that would begin exactly at the end of
  the translation is a miss: the box completes its translation in contact.
- `NormalMesh` points from the triangle toward the box. Touching at the start and
  moving into a triangle is a hit at fraction zero with a normal; moving away
  or parallel while touching is a miss.
- `InitialOverlapPolicy` is explicit. `Report` returns the overlap as a hit at
  fraction zero with `HasStartedOverlapping` set and no normal. `Ignore`
  excludes initially overlapping triangles from the whole cast. `Reject` returns
  `Status::InitialOverlap` and leaves the result unchanged.
- A zero translation performs only the selected initial-overlap test.
- Ordering is smaller fraction, then an initial overlap before a touching
  contact at the same fraction, then the lowest source triangle index.

## Determinism and the correctness oracle

Ordering never uses an epsilon comparison: finite values compare strictly, then
identity breaks ties, which keeps the order transitive. The per-triangle
arithmetic never depends on the current best hit.

Every query input has a `Traversal` field. `QueryTraversal::AllTriangles`
tests every triangle without the hierarchy and is the brute-force correctness
oracle; it returns exactly the hierarchy's result at linear cost. Hierarchy
traversal prunes a node only when a conservative bound proves it cannot hold a
nearer or tied hit: slab and sweep bounds are widened by 2^-40 of the query's
scale, well above `float64` rounding.

The module compiles with `-ffp-contract=off`. The watertight test relies on the
exact sign antisymmetry of `a*b - c*d` between neighboring triangles, which a
fused multiply-add would break.

## Tests and SDK evidence

`ludus_physics_collision_tests` covers analytic floor, wall and slanted-ray
answers; tangent, shared-edge, shared-vertex, origin-on-surface, parallel and
end-of-translation cases; every validation failure with output preservation;
skip policy and scale-free degeneracy; deterministic cooking of a 28 800
triangle terrain; and the hierarchy against the all-triangles oracle on seeded
random triangle soups, random terrain and duplicated geometry, with exact
result equality.

`ludus_physics_collision_allocation_tests` replaces the global allocation
functions to fail each cook allocation in turn, cooking into an empty mesh,
over a cooked mesh and with skipped triangles, and checks that each failure
reports `AllocationFailure` and leaves the mesh unchanged. It also checks that
rejected sources and queries allocate nothing. Sanitizer and MSVC builds skip
this executable because their runtimes replace the same functions.

The installed SDK consumer (`tests/sdk_consumer/collision.cpp`) links
`Ludus::PhysicsCollision`, cooks a quad and runs a raycast and a box cast.

## Attribution

- Sven Woop, Carsten Benthin and Ingo Wald,
  [Watertight Ray/Triangle Intersection](https://jcgt.org/published/0002/01/05/),
  *Journal of Computer Graphics Techniques* 2(1), 2013, pages 65-82: the shear
  into ray space and the edge functions used by the raycast, evaluated in
  `float64` from `float32` corners.
- Thiago Ize, [Robust BVH Ray Traversal](https://jcgt.org/published/0002/02/02/),
  *Journal of Computer Graphics Techniques* 2(2), 2013, pages 12-27: widening
  slab bounds by their rounding error. Ludus uses a looser, scale-aware bound.
- David Eberly,
  [Intersection of Convex Objects: The Method of Separating Axes](https://www.geometrictools.com/Documentation/MethodOfSeparatingAxes.pdf),
  Geometric Tools, sections 4 and 6: the time intervals on the 13 separating
  axes of a moving box and a triangle. The sweep and the hierarchy partition
  are lifted from the editor's Explore as Player preview.

## Remaining P1 work

This slice is not the whole gate. Still to come: primitive and convex-hull
shapes and the general convex cast; shape, collider and world identities;
filters; the geometry store and protected read views; overlap queries and
`Any`/`All` query modes with completion semantics; query debug overlays;
moving the editor's Explore as Player onto these queries with **Static
Collider** as authored input; and Emscripten SDK consumer evidence.
