# ADR 0010: FoundationMath Module Conventions and Contracts

## Status

Proposed (implementation in progress). Supersedes nothing; extends the
foundational-module layering of ADR 0007 and the standard-library policy of
ADR 0003 (see the narrow `<cmath>`/`<limits>` allowance recorded there).

## Context

Ludus needs a small, allocation-free math vocabulary for 2D gameplay/UI, 3D
camera and transform work, primitive spatial queries, and repeatable gameplay
random streams. The requirements, architecture and staged plan are in
`.kiro/specs/math/{requirements,design,tasks}.md`; the self-contained source
research is `docs/architecture/math-research.md`. This ADR freezes the
decisions those documents selected so there is a single authoritative record of
the conventions, status/output contracts and the v1 public API inventory. It is
an engineering judgement for this checkout, not a claim of superiority over GLM,
DirectXMath, RTM or any custom library; none of those has been benchmarked
against this module.

## Decision

### Module shape and dependencies

- A new static library `ludus_foundation_math`, exported as
  `Ludus::FoundationMath`, living at `modules/foundation/math/`.
- It depends only on `Ludus::FoundationBase`. It requires no logger, profiler,
  allocator, containers, Platform, graphics API, jobs, initialization step or
  mutable global service. There is no `MathSystem`, registry or hidden cache.
- Only public headers are installed. Math is **not** added to Base's `core.h`,
  `config.h`, `compiler.h`, `types.h` or the shared foundational PCH; it is an
  opt-in layer that consumers include explicitly.
- Public headers are self-sufficient and lightweight: each includes exactly the
  Base and sibling math headers it names, plus `<span>` where a span API needs
  it. No heavy STL header, no private `internal/` header and no third-party math
  type appears in any installed signature.

### Mathematical conventions (frozen)

| Concern | Contract |
| --- | --- |
| World/local 3D | Right-handed; +X right, +Y up; camera looks toward -Z |
| Cross product | `Cross(+X,+Y) = +Z`; positive rotation follows the right hand |
| Angles / distances | Radians; meter examples. Degrees via named conversion |
| Vector action | Column vectors: `pOut = M * pIn` |
| Matrix composition | `A * B` applies B then A; `world = parent * local` |
| Matrix storage | Column-major; element access `M(row, column)` |
| Quaternion | Hamilton product; components `(x, y, z, w)`; unit action `q*(0,v)*conjugate(q)` |
| Quaternion composition | `a * b` rotates by b then a |
| Projection | NDC X/Y in `[-1,1]`, Z in `[0,1]`, Y up (matches WebGPU) |
| Framebuffer | Top-left; X right, Y down; physical pixels |
| Screen 2D | `Vector2`/`Affine2` carry no implicit camera/unit conversion |

Euler-angle storage and ambiguous `Euler` constructors are absent. There is no
global handedness or convention switch. Values are standard-layout, trivially
copyable, zero/identity-initialized as specified, with natural scalar alignment
and no hidden SIMD padding, virtual functions, swizzle proxies or expression
templates.

#### Name and compose coordinate transforms

Name transforms `SourceToDestination` and suffix coordinate-bearing values with
their space. For example, `worldToView` maps `positionWorld` to `positionView`.
Use the same names for affine transforms and matrices.

Read a column-vector product from right to left. Start with the source space
of the rightmost transform. The destination of each transform must match the
source of the next one to its left. The composed name takes its source from
the rightmost transform and its destination from the leftmost one.

```cpp
const Matrix4 worldToClip = viewToClip * worldToView;
const Vector4 positionClip = worldToClip * positionWorld;
```

A successful inverse swaps the spaces, so `TryInverse(worldToClip, clipToWorld)`
produces a transform from clip space back to world space. Keep homogeneous clip
coordinates distinct from NDC, which requires the perspective divide. When a
pass maps between two variants of a space, qualify both endpoints. For example,
the direct viewport applies `logicalClipToTargetClip` after `viewToLogicalClip`.
Names support code review. The untagged `Matrix4` type accepts either operand
order, so use noncommuting inputs when testing a composition.

Thanks to Tom Forsyth and Sebastian Sylvan for the composition and inverse
checks. Both describe destination-first names for column vectors. Ludus keeps
its source-first spelling and checks the chain in application order.
[Tom Forsyth, *Matrix maths and names* (2012)](https://tomforsyth1000.github.io/blog.wiki.html#%5B%5BMatrix%20maths%20and%20names%5D%5D) ·
[Sebastian Sylvan, *Naming Convention for Matrix Math* (2017), OpenGL style](https://www.sebastiansylvan.com/post/matrix_naming_convention/#opengl-style-matrix-convention)

### Numerical and failure contracts (frozen)

- Checked, fallible operations return `[[nodiscard]] MathStatus` and write
  caller-owned outputs **only on success**: compute a complete local result,
  validate it, then assign. On any failure every output is left byte-for-byte
  unchanged, including when an output aliases an input. Exact single-value
  aliasing is supported by computing into temporaries.
- `MathStatus` is a small enum: `Success`, `NonFiniteInput`, `InvalidArgument`,
  `Degenerate`, `IllConditioned`, `OutOfRange`, `SizeMismatch`. No strings, no
  owning result framework. Multi-failure precedence is not an ABI promise.
- Queries return `MathStatus` plus a structured result that distinguishes a
  normal geometric miss (`Success` + `Miss`) from invalid/numerically
  unsupported input (a failing status, output unchanged). Result records have
  initialized neutral fields; numeric hit fields are meaningful only on `Hit`.
- Every public function is `noexcept`; engine exceptions stay disabled.
- Assertions diagnose programmer preconditions in development builds; they never
  substitute for a runtime check and never perform an unsafe float-to-int cast
  or out-of-bounds access, so a resumable assertion that returns cannot cause
  out-of-range memory access.
- No universal epsilon, no fuzzy `operator==`, and no claim of cross-platform
  bitwise float determinism. Tolerances are named per operation with documented
  units (see `scalar.hpp`, the policy structs and §4 of the design). Initial
  thresholds are Ludus choices calibrated against the M0 corpus and may be
  adjusted with recorded evidence before the API is frozen.
- The CPU contract assumes round-to-nearest IEEE binary32/64 with NaN/Inf and
  signed-zero handling. Math never changes rounding, FTZ/DAZ or FP traps, never
  enables fast math, and rejects translation of its headers under
  `__FAST_MATH__` or a nonzero `__FINITE_MATH_ONLY__` with a useful `#error`.
  Checked `.cpp` algorithms are compiled with `-ffp-contract=off` and no fast
  math; inline operators inherit the consumer's flags, which the module
  documents consumers must keep finite/signed-zero correct.

### v1 public API inventory (frozen for this ADR)

Headers under `include/ludus/foundation/math/`:

- `status.hpp` — `MathStatus`.
- `scalar.hpp` — Pi/Tau, radians/degrees, `Abs/Min/Max/Clamp`, finite checks,
  `Sqrt/Sin/Cos/Atan2`, `Lerp`, `SmoothStep01`, ranged smoothstep,
  `NearlyEqual`, `TryApproachExponential`.
- `vector.hpp` — `Vector2/3/4`, `Vector3d`, arithmetic, `Dot`, `Cross`,
  `Hadamard`, `Length`/`LengthSquared`/`Distance`, `TryNormalize`,
  `TryNormalizeOr`, componentwise `Lerp`.
- `quaternion.hpp` — `Quaternion`, identity, `TryFromAxisAngle`, Hamilton
  `operator*`, `Conjugate`, `Rotate`, `TryRotationBetween`,
  `NlerpShortest`/`SlerpShortest`, `SameRotation`.
- `matrix.hpp` — `Matrix3/4`, `Identity`/`Zero`, action, multiply, `Transpose`,
  `TryInverse` with `InversePolicy`, `Matrix3` from a unit quaternion,
  `TryToRotation`.
- `transform.hpp` — `Affine2/3`, `TransformTRS`, point/vector/normal transforms,
  compose, `TryInverse`, `TryTransformNormal`, TRS→affine conversion,
  `ToMatrix4`, `TryPerspectiveDivide`.
- `projection.hpp` — `TryLookAt`, finite/infinite reverse-Z perspective, finite
  reverse-Z orthographic, NDC↔framebuffer conversion.
- `geometry.hpp` — `Plane`, `Aabb2/3`, `Sphere`, `Ray3`, `RayInterval`,
  `Frustum`, result records.
- `queries.hpp` — plane distance/reflection, AABB transform/merge/contains,
  ray/AABB, ray/sphere, closest segment points, frustum extraction and
  conservative sphere/AABB classification.
- `precision.hpp` — `TryMakeRelative` (double position − double origin → float).
- `random.hpp` — `RandomStream` (PCG32 XSH-RR), `RandomState`.
- `batch.hpp` — `TryTransformPoints`, `TryClassifySpheres`.

The module ships no umbrella header initially. Scope deliberately deferred:
physics, IK, animation curves/squad, quaternion compression, geometric algebra,
exact predicates/CSG, procedural noise, non-uniform distributions, fixed-point
lockstep, public SIMD register types, full double math, reflection,
serialization frameworks, collision acceleration and scene/editor services.

### Standard-math implementation allowance

The checked `.cpp` algorithms use `<cmath>` and `<limits>` for standard numeric
facilities (`std::sqrt`, `std::sin`, `std::fma` is **not** used on the checked
path, `std::nextafter`, `std::numeric_limits`, etc.). This narrow allowance,
and why it introduces no exceptions, allocation or heavy public-header cost, is
recorded in ADR 0003. Standard libm is used first; no hand-written trig
approximations from historical books. `constexpr` is applied only where the
implementation actually supports it on the pinned compilers.

## Consequences

- Consumers get a readable scalar baseline with explicit conventions and
  numerical failure contracts, and a stable public layout that a future bulk
  SIMD kernel can optimize behind without changing the API.
- CPU results are numerically equivalent within documented bounds, not bit-exact
  across libm/Wasm/GPU/compiler versions. The PCG integer sequence and saved
  state have a separate bit-exact contract.
- Because the module owns no world origin, depth state or shader payload type,
  large-coordinate rebasing, depth clear/compare selection and GPU buffer layout
  remain the consuming renderer/simulation's responsibility.
