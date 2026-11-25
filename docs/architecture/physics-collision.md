# Physics collision module

`Ludus::PhysicsCollision` (`modules/physics/collision/`) is the first slice of
physics gate P1 in the [physics systems architecture](physics-systems.md#delivery-gates).
It exports an immutable static triangle mesh with a bounded bounding volume
hierarchy (BVH), a closest raycast, a closest translation-only cast of an
axis-aligned box, and a general closest cast of a posed sphere, box, capsule or
convex hull. A geometry store owns cooked shapes behind leased handles, and a
collision world places colliders that lease them and answers filtered world
queries through protected read views. For physics gate P2 it also
[generates contacts](#contact-generation) between two colliders: candidate
pairs, contact points with feature identities, and reduced patches. It depends only on FoundationBase,
FoundationContainers, FoundationIdentity and FoundationMath, contains no third-party physics code, and builds for native
and Emscripten targets. The installed SDK exports it as a component.

Public headers:

| Header | Contents |
| --- | --- |
| `<ludus/physics/collision/status.h>` | `Status`, the coordinate envelope and `INVALID_TRIANGLE_INDEX` |
| `<ludus/physics/collision/convex_shape.h>` | `ConvexShape`, its `Make*Shape` constructors, and `ConvexHull` with its cook source and result |
| `<ludus/physics/collision/queries.h>` | Raycast, box-cast and convex-cast inputs, results, policies and tolerances |
| `<ludus/physics/collision/triangle_mesh.h>` | Cook source, settings and result, `TriangleFeatureFlags` and `TriangleMesh` |
| `<ludus/physics/collision/contacts.h>` | `ContactSettings`, `ContactFeature`, `ContactPoint`, `ContactPatch`, `ContactPairContacts` and the contact limits |
| `<ludus/physics/collision/geometry_store.h>` | `ShapeHandle`, `ShapeKind` and `GeometryStore` |
| `<ludus/physics/collision/debug_overlay.h>` | `DebugLine`, `DebugLineBuffer`, `DebugLineStyle` and the debug extraction functions |
| `<ludus/physics/collision/collision_world.h>` | `ColliderHandle`, poses, participation, filters, world query inputs and results, contact candidate inputs, `CollisionWorld` and `CollisionReadView` |

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
| Both views valid, index count a multiple of three, known policies, `ActiveEdgeCosine` finite and in [-1, 1] | `InvalidArgument` |
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
only for the call. It then reserves storage for one triangle record (44 bytes,
including its feature flags) and at most one node (36 bytes) per triangle.
Those two allocations are the only ones the cooked mesh keeps.
[Edge classification](#internal-edges) makes three temporary ones. If any
allocation fails the result is `AllocationFailure` and the output mesh keeps
whatever it held before. Any failure leaves the output unchanged.

### Internal edges

A shape sliding across a flat floor must not catch on the seams between the
floor's triangles. Cooking therefore classifies every triangle edge and corner
as *active* or not, on each side of the triangle, and stores the answer in
`TriangleFeatureFlags`. [Contact generation](#convex-shapes-against-a-mesh)
lets only an active feature push along its own direction.

`TriangleMeshCookSettings::EdgePolicy` chooses how:

| Policy | Result |
| --- | --- |
| `InternalEdgePolicy::Classify` (default) | Each edge is classified from the triangles that share it |
| `InternalEdgePolicy::KeepAll` | Every edge and corner is active on both sides; no adjacency search runs |

Under `Classify`, two triangles share an edge when they use the same two
source vertex **indices**. Positions are never compared, so a seam whose
vertices are duplicated rather than shared stays a pair of boundaries.

| Edge | Active on |
| --- | --- |
| Used by one triangle: a boundary | Both sides |
| Used by three or more triangles: nonmanifold | Both sides |
| Used by two triangles that run along it in the same direction: inconsistent winding | Both sides |
| Two consistently wound triangles whose normals differ by less than the crease threshold | Neither side |
| Any other edge of two triangles: a crease | Its convex side only: the front of a ridge, the back of a valley |

The crease threshold is `ActiveEdgeCosine`, the cosine of the angle between
the two normals; the default `DEFAULT_ACTIVE_EDGE_COSINE` is five degrees. A
corner is active on a side when any edge of the mesh that ends at its source
vertex is active on that side. Triangles skipped as degenerate are no one's
neighbour, so the edges they would have shared become boundaries. The front
side of a triangle is the side its winding normal points to.

Classification never removes geometry. It only records where a contact normal
may leave the face normal, and it stays conservative wherever the topology is
not a clean two-triangle seam. `IsTriangleEdgeActive` and
`IsTriangleCornerActive` read the flags, and
`TriangleMesh::TryGetTriangleFeatures` returns them for one source triangle,
as a diagnostic that searches the cooked triangles linearly.

The adjacency search is linear in the triangle count. It files each edge under
its lower vertex index, sorts each file by the higher index and classifies
equal neighbours. Its temporary storage is eight bytes per triangle edge plus
five bytes per referenced vertex, released before cooking returns. Queries do
not read the flags, so a mesh used only for rays, casts and overlaps can cook
with `KeepAll` and skip the search.

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
  than the tolerance inside the shape, encloses the origin exactly, or finds
  the cores at a separation below a thousandth of the tolerance, where the
  separating direction is only rounding. `Report` returns it at fraction zero
  with no normal and no witness, as for the box.
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
as an entity, that every hit copies. A collider may use any stored shape:
a triangle mesh for static environment geometry, or a sphere, box, capsule or
hull, which queries treat as a closed solid.

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

A view also freezes the world's `GeometryStore`. While any view of a world on
the store is active:

- every addition, lease and release on the store fails with `InvalidPhase`;
- every other world on the store cannot create or destroy colliders, because
  those lease from and release to the store; moving and enabling its colliders
  still work.

No shape a query reads, or names by handle, can therefore change during the
query. `GeometryStore::GetReaderCount` reports the active views.

Acquire, move and release views, and mutate the worlds and the store, on one
owner thread. While a view is active, any number of threads may query through
it.

### Stored query shapes

`WorldConvexCastInput` and `WorldOverlapInput` take the query shape by value in
`Shape`, which borrows a hull for the call, or by `QueryShape`, a `ShapeHandle`
of a sphere, box, capsule or hull in the world's store. When `QueryShape` is not
null it takes precedence, and `Shape` is not read. Because the view freezes the
store, a stored hull stays valid for the whole query, and a stale or foreign
handle fails with `InvalidHandle` instead of dangling. A triangle-mesh handle
fails with `Unsupported`. `GeometryStore::TryGetConvexShape` describes a stored
shape as a `ConvexShape` value.

### Filtered world queries

`CollisionReadView::RaycastClosest` and `CastConvexClosest` take world-space
inputs with the same admitted values and failures as the mesh queries, plus a
directed `QueryFilter`. A collider is reported only when it is live, enabled
and queryable; one of its category bits is in `CategoryMask`; it is not a sensor
unless `ShouldIncludeSensors`; and it is not in `IgnoredColliders` (at most
`MAX_QUERY_IGNORED_COLLIDERS`, 64).

Each query moves into each candidate collider's local frame, runs that
collider's query and maps the answer back: normals and witness points rotate into world
axes, and distances and fractions are unchanged by the rigid transform. A convex
shape's rotation is composed with the collider's in `float64`, not through a
`float32` quaternion. Results add the collider handle and binding. Equal
distances or fractions resolve to the lower collider slot, then generation, then
triangle index.

Collider-local coordinates must lie in the 100 km envelope. When a query's
start lies outside it in a collider's frame, the query starts where its path
first reaches that collider's bounds, which lie inside the envelope. A convex
shape that is still outside the envelope there fails with `OutOfRange`.

Convex colliders use the general convex path, with both shapes rounded: a
sphere or capsule target adds its radius to the query shape's.

- A ray is cast as a point. It reports the entry point, the outward surface
  normal and `IsFrontFace`. Its distance uses the convex cast's contact
  tolerance; it is not the watertight mesh test.
- A ray that starts inside a convex collider hits it at distance zero with
  `HasNormal` false. `TriangleFacePolicy::BackFacesOnly` never reports a
  convex collider, because its exit surface needs a separate query.
- A convex cast follows the mesh rules. The witness point lies on the target's
  surface: a rounded target's core witness is moved out by its radius.

### Collider hierarchy

A dynamic bounding volume hierarchy over the colliders' conservative world
bounds selects the candidates of every world query. It follows Erin Catto's
*Dynamic Bounding Volume Hierarchies* (GDC 2019) and Box2D's `b2DynamicTree`:

- **Insertion** descends from the root toward the sibling that adds the least
  surface area to the tree, then walks back up refitting bounds.
- **Balance** is restored with single rotations on that walk: the taller child
  of an unbalanced node rises into its place. The height stays within about
  1.44 log2 of the collider count, under 30 at the world's capacity.
- **Updates:** creating a collider inserts a leaf, destroying it removes the
  leaf, and moving it removes and reinserts the leaf. Each takes logarithmic
  time and never allocates: the world reserves two nodes per collider slot
  when it is initialized. Leaves hold exact bounds, with no fattened margin,
  so even a small move reinserts its leaf.
- `GetColliderHierarchyHeight` reports the height as a diagnostic.

Queries descend depth first and prune a subtree only when a conservative test
proves it cannot hold a reported hit:

- A ray skips nodes it does not enter. Under `Closest`, it also skips nodes
  it enters only beyond the best distance so far, and keeps ties.
- A cast does the same with the shape's bounds swept up to the best fraction.
  Each node is widened by an upper bound on the contact tolerance of every
  collider below it.
- An overlap skips nodes the shape's widened bounds miss.

`QueryTraversal::AllTriangles` visits every collider slot in order, with no
hierarchy, bounds test or mesh hierarchy. It is the brute-force oracle for all
of them.

### Overlap queries

`CollisionReadView::Overlap` reports the colliders that a stationary posed
convex shape overlaps, into a caller-supplied `BorrowedView<ColliderOverlap>`.
A collider overlaps when the shape is deeper inside it than the contact
tolerance, by exactly the rule for a cast's initial overlap. Touching is not
overlapping, and bounds only select candidates.

Each record names the collider and its binding. For a mesh collider it also
gives the lowest overlapping triangle and the number of overlapping triangles;
a convex collider leaves both at their defaults.

| Mode | Behavior |
| --- | --- |
| `OverlapMode::All` | Visits every candidate, writes records until the buffer is full, then sorts them by slot |
| `OverlapMode::Any` | Stops at the first overlapping collider; promises no particular one |

`WorldOverlapResult` separates finding from writing:

- `OverlapCount` is the number of overlapping colliders found: all of them for
  `All`, at most one for `Any`.
- `WrittenCount` is the number of records written.
- `IsComplete` is true exactly when the two are equal.

An `All` query whose buffer was too small still counts every overlap. The caller
can then retry with a buffer of `OverlapCount` records; the records already
written are the first ones the hierarchy search found, sorted by slot, not a
nearest subset. A miss is a
complete, empty result. A failed query leaves the result unchanged; the
buffer's contents are then unspecified.

### Any and All modes for rays and casts

Rays and convex casts come in three modes on `CollisionReadView`:

| Query | Answer |
| --- | --- |
| `RaycastClosest`, `CastConvexClosest` | The single nearest hit |
| `RaycastAny`, `CastConvexAny` | Some hit; the search stops at the first one it finds |
| `RaycastAll`, `CastConvexAll` | Every hit collider, each once, at its own closest hit |

`Any` searches colliders in hierarchy order and lets a mesh stop at the first
triangle its traversal finds. The answer is a genuine hit within the limit,
but neither the nearest nor a stable choice. A miss is certified only after
every candidate was searched, which is the case whenever the answer is a miss.
Under `InitialOverlapPolicy::Reject`, `CastConvexAny` still searches every
candidate, because any starting overlap must fail the query.

`All` writes into a caller buffer and reports a `WorldHitsResult` with the same
completion fields as an overlap query:
- It keeps counting after the buffer fills.
- It then sorts the written records by distance or fraction (a starting overlap
  first), collider slot and generation.
- If the buffer was too small, the written records are the first hits the
  search found, sorted. They are not the nearest ones; retry with `HitCount`
  records.

The first record of a complete `All` result equals the `Closest` answer.

## Contact generation

Contact generation answers what a rigid-body solver needs to know about two
colliders: where they touch, along which normal, how deep, and which features
of each shape made each point. It is pure geometry. A contact query keeps no
state between calls; matching contacts from tick to tick belongs to the
[rigid world](physics-rigid.md#contacts).

```cpp
CollisionReadView view;
world.TryAcquireReadView(view);

StaticArray<ColliderHandle, 64> partners{};
ContactCandidateResult found;
view.FindContactCandidates({ .Collider = crate }, BorrowedView<ColliderHandle>(partners.GetData(), partners.GetSize()), found);

ContactPairContacts contacts;
view.GenerateContacts(floor, crate, {}, contacts);
for (uint32 patch = 0; patch < contacts.PatchCount; ++patch)
{
    // contacts.Patches[patch].NormalWorld points from `floor` toward `crate`.
    // Its points are contacts.Points[FirstPoint .. FirstPoint + PointCount).
}
```

### Candidate pairs

`CollisionReadView::FindContactCandidates` lists the colliders that may touch
one collider. A candidate is another live collider that is enabled and solid,
whose filter and the collider's accept each other (`ShouldCollide`), and whose
world bounds overlap the collider's bounds widened by `MarginMeters`. A
collider that is disabled or not solid has no candidates. Handles are written
in slot order; a buffer that is too small still counts every candidate, as the
[overlap query](#overlap-queries) does.

Candidates come from the [collider hierarchy](#collider-hierarchy). Node
bounds are exact unions of their leaves' bounds, so pruning with the same
comparison misses nothing. `QueryTraversal::AllTriangles` compares against
every collider slot instead and is the brute-force oracle. Bounds are only
candidates: `GenerateContacts` decides whether a pair touches. Which pairs a
simulation tests, such as skipping two static bodies, is the caller's rule.

### Contacts of one pair

`CollisionReadView::GenerateContacts(first, second, settings, outContacts)`
writes the pair's `ContactPairContacts`:

| Value | Meaning |
| --- | --- |
| `ContactPatch::NormalWorld` | Unit normal from the first collider toward the second: moving the second along it separates the pair |
| `ContactPoint::PositionWorld` | Midpoint between the two surface points |
| `ContactPoint::SeparationMeters` | Signed gap along the normal: negative while the shapes interpenetrate |
| `ContactPoint::FirstFeature`, `SecondFeature` | The feature of each shape the point lies on |
| `ContactPoint::TriangleIndex` | The mesh's source triangle, or `INVALID_TRIANGLE_INDEX` for two convex colliders |

A pair makes a contact when its surfaces are within
`ContactSettings::ContactOffsetMeters` of each other (default
`DEFAULT_CONTACT_OFFSET_METERS`, two centimeters; at most
`MAX_CONTACT_OFFSET_METERS`). A positive offset reports points slightly before
the surfaces touch, so a resting pair keeps its points while it jitters. A pair
further apart is a success with zero counts.

The query reads neither filters nor participation flags. It rejects an invalid
handle, the same collider twice, a contact offset that is not finite or out of
range, and two triangle meshes (`Unsupported`), and leaves the output unchanged
on failure. It never allocates.

Two convex colliders are evaluated in world axes about the first collider's
position, so the answer does not depend on where in the world the pair sits. A
convex collider against a mesh is evaluated in the mesh's local frame. Swapping
the two colliders mirrors the result to within rounding: opposite normals, the
same points, the two features exchanged.

### Kernels and penetration depth

Every shape is a polytope *core* plus a rounding *margin*, as in the
[convex cast](#closest-convex-cast): a sphere is a point and a capsule a
segment, each rounded by its radius; boxes, hulls and triangles are their own
cores. A kernel finds the contact normal and the smallest separation.

| Pair | Kernel |
| --- | --- |
| Sphere or capsule against sphere or capsule | Closest points of the two core segments, in closed form |
| Sphere against box | Closest point of the box; a centre inside leaves through the nearest face |
| Capsule against box | Apart: the capsule ends against the box and the segment against its twelve edges. Overlapping: the separating axis test on the box's three face axes and their three cross products with the segment |
| Box against box | The separating axis test on 15 axes |
| Sphere or capsule against triangle | Closest points of the core and the triangle; a segment through the triangle leaves along its face |
| Box against triangle | The separating axis test on 13 axes, the triangle normal first |
| Any pair with a hull | The general convex path |

Rounded pairs report the exact distance between their surfaces. The separating
axis kernels prefer a face axis over an edge axis whose separation is larger
by less than 1e-4 of the pair's size, which keeps the features of a resting
pair stable. For boxes that are apart they report the separation along the
axis of greatest separation. That is a valid separating direction and at most
the true distance; it equals it unless the nearest features are two corners or
a corner and an edge.

The **general convex path** is the one path for hulls. It runs GJK distance on
the two cores, with the same support mappings and simplex reduction as the
convex cast. Cores that are apart give the exact distance and witness points;
subtracting the margins gives the separation, so most contacts of rounded
shapes never need a penetration search. When the cores overlap, penetration
depth comes from the **expanding polytope algorithm (EPA)**.

EPA was chosen over the alternatives because P1 already chose GJK. Minkowski
portal refinement would be a second general path beside it, which the
[architecture](physics-systems.md#collision-pipeline-and-contact-robustness)
rules out. A separating axis test needs each hull's faces and edges, and hulls
store points only. EPA needs nothing but the support mapping the cast already
uses. Ludus's version differs from the published one in three ways:

- The polytope is seeded from four support points of the difference of the
  two cores, not from the final GJK simplex. Each step expands the face with
  the smallest *signed* distance to the origin, so a seed that does not yet
  contain the origin grows toward it. A touching pair then reports a small
  positive gap instead of failing.
- Every bound is explicit: at most 64 vertices, 128 faces and 192 horizon
  edges, and one vertex per step.
- A search that reaches a bound, or meets a face with no area, returns its
  best face. That is a lower bound of the depth. The patch is flagged
  `ContactPatch::IsApproximate` and counted in
  `ContactPairContacts::ApproximatePatchCount`; it is never reported as exact.

Cores are polytopes, so the search ends exactly when a support point repeats.
The specialized kernels double as oracles: a hull with a box's eight corners
must produce the contacts of the box.

### From a normal to contact points

One normal and one depth cannot hold a resting box in place. The kernel's
normal is therefore turned into a patch:

1. Each shape's *support feature* along the normal is found: the vertex, edge
   or face whose corners lie within reach of the contact offset. A hull has no
   stored faces, so its face is recovered as the planar hull of the hull
   points on the support plane.
2. If either feature is a face, the face that looks along the normal most
   squarely is the *reference*; of two parallel faces, the larger. The other
   feature is clipped against the reference face's side planes.
3. Each clipped point is slid along the normal onto the reference plane. The
   distance it travels, less the margins, is its separation. Points further
   apart than the contact offset are dropped.
4. Two edges give their closest points, or two points when they are parallel.

Topological decisions are kept apart from coordinates. A point's features
follow from which side-plane tests it passed, never from comparing the
coordinates those tests produced. A corner exactly on a side plane counts as
inside; the tolerance that decides this never moves a clipped point off the
plane.

A `ContactFeature` packs a `ContactFeatureKind` (vertex, edge or face) and an
index. The same geometry has the same value on every tick:

| Shape | Vertex | Edge | Face |
| --- | --- | --- | --- |
| Sphere | 0 | none | none |
| Capsule | 0 for the -Y end, 1 for the +Y end | 0, the segment | none |
| Box | Bits 0, 1, 2 set for the +X, +Y, +Z side | `axis * 4 + s`: the edge along `axis`; bit 0 and bit 1 of `s` are set for the positive side of the next two axes | `axis * 2`, plus 1 for the positive side |
| Hull | The point's index | The two point indices, eight bits each | The three lowest point indices of the face, eight bits each |
| Triangle | The corner | The edge leaving that corner | 0 |

A clipped point is labelled by the two features that made it: a corner of one
shape on a face of the other, or an edge of each where they cross.

### Patches and reduction

Points that share a normal form a `ContactPatch`. Two convex shapes always
make one patch. Against a mesh, triangles whose contact normals agree within
`CONTACT_PATCH_NORMAL_COSINE` (five degrees) share a patch, so a flat floor of
many triangles supports a box with one patch. Normals further apart are never
merged or averaged.

Each patch is reduced to at most `MAX_CONTACT_PATCH_POINTS` (4):

1. Points closer together than 1e-6 of the pair's size are merged.
2. The deepest point is kept. Among equally deep points an extreme one is
   chosen, which is a corner of their outline.
3. The point farthest from it is kept.
4. The two points that add the most area on either side of that diagonal are
   kept.

Points on one line reduce to its two ends, and a point inside the triangle of
three others is dropped. A box resting on a floor therefore keeps its four
corners, not its deepest point alone.

| Limit | Value | When it is reached |
| --- | --- | --- |
| `MAX_CONTACT_PATCH_POINTS` | 4 | A patch found more points; the reduction above chooses |
| `MAX_CONTACT_PAIR_PATCHES` | 8 | A mesh pair found more normal groups; the patches with the deepest points are kept and `DiscardedPatchCount` counts the rest |
| `MAX_CONTACT_PAIR_POINTS` | 32 | The two limits multiplied |

`GeneratedPointCount` counts points before reduction and `TriangleTestCount`
the mesh triangles that were tested. While a mesh pair is still being
traversed, a patch that collects more than 18 points is thinned to its deepest
point and its farthest points along eight directions; that choice does not
depend on the order triangles were visited in.

### Convex shapes against a mesh

A convex collider against a mesh collider tests the triangles whose bounds the
shape's bounds reach, widened by the contact offset. Each triangle is a
two-sided sheet: the side the shape's interior point is on decides which way
the contact pushes. A shape under a surface is pushed down, away from it.

The kernel's normal says which part of the triangle faces the shape: its
face, an edge or a corner.

| Triangle feature | Contact |
| --- | --- |
| Face | The face normal. The contact exists only where the shape is over the triangle itself |
| Active edge or corner | The kernel's normal: the feature pushes along its own direction |
| Inactive edge or corner | The face normal instead, kept only where the shape is over the triangle; the neighbouring triangle reports the rest |

That is how the [internal-edge classification](#internal-edges) is honored. A
sphere sunk slightly into a flat floor and close to a seam touches its own
triangle through the face. Its neighbour's nearest point is on the seam, off
vertical. With classification that contact becomes a face contact and is
dropped, because the sphere is not over the neighbour. Without it
(`KeepAll`), the sideways normal is reported and a sliding body catches. A
shape exactly over a seam is over both triangles; both report the same point
and the patch merges them.

Mesh contacts depend on poses only. A shape that has passed through a surface
is pushed out of its far side; preventing that is continuous collision's job.

## Query debug lines

`<ludus/physics/collision/debug_overlay.h>` describes collision state and query
answers as plain world-space line segments. An editor or renderer draws them,
choosing a colour for each `DebugLineKind`; PhysicsCollision depends on no
renderer. Each `DebugLine` names its kind and, where one applies, the collider.

| Function | Lines |
| --- | --- |
| `AppendShapeLines` | A posed shape: a box's 12 edges, a sphere's three great circles, a capsule's end circles, side lines and half circles. A hull has no face list, so it draws a marker at each stored point and its oriented bounds |
| `AppendColliderLines` | Every live collider's world bounds: `ColliderBounds` when a `QueryFilter` reports it, `FilteredColliderBounds` when it is disabled, the wrong category, a sensor or ignored. It can also draw every internal hierarchy node |
| `AppendRaycastLines` | The ray up to its hit or limit, the rest beyond the hit, a hit marker and the normal |
| `AppendConvexCastLines` | The shape at its start, the path of its origin, and on a hit the shape at first contact, a witness marker and the normal. A stored `QueryShape` is resolved |
| `AppendOverlapLines` | The shape at its pose and the bounds of every reported collider |

The functions append to a caller-owned `DebugLineBuffer`, which follows the
completion rule of `All` queries. It writes lines until its storage is full,
keeps counting, and reports `GetRequiredCount` and `IsComplete`, so a caller
can retry with enough storage. A function validates every input before it
appends, so a failure adds nothing. `DebugLineStyle` sets the marker size and
the drawn normal length.

The extraction allocates nothing. Functions that read a world take an active
`CollisionReadView`, whose epoch identifies the state drawn.

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

The same executable covers contact generation:

- **Edge classification:** flat seams, ridges, valleys, the crease threshold,
  boundaries, nonmanifold edges, inconsistent winding, unwelded seams,
  interior corners of a grid, and `KeepAll`.
- **Analytic kernels:** contact points, normals, depths and features for every
  pair of sphere, capsule and box, in closed form: overlapping, tangent, apart
  within and beyond the offset, edge, corner and deep-overlap cases. Every
  case is also run with the colliders reversed, which must mirror it, and
  translated by about 8 km, which must not change it.
- **The general path:** hulls with a box's corners against boxes, spheres and
  capsules in eight poses, including crossed edges, a corner on a face and
  deep overlaps that need EPA. Each must match the specialized kernel within
  1e-6 m and report no approximate patch.
- **Topology at a face-to-edge transition:** a turned cube overlaps a floor
  box while one bottom corner sits 1e-3 m and 1e-6 m inside, exactly on, and
  1e-6 m and 1e-3 m beyond the floor's side plane. The patch is compared with
  a clip written in the test, in the floor's plane and without tolerances:
  finite and distinct points inside the oracle polygon, depth within 1e-6 m
  and area within the sliver a four-point patch can lose. The three corners
  that did not cross keep identical features through the whole sweep. The
  crossing corner changes from a face contact to an edge crossing, exactly
  when it passes the plane. The box path and the general path, both collider
  orders and a far translation agree.
- **Reduction:** a twelve-sided prism keeps the largest inscribed square and,
  tilted, its deepest corner.
- **Candidates:** 96 seeded spheres with mixed filters, sensors and disabled
  colliders, a third of them moved, against an oracle that applies the
  documented rule to every pair; hierarchy and brute-force traversal agree
  with it exactly.
- **Meshes:** a sphere over a triangle, a seam, a cell edge and a grid vertex;
  the seam control with and without classification; a box and a capsule
  across many cells; ridges, valleys, borders and the back side; a turned
  scene; and the patch limit in a twelve-sided pit.

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

The convex-collider tests cover:
- rays into sphere, rotated box, lying capsule and hull colliders, with
  analytic distances and normals, starts inside, back-face rays and near misses;
- convex casts onto sphere and box colliders, including target-surface witnesses
  and every overlap policy;
- overlap queries with records, triangle counts, a too-small buffer, an empty
  buffer, `Any`, filters, a miss, an exactly touching sphere and invalid
  requests;
- a translation at the component limit through a collider rotated by 45°.

An independent oracle checks 900 random convex-against-convex pairs. It uses
exact point and segment distances for sphere and capsule pairs and the
15-axis separating gap for box pairs, with golden-section and bisection
searches. The pairs give 383 first contacts, 181 starting overlaps and 336
misses. Contact distances agree within 1 µm, overlap queries agree with the
sign of the starting separation, and rays agree with the exact sphere quadratic
and box slab test (171 hits).

A world of 30 mixed mesh and convex colliders gives exactly the same results
for rays, casts and `All` overlaps (records, triangle counts and completion)
as the brute-force traversal. A regression places boxes, hulls and capsules at
symmetric points of a triangle's plane, where the search once mistook an
overlap for touching.

The debug tests check:
- the documented line count of every shape wire, with box, sphere and capsule
  endpoints on the rotated shape and hull markers at its points;
- the lines of a ray hit and miss, a stored-shape cast (the contact wire sits at
  the hit fraction), and an overlap;
- filter classification of disabled, sensor, category and ignored colliders,
  and the hierarchy node count;
- a full buffer that keeps counting, and failures that append nothing.

The stored-shape tests check:
- a view of one world freezes the shared store and the other world's collider
  creation and destruction, with outputs and lease counts unchanged; that
  world can still move its colliders, and everything resumes after release;
- 120 closest, `Any` and `All` casts and overlaps with a stored hull or capsule
  give exactly the value-shape answers;
- stale, foreign and triangle-mesh handles fail in every cast mode and in
  overlaps, leaving the results unchanged.

The query-mode tests cover:
- `All` rays and casts on stacked floors, with distance order, a short buffer
  (a sorted subset of genuine hits), an empty buffer, a miss, a distance
  limit and invalid buffers;
- `Any` returning a genuine hit that need not be the nearest;
- every overlap policy for `All` and `Any` casts, including `Reject`, which
  still fails an `Any` cast whose first hit is clean.

On 24 random mesh and convex colliders, over 300 queries:
- every complete `All` result's first record equals `Closest` exactly;
- the brute-force traversal writes the same records;
- `Any` hits exactly when `Closest` does, on a listed collider, at a distance
  no nearer than that collider's closest hit;
- short buffers give sorted subsets with the full count.

The hierarchy tests cover:
- the AVL height bound after every insertion of 1,024 colliders sorted along
  a line, after removing half and moving the rest, after emptying, and after
  refilling to capacity;
- 12 rounds of random creation, moves and destruction. After each round,
  closest rays, casts, `All` rays and overlaps through the hierarchy equal the
  brute-force traversal;
- 60 floors 5 mm apart, created in shuffled order, where pruning by the best
  distance must never skip a nearer floor.

Disabling the rotations or over-pruning rays by 5 cm fails these tests. A
hidden `[.benchmark]` case times 2,000 closest rays among 4,000 sphere
colliders: 12 ms through the hierarchy against 3.6 s brute force in a macOS
debug build, at height 15.

On 24 randomly posed colliders with sensors, disabled colliders and category
masks, world queries equal the brute-force traversal exactly (about half of
the queries hit). A rotated, translated collider also agrees within 0.1 mm
with the same triangles baked into a world-space mesh.

The allocation tests also cover store and world initialization under each
injected allocation failure, and they show that adding a shape, creating,
moving and destroying a collider and running world queries allocate nothing,
including convex colliders, overlap queries and the `Any` and `All` modes.

The installed SDK consumer (`tests/sdk_consumer/collision.cpp`) links
`Ludus::PhysicsCollision`, cooks a quad and a tetrahedral hull, runs a
raycast, a box cast and convex casts of a sphere and the hull, and then puts
the quad in a store and world, and runs a ray, an overlap query and an `All`
ray through a read view.

The same corpus provides the Emscripten evidence, in two ways:

- `tools/web-collision-probe` compiles `collision.cpp` with the in-tree module
  into a Node program. The browser presets build it with the other web probes,
  and `./scripts/test web-emscripten-development` runs it as the
  `ludus_web_collision_corpus` CTest.
- The *Browser backends and packages* workflow installs the web SDK, builds
  `tests/sdk_consumer/collision_web` against the installed
  `Ludus::PhysicsCollision`, and runs the resulting `collision.js` with the
  emsdk Node.

Either program exits non-zero if any answer differs. The project also builds
natively against an installed native SDK.

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
- Erin Catto, *Dynamic Bounding Volume Hierarchies*, Game Developers Conference
  2019, and the `b2DynamicTree` of [Box2D](https://box2d.org) (MIT licence):
  surface-area-guided insertion and single-rotation rebalancing of the collider
  hierarchy. Ludus implements its own float64 tree without fattened bounds;
  no Box2D code is copied.
- Elmer G. Gilbert, Daniel W. Johnson and S. Sathiya Keerthi,
  [A Fast Procedure for Computing the Distance Between Complex Objects in Three-Dimensional Space](https://doi.org/10.1109/56.2083),
  *IEEE Journal on Robotics and Automation* 4(2), 1988, pages 193-203: the
  distance iteration of the general contact path.
- Gino van den Bergen, *Proximity Queries and Penetration Depth Computation on
  3D Game Objects*, Game Developers Conference 2001, and *Collision Detection
  in Interactive 3D Environments*, Morgan Kaufmann, 2003, section 4.3.8: the
  expanding polytope algorithm. Ludus seeds the polytope from support points,
  expands by signed distance, and bounds and reports every failure.
- Christer Ericson, *Real-Time Collision Detection*, sections 4.4.1 and 5.2.9:
  the separating axes of two boxes and of a box and a triangle; section 5.1.9:
  the closest points of two segments.
- Dirk Gregorius, *The Separating Axis Test between Convex Polyhedra*, Game
  Developers Conference 2013, and *Robust Contact Creation for Physics
  Simulations*, Game Developers Conference 2015: preferring face axes for
  stable features, and clipping an incident feature against a reference face
  with feature labels on every point.
- Ivan E. Sutherland and Gary W. Hodgman,
  [Reentrant Polygon Clipping](https://doi.org/10.1145/360767.360802),
  *Communications of the ACM* 17(1), 1974, pages 32-42: the clip itself.
- R. A. Jarvis,
  [On the identification of the convex hull of a finite set of points in the plane](https://doi.org/10.1016/0020-0190(73)90020-3),
  *Information Processing Letters* 2(1), 1973, pages 18-21: gift wrapping,
  used to recover a hull's face from its points.
- Ádám Moravánszky and Pierre Terdiman, *Fast Contact Reduction for Dynamics
  Simulation*, *Game Programming Gems 4*, Charles River Media, 2004, section
  3.5, pages 253-263, as recorded in the
  [Gems review](physics-systems-gems-review.md#fast-contact-reduction-for-dynamics-simulation):
  cooked internal-edge information, and reducing a patch to its deepest point
  plus the points that span the most area. Ludus keeps four points, classifies
  edges per side, and keeps boundaries, creases and nonmanifold edges active.
- Kokichi Sugihara,
  [A Robust and Consistent Algorithm for Intersecting Convex Polyhedra](https://diglib.eg.org/items/625c61c6-8ea1-4626-9a52-ecba6bbd219e),
  *Computer Graphics Forum* 13(3) (Eurographics '94), 1994, sections 2 to 5,
  pages C-46 to C-53, and A. Agrawal and A. A. G. Requicha,
  [A Paradigm for the Robust Design of Algorithms for Geometric Modeling](https://diglib.eg.org/items/e291c26e-cb99-4285-9c6f-a185eb0aa43c),
  same issue, sections 1 and 3, pages C-33 to C-38: keeping topological
  decisions apart from coordinate construction, which the clip's feature
  labels and the face-to-edge transition test follow.

## Explore as Player

The editor's [Explore as Player](../development/editor-workspace.md#explore-from-a-first-person-player)
preview is the first consumer. Its adapter collects the visible meshes of
entities with **Static Collider** and submits their positions, indices and
transforms to the shared first-person walking controller. The controller
transforms and cooks the triangles in world space, so mesh space is World space and
the 100 km coordinate envelope applies to the collider geometry and to the
player. The cook uses `DegenerateTrianglePolicy::Skip`, because imported
meshes often carry slivers the preview has always ignored, and
`InternalEdgePolicy::KeepAll`, because the walk uses casts only and they do
not read the edge classification.

The Qt-free walking controller owns the 0.48 × 1.75 × 0.48 m box, step-up,
ground snap, slide, jump, the 45° slope limit and the fall reset. A contact
supports the player only when both its separating normal and the triangle's
face are within 45° of up. Because the module reports a triangle index rather
than the triangle, the controller cooks two partitions: the triangles
whose face can support the player, and every other triangle. Each walk cast
queries both with `InitialOverlapPolicy::Report` and takes the earlier contact.

That split also keeps the stair-tread preference. A box falling onto a step
edge meets the riser's top edge and the tread at the same landing time, and
rounding can make either fraction the smaller. For a straight-down cast the
controller treats fractions within 1e-12 of each other as one landing and uses
the tread's support normal; the module's exact ordering is unchanged.

A query that fails validation blocks the move like an initial overlap, so the
player cannot pass through geometry, and a player who leaves the coordinate
envelope returns to the spawn. Starting reports more than
`MAX_TRIANGLE_MESH_TRIANGLES` collider triangles, collider geometry outside the
envelope and a spawn outside it as separate failures that name the limit.

## Beyond P1

The P1 collision items are delivered; the next physics work lives in later
gates. Drawing these debug lines in the editor viewport belongs to the
inspector work of gate P4. Hull volume and mass properties live in the
[rigid module](physics-rigid.md#mass-properties). Contact generation recovers
a hull's faces from its points when it needs them and stores none; cooked
hull faces, per-triangle materials, compound shapes and heightfields are later
work.
