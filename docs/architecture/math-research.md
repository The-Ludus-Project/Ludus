# Ludus math research and decisions

Research date: 2026-10-01. Checkout inspected:
`88b0b77a13b1688df6d1975d18cb1dfc6b4120fb`.
This is an implementation handoff, not an implemented or benchmarked math library.
Read the [design](../../.kiro/specs/math/design.md) and
[Kiro prompts](math-kiro-handoff.md) for the selected contracts.

## Recommendation and repository fit

Create `Ludus::FoundationMath`, a small static library with plain, fixed-size
values, cheap inline arithmetic, and larger numerical algorithms in `.cpp`
files. It depends only on FoundationBase. Use a readable scalar implementation
first, then optimize specific bulk operations if measurements justify it.
The recommendation is an engineering judgment for this checkout; there is no
evidence yet that it outperforms GLM, DirectXMath, RTM, or every custom library.

The checkout has FoundationBase/Containers/Logging/Profiling, Input, Platform,
RHI, and Text, plus native and browser smoke/probe paths. There is no math module,
general scene graph, renderer camera, physics engine, or jobs service to integrate
with. Audio and shader work and root build files already have uncommitted edits.
The older milestone documents describe historical scope; inspect current CMake
and production code instead of treating those documents as today's inventory.

Native validation uses the pinned Clang/LLVM 18 tools. Browser validation uses
Emscripten 4.0.23 and the WebGPU port in `config/web_toolchain.json`. Public
headers are discovered by existing self-sufficiency/include gates. Math must
remain an opt-in layer: never add it to Base's `core.h`, config/compiler/types
headers, or the shared foundational PCH.

The present standard-library policy does not explicitly list math facilities.
Implementation milestone M0 must document a narrow ADR 0003 allowance for
`<cmath>` and `<limits>` in math implementation files; this design proposes that
addition rather than assuming a blanket STL exemption. Public math headers need
only Base, other explicitly named math headers, and `<span>` where needed.

## Private Gems readings included for Kiro

Discovery used `references/game-dev-gems-toc.md`. The following eleven chapters
were actually inspected. GPG 1 and GPG 4 are scans: the relevant pages were
rendered and read visually. GPG 2, GPG 7, and GEG 3 were read through extracted
text. Page locators count the first PDF page as 1; printed numbering differs,
and some PDFs omit front matter or pages. These findings are original paraphrases,
not reproduced book pages, source listings, or companion CD contents.

### Matrix and quaternion conversions

Jason Shankel, *Game Programming Gems 1* (2000), chapter 2.8,
“Matrix-Quaternion Conversions,” printed pp. 200–204, PDF pp. 196–200.

The chapter derives quaternion multiplication and conversion through the action
`q * v * inverse(q)`. For matrix-to-quaternion conversion, calculating a large
quaternion component first avoids division by a component close to zero. The
rotation represented by `q` equals the rotation represented by `-q`.

**Apply:** Hamilton multiplication, explicit component/storage order, a
largest-component conversion branch, and tests near 180-degree rotations.
**Adapt:** accept matrix-to-rotation conversion only for a finite, approximately
orthonormal basis with positive determinant. A scaled, sheared or reflected basis
is not a rotation. Verify vector action instead of copying formulas across
row/column conventions. The chapter's angle symbol uses a quaternion half-angle;
Ludus factories take the actual physical rotation angle.

### Quaternion interpolation

Jason Shankel, *Game Programming Gems 1*, chapter 2.9,
“Interpolating Quaternions,” printed pp. 205–213, PDF pp. 201–209.

Linear interpolation does not preserve quaternion length and must be normalized
for rotation use. Spherical interpolation follows a great-circle arc, giving
constant angular speed for a linear interpolation parameter. Negating one input
when their dot product is negative selects the shorter rotational path. A
near-coincident pair needs a linear fallback to avoid a tiny sine denominator.
The chapter also derives squad and quaternion splines for smooth tangent changes
across multiple keys, and explains why independent sign changes can affect those
curves.

**Apply:** distinct `NlerpShortest` and `SlerpShortest`, input validation,
hemisphere correction, and a normalized close-angle fallback. **Defer:** squad,
quaternion log/exp, curve authoring and arbitrary-spin interpolation until an
animation consumer supplies continuity and timing requirements. Shortest-path
interpolation cannot represent a requested multi-turn spin.

### Rotation between two directions

Stan Melax, *Game Programming Gems 1*, chapter 2.10,
“The Shortest Arc Quaternion,” printed pp. 214–217, PDF pp. 210–213.

Recovering an angle with acos and normalizing the cross product becomes unstable
as directions converge. The chapter instead constructs the quaternion from the
dot and cross products using a half-angle relation. It explicitly acknowledges
that opposite directions remain unstable and that the sample omits that case
because of its original missile-tracking application.

**Apply:** construct from `(cross(a,b), 1 + dot(a,b))`, then normalize, for the
regular case. **Required adaptation:** reject zero directions, handle equal
directions, and choose a deterministic perpendicular axis for opposite directions.
Near-opposite directions need a stable path that still maps the supplied vectors;
do not simply snap a broad neighborhood to a 180-degree rotation. The design
provides a double-precision intermediate construction and exact-opposite tie rule.

### Vector and plane operations

John Olsen, *Game Programming Gems 2* (2001), chapter 2.2,
“Vector and Plane Tricks,” printed pp. 182–190, PDF pp. 175–183.

Dot products with a plane normal give signed displacement, closest-point
projection, reflection, and a ratio for finding an intersection along a segment.
The chapter emphasizes reusing quantities already calculated by collision code
and notes the parallel denominator and repeated-collision cases. Plane-distance
interpretation depends on a unit normal.

**Apply:** one documented plane equation, normalized plane construction,
signed distance and reflection helpers, and explicit ray intervals. **Adapt:**
derive signs from `dot(n,p) + d = 0`; do not inherit ambiguous notation or OCR
equations. The math module returns numerical query results; bounce iteration,
penetration rules, restitution and contact handling belong to physics/gameplay.

### Closest points between segments

Graham Rhodes, *Game Programming Gems 2*, chapter 2.3,
“Fast, Robust Intersection of 3D Line Segments,” printed pp. 191–204,
PDF pp. 184–197.

Most 3D line pairs do not intersect; their closest points are a more useful result.
The chapter solves a two-parameter minimization, handles degenerate and parallel
segments, and explains why independently clamping an infinite-line answer can
give the wrong finite-segment answer. Near-parallel closest locations can jump
even when distance remains stable.

**Apply:** return both parameters, points and squared distance; explicitly handle
point/segment degeneracy and constrained endpoints. **Adapt:** use bounded,
double-precision calculations and independently validate the minimum. Test a
candidate interior solution and all four endpoint-to-segment candidates, selecting
the smallest distance with a documented tie rule. Do not claim temporal contact
stability or exact topology from a tolerant floating-point query. The reference
algorithm's robustness language does not establish a guarantee for arbitrary
IEEE inputs or another implementation.

### Frustum and camera information

Waldemar Celes, *Game Programming Gems 4* (2004), chapter 2.2,
“Extracting Frustum and Camera Information,” printed pp. 147–156,
PDF pp. 156–165.

Planes transform by the inverse transpose. Pulling a canonical clip plane back
through a point transform uses its transpose, giving frustum coefficients directly
from the matrix. Camera directions/positions can also be recovered, but their
meaning depends on perspective versus orthographic projection and the transform
being inverted. The chapter covers OpenGL first and explicitly adapts to other
canonical clip volumes. It recommends requesting normalization when needed and
avoiding graphics-driver state queries in the render loop.

**Apply:** derive frustum planes from clip inequalities, build them from caller
matrices, and normalize once before distance-based queries. **Adapt:** use
`0 <= z <= w` for Ludus's canonical depth rather than OpenGL's `-w <= z <= w`.
Reverse-Z changes the physical near/far labels. Infinite far means one inactive
plane, not a failed division by its zero normal. Keep the camera's pose explicit
instead of reconstructing it from a projection matrix on every frame.

### Large world coordinates

Peter Freese, *Game Programming Gems 4*, chapter 2.3,
“Solving Accuracy Problems in Large World Coordinates,” printed pp. 157–170,
PDF pp. 166–179.

Float spacing grows with magnitude; artifacts include placement gaps, animation
jitter, and inconsistent collision outcomes. Freese combines an integer segment
with a local float offset, performs relative subtraction before ordinary local
math, and stresses that all participating values must use the same base segment.
The chapter describes renormalization, cached relative transforms, and the cost
of multiple cameras with different bases. Merely converting already rounded
floats to a wider type does not recover lost coordinates.

**Apply:** local coordinates and explicit shared origins; subtract before
narrowing. **Modern Ludus choice:** a small `Vector3d` vocabulary and checked
`TryMakeRelative(position, origin, maxAbsComponent, out)` are enough initially.
Scene/world code owns the origin and coordinate lifetime. Avoid a second global
origin manager or cached mutable matrices in Math. Cell-plus-offset worlds remain
a future option when double world coordinates have a demonstrated precision or
range limit. This double choice is our adaptation, not Freese's selected scheme.

### Random number generation

Chris Lomont, *Game Programming Gems 7* (2008), chapter 2.1,
“Random Number Generation,” printed pp. 113–125, PDF pp. 146–158.

Algorithm, state size, period, seed quality and statistical behavior must fit the
use. Repeatable seeds aid debugging; a long period alone does not prove quality.
The chapter warns about range bias and poor homemade generators, compares
several established algorithms, and separates simulation PRNGs from security
randomness.

**Apply:** caller-owned, explicitly seeded streams, saved state and known-answer
tests. **Modern choice:** PCG32 XSH-RR with precisely specified seeding and
rejection sampling, not the chapter's WELL choice or its float-based integer
range example. Rejection sampling avoids modulo bias but has no hard iteration
bound; do not call it from a deadline-bounded infrastructure callback. Distinct
stream selectors do not by themselves prove statistical independence. Randomness
is a small opt-in header, with no global generator, time-based reseeding, entropy
service, cryptographic role, Gaussian distribution or procedural noise in v1.

### Generic ray queries

Jacco Bikker, *Game Programming Gems 7*, chapter 2.2,
“Fast Generic Ray Queries for Games,” printed pp. 127–141,
PDF pp. 160–174.

Ray queries can serve picking, visibility, physics and audio. Traversal needs an
explicit entry/exit interval: a primitive hit outside the current cell must not
prematurely terminate the search. The chapter separates static and dynamic
geometry and reuses reciprocal directions, while compact node storage relies
on historical machine assumptions.

**Apply:** parametric rays and inclusive bounded intervals; keep primitive math
reusable. **Defer:** kD-trees, BVHs and scene traversal. No unchecked reciprocal
for zero direction components, pointer-bit storage, 32-bit pointer assumptions,
or universal claim that kD-trees are today's best acceleration structure.

### Precision through projective space

Krzysztof Kluczek, *Game Programming Gems 7*, chapter 2.4,
“Using Projective Space to Improve Precision of Geometric Computations,”
printed pp. 153–164, PDF pp. 186–197.

Homogeneous integer coordinates can preserve exact constructions on a quantized
input domain. Cross/dot products avoid premature division, but repeated
constructions quickly increase required integer ranges. The chapter analyzes
overflow bounds and restricts construction classes; its printed implementation
discussion is primarily 2D, with the 3D extension on the companion CD.

**Apply:** distinguish numerical proximity from a topological predicate and delay
perspective division. **Defer:** integer projective CSG and arbitrary precision.
Double intermediates improve numerical behavior but are not exact predicates.
If a future mesh/CSG/navmesh consumer needs reliable orientation/incircle signs,
use a separately reviewed adaptive predicate implementation with proven error
bounds and validated compiler settings. Do not require the unavailable CD.

### Portable SIMD programs

Nicolas Guillemot and Marc Fauconneau Dufresne, *Game Engine Gems 3* (2016),
chapter 16, “Portable SIMD Programs Using ISPC,” PDF pp. 204–213.
Printed page digits are damaged in text extraction; PDF locators are authoritative.

The chapter contrasts SIMD across one small object with SIMD across many
independent objects. Structure-of-arrays data, uniform inputs, avoiding gathers,
and limiting divergence help bulk workloads. Its example culls many spheres
against a fixed frustum, and discusses generated entry points and architecture
selection.

**Apply:** a bulk API with stable output order, caller-owned buffers and scalar
parity tests. Benchmark conversion costs as well as the inner loop. **Defer:**
ISPC, a generic SIMD register wrapper, and dispatch machinery. Compiler reports
and generated code must confirm vectorization; source appearance is insufficient.
An additional compiler/toolchain is not justified for Ludus's first small module.

## Current primary sources and modern adaptations

Public sources were inspected during this handoff. Their documented capabilities
are evidence; the selected contracts and thresholds remain Ludus design choices.

| Source | Finding used and resulting choice |
| --- | --- |
| [Clang 18 floating-point options](https://releases.llvm.org/18.1.8/tools/clang/docs/UsersManual.html#controlling-floating-point-behavior) | Fast math changes NaN/Inf, signed-zero and reassociation assumptions. Contraction has its own option. Keep checked algorithms under precise arithmetic; audit consumer flags for inline math. |
| [LLVM vectorizers](https://llvm.org/docs/Vectorizers.html) | Loop and SLP vectorizers optimize different patterns; floating reductions can require reassociation. Vectorize across independent objects before relaxing numerical semantics. |
| [Emscripten SIMD](https://emscripten.org/docs/porting/simd.html) | Wasm SIMD is enabled with `-msimd128`; relaxed SIMD is a separate mode. Keep browser scalar support and test an explicit SIMD artifact if one is added. Verify with pinned 4.0.23. |
| [WebGPU coordinate systems](https://gpuweb.github.io/gpuweb/#coordinate-systems) | NDC Y points up, framebuffer Y down, and depth spans zero to one. Canonical math uses that clip convention; screen conversion flips Y once. Near versus far depends on the projection and depth state. |
| [Vulkan viewport transform](https://docs.vulkan.org/spec/latest/chapters/vertexpostproc.html#vertexpostproc-viewport) | Negative viewport height can supply the Y conversion. Record one graphics adaptation and test winding; do not add backend conditionals to pure math. |
| [WGSL layout](https://www.w3.org/TR/WGSL/#alignment-and-size) | A `vec3<f32>` has size 12 and alignment 16; matrix columns have prescribed strides. Keep compact CPU values separate from explicit shader transfer layouts. |
| [Nathan Reed on depth precision](https://developer.nvidia.com/blog/visualizing-depth-precision/) | Reverse-Z with floating depth substantially improves perspective depth distribution. Select reverse-Z helpers, with matching clear/comparison contracts; do not claim this cures every precision failure. |
| [Shewchuk's geometric robustness research](https://www.cs.cmu.edu/~jrs/jrspapers.html) | Adaptive arithmetic can decide predicate signs reliably where ordinary roundoff is problematic. Defer that specialized subsystem; never advertise tolerant intersection routines as exact topology. |
| [PCG minimal API](https://www.pcg-random.org/using-pcg-c-basic.html) and [reference source](https://github.com/imneme/pcg-c-basic/blob/master/pcg_basic.c) | Explicit state, seeding and bounded generation are available. Lock PCG32 sequence and state format; retain required license/provenance for adapted code. |
| [GLM clip-space API](https://glm.g-truc.net/0.9.9/api/a00243.html) | Explicit handedness/depth variants and default macro-controlled variants coexist. Avoid macro-dependent conventions in Ludus. |
| [DirectXMath](https://github.com/microsoft/DirectXMath) and [RTM](https://github.com/nfrechette/rtm) | Established libraries provide optimized math implementations. They are credible performance references, not measured losers or required dependencies. |

## Scalar SSE reciprocal estimates

Sebastian Sylvan's [“Using SSE intrinsics to optimize scalar code”](https://www.sebastiansylvan.com/post/scalarsseintrinsics/)
(February 23, 2016) suggests looking for hot loops before tuning individual
operations. Thanks to Sylvan for the narrower idea of using a hardware estimate
and, when the caller needs more accuracy, refining it once with Newton–Raphson.
`RCPSS` estimates `1/x`. A general quotient also needs multiplication by the
numerator. Intel specifies a relative error bound of 1.5 × 2⁻¹² for the
estimates away from exceptional cases. Both instructions treat subnormal inputs
as signed zero, and `RCPSS` also flushes tiny results.
See Intel's [Software Developer's Manual, Volume 2](https://cdrdv2-public.intel.com/774492/325383-sdm-vol-2abcd.pdf),
entries `RCPSS` and `RSQRTSS`.

**Ludus decision.** Keep the checked CPU math in
[ADR 0010](../decisions/0010-foundation-math-module.md). `TryNormalize` scales
the input and computes the length in binary64, including the comparison
against the caller's minimum. Squaring in binary32 can turn a valid tiny
vector into zero or overflow for a valid large one before an estimate even
runs. A regression now exercises both extremes and the inclusive minimum
length check. Slang already exposes `rsqrt` for GPU kernels with suitable
input contracts.

**Probe, November 11, 2026.** Clang 23.1.3 at `-O3 -fno-fast-math
-ffp-contract=off` emitted `rcpss` and `rsqrtss` for the x86-64 estimates,
`sqrtss` plus `divss` for ordinary binary32 inverse square root, and binary64
square root and division in checked normalization on x86-64 and arm64.
On Apple Silicon under Rosetta, a sweep of 257 evenly spaced mantissa
samples for each of the 254 positive normal exponents compared estimates
with a binary64 baseline. The largest observed relative errors were
2.90 × 10⁻⁴ for nonzero reciprocal estimates, 3.25 × 10⁻⁴ for raw inverse
square root, and 2.31 × 10⁻⁷ after one refinement. `RCPSS` returned zero
for 514 inputs where division returned a nonzero value. `RSQRTSS` returned
infinity for the smallest positive subnormal, whose ordinary inverse square
root was finite. These are code-generation and numerical observations. The
article's Haswell timings and this translated run leave native Ludus speedup
unmeasured.

**Revisit when a CPU profile identifies a suitable caller.** A private
estimate needs a bounded input domain, a caller-specific error budget, and a
native end-to-end timing comparison that includes guards and the portable
fallback. For work over independent items, compare a batched implementation
as well. For example, the normal-map mip cooker has a per-texel normalization
loop, so a profile could justify comparing it on a representative texture
corpus. Check its cooked bytes as well as its runtime. A change in a reference
path tracer would also need the engine's
[CPU and GPU acceptance checks](renderer-systems.md#cpu-and-gpu-reference-implementation-requirement).

## Alternatives and deliberate omissions

| Option | Decision and reconsideration trigger |
| --- | --- |
| Public GLM types | Rejected for v1: conventions, templates, include weight and compile macros become part of Ludus's public contract. Revisit if a production prototype shows significantly lower total maintenance with acceptable header budgets. |
| Private RTM or DirectXMath adapter | Credible later kernel source/reference. Revisit only for a measured hotspot, with source/license pinning and native/wasm parity. Do not promise a port works without testing it. |
| Eigen or a general numerical framework | Excess scope for fixed graphics/game values. Consider a separate tooling/solver module when a real linear-algebra consumer needs decompositions or large matrices. |
| SIMD registers in every public vector | Reject: ABI/alignment and debugger costs propagate everywhere, and SIMD across three components is not the only useful layout. Keep registers private to any future batch kernel. |
| Expression templates, arbitrary dimensions/scalars, swizzle proxies | Reject: larger API and compiler/debugger burden than the present consumers warrant. Plain operators and named functions suffice. |
| Full double-precision math | Defer: world subtraction is the demonstrated design need; add only `Vector3d` and checked relative conversion now. |
| Exact predicates, fixed-point lockstep, GPU math execution | Separate future requirements. v1 CPU floats do not guarantee cross-platform bitwise simulation determinism. |
| Physics integration, IK, animation curves, quaternion compression, noise, collision worlds | Listed in the TOC but outside the first module. Add to their owning consumer when data, accuracy and workload requirements are known. |

This package deliberately embeds the actionable book findings and modern
corrections. Kiro can implement it without the gitignored references, book
figures, historical platform code, or online access. All production benchmarks,
library comparison results, GPU interoperability checks and implementation
validation remain outstanding.
