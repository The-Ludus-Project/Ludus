# Curves and surfaces evidence (G0-G4)

This ledger records the G0 contract freeze and reference corpus, the G1 cubic
and bicubic kernel, the G2 prepared paths, tracks and distance maps, the G3
frames, projection and sweep meshes, and the G4 patch topology and CPU surface
meshes. See the
[architecture](../curves-surfaces.md),
[ADR 0024](../../decisions/0024-curves-surfaces-contracts.md) and the
[consulted-source review](../curves-surfaces-gems-review.md).

## G4 patch topology and CPU surface meshes

Implementation date: 2026-10-17. Base: `c3de79d` (`develop` after G2; rebased onto `6615a2c` before review). Scope:
FoundationGeometry `surface.hpp` and `tessellation.hpp`, their implementation
and tests, a `WorkLimits::MaxPatches` field, and the SDK-consumer and Wasm probe
exercise. The G2 box helpers moved into a shared private
`internal/control_box.hpp` without changing their arithmetic. Usage is documented
in the [module guide](../../modules/foundation/geometry.md#surfaces). No frame,
projection, sweep, cooked format or editor binding is added, and no corpus
reference changed.

### Certified cell bound

For a cell with exact corners P00, P10, P01 and P11, the emitted triangle pair
T' differs from the patch S by at most three terms:

- **Control net.** |S - B|, where B is the bilinear interpolant of the corners.
  Degree-elevating B to bicubic puts its controls at the bilinear values
  ((3-i)(3-j) P00 + i(3-j) P10 + (3-i) j P01 + i j P11)/9. S - B is a bicubic
  with the control differences, so by convexity it is at most their largest
  length. Corner differences are exactly zero and are skipped.
- **Twist.** |B - T| for the triangles (00, 10, 11) and (00, 11, 01). On the
  first triangle (u >= v), B - T = -v(1 - u) w with w = P00 - P10 - P01 + P11.
  On the second it is -u(1 - v) w. Both factors are at most 1/4, at the centre,
  so the bound is `Length(w)/4`.
- **Emitted vertices.** |T - T'|. A triangle point is a convex combination of
  its vertices, so it moves by at most the largest vertex error.

Subpatch controls are enclosed by restricting each row to [v0, v1], then each
resulting column to [u0, u1], with the Ramshaw blossoms G2 already uses. Every
product, sum, square root and the 1/9 weight (enclosed by the neighbours of its
float64 rounding) uses FoundationMath's outward helpers. Each emitted position
rounds its enclosure's midpoint to float32. Its error bound is the outward length
of the largest per-axis distance from that value to the enclosure. The same
function produces the positions that are emitted, so every cell bound sees the
exact emitted values. The bound measures the parametric distance between S(u, v)
and T'(u, v), which is an upper bound of their Hausdorff distance.

### Gate evidence

| Gate | Test evidence |
| --- | --- |
| Reversed shared edges | `reversed_shared_edge`: `edge_first` U1 joins `edge_second` U0 with `IsReversed`. `TryMapEdgeParameter` maps every fixture t to (1, FirstV) and (0, SecondV) exactly. Both patches evaluate to the fixture position within 1e-5 (Rounded), and the endpoints match bitwise. The same curve without the declared reversal is `InvalidTopology`, with the 6.08-unit control gap reported as Rounded |
| Corner identity | The pair has 6 corner records for 8 patch corners. First U1V0 and second U0V1 share the record at (3, 0, 0); first U1V1 and second U0V0 share (3, 3, 0). Stable edge and corner IDs (owner = smallest patch ID and side) and the canonical curve are identical when the patch order is swapped. A side joined to another side of the same patch closes it with 3 edges and 2 corners |
| UV/normal seams | On the shared edge, both patches' render vertices name the same `PositionIndex` and copy the same position bitwise. Their parameters are (1, v) and (0, 1 - v), and at least one normal pair differs (the pair is not C1, `CrossDerivativeDifference` is sqrt 2 within 1e-12, and its orientation is inconsistent). Two halves of one bilinear saddle are reported C1 with difference 0 and consistent orientation |
| Twisted bilinear interiors | `saddle` at level 0 has a Certified bound within 1e-12 relative of the exact 9/4. The emitted diagonal passes z = 9/2 at the centre, where the patch is at 9/4, so the bound is attained. A tolerance of 2.24 with `MaxLevel = 0` is `BudgetExceeded`. At 0.01 the grid is (4, 4), the first level sum where (9/4)/2^(LevelU+LevelV) fits |
| Matching boundary samples | The shared edge's samples are emitted once, 2^L - 1 of them, and the total position count equals corners plus edge plus interior samples. Each fixture edge sample lies within its emitted position's Certified `ErrorBound` of the exact reference. In a three-patch chain, the planar third patch inherits the curved edge's v level through the second patch's opposite side, with its own LevelU staying 0 |
| Singular patches | `apex`: the whole u = 0 row has `Degenerate` normals and a zero normal; one triangle per cell along it collapses. `Report` emits and counts both; `Reject` fails with `Degenerate` and the offending parameter, keeping the previous mesh. Corpus normals reproduce `Degenerate`, `IllConditioned` and the regular normal through `TryEvaluateSurfaceNormal` |
| Strict/visual failure | Strict exhaustion of `MaxLevel` is `BudgetExceeded` with the achieved Certified bound (at least 9/16 at level 1). Visual without `IsCoarserMeshAccepted` also fails; with it the mesh keeps the (1, 1) grid and reports `IsWithinPositionError = false`. Strict with a normal bound is `UnsupportedPrecision` (Estimated only), and Strict with a coarser-mesh request is `InvalidArgument`. Cell-test, vertex, triangle and byte budgets, and invalid profiles, fail before output changes |

For every corpus patch, the level-0 bound matches the generator's
`InteriorBound` within 1e-12 relative. Where an analytic maximum exists, it is at
most the bound: `bulge` reports 3 against its true 27/16, which the emitted
triangles reproduce at the centre. Every mesh in the tests is also sampled 25
times per cell. The sampled distance between the patch and its triangles never
exceeds the cell bound plus 1e-5 for float32 evaluation. Halving the twist
factor makes the saddle and certificate checks fail. The allocation probe fails
every allocation of surface preparation and mesh building in turn. Each returns
`OutOfMemory` and keeps the active owner, and evaluation and edge mapping
allocate nothing.

### Numerical limits observed

Strict meshes with `MaxLevel = 12` on the loaded development host (load average
above 75), as workload observations rather than budgets:

| Fixture | Position error | Levels (u, v) | Vertices | Cell tests | Time |
| --- | --- | --- | --- | --- | --- |
| `plane` | 0.001 | (0, 0) | 4 | 1 | < 0.1 ms |
| `saddle` | 0.1 / 0.01 / 0.001 | (3, 2) / (4, 4) / (6, 6) | 45 / 289 / 4225 | 125 / 1021 / 16381 | 17 ms / 0.32 s / 7.0 s |
| `bulge` | 0.1 / 0.01 / 0.001 | (4, 3) / (5, 5) / (7, 6) | 153 / 1089 / 8385 | 509 / 4093 / 32765 | 0.14 s / 2.0 s / 11.8 s |
| `apex` | 0.1 / 0.01 / 0.001 | (0, 5) / (0, 8) / (0, 12) | 66 / 514 / 8194 | 125 / 1021 / 16381 | 8 ms / 0.14 s / 1.4 s |
| three-patch chain | 0.1 / 0.01 / 0.001 | (0, 5) each / (0, 8) each / (0, 12) each | 198 / 1542 / 24582 | 283 / 2299 / 36859 | 20 ms / 0.17 s / 2.9 s |

The ruled `apex` and `edge_*` patches are linear in u, so only v is refined.
Halving the tolerance costs about one level in each curved direction, as the
1/4-per-level twist and second-difference decay predicts. A cell test costs
roughly 0.1-0.4 ms: every control box is rebuilt with outward arithmetic, and
both refinement candidates are evaluated. Sharing row restrictions across a
column and caching vertex errors per grid point are measured follow-ups.

### Departures and limits

- Shared edges must match bitwise after narrowing. The architecture's
  canonical-boundary adjustment term is therefore exactly zero; joins within a
  distance threshold are rejected rather than snapped.
- `IsC1` compares cross-boundary derivative controls with
  `ContinuityDerivative`. G1 tangent-plane classification across an edge is not
  provided.
- Only rectangular dyadic grids are produced, and a direction is refined across
  a whole patch. Quadtree transitions, camera-dependent targets, displacement
  and animated controls are not implemented.
- The normal estimate samples five analytic normals per cell and skips singular
  ones. It is Estimated and is never accepted by a Strict profile.
- Collapsed triangles are detected only by bitwise-equal positions, not by area.
- The Certified claim, like G2's, depends on the CI `ludus_web_math_corpus`
  passing on the pinned Wasm toolchain. The probe's Geometry exercise now also
  builds the saddle mesh and checks its bound against 9/4.

### G4 validation record

Host: macOS 26 on arm64 with the pinned host tools (Clang/LLVM 18.1.8), preset
`macos-clang-development`, heavily loaded (load average 75-88). The local preset
does not treat warnings as errors; the build reported none, and CI enforces
warnings as errors.

| Check | Result |
| --- | --- |
| `ludus_foundation_geometry` and its tests | Built with no warnings |
| `ludus_foundation_geometry_tests` | Passed: 41 test cases, 108,899 assertions (18 new surface and tessellation cases) |
| `ludus_geometry_alloc_tests` | Passed: every surface-preparation and mesh-build allocation failure injected; evaluation allocates nothing |
| Mutation check | Halving the twist factor fails the tight saddle bound and the sampled certificate |
| Pinned clang-tidy on every changed translation unit | Passed after removing two swappable-parameter findings and documenting one suppression on `TryMapEdgeParameter` |
| Format, naming (`check_naming.py`), foundational includes, container policy and changed-line source policy | Passed against `c3de79d` |
| Docs index, Markdown links, strict MkDocs, docs regressions | Passed |
| API reference with pinned Doxygen | Passed: no new gaps |
| ASan/UBSan, full CTest, installed SDK consumer, build-time budget | Not run locally (the documented macOS ASan startup deadlock and host load); CI runs them |
| Wasm math probe | Not run locally because the host has no Emscripten SDK. The `webgpu-probe` workflow runs `ludus_web_math_corpus`, whose Geometry exercise now builds the saddle mesh; the Certified claim depends on that result |

The repository formatter (`scripts/check --format --fix`) deleted the last
member of a multiline designated initializer written inside a ternary. The
affected line was rewritten without a ternary, and the formatter defect is
tracked separately.

## G3 frames, projection and sweeps

Implementation date: 2026-10-17. Base: `c3de79d` (`develop`). Scope: `frame.hpp`,
`query.hpp` and `sweep.hpp` in `Ludus::FoundationGeometry`, a private float64
span and Certified box helper, an ADR 0024 amendment that gives sweeps their own
header, new frame references in the G0 corpus, tests and the SDK consumer. Usage
is documented in the [module guide](../../modules/foundation/geometry.md#transported-frames).
No surface, patch mesh, cursor, cooked format or editor binding is added.

### Reference corpus additions

The generator gained `FRAME_FIXTURES`; no existing reference changed. Each frame
fixture is a unit-width cubic path whose joints are exactly C1. The generator
integrates Bishop's rotation-minimizing transport `r' = -(r . T') T` with the
classical Runge-Kutta rule in 50-digit decimals, at 1024 and 2048 steps per span,
and combines them by Richardson extrapolation. Its self-tests check
step-halving agreement within 1e-13, unit length and tangent orthogonality
within 1e-15, the planar closed form (the out-of-plane normal stays exactly +Z),
the arctangent against pi/4, pi/3 and -3pi/4, and a nonzero loop holonomy.
These references are integrated, not closed form; their error is far below the
1e-3 rad frame tolerance used by the C++ tests.

| Fixture | Exercises |
| --- | --- |
| `twisted_rmf` | The G0 `twisted` cubic with seed +Y; normals at u = 1/4, 1/2, 3/4, 1 |
| `inflection_rmf` | The G0 `inflection` cubic with seed +Z; the normal stays +Z where a Frenet frame flips |
| `loop_holonomy` | A closed Hermite saddle loop without mirror symmetry; residual twist 0.112407 rad. Two symmetric candidate loops had zero holonomy and were rejected |

### Acceptance gates

| Gate | Test evidence |
| --- | --- |
| Straight | `line_uniform` keeps the projected seed at every sample; the Estimated angular error is 0 |
| Inflected | `inflection_rmf` normals equal +Z within float32 storage; with an in-plane seed the binormal stays +Z through u = 1/2, where a Frenet frame flips |
| Vertical start | `vertical_start` with a +Y seed is `DegenerateFrame` with the previous frames kept; a parallel fallback also fails; a +Z fallback succeeds and sets `IsFallbackSeeded` |
| Cusped | `cusp` under `Reject` is `DegenerateFrame` with a 2^-30-wide range within 1e-6 of u = 1/2; `SplitRuns` gives two runs, `Degenerate` at the cusp and +Z normals on both sides. `line_ease` zero-speed endpoints are `Degenerate` and the run lies strictly inside the span |
| Opposite tangents | `line_backtrack` gives three runs, `Degenerate` at both zero-speed parameters and a reseeded middle run with tangent -X. An exact reversal at a joint fails `RotateMinimally` and `Reject` and splits under `SplitRuns`. A 90 degree corner rotates the +Y normal to -X |
| Random-access order independence | 64 frames on `loop_holonomy` evaluated forward, backward and in a stride-37 order are bitwise equal; a quarter turn of roll maps the normal onto the binormal |
| Closed residual twist | `PreserveHolonomy` measures 0.1124056 rad against the 0.1124068 reference and matches the reference normals within 1e-3 rad. `DistributeTwist` closes the seam within 1e-5 rad, also with one authored winding turn. A non-G1 closing joint cannot be periodic |
| Multi-minimum projection | Every `arch` query (symmetric ties, apex minima, endpoint minimum) and the three-way `line_backtrack` tie return a Certified gap that encloses the exact distance, and the earliest minimum within 1e-2 in u. The endpoint minimum returns the stored endpoint. A tie across two spans and a joint minimum return the earlier span. A far-origin path gives the same answer |

`twisted_rmf` frames differ from the integrated references by at most 3.1e-5
rad, below the reported Estimated error of 6.5e-4 rad. Strict frame profiles
return `UnsupportedPrecision`. Other tests cover `MissingCache`,
`StaleRevision`, invalid preparations and every work limit for frames,
transactional strict projection failure (`BudgetExceeded` with the result kept),
the approximate mode's candidate and gap, and scratch capacity. Sweep tests cover
a straight ribbon's exact positions, UVs and both windings. A capped tube uses
the fewest segments within the cross-section budget, keeps vertices at its radius
and its UV seam and cap rims bitwise equal to ring positions, faces outward, and
stays within twice its Estimated longitudinal budget at seven extra samples per
ring interval. A closed tube's seam ring copies the first ring. Width and roll
profiles scale and roll the rings. The arch apex flags self-intersection risk
for a unit tube. Singular or aperiodic frames, bad profiles, caps on ribbons or
closed paths, and every budget are rejected without replacing the plan, and
emission writes nothing on `CapacityExceeded`, `StaleRevision` or
`MissingCache`. The allocation probe fails every frame and sweep-plan
allocation in turn, and finds zero allocations during frame evaluation,
projection with reserved scratch and emission.

### Numerical limits observed

These are workload observations at `TangentAngleError` 1e-3 rad,
`ProjectionGap` 1e-5 and 1e-3 sweep budgets, not budgets.

| Case | Observation |
| --- | --- |
| `twisted_rmf` frames | 7 samples, 11 interval tests, level 3; Estimated error 6.5e-4 rad |
| `loop_holonomy` frames | 19 samples over four spans, level 3; Estimated error 2.6e-4 rad |
| `arch` projections | 0 (endpoint), 12-47 (apex) and 2529-2758 (symmetric ties) splits |
| `line_backtrack` tie | 9 splits |
| Tube of radius 0.05 | 16 segments; 35 (`twisted`), 51 (`inflection`) and 65 (`arch`) rings |

Symmetric ties are expensive because the tie search refines the earliest
ambiguous interval down to the acceptance region. Performance and memory budgets
remain unmeasured.

### Departures and limits

- Frame angular error is Estimated: the summed one-step versus two-half-step
  differences plus the largest one. The regularity decision is Certified from
  outward-rounded derivative boxes, but derivative enclosures do not yet bound
  the angle, as ADR 0024 requires for a strict frame profile.
- Random access transports the preceding baked sample to the analytic tangent
  with one reflection step instead of interpolating quaternions. Twist
  compensation is linear in u between samples, whose distances are estimates.
- A range that cannot be shown regular is 2^-`MaxDepth` wide, but box
  overestimation near a simple zero widens the excluded region to about 1e-6
  in u at depth 30.
- Projection closes the global gap to half of `ProjectionGap`, then resolves
  ties: every point within the full gap is acceptable, and the earliest live
  interval that may hold a point within half the gap is refined until it holds
  an acceptable candidate. The margin makes that search terminate; it shares
  `MaxIterations`. The result is the earliest acceptable candidate, not
  necessarily the exact earliest minimizer. Newton refinement and Bernstein root
  isolation are not used.
- Sweeps need one frame run with G1 joints; mitred corners and split sweeps are
  later work. Longitudinal and cross-section errors and self-intersection risk
  are Estimated. Ring frames come from float32 frame evaluation.
- Sweep mesh generation has its own `sweep.hpp`, amending ADR 0024's header
  list, so G4 patch tessellation keeps `tessellation.hpp`.

### G3 validation record

Host: macOS 26 on arm64 with the pinned host tools (Clang/LLVM 18.1.8), preset
`macos-clang-development`. The machine was heavily loaded by other agents.

| Check | Result |
| --- | --- |
| Full `macos-clang-development` build | Passed with no compiler warnings; the only linker warnings are the existing Homebrew Qt deployment-target notices from editor targets |
| `ludus_foundation_geometry_tests` | Passed: 45 test cases, 108,122 assertions |
| `ludus_geometry_alloc_tests` | Passed: 4 test cases; every frame and sweep-plan allocation failure injected |
| Fixture generator self-tests (`test_fixtures.py`) and committed header | Passed: 17 tests, header current |
| Pinned clang-tidy on every new translation unit | Passed after documented `bugprone-easily-swappable-parameters` suppressions |
| Format, naming (`check_naming.py`) and changed-line source policy | Passed against the `c3de79d` branch point |
| Header self-sufficiency and foundational include gate | Passed: 171 public headers compile standalone; foundational include boundary OK |
| Docs index, strict MkDocs, `check-wiki --documentation-only`, docs/wiki regressions | Passed |
| API reference with pinned Doxygen | Not run locally. The first CI run found one gap, the undocumented `ProjectionAccess` friend declaration, which now has a comment |
| ASan/UBSan | Not run locally: Clang 18 ASan deadlocks at startup on this macOS 26 host (see [building](../../development/building.md)), so the CI sanitizer lanes are required |
| Wasm math probe, installed SDK consumer, build-time budget | Not run locally (no Emscripten SDK). The SDK consumer now bakes frames, projects a point and emits a ribbon; these run in CI |

The frame code acknowledges Wang, Juttler, Zheng and Liu's double-reflection
paper and Carl Dougan's Game Programming Gems 2 chapter 2.5. The projection code
acknowledges Philip J. Schneider's Graphics Gems chapter XI.7. The frame
references acknowledge Bishop's transport equation and Kutta's Runge-Kutta rule.
No source code from those works is copied.

## G2 prepared paths, tracks and distance maps

Implementation date: 2026-10-16. Base: `cb6f02a` (the G0 branch). Scope: the new
`Ludus::FoundationGeometry` module (`status.hpp`, `path.hpp`, `distance.hpp`,
`track.hpp`), FoundationMath `enclosure.hpp`, and their tests, SDK-consumer and
Wasm probe coverage. Usage is documented in the
[module guide](../../modules/foundation/geometry.md). No frame, projection,
surface, mesh, cooked format, cursor or editor binding is added.

### Outward-rounding helper

ADR 0024 makes Certified bounds conditional on reviewed outward-rounded
arithmetic validated with the pinned native and Wasm toolchains. `enclosure.hpp`
adds float64 enclosures with outward add, subtract, multiply, square and square
root. Each operation rounds to nearest, then steps each bound at most one unit
outward with `std::nextafter`; the process rounding mode is never changed. For a
correctly rounded operation without overflow, the exact result lies strictly
between the neighbors of the rounded one. Knuth's TwoSum detects exact sums and
differences, which are not widened. Products and roots are always widened unless
an operand is zero, and widened square lower bounds are clamped at zero. Overflow
is `OutOfRange`, never a truncated bound.

The oracle (`math/tests/enclosure_oracle.hpp`) never calls the helpers' rounding
code. It rebuilds each exact sum or product as an unevaluated pair with Dekker's
Fast2Sum and Veltkamp-split product, then compares every returned bound with that
pair without rounding. A square root is checked by squaring its bounds exactly.
The deterministic corpus has 200,000 cases. Each case checks point sums,
differences, products and roots, plus a random interval product against all four
exact endpoint products. Magnitudes range over 2^-200 to 2^200, and half the
cases are near-cancelling. The native `[enclosure]` tests and the Wasm
`ludus_web_math_corpus` run the same corpus. A mutation that drops the outward
step from sums and products fails it.

The helper lives in FoundationMath rather than Geometry, as the architecture's
"behind Math's implementation boundary" asks. It is compiled with
`-ffp-contract=off`, and the existing Wasm math probe can then run it. Geometry is
also compiled with contraction disabled.

### Prepared owners and conversions

| Gate | Test evidence |
| --- | --- |
| Uneven Catmull-Rom knots | `centripetal_uneven`: knots 0, 1, 3, 6 exactly; the middle span's controls and tangents match the exact references within 1e-12 (Rounded float64 arithmetic); converted joints are C1 in the knot domain |
| Duplicate and closed endpoints | `duplicate_waypoint` is `InvalidDomain` with outputs unchanged; a closed loop's last span ends bitwise at the first waypoint and its closing joint is C1; a closed source that repeats its first waypoint is `InvalidDomain`; `Wrap` on an open path is `InvalidArgument`; a closed path whose ends do not meet is `InvalidTopology` |
| Holds | Track `Hold` returns the endpoint with zero derivative; equal-value keys form a stationary interval whose incoming and outgoing sides differ at its end; a stationary span occupies no distance interval |
| Prefix uncertainty | For all nine cubic fixtures, `TotalLower <= ArcLength <= TotalUpper`, and every reference distance lies inside `TryBoundDistance` at its reference parameter, which includes all preceding leaves |
| Zero speed | All five zero-speed parameters return `Degenerate` tangents with valid positions. Distances at, before and after them resolve within the residual bound, including the cusp's exact zero speed |
| Inversion residual | Every fixture distance resolves with a Certified residual of at most 1e-7. Parameters match the reference within 1e-5, or 2e-3 at the cusp, where u(s) grows like the square root of the residual |
| Budget exhaustion | Leaf, depth, subdivision, byte and zero limits return `BudgetExceeded`, report the achieved Certified gap and keep the previous map; inversion with too few iterations is `BudgetExceeded` with its output unchanged |

The corpus also covers: exact endpoints and stored controls, and reference
samples within 1e-6 relative (Rounded). Far-origin controls narrow bitwise to the
fixture's relative controls only after subtracting the origin; without it, the
256-unit floor yields `UnsupportedPrecision`. Further cases cover the position
floor for every fixture, Hermite conversion, capacity checks, line spans, corner
and domain-scaled continuity, and every frozen status and guarantee name. They
also cover all 19 `mixed_track` samples (Step, Linear, Hermite, both span sides
and every boundary policy), the 43/27 Hermite overshoot left unclamped,
`duplicate_time`, `MissingCache`, `StaleRevision` and deterministic refinement.
The allocation probe fails every preparation allocation in turn. It finds
`OutOfMemory` with the active owner preserved, and zero allocations during
sampling, inversion and caller-owned conversion.

### Numerical limits observed

Building each single-span fixture to a whole-path gap of 1e-7 used the leaf
counts below, at a maximum depth of 12-13 below the span. These are workload
observations, not budgets.

| Fixture | Leaves | Fixture | Leaves |
| --- | --- | --- | --- |
| `line_uniform`, `line_ease` | 1 | `inflection` | 4281 |
| `line_backtrack` | 25 | `twisted` | 3590 |
| `cusp` | 2012 | `vertical_start` | 2788 |
| `parabola` | 2107 | `arch` | 4771 |

The collinear ease line needs no split, because its control polygon equals its
chord. Polygon-minus-chord gaps shrink about fourfold per uniform halving, so the
leaf count grows like the inverse square root of the requested gap. Performance
and memory budgets for prepared assets and integrated consumers remain
unmeasured.

### Departures and limits

- The strict profile is the only build: every length bound is Certified, and
  the map reports that class. There is no Estimated Visual build.
- Inversion bisects on the Estimated partial-span midpoint, then accepts only by
  the Certified residual. It returns the earliest parameter within float64
  resolution, and an exact stored boundary estimate maps to its boundary.
  Safeguarded Newton steps, cursors and linear-table Approximate lookup are
  later work.
- Continuity classification compares float32-rounded controls, so thresholds
  below float32 relative rounding report C1 joints as not C1.
- A closed Catmull-Rom source must not repeat its first waypoint: the repeated
  pair is coincident and is rejected rather than silently dropped.

### G2 validation record

Host: macOS 26 on arm64 with the pinned host tools (Clang/LLVM 18.1.8, CMake
3.29.6), preset `macos-clang-development`.

The local preset does not treat warnings as errors. The build reported no
warnings, and CI enforces warnings as errors.

| Check | Result |
| --- | --- |
| Full `macos-clang-development` build | Passed with no warnings (968 build steps) |
| `ludus_foundation_geometry_tests` | Passed: 23 test cases, 83,831 assertions |
| `ludus_geometry_alloc_tests` | Passed: every preparation allocation failure injected; sampling allocates nothing |
| `ludus_foundation_math_tests` including `[enclosure]` | Passed: 112 test cases, 420,773 assertions; the enclosure corpus has 200,000 cases |
| Full CTest | 87 of 88 passed. `ludus_platform_cocoa_tests` reports failure because every case skips without `LUDUS_TEST_COCOA=1` and a WindowServer session, so it is unrelated to this change |
| Header self-sufficiency and foundational include gate | Passed: 158 public headers |
| Pinned clang-tidy on every new translation unit | Passed after naming-rule fixes and documented `bugprone-easily-swappable-parameters` suppressions |
| Format, naming (`check_naming.py`) and changed-line source policy | Passed against the G0 branch |
| Docs index, strict MkDocs, `check-wiki --documentation-only`, docs/wiki regressions | Passed |
| API reference with pinned Doxygen | Passed: no new gaps |
| ASan/UBSan | Built warning-clean. Execution hit the documented local Clang 18 ASan startup deadlock on macOS 26 (no output after 90 s), so the CI sanitizer lanes remain required |
| Wasm math probe (enclosure corpus and Geometry exercise) | Not run locally because the host has no Emscripten SDK. The `webgpu-probe` workflow now triggers on Geometry changes and runs `ludus_web_math_corpus`. The Certified claim depends on that CI result |
| Installed SDK consumer, build-time budget | Not run locally. The consumer gains `geometry.cpp` and links `Ludus::FoundationGeometry`; both run in CI |

```sh
./scripts/build macos-clang-development
out/host-tools/venv/bin/ctest --test-dir out/build/macos-clang-development \
  -R 'geometry|math|foundational_includes' --output-on-failure
python3 tests/build_contract/verify_header_self_sufficiency.py out/build/macos-clang-development
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --quiet \
  modules/foundation/geometry/src/*.cpp modules/foundation/geometry/tests/*.cpp \
  modules/foundation/math/src/enclosure.cpp modules/foundation/math/tests/enclosure_tests.cpp
./scripts/check --format
python3 scripts/python/source_policy.py --base cb6f02a
```

The arc-length code acknowledges Jens Gravesen's Graphics Gems V chapter IV.7
(reviewed in the [Gems review](../curves-surfaces-gems-review.md#length-of-bezier-curves))
and Lyle Ramshaw's blossoming report. The conversion code acknowledges Barry and
Goldman, and Yuksel, Schaefer and Keyser. The enclosure code acknowledges Moore,
Kearfott and Cloud's interval analysis text and Knuth's TwoSum, and its oracle
acknowledges Dekker. No source code from those works is copied.

## G0 contract freeze and reference corpus

Implementation date: 2026-10-15. Base: `5117c8c`. Scope: ADR 0024, the
test-only corpus in `tests/fixtures/curves_surfaces/`, a FoundationMath
cross-check test and the boundary audit below. No Geometry module, prepared
owner, cache, codec or editor binding is added.

### Reference corpus

`generate_fixtures.py` uses only the Python standard library. Polynomial
samples, derivatives, Catmull-Rom conversion, track semantics, shared edges and
tessellation bounds use exact rationals. Real roots (zero-speed parameters and
projection stationary points) are isolated with Sturm sequences and bisected to
2^-180. Arc lengths split at zero-speed roots and integrate the exact speed with
50-digit tanh-sinh quadrature; distance inversion uses safeguarded Newton with a
residual below 1e-38. Each value is rounded to float64 once. Controls and track
keys are exact float32 values.

`test_fixtures.py` (CTest `ludus_geometry_reference_fixtures`) checks the
oracles without Ludus code:

- Quadrature agrees with closed forms to 1e-35: lines, the ease line, the cusp
  `2(5 sqrt 5 - 8)`, the parabola `3(sqrt 5/2 + asinh(2)/4)` and the reversing
  line's total variation at roots `(5 -+ sqrt 5)/10`.
- Projection ties are certified by exact polynomial identities: mirror symmetry
  for the arch queries and `|C(u)-Q|^2 - 1 = (x(u) - 1/2)^2` for the three-way
  tie on the reversing line. Global minima also hold on a 2049-point rational
  grid.
- Catmull-Rom Bezier controls equal the Barry-Goldman pyramid exactly at 17
  parameters; knots are 0, 1, 3, 6.
- Saddle and bulge tessellation maxima equal 9/4 and 27/16, are attained, and
  never exceed the control-net plus twist bound on a 33 x 33 grid.
- Track samples follow the frozen span-side and boundary-policy rules; the
  Hermite overshoot is 43/27 at 5/3 s.
- The committed header matches the generator, ignoring only whitespace and
  formatter-inserted trailing commas.

`curves_surfaces_fixture_tests.cpp` in `ludus_foundation_math_tests` checks the
G1 kernel against the header. Endpoints, corners and origin-relative controls
must match bitwise (Exact). Interior samples, derivatives, Hermite controls and
normals must fall inside an estimated envelope of one float32 ulp of the larger
of the reference magnitude and the control scale (36x for derivatives). This is
a regression bound for Rounded results, not a certified error. Dense chord
polygons stay within the reference arc lengths; dense samples never beat a
projection reference; the triangle-pair deviation never exceeds the tessellation
bound; normal classifications match `TryPatchNormal`.

### Numeric guarantees versus estimates

ADR 0024 assigns every planned result a class. G1 results are Exact at stored
endpoints and corners and otherwise Rounded. No Certified helper exists, so no
strict profile is available yet. The float32 position floor `2 * ulp32(E)`
follows from half-unit storage plus half-unit output rounding within the convex
hull, at most sqrt(3) units per point.

### Boundary audit

| Boundary | Finding | Action |
| --- | --- | --- |
| Math dependencies | `ludus_foundation_math` links only `Ludus::FoundationBase` | None |
| Kernel headers | `cubic.hpp` and `patch.hpp` include only `core.h`, `status.hpp` and `vector.hpp`; neither is in `core.h` or the foundational PCH | None |
| Kernel consumers | Only Math tests, the allocation probe, the Wasm math probe and the SDK consumer include them; no G1 value is serialized | None |
| Public API documentation | 60 baseline entries for cubic and patch structs, members and overloads lacked Doxygen | Documented each declaration and removed the entries from `docs/api-undocumented.json` |
| Parameter naming | `TrySplitCubic` output references were named `left` and `right` | Renamed to `outLeft` and `outRight`; source compatible |
| Cooked format owner | The architecture placed the cooked format inside GeometryContent, which conflicts with the rule that formats shared by tools and runtime have their own header-only target | ADR 0024 assigns it to a Base-only `Ludus::GeometryFormat` |
| Fixture header | Test-only INTERFACE target linking Base and Containers; not installed or exported | None |

### G0 validation record

Host: macOS 26 on arm64 with the pinned host tools (Clang/LLVM 18.1.8, CMake
3.29.6), preset `macos-clang-development`, warnings as errors.

| Check | Result |
| --- | --- |
| Reference corpus self-checks (`ludus_geometry_reference_fixtures`) | Passed: 12 tests |
| G1 cross-check against the corpus (`[fixtures]`) | Passed: 8 test cases, 87,452 assertions |
| Full FoundationMath unit tests | Passed: 107 test cases, 420,733 assertions |
| FoundationMath allocation probe and foundational include gate | Passed |
| Header self-sufficiency | Passed: 153 public headers; run directly because the loaded host exceeded the 120 s CTest limit |
| Pinned clang-tidy on changed translation units | Passed: `cubic.cpp`, `curves_surfaces_fixture_tests.cpp` |
| Formatting of changed C++ sources | Passed: `scripts/check --format --fix`, then repository formatter rerun unchanged |
| Changed-line source policy | Passed against `5117c8c` |
| Docs index, strict MkDocs and wiki checks | Passed |
| API reference with pinned Doxygen | Passed: no new gaps; baseline only shrinks against `origin/main` |
| ASan/UBSan | Built warning-clean; execution blocked by the documented local Clang 18 ASan startup deadlock on macOS 26, so the CI sanitizer lanes remain required |
| Wasm, SDK consumer | Not rerun: the kernel's arithmetic and exported signatures are unchanged; parameter renames and comments do not affect the Wasm corpus or installed consumer |

```sh
python3 tests/fixtures/curves_surfaces/generate_fixtures.py --check
./scripts/build macos-clang-development --target ludus_foundation_math_tests ludus_math_alloc_tests
out/host-tools/venv/bin/ctest --test-dir out/build/macos-clang-development \
  -R 'ludus_foundation_math_tests|ludus_math_alloc_tests|ludus_geometry_reference_fixtures|ludus_foundational_includes' \
  --output-on-failure
python3 tests/build_contract/verify_header_self_sufficiency.py out/build/macos-clang-development
python3 scripts/python/source_policy.py --base 5117c8c
```

The generator acknowledges H. Takahasi and M. Mori (tanh-sinh quadrature),
P. J. Barry and R. N. Goldman (Catmull-Rom pyramid) and C. Yuksel, S. Schaefer
and J. Keyser (centripetal parameterization) in its module comment. No source
code from those works is copied.

## G1 cubic and bicubic kernel

Implementation date: 2026-10-05. Base: `95683aa`.
Scope: `cubic.hpp`, `patch.hpp` in `Ludus::FoundationMath`; no additional module
dependencies, prepared assets, mesh/collision/render adapters or editor binding.

The implementation uses fixed stack storage, de Casteljau interpolation,
analytic derivative control nets and float64 intermediates. Patch samples reuse
their V rows across the six outputs. All public operations are exception-free
and `noexcept`. Float32 output narrowing checks range before committing results.
This is the readable CPU reference implementation; SIMD/GPU/prepared polynomial
alternatives and integrated performance budgets remain unmeasured.

### Numerical contract and tests

Parameters are finite in `[0,1]`. Controls/results are float32; interpolation
uses float64. Endpoints/corners preserve the stored values, including signed
zero. All failed calls preserve every output. Full samples fail when a partial
or derivative exceeds float32 range, while the position-only overload can still
succeed. Split controls are rounded, so shape equivalence is numerical rather
than symbolic. No interval enclosure, certified length, distance inversion or
global projection guarantee is provided by G1.

`bezier_tests.cpp` uses independently written Bernstein basis polynomials and
analytic basis derivatives, rather than the production de Casteljau/difference
net helpers. It compares 100 deterministic cubic control sets and 25 patch nets
at endpoints and interior parameters. The corpus also covers scalar powers,
2D/3D Hermite domain scaling, split parameter remapping/derivative scaling,
collapsed endpoint children, matched split controls, saddle mixed derivatives,
normal orientation, extreme finite/subnormal partials, nonfinite inputs, invalid
domains, range failures, supported input/output aliasing and unchanged outputs.
The existing allocation probe exercises conversion, sampling, normals and
subdivision under a counting allocator. SDK and Wasm probes call exported APIs.

### Validation record

Pinned native toolchain: Clang/LLVM 18, CMake 3.29.6, Ninja 1.11.1.3, Conan 2.8.1,
C++23, precise floating point (`-ffp-contract=off`), exceptions disabled in engine
targets, warnings as errors. Browser toolchain: Emscripten 4.0.23 from
`config/web_toolchain.json`.

| Check | Result |
| --- | --- |
| New deterministic kernel unit corpus | Passed: 8 test cases, 8,773 assertions |
| FoundationMath allocation probe | Passed: 27 assertions, zero allocations/deallocations |
| Full Development build/tests and public-header gates | Passed: 65 CTest entries, 2 live-display tests skipped |
| Debug math/allocation tests and public-header gates | Passed: 4 CTest entries, including the final normal regressions |
| ASan/UBSan build/tests | Passed: 57 CTest entries, 2 live-display tests skipped; changed kernel rechecked |
| Pinned format/tidy | Passed: format; full tidy scan's 3 affected TUs corrected and passed targeted recheck |
| Installed SDK consumer | Passed: headers installed, linked and executed |
| WebAssembly numeric corpus | Passed: Development and Release Wasm/Node |
| PR CI and mergeability | Tracked by live PR checks; required before ready for review |

Validation commands (after native `init.sh --locked --with-tests --preset-only`
and browser `init.sh --with-web-probes --preset-only` setup):

```sh
./scripts/build linux-clang-development
./scripts/test linux-clang-development
./scripts/build linux-clang-asan-ubsan
./scripts/test linux-clang-asan-ubsan
./scripts/check linux-clang-development --all
# Corrected the three reported TUs, then checked them with pinned clang-tidy:
out/host-tools/bin/clang-tidy -p out/build/linux-clang-development --quiet \
  --warnings-as-errors='*' modules/foundation/math/src/cubic.cpp \
  modules/foundation/math/src/patch.cpp modules/foundation/math/tests/bezier_tests.cpp
./scripts/check --format
./scripts/install-sdk linux-clang-development
./scripts/build linux-clang-debug --target ludus_foundation_math_tests ludus_math_alloc_tests
out/host-tools/venv/bin/ctest --test-dir out/build/linux-clang-debug \
  -R 'ludus_foundation_math_tests|ludus_math_alloc_tests|ludus_header_self_sufficiency|ludus_foundational_includes' --output-on-failure
./scripts/build web-emscripten-development --target ludus_web_math_probe
out/host-tools/venv/bin/ctest --test-dir out/build/web-emscripten-development \
  -R '^ludus_web_math_corpus$' --output-on-failure
./scripts/build web-emscripten-release --target ludus_web_math_probe
out/host-tools/venv/bin/ctest --test-dir out/build/web-emscripten-release \
  -R '^ludus_web_math_corpus$' --output-on-failure
```

After the helper-signature cleanup, rebuild/recheck the affected kernel tests in
Development, Debug and ASan/UBSan, plus both Wasm corpus profiles. These results
validate G1 only; the remaining architecture phases keep their own gates.

### Attribution

Reference comments near subdivision, Hermite conversion and patch normals thank
Philip J. Schneider, Thomas Lowe and Martin Brownlow and identify the work,
book/chapter/pages, adopted idea and departures. The review links the consulted
book pages and distinguishes actual chapter reading from index-only leads.
No article source code was copied; length bounds, root isolation, transported
frames and approximate shading fields are not implemented in this phase.
