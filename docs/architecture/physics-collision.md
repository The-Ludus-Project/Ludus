# Physics collision module

`Ludus::PhysicsCollision` (`modules/physics/collision/`) is the first slice of
physics gate P1 in the [physics systems architecture](physics-systems.md#delivery-gates).
It exports an immutable static triangle mesh with a bounded bounding volume
hierarchy (BVH), a closest raycast, a closest translation-only cast of an
axis-aligned box, and a general closest cast of a posed sphere, box, capsule or
convex hull. A geometry store owns cooked shapes behind leased handles, and a
collision world places colliders that lease them and answers filtered world
queries through protected read views. It depends only on FoundationBase,
FoundationContainers, FoundationIdentity and FoundationMath, contains no third-party physics code, and builds for native
and Emscripten targets. The installed SDK exports it as a component.

Public headers:

| Header | Contents |
| --- | --- |
| `<ludus/physics/collision/status.h>` | `Status`, the coordinate envelope and `INVALID_TRIANGLE_INDEX` |
| `<ludus/physics/collision/convex_shape.h>` | `ConvexShape`, its `Make*Shape` constructors, and `ConvexHull` with its cook source and result |
| `<ludus/physics/collision/queries.h>` | Raycast, box-cast and convex-cast inputs, results, policies and tolerances |
| `<ludus/physics/collision/triangle_mesh.h>` | Cook source, settings and result, and `TriangleMesh` |
| `<ludus/physics/collision/geometry_store.h>` | `ShapeHandle`, `ShapeKind` and `GeometryStore` |
| `<ludus/physics/collision/collision_world.h>` | `ColliderHandle`, poses, participation, filters, world query inputs and results, `CollisionWorld` and `CollisionReadView` |

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

## Convex shapes

A `ConvexShape` is a value that names one of four kinds, in its own shape
space and meters:

| Kind | Constructor | Admitted values |
| --- | --- | --- |
| Sphere | `MakeSphereShape(radius)` | Radius in (0, `MAX_QUERY_DISTANCE_METERS`] |
| Box | `MakeBoxShape(halfExtents)` | Each half extent in (0, `MAX_QUERY_DISTANCE_METERS`] |
| Capsule | `MakeCapsuleShape(halfHeight, radius)` | Segment from -`halfHeight` to +`halfHeight` along shape +Y, rounded by `radius`; half height in [0, `MAX_QUERY_DISTANCE_METERS`], radius positive |
| Hull | `MakeHullShape(hull)` | A cooked `ConvexHull`, borrowed for each query that uses it |

Queries check these values; a shape is not validated when it is made. The
total height of a capsule is `2 * (halfHeight + radius)`.

### Cooking a convex hull

```cpp
ConvexHull hull;
const ConvexHullCookResult cooked = ConvexHull::TryCook(
    { .PointsShape = BorrowedView<const Vector3>(points, pointCount) }, hull);
```

`ConvexHull::TryCook` validates before it allocates:

| Check | Failure |
| --- | --- |
| A valid view | `InvalidArgument` |
| At least one point | `NoGeometry` |
| At least `MIN_CONVEX_HULL_POINTS` (4) points | `DegenerateShape` |
| At most `MAX_CONVEX_HULL_POINTS` (256) points | `CapacityExceeded` |
| Coordinates finite | `NonFiniteInput` with the point |
| Coordinates within `MAX_COORDINATE_METERS` | `OutOfRange` with the point |
| Thicker than `MIN_CONVEX_HULL_THICKNESS_RATIO` (1e-6) of its length | `DegenerateShape` |

The flatness test is scale free. Cooking takes the two points farthest apart
along the widest axis, then the point farthest from their line, then the point
farthest from the plane of those three. Either distance at or below the ratio
times the first separation rejects the set: coincident, collinear and coplanar
points fail. The centroid of those four points is the hull's interior point.

The hull copies its points in one allocation and keeps duplicates and interior
points. Support queries visit every stored point, which is exact for the
convex hull of the set, because an interior point never changes the answer.
The hull builds no face list. Faces, volume and mass properties belong to the
rigid-body gate. A failed cook, including an allocation failure, leaves the
output hull unchanged.

### Closest convex cast

`CastConvexClosest` places a shape at `PositionMesh` with `OrientationMesh`
(shape axes to mesh axes) and translates it by `TranslationMesh` without
rotating. The orientation's squared norm must be within
`UNIT_QUATERNION_SQUARED_TOLERANCE` (1e-5) of one; the cast renormalizes it in
`float64`. Positions, translations and policies follow the box cast.

This is the module's one general convex path: a GJK ray cast (Gino van den
Bergen, 2004) of the origin along the translation against the Minkowski
difference of each candidate triangle and the shape. A sphere is cast as a
point core and a capsule as a segment core, each rounded by its radius; boxes
and hulls are their own cores. Every core is a polytope, so the support search
ends in a finite number of steps rather than converging slowly on a curved
surface. `CastBoxClosest` stays as the exact separating-axis kernel for
axis-aligned boxes.

Contact is decided with an absolute tolerance,
`CONVEX_CAST_RELATIVE_TOLERANCE` (1e-9) times the query scale. The scale is
the largest coordinate of the mesh bounds relative to the starting position,
plus the largest translation component and the shape's largest posed extent.
A 20 m scene therefore resolves contact to about 20 nm.

- A triangle **starts overlapping** when the support search proves it deeper
  than the tolerance inside the shape, or encloses the origin exactly. `Report`
  returns it at fraction zero with no normal and no witness, as for the box.
- A triangle is **touching** at the start when its distance is within the
  tolerance of zero. Moving into it is a hit at fraction zero with a normal;
  moving away or parallel is a miss. Exact touching of two flat faces can
  resolve as either touching or overlap.
- Otherwise the shape **first contacts** a triangle at the smallest fraction
  where their distance falls to the tolerance, and a contact at or after the
  end of the translation is a miss.
- `NormalMesh` is the separating direction at first contact, from the triangle
  toward the shape. `WitnessPositionMesh` is the point of the triangle closest
  to the shape there.
- Ordering, the overlap policies and the zero-translation rule are those of the
  box cast.

Each triangle may spend at most `MAX_CONVEX_CAST_ITERATIONS` (128) support
steps; the search also stops as soon as a support point repeats. A triangle
that exhausts the bound fails the whole query with `Status::NotConverged`. It
reports a numerical failure and never guesses a contact. No test scene has
reached it. Because a pruned node is never visited, a failing triangle may
fail only the traversal that visits it.

## Geometry store and collision world

```cpp
GeometryStore store;
CollisionWorld world;
ShapeHandle floor;
ColliderHandle collider;
if (store.TryInitialize({ .MaxShapes = 64 }) == Status::Success &&
    store.TryAddTriangleMesh(Move(cookedFloor), floor) == Status::Success &&
    world.TryInitialize(store, { .MaxColliders = 256 }) == Status::Success &&
    world.TryCreateCollider({ .Shape = floor, .Pose = { .PositionWorld = {0, 1, 0} }, .Binding = entityBits },
                            collider) == Status::Success)
{
    CollisionReadView view;
    WorldRaycastResult hit;
    if (world.TryAcquireReadView(view) == Status::Success &&
        view.RaycastClosest({ .OriginWorld = eye, .DirectionWorld = forward, .MaxDistanceMeters = 50 }, hit) ==
            Status::Success && hit.HasHit == true)
    {
        // hit.Collider and hit.Binding name what was hit.
    }
}
```

### Identities and leases

`ShapeHandle` and `ColliderHandle` are distinct
[FoundationIdentity handles](object-handles-uuid.md#runtime-handle-representation):
an owner token, a slot and a generation. Each store and world takes a fresh
process-unique owner token when it is initialized, so a handle from another
store or world is rejected with `InvalidHandle`. Releasing a shape or
destroying a collider advances its slot's generation, so every copy of the old
handle becomes stale; a slot released at the terminal generation is retired and
never reused. Generations never wrap.

`GeometryStore` owns cooked triangle meshes, convex hulls and sphere, box and
capsule primitives. Each `TryAdd*` call moves the shape in only on success and
returns a handle carrying one lease. `TryRetain` and `TryRelease` add and drop
leases; the last release frees the shape. A collider takes its own lease when
it is created and releases it when it is destroyed, so the creator may release
its lease at once and the shape lives as long as any collider uses it. Stored
shapes never move.

`CollisionWorld` places colliders. A `ColliderDescription` names a leased shape, a
`RigidPose` (position and unit orientation), `ColliderParticipation`
(`IsSolid`, `IsSensor` and `IsQueryable`; solid and sensor are exclusive), a
`CollisionFilter` (category bits and the categories it collides with, tested
symmetrically by `ShouldCollide`), an enabled flag and an opaque `Binding`, such
as an entity, that every hit copies. This slice admits triangle-mesh shapes
only; a convex shape is rejected with `Unsupported` until overlap queries add
convex-against-convex tests.

Both objects reserve all their slots when they are initialized. After that,
adding and releasing shapes and creating, moving, enabling and destroying
colliders never allocate, and a failure leaves every output and lease count
unchanged. Neither object is copyable or movable: a world keeps its store's
address, and a read view keeps its world's. Destroy worlds before their store;
the store asserts this.

### Read views and phases

Queries run through a `CollisionReadView`. While any view of a world is active,
every mutation of that world fails with `InvalidPhase`, so nothing a query reads
can change or be destroyed. A view records the world's epoch, which every
successful mutation advances. Views are move-only and release their protection
when destroyed or released.

Acquire, move and release views, and mutate the world and store, on one owner
thread. While a view is active, any number of threads may query through it.
Store additions and releases on the owner thread never touch a shape another
lease holds, so they may proceed while other threads query.

### Filtered world queries

`CollisionReadView::RaycastClosest` and `CastConvexClosest` take world-space
inputs with the same admitted values and failures as the mesh queries, plus a
directed `QueryFilter`. A collider is reported only when it is live, enabled
and queryable; one of its category bits is in `CategoryMask`; it is not a sensor
unless `ShouldIncludeSensors`; and it is not in `IgnoredColliders` (at most
`MAX_QUERY_IGNORED_COLLIDERS`, 64).

Each query moves into each candidate collider's local frame, runs that mesh's
query and maps the answer back: normals and witness points rotate into world
axes, and distances and fractions are unchanged by the rigid transform. A convex
shape's rotation is composed with the collider's in `float64`, not through a
`float32` quaternion. Results add the collider handle and binding. Equal
distances or fractions resolve to the lower collider slot, then generation, then
triangle index.

Collider-local coordinates must lie in the 100 km envelope. When a query's
start lies outside it in a collider's frame, the query starts where its path
first reaches that collider's bounds, which lie inside the envelope. A convex
shape that is still outside the envelope there fails with `OutOfRange`.

There is no collider hierarchy yet. Each query tests every reported collider's
conservative world bounds against the query's path before it traverses that
mesh, so its cost grows with the collider count. `QueryTraversal::AllTriangles`
skips that test as well as the mesh hierarchies and is the brute-force oracle
for both.

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

The same executable covers hull cooking, with each failure, scale-free
flatness and output preservation; analytic drops of a sphere, upright and lying
capsules, an axis-aligned and a rotated box and a hull onto a floor; agreement
with the separating-axis box cast on a wall; the touching, overlap and policy
rules; and every convex-cast validation failure. The convex cast is compared
with an independent first-contact oracle on seeded random triangle soups, with
random sizes, orientations and translations for every shape kind. The oracle
uses exact point-triangle and segment-triangle distances for spheres and
capsules and the oriented-box separating-axis gap for boxes and box-shaped
hulls. Translating separation is convex in time, so a golden-section search
and a bisection find the first contact. Contact distances agree within 1 µm
(about 4e-8 m observed), and the hierarchy equals the all-triangles traversal
exactly, also for random point-cloud hulls over terrain.

`ludus_physics_collision_allocation_tests` replaces the global allocation
functions to fail each cook allocation in turn, cooking meshes into an empty
mesh, over a cooked mesh and with skipped triangles, and hulls into an empty
and over a cooked hull. It checks that each failure reports
`AllocationFailure` and leaves the output unchanged, and that rejected sources
and every query, including convex casts of each shape kind, allocate nothing. Sanitizer and MSVC builds skip
this executable because their runtimes replace the same functions.

The world tests cover:
- store and world initialization failures;
- lease counts through shape release and reuse;
- stale, malformed and foreign handles;
- every collider creation failure with output and lease preservation, and capacity exhaustion;
- read-view protection of every mutation, view moves and epochs;
- rotated colliders with analytic hit distances, normals and witnesses;
- each query filter rule and the symmetric pair filter;
- tie ordering by collider slot;
- colliders whose local start lies 108 km outside the envelope.

On 24 randomly posed colliders with sensors, disabled colliders and category
masks, world queries equal the brute-force traversal exactly (about half of
the queries hit). A rotated, translated collider also agrees within 0.1 mm
with the same triangles baked into a world-space mesh.

The allocation tests also cover store and world initialization under each
injected allocation failure, and they show that adding a shape, creating,
moving and destroying a collider and running world queries allocate nothing.

The installed SDK consumer (`tests/sdk_consumer/collision.cpp`) links
`Ludus::PhysicsCollision`, cooks a quad and a tetrahedral hull, runs a
raycast, a box cast and convex casts of a sphere and the hull, and then puts
the quad in a store and world and casts a ray through a read view.

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
- Gino van den Bergen, *Ray Casting against General Convex Objects with
  Application to Continuous Collision Detection*, 2004, and *Collision
  Detection in Interactive 3D Environments*, Morgan Kaufmann, 2003, section
  4.3: the GJK ray cast that advances by the separating-plane distance while it
  refines the simplex, and casting rounded shapes as a core plus a margin.
  Ludus adds the initial-overlap classification, the scale-aware tolerance and
  the bounded, reported failure.
- Christer Ericson, *Real-Time Collision Detection*, Morgan Kaufmann, 2005,
  sections 5.1.2, 5.1.5 and 5.1.6: the closest points on a segment, triangle
  and tetrahedron that reduce the GJK simplex, here with explicit fallbacks for
  degenerate simplices. Section 5.1.9 supplies the test oracle's
  segment-segment distance.

## Explore as Player

The editor's [Explore as Player](../development/editor-workspace.md#explore-from-a-first-person-player)
preview (`apps/editor/src/viewport_walk_collision.cpp`) is the first consumer.
When a walk starts it transforms the triangles of every entity with **Static
Collider** into world space and cooks them, so mesh space is World space and
the 100 km coordinate envelope applies to the collider geometry and to the
player. The cook uses `DegenerateTrianglePolicy::Skip`, because imported
meshes often carry slivers the preview has always ignored.

The walk rules stay in the editor: the 0.48 × 1.75 × 0.48 m box, step-up,
ground snap, slide, jump, the 45° slope limit and the fall reset. A contact
supports the player only when both its separating normal and the triangle's
face are within 45° of up. Because the module reports a triangle index rather
than the triangle, the editor cooks two meshes instead of one: the triangles
whose face can support the player, and every other triangle. Each walk cast
queries both with `InitialOverlapPolicy::Report` and takes the earlier contact.

That split also keeps the stair-tread preference. A box falling onto a step
edge meets the riser's top edge and the tread at the same landing time, and
rounding can make either fraction the smaller. For a straight-down cast the
editor treats fractions within 1e-12 of each other as one landing and uses
the tread's support normal; the module's exact ordering is unchanged.

A query that fails validation blocks the move like an initial overlap, so the
player cannot pass through geometry, and a player who leaves the coordinate
envelope returns to the spawn. Starting reports more than
`MAX_TRIANGLE_MESH_TRIANGLES` collider triangles, collider geometry outside the
envelope and a spawn outside it as separate failures that name the limit.

## Remaining P1 work

This slice is not the whole gate. Still to come:
- convex colliders, with convex-against-convex casts;
- overlap queries and `Any`/`All` query modes with completion semantics;
- a collider hierarchy in place of the linear bounds test;
- retained shape handles for query shapes in place of borrowed hulls;
- query debug overlays;
- Emscripten SDK consumer evidence.

Hull faces, volume validation from faces and mass properties belong to the
rigid-body gate.
