# Physics systems architecture

**Status:** Proposed; no general rigid-body implementation or dependency is added
by this document. Evidence baseline: `origin/main` at `1e2227c`, 2026-10-07.
Selection revised 2026-11-10 against `origin/develop` at `d44e80d`.

Ludus owns its physics API, asset cooking, gameplay integration, diagnostics
and the CPU collision and rigid-body implementation behind them. Jolt was the
first third-party candidate. The [P0 allocation investigation](#p0-allocation-investigation)
and its [follow-up screen](#p0-follow-up-candidate-screen) rejected every stock
engine tested. The [selection revision](#selection-revision) records why Ludus
implements the system itself and what that commits it to.

## Initial design before the Gems review

The initial design selected the following contracts from Ludus's existing
ownership rules and current primary physics documentation. The
[subsequent Gems review](physics-systems-gems-review.md) records which decisions
it strengthens or changes. The contracts below are the revised design and take
precedence over this historical starting table.

| Area | Initial choice |
| --- | --- |
| Ownership | An explicitly owned world, immutable shared geometry, world-local colliders, optional dynamic/kinematic motion, typed generational handles. |
| Clock | CPU-authoritative fixed ticks, explicit substeps, no renderer dependency, game-owned catch-up policy. |
| Shapes | Sphere, box, capsule, convex hull, immutable compound; static mesh and terrain cooked separately from rendering. |
| Collidables | Shape instance plus pose, material, filtering, participation flags and game binding; geometry does not own simulation state. |
| Bodies | Static, kinematic and dynamic; explicit body frame versus center of mass; validated positive mass/inertia. |
| Phantoms | Sensor colliders with deferred overlap events; query-only colliders share geometry and traversal. |
| Solver | Persistent contacts, warm-started sequential impulses, friction, restitution, sleeping islands; qualified backend owns numerical details. |
| Forces and constraints | Forces/torques and impulses have distinct units; constraints use explicit anchors, limits, motors and break policies. |
| Queries | Ray, convex sweep, overlap; any/closest/all; bounded caller buffers, explicit initial-overlap, filter and completion semantics. |
| Concurrency | Owner-thread mutation at tick barriers; read phases; serial diagnostic path before parallel work. |
| Diagnostics | Replayable commands, inspectable snapshots, contact/constraint overlays, stage timings and capacity counters. |

## Integration boundaries

FoundationMath remains the owner of checked numerical vocabulary, not world
state. Physics does not depend on GameplayWorld, RHI, editor widgets or an ECS.
The game maps entity identity to physics handles and applies tick-addressed
commands. Terrain provides immutable physical revisions; the physics adapter
installs them at a safe tick boundary and pins required coverage.

The existing [PhysicsFluid module](../../modules/physics/fluid/README.md) is a
single-owner CPU shallow-water field, not a rigid-body world. Preserve it as an
independent subsystem. The older [Drift design](physics-fluid-simulation.md)
describes that game's analytic model; it does not define this general system.
Future buoyancy samples fluid state through an explicit game or coupling pass.

The editor's [Explore as Player](../development/editor-workspace.md#explore-from-a-first-person-player)
preview is the first collision consumer. Its **Static Collider** component is
authored World data, so the P1 collision world consumes that component and
adds no second collider vocabulary. The editor keeps its own step, slide and
jump rules.

The first P1 slice, the [physics collision module](physics-collision.md),
ships `PhysicsCollision` with an immutable static triangle mesh, a closest
raycast and a closest axis-aligned box cast. It lifts the preview's sweep, and
the preview now cooks its **Static Collider** triangles into that module and
sweeps with its box cast; see [Explore as Player](physics-collision.md#explore-as-player).
A second slice adds sphere, box, capsule and convex hull shapes and the
general convex cast.

## Primary evidence

Thanks to Jorrit Rouwé, [Architecture of Jolt Physics](https://jrouwe.github.io/JoltPhysics/),
for practical body/shape separation, batched installation, and clear determinism
limitations. Thanks to Erin Catto, [Solver2D](https://box2d.org/posts/2024/02/solver2d/),
2024, for comparing sequential-impulse solver variants and explaining soft
constraints and substeps. Thanks to the NVIDIA PhysX team,
[Scene Queries](https://nvidia-omniverse.github.io/PhysX/physx/5.4.1/docs/SceneQueries.html),
for query collection and filtering contracts. These inform the design; no source
code is copied and no performance result is transferred to Ludus.

## Revised decisions and scope

The baseline is 3D rigid-body gameplay and collision queries: crates, doors,
platforms, triggers, picking, projectiles, and modest joint chains. A restricted
plane is useful, but is not a substitute for a dedicated 2D engine's topology
and performance. Characters, ragdolls and vehicles are consumer features added
when the core passes its gates. Deformables, cloth, fracture, articulations and
fluid-solid coupling have separate requirements and delivery plans.

Use one production implementation, owned by Ludus and private to Physics, and
one public contract. The [selection revision](#selection-revision) replaces the
earlier plan to qualify a third-party engine first. Keep the brute-force query
oracle and third-party comparison engines test-only. The CPU baseline has one
rigid solver and no runtime solver selector. GPU simulation is an optional
future workload.

Each gate must prove the supported native platforms and the separate pinned
Emscripten build, audit CPU ISA and compiler flags, and record which
capabilities pass. Browser support in other Ludus modules does not establish
physics support. Verify SDK packaging, creator-owned destruction, jobs,
diagnostics and hot reload. Keep solver-private headers, IDs, serialization and
settings out of public file sets. A comparison engine used by tests is pinned
to an exact revision and license.

Physics has no process-global registration or allocator hooks. Every geometry
store and world takes its memory resources explicitly at creation, following
the [memory rules](memory-management.md). A test-only comparison engine that
needs global hooks owns them inside its own test executable.

**Failure behavior was the selection gate.** Jolt's consulted
[temporary allocator](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Jolt/Core/TempAllocator.h)
aborts when its fixed arena is exhausted; its
[update errors](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Jolt/Physics/EPhysicsUpdateError.h)
can indicate ignored contacts. A status wrapper does not repair either behavior.
Before production adoption, prove conservative storage bounds over admitted
inputs and disable hidden allocation fallbacks, or supply a narrowly reviewed
upstream change with safe failure propagation. Merely returning null from an
allocator whose callers assume success is invalid. Audit initialization and
shape creation too. If qualification cannot satisfy Ludus's rules, record a
failed candidate and revise this selection before implementation; do not ship
abort-on-OOM, exceptions, or an unbounded fallback under a `noexcept` promise.
The two investigations below record the failed candidates. The owned
implementation is held to the same rule and the same allocation-injection tests.

### P0 allocation investigation

The isolated probe used MIT-licensed [Jolt 5.6.0](https://github.com/jrouwe/JoltPhysics/tree/e77f175595e64cb44218cc9d9d56fc365ad0e36a)
(`e77f175595e64cb44218cc9d9d56fc365ad0e36a`) on macOS arm64 with Clang
23.1.3, exceptions disabled, ASan and UBSan. It injected null from the
allocation, reallocation and aligned allocation hooks at individual calls.
Each fault ran in its own process. Thanks to Jorrit Rouwé and the Jolt
contributors for the allocation hooks and source that make these boundaries
inspectable.

| Probe | Observed behavior |
| --- | --- |
| Static controls | A stair tread ray and a capsule sweep into its riser passed with zero Jolt allocation-hook calls during the queries. A saddle-shaped terrain cell hit at 0.5 m using the render triangles and 2 m using Jolt's heightfield, which uses the opposite cell diagonal. Use matching triangles unless a heightfield adapter proves surface parity. |
| First allocation in type registration or world initialization | UBSan detected null pointer arithmetic in the registry hash table. ASan detected a null write in `BodyManager::Init`. These entry points return no allocation status. |
| First and second allocations in two-triangle mesh cooking | The first reached a constructor on null storage in `MeshShapeSettings::Create`. Allowing that allocation and failing the next reached null storage in the internal triangle array copy. Checking only the outer shape allocation would leave the second failure. |
| Active tick with undersized or null scratch | A 16-byte arena aborted on the first 7,344-byte request. Returning null for the same request instead caused undefined behavior while constructing the step context array. |
| First active tick with enough scratch | The job freelist allocated its first block lazily. Failing that allocation reached a null storage reference. Prewarming one job in the serial pool moved the block allocation to setup. The same tick then succeeded with zero Jolt allocation-hook calls and 169,888 bytes of peak scratch. |
| Contact capacity exhausted | With a four-contact limit, eight independent boxes began at 0.4 m on one static floor. The tick returned `ContactConstraintsFull` after moving four boxes up to about 0.429 m and four down to about 0.397 m. With 64 slots, all eight moved up and the tick reported success. |

The tick used one serial job pool of 256 slots, one substep at 60 Hz, 16 body
slots, 64 body-pair slots and a 1 MiB scratch buffer for the successful run.
The peak applies to this fixture. Hook counts cover only allocations routed
through Jolt's hooks. Data-dependent growth also exists outside tick scratch,
for example in the
[broad-phase traversal stacks](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/Collision/BroadPhase/QuadTree.cpp#L1016-L1031).
The same unchecked mesh construction and scratch abort paths are present in
the upstream source snapshot `be646ae6a84f3283c1ff6bd2bb9367aaf978e82c).

**Decision.** Prewarming solves the measured lazy allocation. Adopting Jolt
would still require a conservative reservation contract covering registration,
world and shape construction, body insertion, queries and stepping, or
upstream changes that propagate failure through those paths. A contact
capacity error after advancement must follow the `Faulted` path and preserve
the last published snapshot. Increasing scratch or wrapping a backend result
cannot provide the ordinary preflight guarantee. Reject unmodified Jolt for
production under the current contract. The probe did not exercise Linux,
Windows, the pinned browser toolchain, SDK consumption or replay qualification.

| Replacement screen | Allocation contract observed in source |
| --- | --- |
| PhysX | Its allocator interface explicitly requires a nonnull result. It would need its own storage proof. [PhysX allocator contract](https://github.com/NVIDIA-Omniverse/PhysX/blob/b97365c6ddb32fda8469bc788ec18024862b0368/physx/include/foundation/PxAllocatorCallback.h#L34-L49) |
| Bullet 3.25 | Array growth copies live elements to newly allocated storage without checking the destination. [Bullet array growth](https://github.com/bulletphysics/bullet3/blob/2c204c49e56ed15ec5fcfa71d199ab6d6570b3f5/src/LinearMath/btAlignedObjectArray.h#L280-L298) |
| ReactPhysics3D 0.10.2 | World construction uses placement construction directly on the allocator result. [ReactPhysics3D world creation](https://github.com/DanielChappuis/reactphysics3d/blob/cd958bbc0c6e84a869388cba6613f10cc645b3cb/src/engine/PhysicsCommon.cpp#L224-L240) |

The table is a source audit. An isolated runtime probe separately tested
[ODE 0.16.6](https://bitbucket.org/odedevs/ode/src/a5fc0bc282703ab34d7d3d2500bafae7428eeaa9/)
(`a5fc0bc282703ab34d7d3d2500bafae7428eeaa9`, dual LGPL-2.1-or-later or
modified BSD-3-Clause) on the same macOS arm64 sanitizer toolchain. Thanks to
the ODE contributors for the
[QuickStep retry contract](https://bitbucket.org/odedevs/ode/src/0.16.6/include/ode/objects.h#lines-489:498)
and separate step allocator. The probe advanced one body with a pending force
for one 60 Hz tick. Its control step allocated two scratch blocks.

- Stock QuickStep called `ResetState` on a null arena when the first block
  was denied. A local guard in `ReallocateIslandsMemArena` made each of the
  two failures return zero independently. Body height, vertical velocity and
  pending vertical force stayed unchanged. Each successful retry matched
  the control in those values. [ODE arena reset](https://bitbucket.org/odedevs/ode/src/0.16.6/ode/src/util.cpp#lines-199:209)
- Denying the first world allocation reached the `dxWorld` constructor
  with null storage. The step guard cannot repair world creation. Body
  insertion uses the same unchecked allocation operator and immediately
  writes to the result. [ODE world creation](https://bitbucket.org/odedevs/ode/src/0.16.6/ode/src/ode.cpp#lines-1580:1585),
  [body creation](https://bitbucket.org/odedevs/ode/src/0.16.6/ode/src/ode.cpp#lines-238:246),
  [allocation operator](https://bitbucket.org/odedevs/ode/src/0.16.6/ode/src/objects.h#lines-59:67)
- The runtime fixture disabled triangle meshes. Source review found that the
  OPCODE mesh cooker uses ordinary C++ allocation for its tree, beyond the
  step allocator. A step guard and a check in world creation would still
  leave mesh admission and body insertion without a storage proof.
  [OPCODE tree build](https://bitbucket.org/odedevs/ode/src/0.16.6/OPCODE/OPC_Model.cpp#lines-167:180)

**Selection remained open after this probe.** Reject stock ODE as a production replacement. The
local QuickStep patch addresses the measured step failure. Admission of worlds,
bodies and mesh shapes still requires upstream changes or a conservative storage
contract. Screen future candidates at world creation, mesh cooking and the first
active tick before running the full platform matrix. A bounded candidate must
demonstrate checked worst-case reservations for admitted inputs and recoverable
rejection before publishing world, geometry or tick state.

### P0 follow-up candidate screen

An isolated macOS arm64 probe built MIT-licensed
[Box3D](https://github.com/erincatto/box3d/tree/336263320469e5e9febfe18697ab562488f7e97a)
at `336263320469e5e9febfe18697ab562488f7e97a` with Clang 23.1.3, ASan and
UBSan. It used the serial scalar configuration with release assertions
disabled. Thanks to Erin Catto and the Box3D contributors for
[Announcing Box3D](https://box2d.org/posts/2026/06/announcing-box3d/) and the
public allocator and mesh interfaces used by this probe.

The fixture used a 4 m by 4 m synthetic terrain cell with one corner raised
2 m. Its two triangles have the same winding and shared diagonal as the
editor's terrain mesh. Downward rays hit the flat and sloped triangles at
0 m and 1 m. A 0.5 m cube dropped from a center height of 3 m onto the flat
triangle and settled at 0.254931 m with zero vertical speed. The final pose
and linear and angular velocities matched exactly on a second run of the
same executable. Both runs released all bytes counted by the allocator hook.

| Operation | First allocation denied in a separate run |
| --- | --- |
| World creation | Returned a world ID after Box3D's internal stack allocator failed to obtain its 2,048-byte backing buffer. |
| Mesh cooking | ASan detected a null write inside `b3CreateMesh` after a 32-byte request failed. |
| Dynamic body insertion | UBSan detected a null island record in `b3CreateIsland` after a 64-byte request failed. |
| First tick | ASan detected a null write during broad-phase tree rebuild after a 1,024-byte request failed. |

The control counted four allocation-hook calls on the first tick and 15
over 360 ticks at 60 Hz with four substeps. Two later ticks also allocated.
Raising all five `b3Capacity` hints to 256 preserved those counts and the
final cube state while increasing peak tracked bytes from 311,168 to
747,456. These hints reserve some initial storage. Tick work can still
grow storage, and `b3World_Step` returns no failure status (see
[Box3D capacity fields](https://github.com/erincatto/box3d/blob/336263320469e5e9febfe18697ab562488f7e97a/include/box3d/types.h#L130-L147),
[allocator implementation](https://github.com/erincatto/box3d/blob/336263320469e5e9febfe18697ab562488f7e97a/src/core.c#L136-L174),
and [step interface](https://github.com/erincatto/box3d/blob/336263320469e5e9febfe18697ab562488f7e97a/include/box3d/box3d.h#L48-L57)).

| Other candidates inspected | Allocation gate |
| --- | --- |
| Nudge at `b66b190b3cf4cdf468ee489d31bcba3dbd6ff6d7` | World creation immediately clears the allocator result. The mesh cooker uses its own unchecked heap calls in addition to the configurable allocator. [World creation](https://github.com/RandyGaul/nudge/blob/b66b190b3cf4cdf468ee489d31bcba3dbd6ff6d7/src/nudge.c#L38-L62), [mesh cooking](https://github.com/RandyGaul/nudge/blob/b66b190b3cf4cdf468ee489d31bcba3dbd6ff6d7/src/trimesh.c#L95-L135) |
| Rapier at `475caa3ff4646fd9a796b67d14116256f4962bc7` | Arena insertion grows through infallible `Vec::reserve_exact`. With Rust's default allocation error behavior, exhaustion terminates the process. [Rapier arena growth](https://github.com/dimforge/rapier/blob/475caa3ff4646fd9a796b67d14116256f4962bc7/src/data/arena.rs#L321-L336), [Rust allocation error contract](https://doc.rust-lang.org/std/alloc/fn.handle_alloc_error.html) |
| MuJoCo 3.17.0 | The preallocated arena can discard contacts or disable the constraint solver under pressure. Stack exhaustion is a hard error. [MuJoCo memory allocation](https://mujoco.readthedocs.io/en/3.17.0/modeling.html#memory-allocation) |

Reject stock Box3D under P0. Its terrain and rigid motion control establishes
the minimum scene fixture. A qualification attempt would need checked
reservations or fallible construction for world, mesh and body setup, and
preflight covering broad-phase, solver and contact growth before a tick
mutates state. The macOS probe establishes the rejection. The platform matrix
was not run. The selection revision below closes backend selection.

### Selection revision

**Decision.** Ludus implements its own CPU collision and rigid-body system.
No third-party physics engine enters an engine target or the installed SDK.
This replaces the earlier instruction to qualify an existing engine before
budgeting a solver.

| Reason | Evidence |
| --- | --- |
| The allocation gate has no stock candidate | Jolt, ODE and Box3D failed the runtime probes above. PhysX, Bullet, ReactPhysics3D, Nudge, Rapier and MuJoCo failed the source screen. Every remaining route needs a maintained fork or a storage proof over code Ludus does not own. |
| Preflight needs control of the step order | Jolt reported contact exhaustion after it had moved bodies. An owned step counts contact storage before it changes any pose. See the [failure contracts](#world-phases-commands-and-failure-contracts). |
| Simulation research needs an open solver | Researchers read, change and replace solvers, inspect state every tick and compare against a baseline. A private third-party engine offers none of that through the Ludus API. |
| A Ludus-owned collision consumer already exists | Explore as Player sweeps a box against up to eight million static triangles with `PhysicsCollision`, using Ludus containers and no third-party code. |

**What this commits Ludus to.** A rigid-body engine has a long robustness
tail: general convex narrowphase, contact reduction, internal mesh edges,
continuous collision, joints, islands and sleeping. This revision claims no
parity with any shipping engine in features, robustness or speed. The scope is
what the [delivery gates](#delivery-gates) name. Vehicles, ragdolls, character
controllers, SIMD kernels and parallel islands wait for a concrete consumer and
measurements.

**Solver baseline.** The rigid solver is a warm-started sequential-impulse
solver with soft constraints, substeps and a relaxation pass. Thanks to Erin
Catto, [Solver2D](https://box2d.org/posts/2024/02/solver2d/), 2024, which
compares eight solver variants and names this combination Soft Step. Ludus
adopts the structure, not the code or the tuning. The contracts in this page
already use its vocabulary: accumulated impulses, impulse bounds and break
thresholds inferred from impulse. The post reports 2D results; the 3D behavior
is a P2 measurement.

**Comparison engines.** The pinned Jolt and Box3D probes remain useful as
test-only comparisons on shared scenes, where their allocation behavior does
not matter. A comparison target is optional and never installed, and no engine
or SDK target depends on it. Agreement with another engine is evidence about
behavior, not proof of correctness: use analytic answers and conservation
checks first. Convert frames, mass properties, material mixing and tick
settings to the Ludus contract before comparing. P2 decides from measured build
cost whether a comparison runs in batch CI.

**Reopening the selection.** Reconsider a third-party engine only when an
upstream release propagates allocation failure through its world, shape, body
and step paths, or when a consumer needs a capability whose owned cost is
measured and exceeds maintaining a qualified fork. Record that evidence here
before changing the decision.

## Module and ownership model

Use two targets. `modules/physics/collision/`, proposed `PhysicsCollision`,
owns shapes, the geometry store, colliders, filters, acceleration hierarchies
and the ray, cast and overlap queries. `modules/physics/rigid/`, proposed
`PhysicsRigid`, owns bodies, contacts, constraints, the solver and tick events,
and depends on `PhysicsCollision`. A collision world is usable alone. A rigid
world owns exactly one collision world and adds motion to it. The earlier
single-target plan waited for an independently shippable collision-only
consumer; the editor preview and the simulation siblings below are such
consumers. Reuse FoundationMath's `Aabb3`, `Plane`, `Sphere` and `Ray3`
vocabulary. Offline cooking lives in a separate tool target; it may depend on
content/filesystem services. The gameplay adapter belongs to the consumer and
maps `EntityId` to physics identity.

```mermaid
flowchart TD
    Assets[Authored collision assets] --> Cook[Offline collision cooker]
    Cook --> Geometry[Immutable geometry store]
    Terrain[Retained terrain physical revision] --> Adapter[Consumer adapter]
    Game[Game commands and entity bindings] --> Adapter
    Adapter --> World[Physics world]
    Geometry --> World
    Math[FoundationMath] --> World
    Jobs[FoundationThreading] --> World
    World --> Events[Tick events and pose snapshots]
    World --> Queries[Protected query views]
    Events --> Game
    Queries --> Game
    Geometry --> Siblings[Simulation sibling modules]
    Queries --> Siblings
    World --> Debug[Optional debug extraction]
    Debug --> Editor[Editor or renderer consumer]
```

The diagram expresses ownership and calls; Physics never calls back into game,
editor or renderer code. Logging/profiling are opt-in implementation facilities.
Use the existing [bounded jobs](../decisions/0020-bounded-task-graphs.md),
[memory rules](memory-management.md), [entity phases](entity-world.md), and
[terrain coverage contract](terrain.md#query-and-physics-contracts).

| Object | Owner and lifetime | Contains |
| --- | --- | --- |
| Geometry store | Explicitly owned service, outlives worlds that lease its geometry | Immutable cooked shape revisions, bounds, support data, mesh acceleration and mass integrals |
| Shape lease | Creator-owned retained asset reference | Geometry identity/revision; world insertion retains its own lease |
| Collider | One physics world | Shape lease, local pose, material/filter, participation flags, stable subshape map and copied game binding |
| Body | One physics world | Body frame, attached colliders, motion type; dynamic motion/inertia only when needed |
| Constraint | One physics world | Endpoint handles, body-local frames, validated settings, state and diagnostic impulses |
| Read view | World-controlled scoped reader | Epoch and protected access; prevents mutation/destruction until released |
| Event/pose batch | World output until next publication, or caller-owned copy | Tick and epoch, value handles/bindings, no implementation pointers |

Keep `ShapeHandle`, `ColliderHandle`, `BodyHandle` and `ConstraintHandle` distinct.
World handles contain owner token, slot and generation, following the
[identity architecture](object-handles-uuid.md). Validate in release builds;
retire rather than wrap exhausted generations. Runtime owner tokens do not enter
saves or network packets. The geometry store has its own owner identity.

A collider is the general collidable. It may be attached to a body, or have a
standalone pose for static/query/sensor use. It never owns a second independent
motion integrator. Attached collider pose is `body pose * collider local pose`.
The implementation stores attached colliders directly and keeps filtering,
material and sensor modes per collider. Static and query-only colliders create
no dynamic body record. A combination the current gate does not implement is
rejected with `Unsupported`.

Destroying a body removes its constraints and attached colliders in one accepted
owner-phase operation. Detached colliders are a separate explicit operation,
never an accidental side effect. Close public access before reclaiming memory;
join readers/jobs and retire geometry leases afterwards. Events may reference
already destroyed incarnations: include copied bindings and an end reason;
never require a pointer lookup to interpret an event. Reload reconstructs a
world from versioned values, not module-owned vtables or raw solver state.

### Simulation siblings and research solvers

Other simulations are sibling modules under `modules/physics/`, as
[PhysicsFluid](fluid-field.md) already is. A sibling owns its state, its solver
and its failure contract. It may use `PhysicsCollision` for geometry and
queries, together with the tick, debug extraction and recording value types
that module exports. It does not reach into `PhysicsRigid` internals.

| Rule | Reason |
| --- | --- |
| No virtual solver interface or runtime selector inside `PhysicsRigid` | One production rigid path stays readable, testable and replayable |
| An alternative rigid solver is its own world type in its own module | It runs the shared scenes without weakening the baseline's contracts |
| Coupling is an explicit pass owned by the game or a coupling module | Each side keeps its clock and failure behavior; impulses cross at a named boundary |
| A sibling outside this repository consumes the installed SDK | Public `PhysicsCollision` headers and CMake targets are the supported seam, with the same Doxygen and SDK consumer checks as any public API |
| A sibling inside this repository follows every engine standard | No exceptions, bounded capacities, explicit failure results, tests and documentation |

Research use adds four acceptance needs, each assigned to a
[delivery gate](#delivery-gates): stepping a world headless from an SDK
consumer; exporting body state, contacts and diagnostics as plain values every
tick; same-build replay of a recorded command stream; and shared scenes in the
[Ludus scene source format](scene-source-format.md), so that two solvers run
identical content.

The first comparison solver candidate is position-based rigid-body dynamics.
Thanks to Matthias Müller, Miles Macklin, Nuttapong Chentanez, Stefan Jeschke
and Tae-Yong Kim, [Detailed Rigid Body Simulation with Extended Position Based Dynamics](https://doi.org/10.1111/cgf.14105),
*Computer Graphics Forum* **39(8)**, 2020, pages 101-112, presented at SCA 2020.
It is a lead for a sibling module, not adopted design. This revision did not
reproduce its results.

## Shapes, cooking and mass properties

| Shape | Initial use and restrictions |
| --- | --- |
| Sphere, box, capsule | Dynamic, kinematic, static, sensor and query shapes; positive finite dimensions |
| Convex hull | General dynamic shape; bounded vertices/faces, validated closed volume and support mapping |
| Immutable compound | Rigid union of primitives/hulls; stable leaf IDs, bounded depth and leaf count |
| Triangle mesh | Static environment and queries; kinematic use only after separate qualification; never unrestricted dynamic concave bodies |
| Heightfield | Static terrain adapter with matching cell diagonal, holes, materials and physical resolution |

No infinite plane in the first public surface: use finite authored ground within
explicit bounds. No runtime shear, negative/mirrored scale or nonuniform scale.
Bake those transforms into a new validated asset. Positive uniform scaling also
creates a distinct cooked revision; colliders themselves use rigid transforms.
Compound cycles and degenerate hulls fail cooking. Report triangle/feature IDs
and scale-aware tolerances in errors, rather than accepting unusable geometry.

Cooking validates finite coordinates, winding, degenerate triangles, indices,
units and limits; builds local-space BVHs and stable feature/material mappings;
and records source digest, cooker and format revision, settings and target
precision. Encode fields explicitly with checked sizes/byte order. Do not save
pointers, native struct bytes or runtime handles. A digest accelerates cache
lookup, but the revisioned format defines compatibility.

Automatic convex decomposition is an optional offline authoring aid. The Gems
review adds bounded hull count, vertices per hull, recursion/work and reproducible
settings to its cook contract. Show the proxy over the render asset and validate
passages, holes and support surfaces in meters. A volume-error metric does not
bound local clearance or prevent a blocked doorway. Accept artist-authored
convex proxies first; select a maintained decomposition tool only after this
quality/cost comparison, rather than adopting historical V-HACD by default.

The Gems review adds adjacency and internal-edge classification to mesh cooking.
Suppress only features proven safe under the chosen one/two-sided surface policy;
retain boundaries, creases and material/filter discontinuities. Nonmanifold edges
need an explicit reject/conservative policy. Tile borders require compatible
adjacency/halo data; otherwise preserve boundary features and test the seam.
Never delete gameplay geometry to improve contact counts.

Mass is in kg; local inertia is in kg m² about the center of mass (COM).
Store `BodyFromCOM` separately from the authored body frame. Integrate COM pose;
recover body pose for gameplay and rendering. Anchor conversion must use the
same frame definition. This prevents asymmetric compounds from appearing to
rotate about their visual origin.

Compute primitive mass properties analytically and hull volume integrals in the
cooker using checked `float64`. Compose disjoint authored mass volumes with
rotated tensors and the parallel-axis theorem. Overlapping compound leaves
**double-count mass** if simply summed: require a separate mass proxy or an
explicitly documented additive-density choice. Colliders marked massless (such
as sensors) contribute no mass. Allow validated explicit mass/COM/inertia
replacement for art-directed bodies; reject nonpositive eigenvalues and nonfinite
or excessively ill-conditioned tensors. Dynamic planes/open meshes are invalid
mass sources. Fixed axes are explicit degrees of freedom, not enormous masses.

These contracts adopt mass-frame clarity from Eric Lengyel's chapter 14,
*Game Engine Gems 1*, and the transformation concerns in Gems 7; see the
[verified review](physics-systems-gems-review.md). A comparison engine's mass
properties are evidence only after conversion to this contract. Shape replacement
invalidates affected contacts, recomputes/validates mass, and wakes dependents.
Default replacement preserves body-frame pose and COM velocity values;
it does not claim momentum conservation. An explicit momentum-preserving mode
is a later separately tested feature.

## Units, precision and simulation clock

Use meters, seconds, kilograms, radians, Y-up and the right-handed Math convention.
World positions and query origins use `float64`; local geometry, orientations,
velocities, inertia and solver data use `float32` initially. Compute narrowphase
relative to a nearby pair/query origin before narrowing. Reject values outside
the declared coordinate/size envelope; doubles alone do not make enormous shapes
or tiny tolerances valid. Define the conversion from `float64` positions to
pair-local `float32` values in one place, then establish the supported envelope
with tests. Do not promise arbitrary planet-scale physics.

One world has a fixed tick duration, initially 1/60 s as a tuning starting point.
`Step` advances one tick, not a frame delta. The game owns accumulation, input
sampling, catch-up limits, pause and interpolation. Local games may explicitly
discard excess debt and record it; authoritative replay/network simulation must
not silently discard ticks. Worlds can tick independently with explicit ownership.

Substeps subdivide that tick. Hold continuous force commands across substeps;
apply an impulse once. Changing substep count changes integration and solver
behavior, so it is versioned replay configuration. State what each substep
recomputes. The baseline detects collisions once per tick, then updates contact
separation, integrates and solves in every substep. Do not equate collision
passes with solver substeps. Compare fixed substep/iteration settings at equal
cost. Soft constraints plus smaller steps are the selected starting point, not
proof that a 2D result transfers to 3D.

Keep previous/current body poses for presentation interpolation. Teleports,
origin shifts and newly created bodies reset interpolation. Render frequency,
visual LOD, debug overlays and hidden windows must not alter accepted tick inputs.

## World phases, commands and failure contracts

```text
Idle -> Commit -> ReadPreStep -> Simulate -> Publish -> ReadPostStep -> Idle
                                    \-> Faulted
```

Mutation and command admission happen on the owner thread. Readers may run in
parallel within a read phase. The owner closes admission and joins them before
commit, simulation or destruction. A view is a protected borrow, not a copied
pointer plus epoch; epoch checking alone cannot prevent use-after-free. No
mutation or query against the live mutable world during `Simulate` in the initial
API. A later immutable query snapshot requires its own storage/lifetime budget.
Tick-wide admission/preflight precedes `Commit`; it must reserve the resources
needed by the staged changes and admitted simulation bounds before applying
tick commands. Commit itself cannot discover an ordinary capacity failure after
partial publication. Unexpected numerical failure closes the world as described
below rather than leaving it apparently ready for another tick.

The owned step orders detection before mutation. Broadphase and narrowphase
read committed state and write only step scratch. The step then counts the
contacts, rows and islands it found and compares them with the reserved maxima.
Exhaustion at that point changes no pose or velocity. This is the behavior the
Jolt probe could not provide. The world stays in its committed pre-step state,
and the game can remove load or recreate the world with larger maxima.

Commands carry target tick, producer ID and producer-local sequence. The game
assigns deterministic producer IDs and merges by that key; thread arrival order
is not command order. Bodies created earlier in a tick may be used by later
ordered commands only after their creation results are resolved. Start with
synchronous creation at commit and ordinary value commands, rather than a complex
promise/future handle graph.

`TryCreate*`, `TryReplaceShape`, `TryEnqueue*` and configuration operations are
`[[nodiscard]]`, `noexcept`, and preserve outputs and installed state on failure.
A recorded command is an admission result, not proof its target remains valid
at application time; publish a command result keyed by sequence. Preflight a
structural batch, including solver storage and retirement records, before
publishing any change. Commands submitted for a closed/past tick fail explicitly.

| Result class | Required behavior |
| --- | --- |
| Invalid input/handle/phase, unsupported capability | No mutation, output unchanged; include offending field/operation |
| Admission or initialization capacity/allocation failure | No partial publication; preserve prior usable world and outputs |
| `Step` preflight failure | Tick does not advance, commands remain pending, last snapshot remains usable |
| Contact, row or island capacity exhausted after detection | Tick does not advance; no pose or velocity changes; commands already committed for the tick stay applied and are not applied again; accepted forces stay pending; last snapshot remains usable; report the exhausted table and its required count |
| Successful step, complete outputs | Tick advances exactly once; accepted forces consumed; output epoch advances |
| Successful step, incomplete events/debug output | Tick advances; report missing output explicitly; mandatory gameplay events follow the stricter rule below |
| Nonfinite result, exhausted CCD budget without a conservative stop, or mandatory-output failure after advancement starts | Enter `Faulted`; state may be partially advanced, no success tick/event publication, normal queries/steps rejected |

A faulted world permits diagnostic extraction and explicit destruction/rebuild
from a last known complete checkpoint. Do not call it rolled back or retry the
same tick on partial state. Atomic rollback requires a separately budgeted full
checkpoint including contact/solver caches. Snapshot publication and draining
commands occur only after the success barrier. Never hide lost contacts behind
an ordinary success result. Assertions catch programmer invariants; public bad
inputs and bounded exhaustion remain explicit results.

The first API consists of small descriptors and out-parameters, scoped read
views, commands, `StepResult`, and event spans. Suggested vocabulary:
`TryCreateBody`, `TryCreateCollider`, `TryCreateConstraint`, `TryAcquireReadView`,
`RaycastClosest`, `CastConvexClosest`, `Overlap`, `TryEnqueueForce`,
`TryEnqueueImpulse`, `TrySetKinematicTarget`, `Step`, `GetDiagnostics`.
These are proposed contracts, not available SDK symbols. Use Ludus aliases,
explicit includes, private implementations and Doxygen at every public
declaration when implementing; do not expose allocating STL templates or heavy
headers. Split validation, conversion, traversal and collection into named,
readable routines instead of one templated universal query function.

## Bodies, forces and materials

| Motion type | Contract |
| --- | --- |
| Static | No integration; broadphase pose changes only through explicit barrier commands, with wake/cache updates |
| Kinematic | Tick-addressed body-frame target or explicit velocity; infinite solver mass, pushes dynamic bodies, no force response |
| Dynamic | Finite positive mass/inertia; force integration, collision/constraint response, sleep and selective CCD |

Do not resolve static-static, static-kinematic or kinematic-kinematic solid
contacts in the baseline. Sensor overlap is independent of that response rule.
A kinematic target computes velocities over the tick, including a validated
shortest-arc rotation; preserve explicit turns as angular velocity commands.
Teleport is discontinuous, clears affected caches and has a game-specified
initial-overlap policy. It is not a fast kinematic move or a swept hit.

Linear velocity is COM velocity in m/s; angular velocity is world radians/s.
Force is N, torque N m, linear impulse N s, angular impulse N m s. A force at
world point p adds torque `(p - COM) cross F`; an impulse uses the equivalent
angular impulse. Body-local force/point commands are converted at application
pose. Forces persist for one outer tick and are cleared only after success.
Persistent gravity/drag/buoyancy fields are explicit passes writing those inputs,
not world-owned callbacks invoked unpredictably from solver jobs.

Wake on accepted nonzero force/impulse, kinematic motion touching a sleeper,
constraint/mass/material/filter/shape changes, support removal and gravity
changes. A per-command wake policy may be explicit for special gameplay, but
must not silently lose a force applied to a sleeping body. Clamp damping and
speed only through declared game/solver configuration, and count clamping.
Preserve gyroscopic behavior where supported and tested; fast asymmetric tops
are a regression scene rather than an assumed stable free-spin case.

Materials specify dimensionless friction and restitution plus optional surface
velocity. Initially use one Coulomb coefficient for static/sliding friction;
rolling/spinning friction and anisotropy are later capabilities. Default mixing
is geometric-mean friction and maximum restitution, with named pair overrides.
An authored zero-friction surface therefore stays slippery. Restitution applies
only above a documented approach-speed threshold, so resting contacts do not
bounce on every correction. The solver implements this mixing; a comparison
engine is configured to match it before results are compared. Contact
modification, if later needed for one-way surfaces, is a pure bounded policy over immutable data;
it cannot allocate, mutate game state or run arbitrary gameplay code.

## Collision pipeline and contact robustness

Ludus owns the algorithms and data layout. The following is the baseline
pipeline with its observable acceptance requirements. Its order is versioned
replay configuration.

1. Commit admitted commands, retain collision coverage, update dirty proxies.
2. Generate candidate pairs from fat AABBs and conservative layer filters, using
   committed poses and velocities.
3. Traverse cooked static geometry, run specialized primitive tests or the
   general convex algorithm, generate and reduce contact patches into scratch.
4. Match persistent contact features, invalidate incompatible caches, build
   dynamic islands and prepare contact/joint rows. Check contact, row and island
   counts against the reserved maxima here, before any body state changes.
5. In each substep: integrate velocities, warm start, solve with friction and
   limits, integrate poses and relax. Then apply restitution, run the selected
   CCD path and evaluate sleep.
6. Join jobs; validate completion; publish body changes, events and diagnostics.

Start with a dynamic AABB hierarchy for moving proxies plus cooked mesh/terrain
BVHs. The editor preview's triangle hierarchy is the starting point for the
static side. Sleeping bodies remain
queryable. Broadphase layers coarse-cull conservatively; final pair filtering
checks both masks, enabled flags, same-body exclusion and joint pair suppression.
No rendered mesh scan, all-pairs production loop or camera-driven physics culling.
An all-pairs test oracle verifies that filters/tree updates introduce no misses.
Measure tree quality, proxy updates, candidates and accepted contacts before
adding grids, sweep-and-prune, compressed nodes or alternative trees.

For general convex tests, implement one support-mapping path plus measured
specialized kernels. GJK distance with EPA penetration is one conventional option;
MPR from XenoCollide is another overlap/penetration option. They are not automatic
replacements for each other, and an overlap boolean is not a distance/manifold
contract. Require bounded iterations, feature ties, zero directions, deep overlap,
degeneracy and explicit numerical failure handling. Choose the general path at
P1, where convex casts and overlaps first need it, and do not ship two.
P1 chose GJK: the [convex cast](physics-collision.md#closest-convex-cast) is a
GJK ray cast on polytope cores with spheres and capsules as rounded cores, and
overlap queries are to reuse the same support and simplex code. EPA
penetration depth remains a separately qualified option.

The Gems review strengthens persistence: key contacts by full collider
incarnations, geometry revision, leaf IDs and feature identity, not memory
addresses. A hash collision never establishes equality. Reduction
retains depth and useful support area within each compatible normal/material
patch; do not average opposite normals, merge different gameplay surfaces or
keep only the deepest point of a box on a floor. Point/patch capacities are
documented limits of the implementation. Four points per ordinary convex patch is
a reasonable evaluation case, not a global four-contact cap for concave geometry.
Record generated/reduced counts and cache hit/invalidation reasons.

Match warm-start impulses only while geometry, normals, material policy and
step settings remain compatible. Project/clamp tangent impulses to the current
friction basis and bound before use. Changing tick configuration clears caches.
Internal triangle edges and terrain seams need a sliding-capsule regression;
this checks the cooked topology, not just solver iterations. Keep tolerances
separate: geometric degeneracy, contact offset, allowed penetration, restitution
threshold and query epsilon have different units and purposes.

## Constraints and solver tuning

Expose validated descriptors for fixed, distance, ball/socket, hinge and slider
constraints first. Add swing/twist and a constrained six-DOF joint for ragdolls
only after that workload is requested. Endpoint B may explicitly be the world;
other endpoints must be distinct compatible live bodies in the same world.
Store anchors/axes in named body-local frames. Normalize/reject axes once on
creation, and convert to COM-local solver frames explicitly.

The Gems review adds a row-oriented diagnostic model. An internal scalar row
has endpoint Jacobians, effective mass, target velocity/bias, softness, impulse
bounds and accumulated impulse. Contact normal rows use nonnegative impulses;
friction bounds depend on the current normal impulse; motor bounds derive from
maximum force/torque times the substep duration. These rows are the solver's
internal model and its diagnostic output, not a public unrestricted row
callback. Inspect residuals, saturation and anchor error per joint.

Use frequency in Hz and dimensionless damping ratio for spring-like tuning,
with explicit conversion to solver coefficients and limits relative to substep
duration. Keep
hard limits distinct from motor targets. Motors cannot silently have infinite
strength; zero force disables them. A break threshold is force/torque inferred
from impulse over the actual solver duration, with an explicit aggregation rule:
maximum sampled row force magnitude over the tick, separately for linear and
angular rows. This is an approximation to peak physical load and needs substep-
sensitivity tests. Publish one break event and remove at the next commit barrier.

For joint orientation, use a documented relative quaternion convention and
swing/twist reference axis, not independent Euler clamps. `q` and `-q` represent
identical limits. Near 180-degree swing the twist decomposition is ambiguous;
choose a stable convention, record it, and test continuity around the
singularity. Limit visualization uses the same convention. More anatomical
quaternion-volume limits remain a later feature; independent swing/twist limits
do not reproduce every shoulder pose.

The baseline is the warm-started sequential-impulse solver with soft
constraints, substeps and relaxation named in the
[selection revision](#selection-revision). Compare substep and iteration
settings at equal time and quality. Bound correction speeds and restitution;
avoid fixing jitter by increasing all iteration counts. Mass ratios, redundant
constraints and long chains are explicit stress cases. A position-based rigid
solver is the first comparison candidate and lives in a
[sibling module](#simulation-siblings-and-research-solvers); it does not replace
this baseline without the adoption evidence the review requires. Direct sparse
solves, shock propagation and GPU solvers need a specific workload and
demonstrated benefit.

## Sleeping and continuous collision

Sleep is an island decision respecting dynamic contact/joint connectivity.
Sleeping skips integration/solving, not queries or wake dependencies. Keep
kinematic contacts and support changes able to wake the affected island.
A sensor alone must not indefinitely prevent unrelated dynamic bodies sleeping.

The Gems review adds motion-envelope tests to the sleep suite: sample COM and two
noncollinear body-fixed offsets across a dwell interval, retaining their world
AABBs. Compare position-envelope thresholds in meters and orientation motion
with a declared offset length. Use size-aware offsets with bounded limits;
reset on wake, geometry change or envelope escape. This tolerates small jitter
while detecting rotation about COM. Dwell is seconds, not frames. Start with a
velocity-threshold island rule, compare it with this envelope policy, and
retain one algorithm.
Large full revolutions between samples can alias the envelope, so also veto
sleep for high angular velocity, driven motors or unresolved motion. This is
Ludus's additional guard, not a guarantee supplied by the historical chapter.

Discrete collision is the default. Opt in fast/thin gameplay objects to a
qualified sweep/TOI CCD mode; use swept query projectiles when gameplay only
needs hit events. Bound passes and report exhaustion. At an exhausted TOI budget,
stop remaining risky motion conservatively and record it. A mode that cannot
stop conservatively faults the step. Never silently switch to
discrete motion and call it continuous collision.

Speculative contacts can address some rotational motion but may create ghost
contacts. Linear convex casts alone do not cover fast spinning blades or two
moving bodies. Document the admitted angular-motion and dynamic-dynamic CCD
matrix; reject `Unsupported` modes and test thin walls, opposing fast bodies,
initial overlap, repeated impacts, and angular tunneling. CCD against triggers
is separate from solid response. Teleport has no traversed trigger events.

## Phantoms, filtering and events

Use participation flags `Solid`, `Sensor` and `Query`, with `Solid` and `Sensor`
mutually exclusive on one collider. Query participation is independent.
A phantom is a sensor volume plus a bounded overlap set; it has no response
impulse. Reuse geometry and narrowphase rather than a phantom class hierarchy.
Standalone moving sensors update their poses through commit commands.

Simulation pair filtering is symmetric: each collider must accept the other's
category. Query filtering is directed: a query mask accepts target categories,
with explicit ignored body/collider sets, enabled state and sensor inclusion.
Make these separate descriptors; do not force bullets, AI and editor picking
through one gameplay collision matrix. Filters are immutable within a step;
changed filters generate reasoned exits/enters at the next boundary.

Sensor pairs include static and kinematic targets when enabled, even though those
pairs produce no solid response. Solid contact generation skips those pairs, so
sensors use a bounded overlap pass over the same narrowphase. Exact narrowphase
overlap determines occupancy; broadphase
AABB overlap is only a candidate. Aggregate leaf overlaps by collider pair with
counts, so changing compound leaves does not emit false exit/enter sequences.

Publish tick-end `Begin`, optional `Stay`, and `End`, and separately optional
solid contact/impact, constraint break, wake/sleep, and command-result events.
Initial occupancy produces `Begin` on the first completed tick. Final occupancy
controls these events; crossing a thin volume between endpoints is a separate
opt-in swept sensor query with an explicit hit fraction. Reentrant callbacks
from solver threads never run gameplay. Worker listeners write bounded POD
records only; merge/sort by stable collider-pair/constraint identities and event
kind before publication. Subscription selects costly contact detail; enabling
it must not change simulation results.

Required gameplay sensor pairs/events must be provisioned from admitted pair
bounds or fail the tick before success publication. Optional impact/debug streams
may truncate with counts and `Incomplete`; they cannot be used as the sole
source of gameplay occupancy. A bounded `GetSensorOverlaps` read exposes current
complete occupancy for resynchronization. That does not recover a lost one-tick
crossing or impact, so consumers requiring every such event need the mandatory
capacity path. End reasons include separation, filtering, disabling, destruction
and replacement. Preserve a copied consumer binding and incarnation identity
for destruction events. The game decides trigger priority and effects.

## Ray, shape and overlap queries

All queries operate against a protected world epoch. They never load content,
allocate, wait on asset I/O, or implicitly run simulation. Batched queries share
one read phase; results name its tick, geometry revision and collider identity.
Use the public API for AI, audio, cameras and editor picking, while render-only
triangles belong to a separate picking service with an explicitly different
coverage policy. Missing terrain coverage is `NotResident`, never a proven miss.

| Operation | Input and output contract |
| --- | --- |
| Raycast | Finite origin, normalized direction, explicit finite max distance in meters; hit distance/position, surface normal, collider/body, leaf/triangle and material |
| Convex cast | One retained convex shape, rigid starting pose and translation delta; no rotation in the baseline; hit fraction in [0,1], distance and witness data |
| Shape overlap | Retained shape/pose plus filters; exact intersecting collider/leaf identities; penetration detail is a separate qualified option |
| Broadphase AABB query | Candidate set only; never label it exact collision/overlap |

A zero-length ray is `InvalidArgument`; a zero-translation cast performs only
its selected initial-overlap test. Queries specify `Any`, `Closest`, or `All`.
`Any` returns any acceptable hit and may stop early; it promises no nearest or
stable target. `Closest` is nearest among all admitted candidates, ties ordered
by collider incarnation then leaf/feature ID. Traversal may prune only with a
conservative bound proving it cannot hide a better hit. Finite values and strict
distance ordering precede identity ties; avoid nontransitive epsilon comparators.

`All` writes a caller span and returns written count and completion. Overflow
means an incomplete subset, not the nearest N and not a miss. Sort written
results by distance/fraction then identity when ordered results were requested;
a sorted incomplete subset still is not globally nearest. Offer a separate
bounded nearest-N collector later if needed. `Closest` with exhausted traversal
work returns incomplete rather than a certified closest. `Any` can certify a
hit once found, but certifies a miss only after complete traversal. Invalid-input
and stale-view failures preserve outputs; successful misses carry explicit empty
result/count semantics. Work exhaustion or buffer overflow may publish partial
outputs only with the corresponding incomplete status.

Initial-overlap policy is explicit: report fraction zero, ignore initially
intersecting colliders for that entire cast, or reject the query. With the report
policy, expose `StartedOverlapping`; normal/witness validity flags handle cases
without a unique normal. A ray starting inside a closed shape reports distance
zero under report policy, with no fabricated surface normal. Request a separate
exit-surface query if that is needed. Mesh front/back-face and one/two-sided
policies are explicit. Normal means geometric surface normal for a ray; cast
normal points from target toward swept shape; orient or flag ambiguous results.
Optional normal-against-ray presentation is a named conversion.

Meshes are tested in local space; rigid rotation preserves ray length and
fraction. With scale baked into assets, no accidental normalization changes
world distances. Use conservative interval bounds and deterministic edge/tangent
handling; normal transforms use the correct frame. A query is not a dynamic-body
CCD integrator. Revalidate hit handles before deferred commands, since another
tick may destroy or replace the target. Closest-point/distance queries are useful
later but need distinct witness, signed-distance and degeneracy contracts.

## Performance, debugging and acceptance

Preallocate world tables, command/result buffers, broadphase nodes, candidate
pairs, manifolds, rows, islands, jobs, mandatory events and step scratch from
explicit admitted maxima. Use checked products and account for retirements,
sleeping bodies and retained shape revisions. Initialization/cooking may allocate
fallibly; active ticks/queries do not grow or fall back to heap allocation.
Never confuse no allocation with bounded worst-case pair work: dense overlapping
objects can still create quadratic candidates. Admission limits and stress tests
must cover that case. Record capacity high-water marks and every incomplete/fault
reason. Required bounds are established at each gate, not measured in this design.

Keep cold descriptors and hot motion/contact arrays separate. The public debug
snapshot is readable AoS; private SoA/SIMD is acceptable when it wins
measured workloads. First use one owner and serial execution for small scenes;
add FoundationThreading jobs after serial correctness.
No second worker pool or blocking wait from one of its exhausted workers.
Parallel islands and collision tasks require deterministic insertion/merge rules;
a giant connected pile needs separate scaling evidence. SIMD flags and reduction
order are part of replay configuration. Compare one/many workers, desktop/mobile
class CPUs, and admitted browser builds; do not infer speed from core count.

Expose debug extraction as plain bounded values: wire shapes, AABBs, COM/body
frames, collider leaf IDs, contact normals/depth/impulses, friction tangents,
joint anchors/limits/errors/motor saturation, CCD sweeps and sleep envelopes.
Display active/sleeping, filter decisions, missing coverage, query epoch and
fault reasons. An inspector edits descriptors through the same command path as
gameplay, with undo replaying authored values. Pause, step-one-tick, isolate a
pair/island, disable presentation, and record/replay a failing tick are required
investigation tools. Logging is rate limited; per-contact logs are not the
primary debugger.

Record world settings, engine build fingerprint, solver revision, cooked content
digests, ordered commands and tick hashes. Checkpoints for exact same-build continuation
must include contact and solver caches, sleep and constraint state with a format revision;
transforms/velocities alone are a portable rebuild, not exact rollback. Persistent
save files use authored IDs and reconstructed bindings. Determinism is initially
same-build/same-configuration replay proved by tests. Cross-platform bitwise
lockstep, rollback across solver revisions, and deterministic execution across
worker counts are separate qualified claims. Network snapshots/correction can
be added without claiming lockstep. Remap process-local world handles to stable
replay creation ordinals when hashing; compare named fields, not padding bytes.

| Suite | Required evidence |
| --- | --- |
| Geometry and queries | Analytic primitive answers; all-pairs traversal oracle; tangent/edge/inside/zero/invalid cases; reversed pairs; mesh seams/holes; capacity and completion semantics |
| Body/mass | Off-center impulse, asymmetric compound COM, rotated inertia, overlapping mass proxies, fixed axes, invalid tensor, shape replacement |
| Dynamics | Falling/bouncing/friction, inclined rest, box stacks, large mass ratio, long chains, motor saturation/breaking, fast asymmetric spin, energy/drift diagnostics |
| Sleep | Jittering rest, slow translation, pure rotation about each local axis, support removal, moving platform, motor/force wake, full-turn alias guard |
| CCD/sensors | Thin wall and trigger, dynamic-dynamic motion, spinning thin body, start overlap, exhausted passes, leaf aggregation, destruction/filter exits, event overflow |
| Ownership/failure | Stale/cross-world handles, nonwrapping retirement, command rejection, readers versus destruction, allocation injection, contact/row/scratch exhaustion, faulted recovery |
| Replay/concurrency | Same input at differing presentation rates, checkpoint continuation, ordered producers, one/many-worker qualification, reload without retained pointers |
| Terrain/SDK/platform | Physical coverage before teleport, retained revisions, tiled adjacency, native and Emscripten capability matrix, installed public SDK consumer |
| Research use | Headless stepping from an SDK consumer, plain-value state and contact export every tick, shared scene source files, a sibling module built against the installed SDK |

Benchmarks include sparse broadphase, mostly sleeping static scenery, a dense
awake pile, long constrained chains, mixed streaming terrain, and incoherent ray/
convex queries. Report p50/p95/p99 step and query times, maximum hitch, stage
breakdown, memory, allocations, candidate/manifold/row counts, cache retention,
active fraction, and numerical quality. Fix content, seed, hardware, compiler,
worker count, precision, timestep, substeps and configuration. Tune budgets to
an actual game's frame allocation before the shipping gate; no invented AAA
millisecond or object-count promise. Record gains and regressions at matched
quality, including the serial path and debug-capture overhead.

## Delivery gates

| Gate | Reviewable output and acceptance |
| --- | --- |
| P0: backend feasibility | Closed without a qualified engine. Stock Jolt 5.6.0, ODE 0.16.6 and Box3D failed the allocation gate on macOS arm64, and six further engines failed the source screen. The outcome is the [selection revision](#selection-revision). The native/web matrix, SDK and replay evidence were not collected and move to the gates below |
| P1: collision and query world | In progress: the [static triangle mesh and closest queries](physics-collision.md), sphere, box, capsule and convex hull shapes and the general GJK convex cast have landed, and Explore as Player sweeps against them with **Static Collider** as authored input. `PhysicsCollision`: shapes, identities, immutable cooking, geometry store, colliders, filters and protected ray, cast and overlap queries; brute-force correctness oracle; allocation injection at every creation path; failure contracts and query overlays; installed SDK consumer on native and Emscripten |
| P2: rigid motion | `PhysicsRigid`: static/kinematic/dynamic integration, mass frames, forces/materials, convex and mesh contacts, the soft-constraint substep solver and selective CCD; stack/friction/spin/replay scenes; zero-growth active path; contact capacity rejected before any pose changes; headless stepping and plain-value state export; comparison with a pinned test-only engine on shared scenes |
| P3: sensors and joints | Complete occupancy/events, distance/ball/hinge/slider, motor/break/sleep diagnostics; lifecycle, capacity and singularity regressions |
| P4: engine integration | GameWorld binding, terrain revision/coverage installation, content cooking, inspector/recording, SDK and hot-reload boundaries; one sibling simulation built against the installed SDK |
| P5: workload qualification | Representative performance and quality budgets, job/SIMD changes with evidence, supported target matrix and production failure behavior; a comparison solver sibling only if a workload exposes a baseline limit |

P1-P3 implement the Ludus-owned kernels in dependency order. Each gate ships a
small, readable, serial implementation with its tests before any optimization.
Character controllers, ragdolls, vehicles,
fluids and advanced solvers follow only when a concrete consumer needs them.
Every implementation gate includes public Doxygen, canonical usage docs,
warning-clean pinned builds/tests, format/tidy, ASan/UBSan and SDK consumption;
web gates use Ludus's separately pinned Emscripten toolchain. This documentation
change does not claim those runtime validations have already occurred.

## Research evidence and adopted revisions

The [Gems review](physics-systems-gems-review.md) records verified page ranges,
authors, decisions adopted and useful deferred leads. It changes the starting
design by requiring mass-frame/overlap contracts, persistent compatible contact
patches, mesh-edge cooking, jitter/rotation sleep tests, quaternion-limit
singularity tests, and explicit static/dynamic query coverage. Numerical solver
choices remain workload-qualified rather than dictated by historical timing.

Thanks to Miles Macklin, Kier Storey, Michelle Lu, Pierre Terdiman,
Nuttapong Chentanez, Stefan Jeschke and Matthias Müller,
[Small Steps in Physics Simulation](https://matthias-research.github.io/pages/publications/smallsteps.pdf),
*SCA 2019*, for evidence supporting evaluation of smaller substeps in compliant
constraint simulation. It motivates an experiment, not a proof of our
implementation's 3D collision cost or a prescribed substep count. Thanks to Erin Catto,
[Iterative Dynamics with Temporal Coherence](https://box2d.org/files/ErinCatto_IterativeDynamics_GDC2005.pdf),
*GDC 2005*, for contact caching and accumulated impulses. Thanks to the Box2D
maintainers, [Simulation](https://box2d.org/documentation/md_simulation.html),
for handle-based storage, fixed steps, and post-step event consumption. Ludus's
stronger owner/generation, capacity, and fault contracts remain our decisions.

The PhysX team's [Advanced Collision Detection](https://nvidia-omniverse.github.io/PhysX/physx/5.5.1/docs/AdvancedCollisionDetection.html)
distinguishes swept and speculative CCD. This informs our capability tests;
it does not make one implementation's CCD modes available in another.
Recheck upstream source at the pinned revision before using an engine as a
test-only comparison.

The [conference, journal and reading plan](physics-systems-gems-review.md#conferences-journals-and-reading-plan)
maps production and research sources to these gates, with initial Jolt,
multiphysics and AVBD readings, experiments and adoption criteria. It extends the
research queue without changing the single owned CPU baseline decision.
