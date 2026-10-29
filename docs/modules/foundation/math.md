# Ludus::FoundationMath

A small, allocation-free math vocabulary for 2D gameplay/UI, 3D camera and
transform work, primitive spatial queries, and repeatable gameplay random
streams. It depends only on `Ludus::FoundationBase` and is an opt-in layer: it is
**not** part of Base's `core.h` or the foundational PCH.

See the specification in `.kiro/specs/math/` (requirements, design, tasks), the
decision record `docs/decisions/0010-foundation-math-module.md`, and the
accuracy/benchmark ledger in `docs/architecture/math-evidence/`.

## Conventions (frozen)

- Right-handed world/local; **+X right, +Y up, camera looks toward −Z**.
- `Cross(+X, +Y) = +Z`; positive rotation follows the right hand.
- Column vectors: `pOut = M * pIn`. Matrix composition `A * B` applies **B first**
  (`world = parent * local`). Column-major storage; element access `At(row, col)`.
- Hamilton quaternions stored `(x, y, z, w)`, default identity `(0,0,0,1)`;
  `a * b` rotates by **b then a**; the rotations of `q` and `−q` are identical.
- Radians for angles (named `DegreesToRadians`/`RadiansToDegrees`); meter examples
  for distance. No implicit units, no global convention switch.
- Projection NDC: X/Y in `[-1, 1]`, **depth Z in `[0, 1]`, Y up** (WebGPU
  canonical). All projection factories are **reverse-Z** (near→1, far→0).
- Framebuffer coordinates are top-left, Y down, physical pixels.

## Error handling

Every API is `noexcept` (the engine is `-fno-exceptions`). Fallible operations
return `[[nodiscard]] MathStatus` and write caller-owned outputs **only on
success**; on any failure every output — including an aliased input — is left
unchanged. A geometric *miss* is `Success` plus a `Miss` classification, distinct
from invalid input (a failing status). There is no universal epsilon and no fuzzy
`operator==`; tolerances are named per operation (see `scalar.hpp` and the policy
structs). CPU results are numerically equivalent within documented bounds, **not**
bit-exact across libm/Wasm/GPU/compiler versions. The PCG integer sequence and
saved state are the one bit-exact contract.

## Quick examples

### 2D affine transform

```cpp
#include <ludus/foundation/math/transform.hpp>
using namespace ludus::foundation::math;

Affine2 t = Affine2::Identity();
t.Translation = Vector2{100.0f, 50.0f};        // move 100,50 px
const Vector2 screen = TransformPoint(t, Vector2{0.0f, 0.0f});
```

### Parent/local 3D composition

```cpp
TransformTRS parentData;
parentData.Scale = Vector3{2.0f, 1.0f, 1.0f};  // non-uniform scale
TransformTRS childData;
TryFromAxisAngle(Vector3{0, 0, 1}, kHalfPiF, childData.Rotation);

const Affine3 parent = ToAffine3(parentData);
const Affine3 child  = ToAffine3(childData);
const Affine3 world  = Compose(parent, child); // retains shear from the mix
const Vector3 p = TransformPoint(world, Vector3{1, 0, 0});
```

### Camera / view + projection

```cpp
#include <ludus/foundation/math/projection.hpp>

Matrix4 view{};
if (TryLookAt(Vector3{0, 2, 5}, Vector3{0, 0, 0}, Vector3{0, 1, 0}, view) == MathStatus::Success)
{
    Matrix4 proj{};
    // reverse-Z: clear depth to 0, compare Greater/GreaterEqual in the renderer.
    (void)TryPerspectiveReverseZ(DegreesToRadians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f, proj);
    const Matrix4 worldToClip = proj * view;
}
```

### Large-origin conversion

```cpp
#include <ludus/foundation/math/precision.hpp>

Vector3 local{};
// Subtract the shared double origin BEFORE narrowing to float.
(void)TryMakeRelative(playerWorldPosition, cameraOrigin, /*maxAbsComponent=*/4096.0, local);
```

### Random samples independent of job order

For checked examples, address ownership and reproduction requirements, see the
[wiki randomness guide](../../wiki/guides/randomness.md).

```cpp
#include <ludus/foundation/math/addressed_random.hpp>

RandomKey lootKey;
// These values belong to the world/content protocol. Never derive them from
// a worker index, pointer, iteration position or rendering frame.
if (TryMakeRandomKey(/*worldSeed=*/42, /*persistentLootDomain=*/7, lootKey) == MathStatus::Success)
{
    const RandomAddress drop{ /*entityId=*/123, /*dropOccurrence=*/5, /*itemSlot=*/0 };
    uint32 ticket = 0;
    if (TrySampleBounded(lootKey, drop, 1000, ticket) == MathStatus::Success)
    {
        // ticket repeats for this logical drop regardless of worker scheduling.
    }
}
```

### Domain manifests

Keep a game's domain IDs in one checked-in table and let the compiler check it.
Game IDs use `[1, 0x00FFFFFF]`; engine modules use IDs from `0x01000000`. An ID is
never reused, and renaming a label leaves derived keys unchanged. See
[stable keys and domains](../../architecture/randomness.md#stable-keys-domains-and-identities).

```cpp
#include <ludus/foundation/math/random_domains.hpp>

constexpr RandomDomain GAME_DOMAINS[] = {
    {1, "loot selection"},
    {2, "combat spread"},
};
static_assert(CheckRandomDomains(GAME_DOMAINS, RandomDomainOwner::Game).Status == MathStatus::Success);
```

Persist the root seed, stable domain/address assignments, generator identity
`Philox4x32-10`, and all three v1 contract IDs with the owner's replay/content
version. Use `TryMakeRandomAddress` before narrowing external counters. Reusing
an address repeats a sample; changing a bound changes its mapping. Checked
functions preserve outputs on failure. Rejection attempts stay inside the
address, have a 65536-attempt cap, and report `OutOfRange` on exhaustion. This is
non-cryptographic randomness. `PreparedBound32` amortizes threshold division;
`TrySampleBlock` shares one evaluation across four aligned dimensions.
`RandomMaxDimension` and `RandomBoundedAttemptCount` name the layout's 16-bit
dimension limit and the retry budget.

Kernels use the same contract through `math.slang` (installed with the SDK shader
includes): `SampleRandomUInt32`, `SampleRandomFloat01` and `TrySampleRandomBounded`
reproduce the CPU results bit for bit for the same key and address. The 64-bit
scope is passed as two words, dimensions above 65,535 are a caller error, and
bounded failure is a `false` result to branch on. See
[GPU addressed sampling](../../architecture/randomness.md#gpu-addressed-sampling)
for the exact contract and its Metal/Vulkan evidence.

References: Salmon et al., [Random123 (SC11)](https://www.thesalmons.org/john/random123/papers/random123sc11.pdf),
Lemire, [Fast Random Integer Generation in an Interval](https://arxiv.org/abs/1805.10941),
Vigna's [SplitMix64 finalizer](https://prng.di.unimi.it/splitmix64.c), and
Game Programming Gems 1 §2.0, 2 §1.16, and 3 §2.1. The
[architecture](../../architecture/randomness.md) and
[chapter review](../../architecture/randomness-gems-review.md) explain
the selected ideas and limitations. Random123's BSD notice ships in the SDK.

### Weighted reservoir for resampling

`weighted_reservoir.hpp` streams weighted candidates in a fixed order and keeps
one, with replacement, for resampled importance sampling. Give each candidate
its own ticket dimension and record the payload of the candidate the update
selects:

```cpp
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/weighted_reservoir.hpp>

WeightedReservoir reservoir;
uint32 selectedLight = 0;
float32 selectedTarget = 0.0f;
for (uint32 i = 0; i < candidateCount; ++i)
{
    const RandomAddress ticketAddress{ pixelScope, sampleIndex, static_cast<uint16>(13u + 2u * i) };
    const ReservoirCandidate candidate{ .Weight = targets[i] / sourcePdfs[i],
                                        .Ticket = SampleFloat01(key, ticketAddress) };
    bool isSelected = false;
    if (TryAddReservoirCandidate(candidate, reservoir, isSelected) != MathStatus::Success)
    {
        return; // invalid weight: fail the sample explicitly
    }
    if (isSelected == true)
    {
        selectedLight = lights[i];
        selectedTarget = targets[i];
    }
}
float32 contributionWeight = 0.0f; // zero for an empty reservoir
(void)TryComputeReservoirContributionWeight(reservoir, selectedTarget, contributionWeight);
```

The rule is `w_sum += w`, then select when `ticket * w_sum < w`. Zero weights
never win, an all-zero stream is a valid empty result, and the contribution
weight is `w_sum / (M * target(selected))`. NaN, infinite or negative weights,
tickets outside `[0,1)`, overflow and invalid targets fail with `MathStatus`
and leave outputs unchanged. Weights below `WeightedReservoirMinimumWeight`
(`2^-102`) count as zero, so that CPU and GPU selections agree. Kernels use the
same contract through `weighted_reservoir.slang`, installed beside `math.slang`:
`MakeWeightedReservoir`, `TryAddReservoirCandidate`, `HasReservoirSelection` and
`TryComputeReservoirContributionWeight` return `false` on failure. See the
[sequential weighted reservoir](../../architecture/randomness.md#sequential-weighted-reservoir)
contract, its exact-frequency and GPU parity evidence, and the credit to Chris
Wyman's *Ray Tracing Gems II* chapter 22.

### Repeatable random stream

```cpp
#include <ludus/foundation/math/random.hpp>

RandomStream loot;
(void)loot.TryReseed(/*seed=*/questId, /*selector=*/kLootStream);
uint32 roll = 0;
(void)loot.TryNextBounded(100, roll);         // unbiased [0,100)
float chance = loot.NextFloat01();            // [0,1)
```

The caller owns the stream and its logical call order; save/restore
`RandomState` for bit-exact replay. Bounded sampling uses rejection and has no
hard worst-case iteration bound — do not call it from a deadline-bounded
infrastructure callback. This is not a cryptographic generator.

## Cubic curves and bicubic patches

Include `cubic.hpp` or `patch.hpp` explicitly. `CubicBezier1/2/3` have four
scalar/2D/3D controls; `BicubicBezierPatch` has `Control[u][v]`. Evaluation,
Hermite conversion and subdivision allocate nothing and preserve outputs on
failure. Inputs use normalized finite parameters in `[0,1]`; there is no implicit
clamping or extrapolation. Results are rounded float32 values computed using
float64 intermediates, without a certified error bound.

```cpp
#include <ludus/foundation/math/cubic.hpp>
using namespace ludus::foundation::math;

CubicBezier3 curve;
const CubicHermite3 source{{0, 0, 0}, {3, 6, 9}, {1, 2, 3}, {1, 2, 3}};
if (IsSuccess(TryFromHermite(source, 3.0, curve)))
{
    CubicSample3 sample;
    if (IsSuccess(TryEvaluateCubic(curve, 0.5, sample)))
    {
        // Position = (1.5,3,4.5). FirstDerivative is per normalized u.
        // Divide it by the source width 3 to recover the source-domain velocity.
    }
}
```

The position-only overload remains usable when derivatives overflow float32.
Patch samples include `DU`, `DV`, `DUU`, `DUV`, `DVV`. `TryPatchNormal` checks
analytic `Cross(DU,DV)` and an explicit `PatchNormalPolicy`; zero/parallel
partials are singular, and a caller can reject short or nearly parallel partials.
Subdivision produces two/four rounded children in their own normalized domains,
with matching stored split controls. Prepared paths, tracks and distance maps
live in [`Ludus::FoundationGeometry`](geometry.md); transported frames,
adjacency, meshes and editor documents remain future work. See the
[architecture](../../architecture/curves-surfaces.md),
[frozen G0 contracts](../../decisions/0024-curves-surfaces-contracts.md) and
[evidence](../../architecture/math-evidence/curves-surfaces.md).

## Outward-rounded enclosures

`enclosure.hpp` provides `Enclosure`, a closed float64 interval `[Lower, Upper]`,
and `TryAddOutward`, `TrySubtractOutward`, `TryMultiplyOutward`,
`TrySquareOutward` and `TrySquareRootOutward`. Each result contains the exact real
result for every operand inside the inputs. The helpers round to nearest, as the
process already does, then step each bound at most one float64 unit outward; the
rounding mode is never changed. Sums and differences detect an exact result with
an error-free transformation and stay tight; products and roots are always
widened unless an operand is zero. Invalid or nonfinite bounds and overflow
return a status and leave the output unchanged.

```cpp
#include <ludus/foundation/math/enclosure.hpp>
using namespace ludus::foundation::math;

Enclosure sum;
if (IsSuccess(TryAddOutward(MakePointEnclosure(0.1), MakePointEnclosure(0.2), sum)))
{
    // sum.Upper == 0.1 + 0.2; sum.Lower is the next float64 below it.
}
```

These enclosures are the reviewed outward-rounded arithmetic behind ADR 0024's
Certified guarantee class. A 200,000-case corpus checks them against exact
Dekker sums and products on the pinned native toolchain (`[enclosure]` tests)
and on the pinned Emscripten toolchain (`ludus_web_math_corpus`). They are not a
general interval library: there is no division, transcendental function or
dependency tracking.

## Projected solid angle sampling

`projected_solid_angle.hpp` samples a convex polygonal light uniformly in
projected solid angle: directions are distributed proportionally to cos θ inside
the polygon, which removes the cosine term from a diffuse light estimate. It
implements section 3 and supplement A of Christoph Peters, "BRDF Importance
Sampling for Polygonal Lights", *ACM Transactions on Graphics* 40(4), Article 140,
2021 ([DOI](https://doi.org/10.1145/3450626.3459672)). The shader port
`shaders/projected_solid_angle.slang` is installed beside `math.slang` and
evaluates the same operations.

```cpp
#include <ludus/foundation/math/projected_solid_angle.hpp>

// Vertices relative to the shading point, in a frame whose +z is the shading normal.
ProjectedPolygonInput light;
light.VertexCount = 4;
light.PositionsTangent[0] = Vector3{1, -1, 0.5f};
light.PositionsTangent[1] = Vector3{1, -1, 2};
light.PositionsTangent[2] = Vector3{1, 1, 2};
light.PositionsTangent[3] = Vector3{1, 1, 0.5f};
ProjectedSolidAnglePolygon polygon;
if (TryPrepareProjectedSolidAnglePolygon(light, polygon) == MathStatus::Success && polygon.ProjectedSolidAngle > 0)
{
    Vector3 direction;
    if (TrySampleProjectedSolidAnglePolygon(polygon, Vector2{u0, u1}, direction) == MathStatus::Success)
    {
        const float density = EvaluateProjectedSolidAnglePolygonDensity(polygon, direction); // per steradian
    }
}
```

The polygon must be convex and planar with no three collinear vertices and at
most `MAX_PROJECTED_POLYGON_INPUT_VERTICES` (7) vertices; either winding is
accepted. FoundationGeometry's convex polygon partition produces such polygons
from emissive meshes. Preparation clips to the horizon (at most eight vertices
remain), assigns each edge's great circle an ellipse in the projected disk, sorts
vertices with the paper's bitonic sorting networks and computes the sector areas
of Equation (1). A polygon below the horizon or edge-on to the shading point
prepares successfully with zero projected solid angle. Sampling is continuous in
both random numbers, so stratification carries over. Decentral samples (the zenith
outside the polygon) use the two-quad initialization and exactly
`PROJECTED_POLYGON_ITERATION_COUNT` (2) tangent-line iterations; the iteration is
skipped within 1e-5 of a sector boundary, as in the paper. The density evaluator
assumes the direction lies in the polygon, for example because another technique
of the same light sampled it or a ray hit the light.

Departures from the author's `polygon_sampling.glsl`, none of which changes the
sampled distribution:

- Ellipse kinds (outer, inner, degenerate) are stored explicitly instead of in
  the sign bit of u.x and as an infinite u.x, and `positive_atan(y / x)` never
  divides by zero. Ludus compiles Metal shaders with fast math and WGSL does not
  guarantee infinities, NaNs or signed zeros; the paper's encoding produced NaN
  for a vertex at the zenith on Metal. The shader tests finiteness on bit patterns.
- The arctangent is `ComputeArctangent` from FoundationMath's `math.slang`, not
  the backend's `atan`. Vulkan bounds `atan` only to 4096 ulp. Mesa lowers it to a
  polynomial whose absolute error reaches 3.3e-6 near |x| = 1 on lavapipe 25.2.
  Sector areas are differences of these angles, so on that driver the
  99th-percentile relative area error against C++ was 1.5e-3.
  `ComputeArctangent` uses Stephen L. Moshier's Cephes `atanf` range reduction and
  polynomial and stays within 3 ulp of float64 on every backend.
- Sector selection skips sectors without area, so rounding at the end of the
  distribution cannot select an empty sector, where the original produces NaN.
- The iteration's approximate exponent-flip reciprocal is an exact reciprocal,
  the variant the author's comment offers for ports.
- Horizon clipping is an independent implementation with the same result as
  supplement A.1, not its generated code. Its output index is dynamic on GPUs;
  the paper's spill-free switch is a possible later optimization.
- The paper's biased variant (section 6.4) is not provided.

Kahan's difference of products needs a fused multiply-add. The implementation
calls `std::fma` explicitly; contraction stays disabled. Shader targets that
evaluate `fma` unfused keep the algorithm but lose its 1.5 ulp bound.

Evidence: `projected_solid_angle_tests.cpp` checks 4000 random polygons and the
boundary cases of supplement A.2 against an independent float64 oracle, which
clips the polygon, integrates the cosine with Lambert's edge formula and inverts
the distribution by bisection. On Apple M5 the backward error scaled by the
projected solid angle has a 99th percentile of 1.1e-7, matching the paper's
Fig. 6b; the relative backward error grows only for small projected solid angles
(99th percentile 4.2e-5). Two iterations bring the residual sector area below
2e-7 for 99% of samples, against 4.8e-2 relative error for the initialization
alone (paper section 3.6 reports 0.02). A stratified quadrature integrates the
density to one within 0.5%. The renderer's `[polygon-sampling]` GPU test compares
the shader with C++ on 4096 cases; it runs natively on Metal here, and the
WGSL build compiles but is first executed in CI. On Mesa lavapipe 25.2 (Vulkan)
the relative area error has a 99th percentile of 8.8e-5 and the direction error
1.6e-5.

## Linearly transformed cosines over polygons

`linearly_transformed_cosine.hpp` integrates and samples a linearly transformed
cosine (LTC) inside a convex polygon. An LTC with linear transform `M` has the
density `p(ω) = max(0, (M⁻¹ω).z) · |M⁻¹| / (π · ‖M⁻¹ω‖⁴)`. That is a clamped
cosine of the direction `M⁻¹ω / ‖M⁻¹ω‖`, times the Jacobian of that map. Fitted
LTCs approximate specular BRDF lobes.

The header implements these sources from their text:

- Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt, "Real-Time
  Polygonal-Light Shading with Linearly Transformed Cosines", *ACM Transactions
  on Graphics* 35(4), Article 41, 2016, SIGGRAPH 2016
  ([DOI](https://doi.org/10.1145/2897824.2925895)):
  - section 3.1, Equation 1: the density;
  - section 3.2, Equation 3: the polygon integral;
  - Algorithm 1: importance sampling.
- Christoph Peters, "BRDF Importance Sampling for Polygonal Lights", section 4.1
  ([DOI](https://doi.org/10.1145/3450626.3459672)): combining the LTC with
  [projected solid angle sampling](#projected-solid-angle-sampling).

No code from either paper's GPL-licensed utilities is used. The shader port
`shaders/linearly_transformed_cosine.slang` is installed beside
`projected_solid_angle.slang`, which it includes.

```cpp
#include <ludus/foundation/math/linearly_transformed_cosine.hpp>

// M^-1 in the shading frame (+z is the shading normal), for example a fitted BRDF lobe
// rotated so that its view direction matches the receiver's.
LinearlyTransformedCosine lobe;
if (TryMakeLinearlyTransformedCosine(inverseTransform, lobe) == MathStatus::Success)
{
    LinearlyTransformedCosinePolygon polygon;
    if (TryPrepareLinearlyTransformedCosinePolygon(light, lobe, polygon) == MathStatus::Success &&
        GetLinearlyTransformedCosinePolygonIntegral(polygon) > 0)
    {
        Vector3 direction;
        if (TrySampleLinearlyTransformedCosinePolygon(polygon, Vector2{u0, u1}, direction) == MathStatus::Success)
        {
            const float density = EvaluateLinearlyTransformedCosinePolygonDensity(polygon, direction);
        }
    }
}
```

**Preparation.** Preparation transforms the polygon's vertices by `M⁻¹`, clips
them to `z ≥ 0` and prepares the result for projected solid angle sampling.

- **Integral.** The LTC's integral over the polygon (Heitz et al., Equation 3)
  is the transformed polygon's projected solid angle divided by π, a value in
  `[0, 1]`.
- **Empty clip.** If the transformed polygon is empty, the integral is zero. The
  caller must then disable the LTC technique, as Peters prescribes.
- **No horizon clip.** As in Peters' implementation, the polygon is **not**
  clipped against its own shading horizon. The LTC can be positive slightly
  below the horizon, so a sample can point below it, and the caller's BSDF then
  evaluates to zero.

**Sampling.** A sample maps the transformed polygon's projected-solid-angle
sample back with `M` and normalizes it (Heitz et al., Algorithm 1). Its density
is `p(ω)` divided by the integral. As for projected solid angle sampling, the
density evaluator assumes the direction lies in the polygon.

A sharp lobe pointing away from a light squeezes the transformed polygon into a
sliver. When the integral is negligible (the tests observe it below 1e-6), the
single-precision sample of that sliver can be inaccurate and can miss the
polygon. That is the same limit the paper reports for tiny, distant polygons
(section 3.7). A renderer that intersects the sampled direction with the light
then adds nothing for that sample. Its MIS weight is proportional to the
negligible integral.

**Representation.** The distribution depends only on the direction of `M⁻¹ω`
(Heitz et al., section 3.1).

- `TryMakeLinearlyTransformedCosine` stores `M⁻¹` scaled by an exact power of
  two, so its largest-magnitude entry lies in `[1, 2)`.
- For `M` it stores the adjugate of the scaled `M⁻¹`. The adjugate is a positive
  multiple of `M`, so no division is needed.
- Matrices that are not invertible with a positive determinant are rejected.
  Peters requires `|M| > 0`, and a negative determinant would mirror the
  hemisphere.
- Scaled determinants below `MIN_LINEARLY_TRANSFORMED_COSINE_DETERMINANT` (2⁻³²)
  are rejected as ill-conditioned.
- The renderer's sharpest lobe, at perceptual roughness 0.045, has a scaled
  determinant of about `α = 0.002`, far above that limit.

**Shader port.** Metal compiles shaders with fast math, so the port classifies
the scale exponent, determinant and finiteness on bit patterns. It passes
matrices as three `float3` columns, the layout of the C++ `Matrix3`.

**Evidence.** `linearly_transformed_cosine_tests.cpp` compares against an
independent float64 oracle:

- integrals, on 2000 random polygons and lobes with α down to 0.002, against
  the transform and Lambert's formula evaluated in float64;
- densities, against Equation 1;
- integrals, against area-sampled Monte Carlo integration of Equation 1;
- `E[1/pdf]`, against the solid angle of the polygon's support (Van Oosterom
  and Strackee's formula);
- the density's integral over the sphere.

It also covers the identity LTC, scale invariance, empty clips, samples below
the horizon and invalid inputs.

Measured results:

- The integrals agree with the float64 transform within `1e-6 + 1e-3 · integral`.
- Densities match Equation 1 to a 99th-percentile relative error of 5e-6.
- Samples fall outside the polygon at 3 in 14,176 (edge rounding), once
  negligible integrals are excluded.

The renderer's `[polygon-sampling]` GPU test compares the shader with C++ on
4096 cases. These include the identity, a shear reaching below the horizon, an
empty clip, rejected matrices, and random lobes down to α = 0.002.

| Metal on Apple M5, integrals ≥ 1e-4 | 99th percentile | Largest |
| --- | --- | --- |
| Relative integral and density error | 1.9e-4 | 5.2e-4 |
| Direction error | 1.8e-4 | 1.5e-3 |

These errors are larger than the projected-solid-angle port's, because the GPU
rounds the transformed vertices differently.

Below 1e-4, a sliver can hug the transformed polygon's horizon so closely that
its width in `1 - x² - y²` is less than one float32 step (2⁻²⁴). The lift
`z = sqrt(1 - x² - y²)` then resolves `z` only in steps of about 2.4e-4. A GPU
sample one ulp away from the C++ sample can round onto the horizon, where the
density is zero. The sliver's sector areas are differences of O(1) angles, so a
quarter-ulp change in the arctangent can move such a sample by about 2°. On
Metal, one of 889 sliver samples lands on the horizon. The float64 ideal sample
of that case lies between the C++ sample and the GPU sample. The test accepts
such samples only for slivers within `sqrt(8 · 2⁻²⁴)` of the horizon, and only
for at most 1% of sliver samples. The Vulkan/SPIR-V and WGSL builds
compile but are first executed in CI.

## Shading normal clipping

`shading_normal.hpp` clips a shading normal to the hemisphere of the outgoing
(view) direction. It implements one sentence of Christoph Peters, "BRDF Importance
Sampling for Polygonal Lights", section 6.1
([DOI](https://doi.org/10.1145/3450626.3459672)), from its text: "Shading normals
get clipped to the hemisphere of the outgoing light direction to avoid black
pixels due to normal mapping." A normal-mapped normal can face away from the
viewer although the surface faces it, and the BSDF is then zero.

```cpp
#include <ludus/foundation/math/shading_normal.hpp>

// normal and view are unit vectors; view points from the surface to the viewer.
Vector3 clipped;
if (TryClipShadingNormalToOutgoingHemisphere(normal, view, clipped) == MathStatus::Success)
{
    // Build the shading frame, sample and evaluate the BSDF with `clipped`.
}
```

The paper names neither the construction nor a margin. Ludus's choices:

- A normal whose cosine with the view is at least
  `SHADING_NORMAL_MINIMUM_OUTGOING_COSINE` (0.01) is returned unchanged.
- Otherwise the normal moves along the view direction until that cosine is
  0.01, and is renormalized. The result is continuous in both inputs; its cosine
  with the view is at least 0.01/√1.0001, and a normal facing directly away
  becomes the view direction.
- The margin is not zero because the BSDF is zero for a view on the shading
  horizon.

Unit-length inputs are a precondition that is not detected. A nonfinite input
returns `NonFiniteInput` and leaves the output unchanged.

The shader port `shaders/shading_normal.slang`
(`ClipShadingNormalToOutgoingHemisphere`) is installed beside `math.slang`, and
render graphs that declare `polygonal_light_sampling` include it. Whether a back
face is clipped is the caller's decision: the
[polygonal-light](../../development/render-graph-authoring.md#compare-brdf-importance-sampling-for-polygonal-lights)
and [RIS](../../development/render-graph-authoring.md#resample-many-area-lights-with-projected-solid-angle-sampling)
templates clip front faces only.

**Evidence.** `shading_normal_tests.cpp` checks a facing normal, the margin
itself, a normal facing away, a grazing normal and nonfinite inputs. It compares
4096 random pairs of unit vectors with a float64 evaluation of the same
construction, within 10⁻⁶ divided by the length of `n + (m − c)·v`: that sum
cancels for a normal facing nearly away, and normalization magnifies its float32
rounding. The clipped cosine equals `m / √(1 + m² − c²)`, which is smallest at
`c = 0`. The renderer's GPU tests compare the shader port with the C++ function:
the RIS weight kernel on 4096 views below the shading horizon within 10⁻⁵ per
component, and the polygonal-light estimators kernel through its CPU oracle.

## GPU transfer

CPU value layout is **not** a shader ABI. Use explicit flat-column export helpers
(no `reinterpret_cast` of a `Vector3`/`Matrix3`/`Affine3` into a buffer struct).
Depth clear/compare selection, the single Vulkan Y adaptation, and per-buffer
uniform/storage layout belong to the consuming graphics code. See the shader
fixture plan under `docs/architecture/math-evidence/`.

## PCG provenance and license

The PCG32 (XSH-RR) algorithm and seeding sequence in `random.hpp`/`random.cpp`
are adapted from Melissa O'Neill's PCG reference `pcg-c-basic`
(<https://www.pcg-random.org/>), distributed under the Apache License 2.0. Only
the small integer algorithm is reimplemented in Ludus style; no source file is
copied. The frozen known-answer sequence for `seed=42, selector=54` is
`a15c02b7, 7b47f409, ba1d3330, 83d2f293, bfa4784b, cbed606e`.

## Reference-rendering helpers

`DecodeSrgb` / `EncodeSrgb` implement the W3C CSS Color 4 §10.2 transfer for
finite channels in [0,1]; outside that domain they return NaN. Alpha is not sRGB.
`NextFloatDown` returns the adjacent lower FP32 value (IEEE infinities/NaNs follow
the math boundary). `TryOffsetRayOrigin` accepts finite position, nonnegative
component error and unit geometric normal/direction, projects error onto the
oriented normal and rounds each changed component outward. Failure preserves
output. It follows PBRT 4e §6.1, with a minimum normal step near zero for GPU FTZ;
callers must supply conservative reconstruction error, not a scene-sized epsilon.
The [RT1 profile](../../architecture/renderer-systems.md#l6-implemented-scope-rt0-and-rt1)
records its actual reconstruction allowance and thin-geometry limitations.
