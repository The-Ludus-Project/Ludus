#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/status.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::math
{
/// @brief Exact coefficients for dv/dt = -rate * (v - constantTargetVelocity).
///
/// displacement = target * dt + (velocity - target) * DistanceScale.
/// velocityNext = target + (velocity - target) * VelocityScale.
struct LinearDragStep final
{
    float64 VelocityScale = 1.0;
    float64 DistanceScale = 0.0;
};

/// @brief Finite nonnegative rate/dt.
///
/// Zero rate gives {1, dt}; tiny rates retain displacement precision. Outputs are unchanged on failure; no
/// allocation.
[[nodiscard]] MathStatus TryComputeLinearDragStep(float64 rate, float64 dt, LinearDragStep& out) noexcept;

/// @brief Interpolation weight of one exponential-damping step with a half-life.
///
/// Computes `1 - exp(-ln(2) * deltaSeconds / halfLifeSeconds)` through `expm1`, so very small steps keep their
/// precision. A zero half-life is an explicit snap (weight 1), and a zero delta advances nothing (weight 0, even with
/// a zero half-life). Repeated steps compose exactly in real arithmetic: two half steps leave the same remaining
/// fraction as one full step.
/// @param halfLifeSeconds Finite nonnegative seconds after which half of a held error remains.
/// @param deltaSeconds Finite nonnegative elapsed seconds.
/// @param outWeight Receives a weight in [0, 1] on Success; unchanged on failure.
/// @return NonFiniteInput or InvalidArgument (negative input); outputs are preserved. No allocation.
[[nodiscard]] MathStatus
TryComputeExponentialDampingWeight(float64 halfLifeSeconds, float64 deltaSeconds, float64& outWeight) noexcept;

/// @brief Coefficients of one exact critically damped step toward a held target.
///
/// With displacement `e = x - target`, velocity `v`, `omega = 2 / smoothTimeSeconds`, `c = v + omega * e` and
/// `b = exp(-omega * dt)`, the step is `eNext = (e + c * dt) * b` and `vNext = (v - omega * c * dt) * b`.
/// The smooth time is a response convention, not a deadline at which the target is reached.
struct CriticalSpringStep final
{
    /// Angular frequency `2 / smoothTimeSeconds` in 1/seconds; zero for a snap.
    float64 Omega = 0.0;
    /// Elapsed seconds of this step.
    float64 DeltaSeconds = 0.0;
    /// `exp(-Omega * DeltaSeconds)`, in [0, 1].
    float64 Decay = 1.0;
    /// True when the smooth time was zero: the step clears displacement and velocity.
    bool IsSnap = false;
};

/// @brief Prepare one critically damped step, shared by every channel with the same frequency and delta.
/// @param smoothTimeSeconds Finite nonnegative response time; zero snaps.
/// @param deltaSeconds Finite nonnegative elapsed seconds; zero advances nothing, even for a snap.
/// @param outStep Replaced only on Success.
/// @return NonFiniteInput, InvalidArgument (negative input) or OutOfRange (a smooth time so small that its frequency
/// is not representable); failure preserves output. No allocation.
[[nodiscard]] MathStatus
TryComputeCriticalSpringStep(float64 smoothTimeSeconds, float64 deltaSeconds, CriticalSpringStep& outStep) noexcept;

/// @brief Position and velocity of a critically damped channel, relative to its held target.
struct CriticalSpringState final
{
    /// Current position minus target, in world units.
    Vector3d Displacement{};
    /// Current velocity in world units per second.
    Vector3d Velocity{};
};

/// @brief Advance a displacement from a held target and its velocity by one prepared critical step.
/// @param step Coefficients from TryComputeCriticalSpringStep.
/// @param current Finite displacement and velocity before the step.
/// @param outState Receives the state after the step on Success; may alias the caller's current state.
/// @return NonFiniteInput for nonfinite inputs, OutOfRange when a product overflows; the output is preserved on
/// failure. A zero delta returns the input unchanged. No allocation.
/// @note Thanks to Thomas Lowe, "Critically Damped Ease-In/Ease-Out Smoothing", Game Programming Gems 4,
/// section 1.10, pp. 95-101, for the analytic step; Ludus evaluates exact exponentials rather than Lowe's rational
/// approximation. See "Damping and continuity" in docs/architecture/camera-systems.md.
[[nodiscard]] MathStatus TryAdvanceCriticalSpring(const CriticalSpringStep& step,
                                                  CriticalSpringState current,
                                                  CriticalSpringState& outState) noexcept;

struct RadialSweepContact final
{
    bool Hit = false;
    float64 Fraction = 0.0;
    float64 Radius = 0.0;
    Vector3d Normal; ///< Outward unit normal; zero at a coincident center.
};

/// @brief Earliest overlap of a linearly moving point and a radial band whose radius
/// interpolates from radiusStart to radiusEnd over the SAME normalized interval.
///
/// halfWidth includes any hull radius. Supports expansion/shrinkage and initial
/// overlap. Finite coordinates, nonnegative finite radii/width required. A miss
/// is Success with Hit=false. Numeric range failures leave out unchanged.
/// Use Z=0 for 2D annuli. This is geometry, not a force or fluid integrator.
[[nodiscard]] MathStatus TrySweepRadialBand(Vector3d start,
                                            Vector3d end,
                                            Vector3d origin,
                                            float64 radiusStart,
                                            float64 radiusEnd,
                                            float64 halfWidth,
                                            RadialSweepContact& out) noexcept;
} // namespace ludus::foundation::math
