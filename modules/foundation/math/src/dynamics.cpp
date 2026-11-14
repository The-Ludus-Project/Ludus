#include <ludus/foundation/math/dynamics.hpp>

#include <ludus/foundation/math/scalar.hpp>

#include <cmath>
#include <limits>

namespace ludus::foundation::math
{
namespace
{
constexpr float64 kRoundoff = 64.0 * std::numeric_limits<float64>::epsilon();
/// ln(2), converting a half-life into an exponential rate.
constexpr float64 NATURAL_LOG_OF_TWO = 0.693147180559945309417;

[[nodiscard]] Vector3d Divide(Vector3d value, float64 divisor) noexcept
{
    return {value.X / divisor, value.Y / divisor, value.Z / divisor};
}

[[nodiscard]] float64 MaxComponent(Vector3d value) noexcept
{
    return Max(Abs(value.X), Max(Abs(value.Y), Abs(value.Z)));
}
} // namespace

MathStatus TryComputeLinearDragStep(float64 rate, float64 dt, LinearDragStep& out) noexcept
{
    if (IsFinite(rate) == false || IsFinite(dt) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (rate < 0.0 || dt < 0.0)
    {
        return MathStatus::InvalidArgument;
    }
    LinearDragStep result;
    result.DistanceScale = dt;
    if (rate > 0.0 && dt > 0.0)
    {
        // An overflowing exponent is the limiting solution, not an error.
        if (dt > 1.0 && rate > std::numeric_limits<float64>::max() / dt)
        {
            result.VelocityScale = 0.0;
            result.DistanceScale = 1.0 / rate;
        }
        else
        {
            const float64 exponent = rate * dt;
            if (exponent > 0.0)
            {
                result.VelocityScale = std::exp(-exponent);
                result.DistanceScale = -std::expm1(-exponent) / rate;
            }
        }
    }
    out = result;
    return MathStatus::Success;
}

MathStatus
TryComputeExponentialDampingWeight(float64 halfLifeSeconds, float64 deltaSeconds, float64& outWeight) noexcept
{
    if (IsFinite(halfLifeSeconds) == false || IsFinite(deltaSeconds) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (halfLifeSeconds < 0.0 || deltaSeconds < 0.0)
    {
        return MathStatus::InvalidArgument;
    }
    if (deltaSeconds == 0.0)
    {
        outWeight = 0.0;
        return MathStatus::Success;
    }
    if (halfLifeSeconds == 0.0)
    {
        outWeight = 1.0;
        return MathStatus::Success;
    }
    // ln(2) * dt / h may overflow for a huge delta over a tiny half-life; the limit is a full snap.
    const float64 ratio = deltaSeconds / halfLifeSeconds;
    const float64 weight = IsFinite(ratio) == true ? -std::expm1(-NATURAL_LOG_OF_TWO * ratio) : 1.0;
    outWeight = Clamp(weight, 0.0, 1.0);
    return MathStatus::Success;
}

MathStatus
TryComputeCriticalSpringStep(float64 smoothTimeSeconds, float64 deltaSeconds, CriticalSpringStep& outStep) noexcept
{
    if (IsFinite(smoothTimeSeconds) == false || IsFinite(deltaSeconds) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (smoothTimeSeconds < 0.0 || deltaSeconds < 0.0)
    {
        return MathStatus::InvalidArgument;
    }
    CriticalSpringStep result{ .DeltaSeconds = deltaSeconds };
    if (smoothTimeSeconds == 0.0)
    {
        result.IsSnap = true;
        outStep = result;
        return MathStatus::Success;
    }
    result.Omega = 2.0 / smoothTimeSeconds;
    if (IsFinite(result.Omega) == false)
    {
        return MathStatus::OutOfRange;
    }
    const float64 exponent = result.Omega * deltaSeconds;
    result.Decay = IsFinite(exponent) == true ? std::exp(-exponent) : 0.0;
    outStep = result;
    return MathStatus::Success;
}

MathStatus TryAdvanceCriticalSpring(const CriticalSpringStep& step,
                                    Vector3d displacement,
                                    Vector3d velocity,
                                    Vector3d& outDisplacement,
                                    Vector3d& outVelocity) noexcept
{
    if (IsFinite(displacement) == false || IsFinite(velocity) == false || IsFinite(step.Omega) == false ||
        IsFinite(step.DeltaSeconds) == false || IsFinite(step.Decay) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (step.DeltaSeconds == 0.0)
    {
        outDisplacement = displacement;
        outVelocity = velocity;
        return MathStatus::Success;
    }
    if (step.IsSnap == true)
    {
        outDisplacement = {};
        outVelocity = {};
        return MathStatus::Success;
    }
    // Thanks to Thomas Lowe, "Critically Damped Ease-In/Ease-Out Smoothing", Game Programming Gems 4, section 1.10,
    // pp. 95-101, and Ryan Juckett, "Damped Springs" (https://www.ryanjuckett.com/damped-springs/), for the exact
    // constant-target solution. Ludus uses accurate exponentials and checked ranges instead of the rational
    // approximation; reading notes: docs/architecture/camera-systems-gems-review.md.
    const Vector3d coupled = velocity + displacement * step.Omega;
    const Vector3d nextDisplacement = (displacement + coupled * step.DeltaSeconds) * step.Decay;
    const Vector3d nextVelocity = (velocity - coupled * (step.Omega * step.DeltaSeconds)) * step.Decay;
    if (IsFinite(coupled) == false || IsFinite(nextDisplacement) == false || IsFinite(nextVelocity) == false)
    {
        return MathStatus::OutOfRange;
    }
    outDisplacement = nextDisplacement;
    outVelocity = nextVelocity;
    return MathStatus::Success;
}

MathStatus TrySweepRadialBand(Vector3d start,
                              Vector3d end,
                              Vector3d origin,
                              float64 radiusStart,
                              float64 radiusEnd,
                              float64 halfWidth,
                              RadialSweepContact& out) noexcept
{
    if (IsFinite(start) == false || IsFinite(end) == false || IsFinite(origin) == false ||
        IsFinite(radiusStart) == false || IsFinite(radiusEnd) == false || IsFinite(halfWidth) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (radiusStart < 0.0 || radiusEnd < 0.0 || halfWidth < 0.0)
    {
        return MathStatus::InvalidArgument;
    }
    Vector3d relative = start - origin;
    Vector3d travel = end - start;
    if (IsFinite(relative) == false || IsFinite(travel) == false)
    {
        return MathStatus::OutOfRange;
    }
    // Normalize geometry before products, preventing overflow in coefficients.
    const float64 scale =
        Max(Max(MaxComponent(relative), MaxComponent(travel)), Max(Max(radiusStart, radiusEnd), halfWidth));
    RadialSweepContact result;
    if (scale == 0.0)
    {
        result.Hit = true;
        out = result;
        return MathStatus::Success;
    }
    relative = Divide(relative, scale);
    travel = Divide(travel, scale);
    const float64 r0 = radiusStart / scale;
    const float64 r1 = radiusEnd / scale;
    const float64 width = halfWidth / scale;
    if ((halfWidth > 0.0 && width == 0.0) || (radiusStart > 0.0 && r0 == 0.0) || (radiusEnd > 0.0 && r1 == 0.0))
    {
        return MathStatus::OutOfRange;
    }
    const float64 growth = r1 - r0;
    float64 first = 2.0;
    Vector3d normal;
    const auto candidate = [&](float64 fraction) noexcept {
        if (IsFinite(fraction) == false || fraction < -kRoundoff || fraction > 1.0 + kRoundoff)
        {
            return;
        }
        fraction = Clamp(fraction, 0.0, 1.0);
        const Vector3d atHit = relative + travel * fraction;
        const float64 distance = std::hypot(atHit.X, atHit.Y, atHit.Z);
        const float64 radius = r0 * (1.0 - fraction) + r1 * fraction;
        // Verify the unsquared equation, rejecting extraneous signed-radius
        // roots. Roundoff tolerance is relative to the normalized geometry.
        if (Abs(distance - radius) <= width + kRoundoff && fraction < first)
        {
            first = fraction;
            normal = distance > 0.0 ? Divide(atHit, distance) : Vector3d{};
        }
    };
    candidate(0.0);
    if (first != 0.0)
    {
        const float64 offsets[2]{width, -width};
        for (const float64 offset : offsets)
        {
            const float64 radius = r0 + offset;
            const float64 a = Dot(travel, travel) - growth * growth;
            const float64 b = 2.0 * (Dot(relative, travel) - radius * growth);
            const float64 c = Dot(relative, relative) - radius * radius;
            if (a == 0.0)
            {
                if (b != 0.0)
                {
                    candidate(-c / b);
                }
                continue;
            }
            const float64 bb = b * b;
            const float64 ac4 = 4.0 * a * c;
            const float64 discriminant = bb - ac4;
            if (discriminant < -kRoundoff * (bb + Abs(ac4)))
            {
                continue;
            }
            const float64 root = std::sqrt(Max(discriminant, 0.0));
            // q/a and c/q avoid cancellation in the smaller quadratic root.
            const float64 q = -0.5 * (b + std::copysign(root, b));
            if (q == 0.0)
            {
                candidate(-b / (2.0 * a));
            }
            else
            {
                candidate(q / a);
                candidate(c / q);
            }
        }
    }
    if (first <= 1.0)
    {
        result.Hit = true;
        result.Fraction = first;
        result.Radius = radiusStart * (1.0 - first) + radiusEnd * first;
        result.Normal = normal;
    }
    out = result;
    return MathStatus::Success;
}
} // namespace ludus::foundation::math
