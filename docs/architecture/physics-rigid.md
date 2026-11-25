# Physics rigid module

`Ludus::PhysicsRigid` (`modules/physics/rigid/`) holds the first two slices of
physics gate P2 in the [physics systems architecture](physics-systems.md#delivery-gates),
tracked as P2.1 and P2.2. A rigid world owns exactly one
[collision world](physics-collision.md#geometry-store-and-collision-world) and
adds static, kinematic and dynamic bodies to it. Bodies have mass frames, a
fixed tick with substeps, ordered force, impulse and kinematic-target commands,
interpolation poses and plain-value state export. Each tick also generates the
bodies' [contacts](#contacts), matches them to the previous tick's and exports
them. **Nothing responds to contacts yet**: bodies fall, spin and follow
kinematic targets through each other. The solver and continuous collision are
the later P2 slices.

The module depends on FoundationBase, FoundationContainers, FoundationIdentity,
FoundationMath and PhysicsCollision. It contains no third-party physics code,
and the installed SDK exports it as a component. Its sources build for native
and Emscripten targets like PhysicsCollision; the Wasm SDK consumer and the
comparison scenes belong to P2.7.

Public headers:

| Header | Contents |
| --- | --- |
| `<ludus/physics/rigid/status.h>` | `Status`, `ToRigidStatus` and the velocity and command envelopes |
| `<ludus/physics/rigid/mass_properties.h>` | `InertiaTensor`, `MassProperties`, `BodyLocalPose`, `MassPart`, `PrincipalMassFrame` and the mass functions |
| `<ludus/physics/rigid/body.h>` | `BodyHandle`, `MotionType`, `MassSource`, body and collider descriptions, `BodyState` and `BodyMassInfo` |
| `<ludus/physics/rigid/commands.h>` | `CommandKey`, force, impulse and kinematic-target commands, and `CommandResult` |
| `<ludus/physics/rigid/contacts.h>` | `ContactTable`, `ContactRetirement`, `ContactDiagnostics`, `ContactPointState` and `ContactExportResult` |
| `<ludus/physics/rigid/rigid_world.h>` | `RigidWorldSettings`, `WorldPhase`, `StepResult`, diagnostics and `RigidWorld` |

The Doxygen comments beside each declaration are the API contract. This page
explains the model, its limits and how to use it.

## Stepping a world headlessly

```cpp
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/rigid/rigid_world.h>

using namespace ludus::physics;

collision::GeometryStore store;
collision::ShapeHandle sphere;
store.TryInitialize({ .MaxShapes = 16 });
store.TryAddPrimitive(collision::MakeSphereShape(0.5), sphere);

rigid::RigidWorld world;
world.TryInitialize(store, { .MaxBodies = 64, .MaxColliders = 64, .MaxPendingCommands = 256 });

const rigid::BodyColliderDescription collider{ .Shape = sphere };
rigid::BodyHandle ball;
world.TryCreateBody(
    {
        .Pose = { .PositionWorld = {0.0, 10.0, 0.0} },
        .Colliders = BorrowedView<const rigid::BodyColliderDescription>(&collider, 1),
    },
    ball);

world.TryEnqueueImpulse({ .Key = { .TargetTick = world.GetNextTick() },
                          .Body = ball,
                          .LinearImpulseInFrame = {10.0, 0.0, 0.0} });
StaticArray<rigid::BodyState, 64> states{};
const BorrowedView<rigid::BodyState> stateBuffer(states.GetData(), states.GetSize());
for (uint32 tick = 0; tick < 60; ++tick)
{
    const rigid::StepResult step = world.Step();
    // step.Result is Success, or why the tick did not advance; see "Phases and failures".
    rigid::BodyExportResult exported;
    world.TryExportBodyStates(stateBuffer, exported);
}
```

Every call above returns a status; check it. The installed SDK consumer,
`tests/sdk_consumer/rigid.cpp`, runs this loop against the installed SDK.

## Mass properties

Mass is in kilograms and inertia in kilogram square meters about the center of
mass (COM). `TryComputeShapeMassProperties` integrates a solid convex shape of
uniform density in its own space:

| Shape | Method |
| --- | --- |
| Sphere | `m = 4/3 pi r^3 rho`, `I = 2/5 m r^2` |
| Box | `m = 8 a b c rho` for half extents, `Ixx = m (b^2 + c^2) / 3` |
| Capsule | Cylinder plus two hemispheres; each cap moved by the parallel-axis theorem from its COM at `3r/8` |
| Hull | Incremental convex hull triangulation of the cooked points, then exact tetrahedron integrals in `float64` |

A cooked `ConvexHull` stores points, not faces. The hull integrator therefore
triangulates the points with the incremental algorithm, removing the faces each
new point sees and coning the horizon to it. Points within a scale-relative
tolerance of the current hull are dropped, which leaves the solid unchanged.
It then sums signed tetrahedra from an interior point to each outward face:
volume, first moment, and the covariance `V / 20 * (sum v v^T + s s^T)`. The
triangulation lives in fixed stack scratch for `MAX_CONVEX_HULL_POINTS` points
and allocates nothing. A rounding failure that would break the closed surface
reports `DegenerateShape` rather than a wrong volume.

`TryComposeMassProperties` combines up to `MAX_MASS_PARTS` parts posed in a
body frame. It rotates each part's tensor (`R I R^T`) and moves it with the
parallel-axis theorem (`m (|d|^2 E - d d^T)`). Parts are **additive**:
overlapping parts count their shared volume twice. When colliders overlap,
author a separate mass proxy and pass it as explicit mass properties.

`TryValidateMassProperties` admits a mass in
[`MIN_MASS_KILOGRAMS`, `MAX_MASS_KILOGRAMS`], a COM inside the coordinate
envelope and a symmetric tensor whose principal moments are positive, satisfy
the triangle inequality (each at most the sum of the other two) and stay above
`MIN_INERTIA_CONDITION_RATIO` of the largest. It rejects nonfinite values,
indefinite tensors, unrealizable moments and ill-conditioned rods.
`TryComputePrincipalMassFrame` diagonalizes the tensor with cyclic Jacobi
rotations and returns `BodyFromCenterOfMass`: the COM and the right-handed
principal axes, kept separate from the authored body frame.

## Bodies

| Motion type | Behavior in this slice |
| --- | --- |
| `Static` | Never integrated; moves only by `TryTeleportBody` |
| `Kinematic` | Follows its velocity or a tick-addressed target; infinite mass, no force response; its COM is its body origin |
| `Dynamic` | Positive mass and inertia; integrates gravity, forces and impulses |

`TryCreateBody` is synchronous. It validates the whole description, computes
the mass frame, checks body and collider capacity, creates every collider in
the owned collision world and only then publishes the body. Any failure creates
nothing. Each `BodyColliderDescription` places a stored shape in the body
frame, with participation, filter, binding, density and an opaque `Material`
identifier. A dynamic body takes
its mass from `MassSource::Colliders` (every non-sensor collider with
`ShouldContributeMass`) or from validated `MassSource::Explicit` properties, which
also allows a body with no colliders. Triangle meshes are never mass sources:
`InvalidMassProperties`.

Attached colliders follow their body: after every tick and teleport, each
collider's world pose is `body pose * collider local pose`. Destroying a body
destroys its colliders. `TryGetBodyColliders` lists them;
`TryAcquireCollisionReadView` opens a protected read phase for queries on
other threads.

## Clock, integration and interpolation

One world has a fixed `TickSeconds` (default 1/60 s) and `SubstepCount`
(default 4). `Step` advances one tick, not a frame delta; the game owns
accumulation, catch-up and pause. Both settings change results and belong to
replay configuration.

Each substep of length `h` integrates every dynamic body's COM state:

1. `v += h (g * GravityScale + F / m)`.
2. In the principal frame, `w += h I^-1 tau`, then the gyroscopic term: solve
   `I (w2 - w1) + h wm x (I wm) = 0`, `wm = (w1 + w2) / 2`, by Newton's method.
3. `x += h v`, and the orientation turns by the exact rotation of `w` over `h`.

The gyroscopic solve follows the Newton formulation in Erin Catto's
[Numerical Methods](https://box2d.org/files/ErinCatto_NumericalMethods_GDC2015.pdf)
(GDC 2015) but iterates the implicit midpoint form instead of one
backward-Euler step. The implicit midpoint rule preserves the torque-free
invariants, kinetic energy and |L|; the backward-Euler step lost 31% of a fast
asymmetric top's energy in 10 s. Body poses are recovered from the COM state
through `BodyFromCenterOfMass`, so an asymmetric compound rotates about its COM,
not its visual origin.

Kinematic bodies move with their velocity. A `KinematicTargetCommand` for tick
T sets the constant linear velocity and the shortest-arc angular velocity that
reach the target during T, then places the body exactly on the target. From
tick T + 1 the body rests there unless a new target or `TrySetBodyVelocity`
moves it. A target of half a turn or more per tick takes the shorter way round;
keep explicit turns as angular velocities.

Each body keeps its previous and current published pose.
`TryGetInterpolatedPose` blends them linearly in position and by normalized
shortest-arc blend in orientation. Creation and `TryTeleportBody` set both
poses equal, which resets interpolation. A teleport is discontinuous: it is not
a fast kinematic move or a sweep, keeps velocities, and does not test initial
overlap in this slice.

## Commands

Commands carry a `CommandKey`: target tick, producer ID and producer-local
sequence. The world keeps pending commands sorted by key and applies one tick's
commands in that order, never in arrival order. Admission happens on the owner
thread and is allowed during a collision read phase.

| Command | Effect |
| --- | --- |
| `TryEnqueueForce` | Force and torque, plus `(p - COM) x F` for an application point, held through every substep of the tick, then cleared after success |
| `TryEnqueueImpulse` | `dv = J / m` and `dw = I_world^-1 (angular + (p - COM) x J)`, applied once at the start of the tick |
| `TrySetKinematicTarget` | Body-frame target pose for the tick, as above |

`CommandFrame::Body` vectors and points are converted at the body's pose when
the tick applies them. Admission fails without admitting anything for a closed
tick (`PastTick`), a stale handle, the wrong motion type, nonfinite or
out-of-envelope values, a duplicate key (`DuplicateCommand`) or a full table.
An admitted command is not proof that its body survives: after a successful
tick, `GetCommandResults` lists each applied command by key with `Applied`,
`BodyDestroyed` or `Superseded` (an earlier kinematic target for the same body
and tick).

## Contacts

A world initialized with `MaxContactPairs` and `MaxContactPoints` generates
contacts every tick. Both default to zero, which turns contact generation
off: `Step` then runs no detection.

```cpp
world.TryInitialize(store,
                    {
                        .MaxBodies = 64,
                        .MaxColliders = 64,
                        .MaxPendingCommands = 256,
                        .MaxContactPairs = 256,
                        .MaxContactPoints = 1024,
                    });
// ... create bodies ...
const rigid::StepResult step = world.Step();
if (step.Result == rigid::Status::CapacityExceeded)
{
    // step.ExhaustedTable and step.RequiredCount say which table was too small and by how much.
}
StaticArray<rigid::ContactPointState, 1024> points{};
rigid::ContactExportResult exported;
world.TryExportContacts(BorrowedView<rigid::ContactPointState>(points.GetData(), points.GetSize()), exported);
```

### Which pairs make contacts

Contact geometry comes from the collision module's
[contact generation](physics-collision.md#contact-generation). The rigid world
decides which pairs to ask about:

1. Every enabled, solid collider of a **dynamic** body asks the collider
   hierarchy for its candidates: enabled, solid colliders whose filter and its
   own accept each other and whose bounds, widened by `ContactOffsetMeters`,
   overlap its own.
2. A candidate on the same body is skipped.
3. Two dynamic colliders find each other from both sides; the one with the
   lower collider slot reports the pair.

Static against static, static against kinematic and kinematic against
kinematic pairs therefore produce no solid contacts, and are never tested.
Sensors are not solid and make no contacts. A pair of two triangle meshes is
counted in `UnsupportedPairCount` and skipped. In every pair the *first*
collider is the one with the lower slot, and normals point from it toward the
second.

### Detection before anything changes

`Step` generates contacts in its `Detect` phase, before it applies a single
command. Detection reads the poses the previous tick published and writes only
scratch tables. Commands change velocities, and contact geometry depends on
poses alone, so detecting first changes no result.

It does change what a capacity failure costs. Every pair is found, generated
and counted even after a table is full, so the required count is exact. If the
tick's contacts do not fit:

- `Step` returns `CapacityExceeded` with `Phase` `Idle`, the `ExhaustedTable`
  (`Pairs` is checked before `Points`) and its `RequiredCount`.
- The tick does not advance. No pose or velocity has changed, the tick's
  commands are still pending, and forces are still held.
- The contacts of the last completed tick are still published, and the cache
  they form is intact.

Remove load, or recreate the world with at least `RequiredCount` entries, and
step again: the same tick then runs, with its commands applied once.

| Capacity | Limit | Reserved |
| --- | --- | --- |
| `MaxContactPairs` | `MAX_RIGID_WORLD_CONTACT_PAIRS` (2^22) | Two generations of pair records |
| `MaxContactPoints` | `MAX_RIGID_WORLD_CONTACT_POINTS` (2^24) | Two generations of point records and of patch records |
| `ContactOffsetMeters` | `collision::MAX_CONTACT_OFFSET_METERS` (1 m) | Nothing; the default is two centimeters |

A pair holds at most `collision::MAX_CONTACT_PAIR_POINTS` (32) points in
`collision::MAX_CONTACT_PAIR_PATCHES` (8) patches of at most
`collision::MAX_CONTACT_PATCH_POINTS` (4). The candidate scratch holds one
handle per collider slot, so it cannot overflow.

### Persistent contacts

A solver starts each tick from the impulses of the last one, which is only
sound while a contact point is still the same point. Each point is therefore
identified, and its record carries a `PersistedTickCount` and impulse slots
that stay zero until the solver arrives.

A point's identity is the pair of collider incarnations (slot and generation),
their shapes, the mesh triangle, and the two
[contact features](physics-collision.md#from-a-normal-to-contact-points). Pair
records are kept sorted by their two collider slots and joined with the
previous tick's in one pass; points are matched by comparing those values in
full. No hash takes part, so a hash collision cannot stand for equality.

A cached point is carried over only while everything it depended on still
agrees. Otherwise it retires, under exactly one `ContactRetirement` reason,
the first that applies:

| Reason | When |
| --- | --- |
| `Separated` | The pair has no contacts now: it moved apart, or a collider was destroyed, disabled or filtered out. A new collider in a reused slot is a new pair |
| `GeometryChanged` | A collider of the pair uses a different shape |
| `MaterialChanged` | A collider's `Material` changed (`TrySetColliderMaterial`) |
| `SettingsChanged` | The world's contact settings changed |
| `Teleported` | A body of the pair was teleported, even to the same pose |
| `FeatureChanged` | The pair still touches, but no current point has this point's triangle and features |
| `NormalChanged` | A current point has the same features, but its patch normal turned by more than `collision::CONTACT_PATCH_NORMAL_COSINE` (five degrees) |

Shapes and contact settings cannot change after creation in this slice, so
`GeometryChanged` and `SettingsChanged` are checked but not yet reachable.
Colliders with different materials are different colliders, so a patch never
mixes materials. Friction and restitution values, and what a cached impulse is
worth after a change, belong to the solver.

### Diagnostics and export

`GetContactDiagnostics` describes the last detection, including one whose
`Step` failed on capacity:

| Field | Meaning |
| --- | --- |
| `CandidatePairCount`, `TestedPairCount`, `UnsupportedPairCount` | Bounds candidates, pairs sent to contact generation, and mesh-mesh pairs skipped |
| `PairCount`, `PatchCount`, `PointCount` | What the tick's contacts are, or would have been |
| `GeneratedPointCount` | Points before reduction |
| `TriangleTestCount`, `ApproximatePatchCount`, `DiscardedPatchCount` | Mesh work, patches whose penetration search stopped at a bound, and patches beyond the pair limit |
| `PersistedPointCount`, `NewPointCount` | Cache hits, and points with no usable cached point |
| `RetiredPointCounts`, `GetRetiredPointCount(reason)` | Cached points not carried over, by reason |
| `PairHighWater`, `PointHighWater` | Largest counts of any detection since initialization |
| `ExhaustedTable`, `RequiredCount` | The table the last detection exhausted, and what it needed |

Every current point is persisted or new, and every cached point was persisted
or retired. A detection that failed on capacity matched nothing, so its cache
counts are zero.

`TryExportContacts` writes one plain-value `ContactPointState` per point of the
last completed tick: both bodies and colliders, the patch index and normal,
position, separation, features, triangle, `PersistedTickCount` and
`IsApproximate`. Points come in pair order, then patch, then point. A buffer
that is too small still reports `PointCount`. The contacts are those detected
at the start of that tick, from the poses the tick before published.

## Phases and failures

```text
Idle -> Detect -> Commit -> Simulate -> Publish -> Idle
           \-> Idle (capacity)   \-> Faulted
```

Callers observe `Uninitialized`, `Idle` or `Faulted`; the other phases exist
during `Step`. The rows of the
[failure table](physics-systems.md#world-phases-commands-and-failure-contracts)
that apply without contacts behave as follows:

| Result class | Behavior |
| --- | --- |
| Invalid input, handle or phase; unsupported capability | No mutation, output unchanged, the failing field named by the status |
| Initialization or admission capacity or allocation failure | `TryInitialize` installs nothing; a full body, collider, lease or command table creates or admits nothing |
| `Step` preflight failure | `InvalidPhase` (uninitialized, or a collision read view active) or `Faulted`: the tick does not advance, commands stay pending, published state is unchanged |
| Contact capacity exhausted after detection | `CapacityExceeded` with the exhausted table and its required count: the tick does not advance; no pose, velocity or cached contact changes; commands stay pending and forces stay held |
| Successful step | The tick advances exactly once, forces are consumed, the epoch advances and results are published |
| Nonfinite state or a COM outside the coordinate envelope after advancement begins | `Faulted`: state may be partially advanced, no tick or results are published, and every later mutation, command and step is rejected with `Faulted` |

A faulted world still answers `TryGetBodyState`, `TryExportBodyStates` and
`GetDiagnostics`, which reports the `FaultReason` and the first faulted body;
destroy it and rebuild from a last complete checkpoint. The architecture's
capacity row says that commands already committed for the tick stay applied.
This world detects [before it commits](#detection-before-anything-changes), so
at exhaustion nothing has been committed and the commands simply stay pending.
Row and island capacities arrive with the solver, and incomplete event output
with events.

Every table is reserved at initialization from `RigidWorldSettings`: body slots,
collider attachments, pending commands, results, two generations of contact
pairs, patches and points, and the candidate scratch. Body creation, command
admission, `Step` with its contact detection, export and teleport never
allocate; the allocation tests
replace the global allocator to fail every initialization allocation in turn and
to prove that an active tick performs none.

## Precision and units

Units are meters, seconds, kilograms and radians; Y is up. Body COM positions,
velocities, orientations and the mass frame are stored and integrated in
`float64`, and published poses use `collision::RigidPose` with a `float32`
orientation. The [precision rule](physics-systems.md#units-precision-and-simulation-clock)
starts solver data in `float32`; this serial baseline keeps motion state in
`float64` so that substeps do not round through `float32`, and a narrower solver
representation is a measured later change. `MAX_LINEAR_SPEED_METERS_PER_SECOND`,
`MAX_ANGULAR_SPEED_RADIANS_PER_SECOND` and `MAX_COMMAND_COMPONENT` bound
admitted values; simulated speeds are not clamped.

## Evidence

`ludus_physics_rigid_tests` checks against analytic answers:

- Free fall matches the discrete semi-implicit Euler closed form to 1e-12 and
  the continuous answer within the method's first-order error.
- An off-center impulse on a 1 x 2 x 3 m box gives `dv = J / m` and
  `w = (r x J) / I` exactly.
- An asymmetric two-sphere compound has its COM at the composed position and
  rotates about it, with the body origin on the analytic circle.
- A box rotated 45 degrees maps angular momentum to the inverse of the rotated
  tensor, and the principal frame recovers the box's moments and axes.
- Hulls of a box, the unit corner tetrahedron and an octahedron match their
  closed forms; interior, duplicate and coplanar points change nothing.
- A fast asymmetric top spinning at 20 rad/s about its intermediate axis flips
  (the tennis racket effect), with relative drift over 10 s at 60 Hz and four
  substeps of 1.2e-12 in kinetic energy, 5.8e-13 in |L| and 2.0% in the
  direction of L, measured on macOS arm64.
- Kinematic targets take the shortest arc and land exactly; teleports reset
  interpolation.

The contact suite checks:

- **Pair rule:** static and kinematic bodies overlapping each other make no
  contacts and are never tested; a dynamic body contacts each of them; two
  dynamic bodies make one pair; colliders of one body, sensors, disabled
  colliders and rejected categories make none.
- **All-pairs oracle:** 72 seeded bodies of mixed motion types, one or two
  turned colliders each, with mixed filters, sensors and disabled colliders.
  The test applies the pair rule to every pair of colliders and asks contact
  generation directly; the world's exported contacts equal the oracle's, value
  for value, over three ticks with a third of the bodies teleported between
  them.
- **Resting patch:** a cube sunk one centimeter into a floor, as a box and as
  a triangle mesh, keeps its four corners with the floor normal and the
  analytic depth, and all four persist on the following ticks.
- **Mesh seams:** a capsule lying on a flat floor of one-meter cells slides
  diagonally for 240 ticks across cell edges and diagonals. Every normal stays
  the floor's normal, every depth stays one centimeter, and each tick has
  exactly its two ends. The same floor cooked with
  `InternalEdgePolicy::KeepAll` is the control: it reports sideways normals.
- **Capacity:** three cubes on a floor exceed two pairs, then ten points.
  `Step` names the table and the required count, and the tick, every pose and
  velocity, the pending impulse and the published contacts are unchanged,
  twice in a row. After one cube is removed the tick runs, the impulse applies
  once and the first cube's cached points are still matched.
- **Cache:** reuse on rest, and retirement by teleport of either body,
  separation and return, destruction with slot reuse, a material change,
  corners sliding off a floor's edge (`FeatureChanged` for those two, the
  other two persist) and a sphere passing another (`NormalChanged`).

The command and world suites cover every applicable failure row, and
`ludus_physics_rigid_allocation_tests` covers allocation injection and the
zero-allocation tick, with contact detection, matching and export inside it.

## Limits of this slice

Contacts are generated and cached but not solved: no contact response,
friction, restitution, constraints, sleeping, continuous collision, fixed
axes, damping, shape replacement or momentum-preserving mass changes. Contact
candidates use poses widened by the contact offset, not velocities; fast
bodies are continuous collision's job. Static bodies do not wake anything,
because nothing sleeps yet. The
first-order splitting of velocity and orientation updates bounds how well the
direction of angular momentum is kept; the scene above records it.

## Attribution

- Mass-frame clarity and parallel-axis composition follow Eric Lengyel's
  chapter 14 of *Game Engine Gems 1*, as recorded in the
  [Gems review](physics-systems-gems-review.md).
- The hull triangulation is the incremental algorithm of Mark de Berg, Otfried
  Cheong, Marc van Kreveld and Mark Overmars, *Computational Geometry:
  Algorithms and Applications*, third edition, Springer, 2008, chapter 11.
- Tetrahedron second moments: Fabio Tonon,
  [Explicit Exact Formulas for the 3-D Tetrahedron Inertia Tensor in Terms of its Vertex Coordinates](https://doi.org/10.3844/jmssp.2005.8.11),
  *Journal of Mathematics and Statistics* 1(1), 2005, pages 8-11.
- Feature-keyed persistent contacts: Ádám Moravánszky and Pierre Terdiman,
  *Fast Contact Reduction for Dynamics Simulation*, *Game Programming Gems 4*,
  Charles River Media, 2004, section 3.5, pages 253-263, as recorded in the
  [Gems review](physics-systems-gems-review.md#fast-contact-reduction-for-dynamics-simulation).
  Ludus compares full identities instead of a feature hash.
- Coherence only while the contact geometry persists: David Baraff,
  [Curved Surfaces and Coherence for Non-Penetrating Rigid Body Simulation](https://www.cs.cmu.edu/~baraff/papers/sig90.pdf),
  *Computer Graphics* (SIGGRAPH '90) 24(4), 1990, pages 19-28.
- The implicit gyroscopic Newton solve: Erin Catto,
  [Numerical Methods](https://box2d.org/files/ErinCatto_NumericalMethods_GDC2015.pdf),
  Game Developers Conference 2015.
