# Checked drag and radial contact kernels

Link `Ludus::FoundationMath` and include
`<ludus/foundation/math/dynamics.hpp>` from the installed SDK. Both functions
return `MathStatus`, allocate nothing, are noexcept, and leave caller outputs
unchanged on failure. A geometric miss is Success with `Hit=false`.

```cpp
namespace math = ludus::foundation::math;
math::LinearDragStep step;
if (math::IsSuccess(math::TryComputeLinearDragStep(0.7, tickSeconds, step)))
{
    // Constant target velocity u. Position/velocity are game-owned vectors.
    position = position + u * tickSeconds + (velocity - u) * step.DistanceScale;
    velocity = u + (velocity - u) * step.VelocityScale;
}
math::RadialSweepContact contact;
if (math::IsSuccess(math::TrySweepRadialBand(start, end, center,
                                          radiusStart, radiusEnd,
                                          hullRadius + ringHalfWidth, contact)) && contact.Hit)
{
    // Fraction is in [0,1], Normal points away from center (zero at center).
    // Game decides whether/how to apply an impulse and remembers consumption.
}
```

Drag inputs are finite and nonnegative. Zero rate is ballistic; zero duration
preserves velocity and yields zero displacement. Coefficients may be cached
until rate or tick duration changes. The target velocity is constant over the
interval; the caller owns unit consistency, bounded state, and speed limiting.

The sweep is a point against an annular/spherical radial band. Use Vector3d with
Z=0 for a 2D ring. Endpoint radii and width must be finite/nonnegative; shrinking
bands are supported. Linear motion and linear radius share the same interval.
Initial overlap and tangency count as contact. Geometry is scaled before root
solving; a 64-machine-epsilon tolerance in normalized coordinates accommodates
roundoff, so near-boundary classifications have that relative uncertainty.
Coordinate differences that overflow, or radii/width lost during normalization,
return OutOfRange. This is not an arbitrary-precision geometry predicate.

An exponential drag trajectory needs a chord approximation or subdivision to
use this linear sweep. Do not silently interpret its fraction as an exact time
on a nonlinear path. See the [simulation design](../architecture/physics-fluid-simulation.md)
and [reference review](../architecture/physics-fluid-reference-review.md).

## Presentation damping kernels

The same header provides the two damping kernels used by
[gameplay cameras](../architecture/camera-systems.md#damping-and-continuity).
`TryComputeExponentialDampingWeight(halfLife, dt, weight)` returns
`1 - exp(-ln(2) dt / halfLife)` through `expm1`; a zero half-life snaps
(weight 1) and a zero delta advances nothing (weight 0). Use the weight with
vector interpolation or `SlerpShortest`.

```cpp
math::CriticalSpringStep step;
math::CriticalSpringState spring;
if (math::IsSuccess(math::TryComputeCriticalSpringStep(smoothTimeSeconds, dt, step)) &&
    math::IsSuccess(math::TryAdvanceCriticalSpring(
        step, { .Displacement = position - target, .Velocity = velocity }, spring)))
{
    position = target + spring.Displacement;
    velocity = spring.Velocity;
}
```

The critical spring is the exact step toward a held target, with
`omega = 2 / smoothTimeSeconds`. Steps subdivide exactly in real arithmetic, so
different render rates reach the same state for a held target; a moving target
sampled per frame differs by its sampling. A zero smooth time snaps and clears
velocity. Inputs are finite and nonnegative; overflowing products return
OutOfRange and preserve both outputs. One step can be shared by every channel
with the same frequency and delta.

## Validation record

On October 3, 2026, native Development completed 47 CTest entries: 45 passed and two
live Wayland/keyboard entries were explicitly skipped. ASan/UBSan completed 41
entries: 39 passed with the same two live-platform skips. The numerical suite passed 495 assertions
across seven cases; the allocation probe passed 14 assertions. Native/browser
warning-clean builds, public header/include gates, pinned clang-format 18,
clang-tidy 18, SDK dependency/manifest audits and installed consumer execution
passed. Existing pinned tools/dependencies were reused in an isolated checkout;
the tidy runner was invoked against its configured compile database because the
reused dependency bootstrap fingerprint was stale after changing the source
manifest. Generated-source analysis was completed after building its module maps.

The companion Sandbox tests exercise the production SDK and an independent
bounded-world reference. Browser smoke evidence uses Chromium SwiftShader and
emulated touch; it does not certify physical mobile hardware or native rendering.
