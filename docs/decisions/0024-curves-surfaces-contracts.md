# ADR 0024: Curves and surfaces domains, statuses and numeric guarantees

Status: Accepted for G0. Later phases implement these contracts; changing one
requires amending this record. Amended in G3: sweep mesh generation has its own
public header, `sweep.hpp` (see the boundary table).

## Context

The [curves and surfaces architecture](../architecture/curves-surfaces.md)
proposes prepared paths, tracks and patches above the implemented G1 cubic and
bicubic kernel in FoundationMath. Its G0 gate requires frozen domains,
status and tolerance vocabularies, a representative fixture set, agreement on
which results are guarantees and which are estimates, and an audit of header,
dependency and format boundaries before G2 creates owners. The architecture
keeps the rationale; this record fixes the names, rules and classes that G2-G5
code and tests must use.

## Decision

### Domains

| Value | Meaning | In memory | Range and unit | Encoded field |
| --- | --- | --- | --- | --- |
| `SegmentParameter` | Span index and local curve parameter | `usize SpanIndex`, `float64 U` | U in [0,1], dimensionless | `uint32`, `float64` |
| `SurfaceParameter` | Patch index and local patch parameters | `usize PatchIndex`, `float64 U`, `float64 V` | [0,1]^2, dimensionless | `uint32`, 2 x `float64` |
| `PathDistance` | Prefix arc length from the path start | `float64` | Declared asset units, [0, total length] | `float64` |
| `TrackTime` | Seconds from the track's local epoch | `float64` | Seconds, [first key, last key] | `float64` |

These are distinct types with no implicit conversion. Controls are `float32`
relative to a `float64` chunk origin; sources may store `float64` coordinates.
Distance is measured in the asset's local metric (see the architecture for
transform rules). A surface parameter is never a texture coordinate.

**Span selection.** An exact interior key or span boundary selects the outgoing
span at u = 0. The terminal endpoint of an open domain selects the last span at
u = 1. `SpanSide::Incoming` must be requested explicitly and selects the
previous span at u = 1; at the first endpoint there is no incoming span, so
both sides select the first span. A Step span holds its left key's value for u
in [0,1) and the right key's value only at u = 1.

**Derivative units.** Kernel derivatives are per normalized u. For a span of
domain width h, the domain derivative is `(dP/du)/h` and the second derivative
`(d2P/du2)/(h*h)`. Track derivatives are value units per second. Surface
partials are per normalized U and V.

**Boundary policies.** They apply only outside the closed domain
[start, end] of duration D = end - start; inside it every policy evaluates the
same value.

| Policy | Valid for | Outside the domain |
| --- | --- | --- |
| `Reject` | All | `InvalidDomain`; outputs unchanged |
| `Clamp` | All | Evaluate the nearest endpoint; the derivative is the one-sided derivative from inside the domain |
| `Hold` | Tracks | Nearest endpoint value with zero derivatives |
| `Loop` | Tracks | Evaluate start + mod(x - start, D), which lies in [start, end) |
| `PingPong` | Tracks | Reflect with period 2D; the reflected half negates the first derivative |
| `Wrap` | Closed paths | As `Loop`, over parameter or distance; an open path rejects it as `InvalidArgument` |

No policy extrapolates. A `Loop` of a nonperiodic track jumps at its seam; that
is authored behavior, not a runtime repair.

### Status vocabulary

FoundationMath keeps `MathStatus`. Preparation, sampling and queries above it
return a `GeometryStatus` with exactly these enumerators:

| Enumerator | Meaning |
| --- | --- |
| `Success` | Outputs are written and meaningful |
| `NonFiniteInput` | A NaN or infinity where a finite value is required |
| `InvalidArgument` | A malformed request or profile: negative tolerance, unknown policy, `Wrap` on an open path |
| `InvalidDomain` | A parameter, time or distance outside its domain under `Reject`; nonincreasing keys; coincident consecutive waypoints |
| `InvalidTopology` | Unsupported or inconsistent adjacency, edge or corner records |
| `MissingCache` | The snapshot lacks the cache the request needs; sampling never builds it |
| `StaleRevision` | A cursor, view or cache revision does not match its snapshot |
| `Degenerate` | A requested direction or normal is undefined at that point; position may still be valid |
| `DegenerateFrame` | A framed profile cannot define a transported frame run under its policy |
| `IllConditioned` | Below a named conditioning threshold |
| `OutOfRange` | Finite inputs whose result cannot be represented |
| `UnsupportedRepresentation` | Valid source geometry this version cannot represent, such as rational or trimmed input |
| `UnsupportedPrecision` | The requested tolerance is below the arithmetic floor, or a strict profile needs a certificate that is unavailable |
| `BudgetExceeded` | A work, depth, iteration or output budget was exhausted |
| `CapacityExceeded` | A caller-owned output range is too small; detected before writing |
| `OutOfMemory` | A fallible allocation failed |
| `Cancelled` | The caller cancelled preparation |

A geometric miss is still `Success` with a result classification, as in
FoundationMath. `MathStatus` crosses the boundary unchanged by name, except
that an invalid kernel parameter becomes `InvalidDomain` and `SizeMismatch`
becomes `InvalidArgument`. Enumerator names are frozen; numeric values and
multi-failure precedence are not an ABI or file format, so serialized
diagnostics store names. Diagnostics carry the code, resource and source
element, property path, requested tolerance, achieved bound with its guarantee
class, and the offending range with its domain kind. Bad content is a status,
never an assertion.

### Guarantee classes

| Class | Meaning | Satisfies a strict profile |
| --- | --- | --- |
| Exact | Bitwise defined: stored endpoints and corners, decisions on stored values, exact identities | Yes |
| Rounded | One computation with float64 intermediates narrowed to float32; no error bound is promised | Not as a bound |
| Certified | An enclosure from reviewed outward-rounded arithmetic, validated with the pinned native and Wasm toolchains | Yes |
| Estimated | A heuristic, padded or sampled bound; reported with its class | No |
| Approximate | A separately named mode that returns its candidate and achieved residual | No |

A strict preparation profile accepts only Exact or Certified bounds; otherwise
it fails with `UnsupportedPrecision` and leaves the active snapshot unchanged.
Visual profiles may accept Estimated bounds and must report them. No outward
rounding helper exists yet, so strict profiles are unavailable until G2 adds
and validates one.

| Result | Phase | Class |
| --- | --- | --- |
| Kernel endpoint and corner positions | G1 | Exact |
| Kernel interior positions, derivatives, split controls, Hermite controls and normals | G1 | Rounded |
| `TryPatchNormal` Degenerate/IllConditioned decision | G1 | Exact for exactly zero partials; Rounded near a threshold |
| Hull of a span's stored controls, enclosing its exact curve | G2 | Exact |
| Hulls of subdivided leaves and transformed bounds | G2 | Certified with outward rounding, otherwise Estimated |
| Chord/control-polygon length interval and prefix uncertainty | G2 | Certified with outward accumulation, otherwise Estimated |
| Midpoint length estimate | G2 | Estimated, inside the interval |
| Distance inversion residual bound | G2 | Certified from Certified partial-span enclosures |
| Linear distance-table lookup | G2 | Approximate |
| Strict projection global distance gap | G3 | Certified |
| `ProjectPointApproximate` | G3 | Approximate |
| Transported frame angular error | G3 | Estimated until derivative enclosures are validated |
| Sweep self-intersection risk | G3 | Estimated; never a proof of simplicity |
| Control-net plus twist position error of a tessellation cell | G4 | Certified with outward rounding |
| Tessellation normal-angle error | G4 | Estimated unless derivative enclosures are Certified |
| Continuity classification | G2, G4 | Threshold decision on Rounded values, reported with its thresholds |
| Float32 position floor | G0 | Exact policy value |
| Performance and memory budgets | All | Unmeasured |

### Tolerance profile

| Field | Unit | Valid values | Use |
| --- | --- | --- | --- |
| `PositionError` | Asset units | Finite, > 0, at least the position floor | Absolute geometric error |
| `RelativePositionError` | Dimensionless | [0, 1) | Effective error is max(`PositionError`, relative x declared asset scale) |
| `DistanceError` | Asset units | Finite, > 0 | Whole-path prefix gap, not a per-leaf tolerance |
| `TangentAngleError` | Radians | (0, pi/2) | Frame and tangent accuracy |
| `ProjectionGap` | Asset units | Finite, > 0 | Global upper/lower distance gap |
| `ContinuityPosition` | Asset units | Finite, >= 0 | C0 classification |
| `ContinuityDerivative` | Dimensionless | [0, 1) | Relative difference of domain-scaled derivatives for C1/C2 |
| `ContinuityAngle` | Radians | [0, pi/2) | G1 direction classification |
| `NormalMinDerivativeLength` | Asset units per unit parameter | Finite, >= 0 | Same meaning as `PatchNormalPolicy` |
| `NormalMinSinAngle` | Dimensionless | [0, 1) | Same meaning as `PatchNormalPolicy` |
| `NormalAngleError` | Radians | (0, pi/2) | Tessellation normal accuracy |
| `CoincidenceDistance` | Asset units | Finite, > 0 | Source waypoint coincidence; editors merge visibly |

Work limits (sizes, leaves, depth, iterations, scratch and output bytes) are a
separate record with no engine defaults. Validation returns `NonFiniteInput`
or `InvalidArgument`; a request below the floor returns `UnsupportedPrecision`.

**Position floor.** For chunk-relative controls whose largest absolute
component is E > 0 (in the float32 normal range), the floor is
`2 * ulp32(E)`, with `ulp32(E) = 2^(floor(log2 E) - 23)`. Storing a control
rounds each component by at most half a unit; the convex hull keeps outputs
within E, so rounding a result adds at most another half. One unit per
component gives at most sqrt(3) units in length, below two. This is a
representability floor, not a promise that any computation achieves it. A
chunk whose extent cannot meet a requested error must be split or rejected.

### Representative fixture set

`tests/fixtures/curves_surfaces/generate_fixtures.py` generates
`reference_fixtures.hpp` from exact rational and 50-digit decimal arithmetic
without Ludus code; `test_fixtures.py` checks its oracles against closed forms
and exact identities and rejects a stale header. The header is test-only, is
not installed, and is reached through the `ludus_geometry_reference_fixtures`
target. Regenerate with `python3 tests/fixtures/curves_surfaces/generate_fixtures.py --write`
then `./scripts/check --format --fix`.

| Group | Fixtures | Main consumers |
| --- | --- | --- |
| Cubic spans | `line_uniform`, `line_ease`, `line_backtrack`, `cusp`, `parabola`, `inflection`, `twisted`, `vertical_start`, `arch` | G1 samples; G2 lengths, zero speed and inversion; G3 frames and projection ties |
| Patches | `plane`, `saddle`, `bulge`, `apex`, `edge_first`, `edge_second` | G1 samples and normals; G4 tessellation bounds and singular normals |
| Shared edge | `reversed_shared_edge` | G4 orientation mapping and corner identity |
| Waypoints | `centripetal_uneven`, `duplicate_waypoint` | G2 nonuniform Catmull-Rom conversion and rejection |
| Tracks | `mixed_track`, `duplicate_time` | G2 Step/Linear/Hermite, span sides, every boundary policy, overshoot |
| Precision | `far_origin` | G0/G2 origin subtraction and the position floor |

Each fixture's `Purpose` string names what it exercises. Adding fixtures is
routine. Changing an existing reference changes a frozen contract and requires
amending this record. The [evidence ledger](../architecture/math-evidence/curves-surfaces.md)
records the corpus checks and the G1 cross-check.

### Header, dependency and format boundaries

| Target | Public headers | May depend on |
| --- | --- | --- |
| `Ludus::FoundationMath` (implemented) | `cubic.hpp`, `patch.hpp` | Base only |
| `Ludus::FoundationGeometry` at `modules/foundation/geometry` (G2) | `status.hpp`, `path.hpp`, `track.hpp`, `distance.hpp`, `frame.hpp`, `surface.hpp`, `query.hpp`, `sweep.hpp`, `tessellation.hpp` | Base, Math, Containers |
| `Ludus::GeometryFormat` at `modules/content/geometry_format` (G5) | Versioned cooked record layouts, limits and constants | Base only (header-only INTERFACE target) |
| `Ludus::GeometryContent` at `modules/geometry/content` (G5) | Source codecs, validation, cooking, loading, publication | Content, FoundationGeometry, GeometryFormat |

G3 amendment: sweep planning and emission live in `sweep.hpp`, not in
`tessellation.hpp`. A framed path consumer then parses no mesh API, and the
patch tessellation header that G4 adds stays independent of path sweeps.

`status.hpp` includes only Base. No Geometry header is added to `core.h` or the
foundational PCH. No public Geometry header includes Content, RHI, Logging,
Threading or a heavy standard header; consumers log and profile. The cooked
format lives in its own target, so a runtime loader never links a cooker and a
cooker never links the renderer. That follows the repository rule for formats
shared by tools and runtime and refines the architecture's earlier placement of
the format inside GeometryContent.

Cooked fields are fixed-width little-endian values written field by field:
`uint32` indices and counts, `float32` controls, `float64` origins, domains and
tolerances. Never encode `usize`, pointers, native layouts, SIMD registers or
FoundationMath values. Readers validate count products, offsets, alignment,
overlap, enum values, monotone domains and finiteness before exposing a view.

## Consequences

G2 creates FoundationGeometry with these names and the fixture corpus as its
first regression input. Strict profiles stay unavailable until a Certified
helper exists. G1 behavior is unchanged; the audit documented its public
declarations and renamed the split outputs to `outLeft` and `outRight`, which
is source compatible.

Not decided here: default work limits, performance and memory budgets, GPU
profiles, rational, trimmed or subdivision representations, editor commands
and the cooked schema's concrete records. Each needs its own evidence in the
phase that introduces it.
