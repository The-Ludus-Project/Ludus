// Linearly transformed cosines over convex polygons (Heitz, Dupuy, Hill and
// Neubelt 2016, sections 3-4; Peters 2021, section 4.1). Implemented from the
// papers' text. The shader port in shaders/linearly_transformed_cosine.slang
// evaluates the same operations and classifies floats on their bits, because
// Metal compiles shaders with fast math.

#include <ludus/foundation/math/linearly_transformed_cosine.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <cmath>
#include <cstring>

namespace ludus::foundation::math
{
namespace
{
using core::uint32;

// IEEE binary32 fields.
constexpr uint32 FLOAT32_EXPONENT_BITS = 0x7f800000u;
constexpr uint32 FLOAT32_MANTISSA_BIT_COUNT = 23;
constexpr uint32 FLOAT32_SMALLEST_NORMAL_BITS = 0x00800000u;
// A largest-magnitude entry with this biased exponent or above (2^127) has no normal power-of-two scale into [1, 2).
constexpr uint32 MAX_SCALABLE_BIASED_EXPONENT = 253;
// The biased exponent of 2^k is 127 + k, so the scale 2^(127 - e), which moves biased exponent e to 127 (the
// interval [1, 2)), has biased exponent 254 - e.
constexpr uint32 TWICE_FLOAT32_EXPONENT_BIAS = 254;

[[nodiscard]] uint32 GetBits(float32 value) noexcept
{
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
[[nodiscard]] float32 MakeFloat(uint32 bits) noexcept
{
    float32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
[[nodiscard]] bool IsFiniteBits(float32 value) noexcept
{
    return (GetBits(value) & FLOAT32_EXPONENT_BITS) != FLOAT32_EXPONENT_BITS;
}
/// True for a positive normal value: excludes zeros, subnormals (which GPUs may flush), negatives and nonfinite values.
[[nodiscard]] bool IsPositiveNormalBits(float32 value) noexcept
{
    const uint32 bits = GetBits(value);
    return bits >= FLOAT32_SMALLEST_NORMAL_BITS && bits < FLOAT32_EXPONENT_BITS;
}
[[nodiscard]] float32 GetLargestMagnitude(Vector3 value) noexcept
{
    return Max(Abs(value.X), Max(Abs(value.Y), Abs(value.Z)));
}
/// p(w) times pi divided by normalization: max(0, (M^-1 w).z) |M^-1| / |M^-1 w|^4, or zero.
[[nodiscard]] float32 EvaluateCosineJacobian(const LinearlyTransformedCosine& distribution,
                                             Vector3 directionTangent) noexcept
{
    if (IsFinite(directionTangent) == false)
    {
        return 0.0F;
    }
    const Vector3 cosineDirection = distribution.InverseTransform * directionTangent;
    const float32 lengthSquared = Dot(cosineDirection, cosineDirection);
    if ((cosineDirection.Z > 0.0F) == false || IsPositiveNormalBits(lengthSquared) == false)
    {
        return 0.0F;
    }
    const float32 inverseLength = 1.0F / std::sqrt(lengthSquared);
    const float32 cosine = cosineDirection.Z * inverseLength;
    // Equation 1: the clamped cosine times the Jacobian |M^-1| / |M^-1 w|^3.
    const float32 result = cosine * (distribution.InverseDeterminant * (inverseLength * inverseLength * inverseLength));
    return IsFiniteBits(result) == true ? result : 0.0F;
}

// Hill and Heitz 2016, slide 77: theta / sin(theta) ~ a + (b + c y) y with y = |cos(theta)| on [0, 1]; an obtuse edge
// uses pi / sin(theta) minus the acute value.
constexpr float32 EDGE_RATIO_FIT_CONSTANT = 1.5708F;
constexpr float32 EDGE_RATIO_FIT_LINEAR = -0.879406F;
constexpr float32 EDGE_RATIO_FIT_QUADRATIC = 0.308609F;
// sin^2(theta) floor (2^-23) of an obtuse edge's reciprocal. Only an edge within rounding of pi reaches it, where the
// receiver lies on the edge's line and the edge's cross product, which scales the ratio, vanishes.
constexpr float32 MIN_EDGE_SINE_SQUARED = 1.1920928955078125e-7F;
constexpr float32 INVERSE_TWO_PI = 0.5F / kPiF;

/// The unit direction of a vertex, or zero when its squared length is not a positive normal float.
[[nodiscard]] Vector3 NormalizeVertexDirection(Vector3 positionTangent) noexcept
{
    const float32 lengthSquared = Dot(positionTangent, positionTangent);
    if (IsPositiveNormalBits(lengthSquared) == false)
    {
        return {};
    }
    return positionTangent * (1.0F / std::sqrt(lengthSquared));
}
/// theta / sin(theta) times d0 x d1 for the edge between unit directions d0 and d1 (slides 72-77).
[[nodiscard]] Vector3 IntegrateApproximateEdge(Vector3 direction0, Vector3 direction1) noexcept
{
    const float32 cosine = Dot(direction0, direction1);
    const float32 magnitude = Abs(cosine);
    float32 ratio =
        EDGE_RATIO_FIT_CONSTANT + (EDGE_RATIO_FIT_LINEAR + EDGE_RATIO_FIT_QUADRATIC * magnitude) * magnitude;
    if (cosine < 0.0F)
    {
        ratio = kPiF / std::sqrt(Max(1.0F - cosine * cosine, MIN_EDGE_SINE_SQUARED)) - ratio;
    }
    return Cross(direction0, direction1) * ratio;
}
} // namespace

MathStatus TryMakeLinearlyTransformedCosine(const Matrix3& inverseTransform,
                                            LinearlyTransformedCosine& outDistribution) noexcept
{
    if (IsFinite(inverseTransform) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    // The distribution depends only on the direction of M^-1 w (Heitz et al. 2016, section 3.1), so scale M^-1 by an
    // exact power of two that moves its largest-magnitude entry into [1, 2). The determinant and the adjugate then stay
    // well inside single precision.
    const float32 largestMagnitude =
        Max(GetLargestMagnitude(inverseTransform.Columns[0]),
            Max(GetLargestMagnitude(inverseTransform.Columns[1]), GetLargestMagnitude(inverseTransform.Columns[2])));
    const uint32 biasedExponent = (GetBits(largestMagnitude) & FLOAT32_EXPONENT_BITS) >> FLOAT32_MANTISSA_BIT_COUNT;
    if (biasedExponent == 0 || biasedExponent > MAX_SCALABLE_BIASED_EXPONENT)
    {
        return MathStatus::Degenerate;
    }
    const float32 scale = MakeFloat((TWICE_FLOAT32_EXPONENT_BIAS - biasedExponent) << FLOAT32_MANTISSA_BIT_COUNT);
    LinearlyTransformedCosine result;
    result.InverseTransform = Matrix3{{
        inverseTransform.Columns[0] * scale,
        inverseTransform.Columns[1] * scale,
        inverseTransform.Columns[2] * scale,
    }};
    const Vector3 column0 = result.InverseTransform.Columns[0];
    const Vector3 column1 = result.InverseTransform.Columns[1];
    const Vector3 column2 = result.InverseTransform.Columns[2];
    // The rows of the adjugate are the cross products of column pairs; their dot product with the remaining column is
    // the determinant.
    const Vector3 row0 = Cross(column1, column2);
    const Vector3 row1 = Cross(column2, column0);
    const Vector3 row2 = Cross(column0, column1);
    const float32 determinant = Dot(column0, row0);
    if ((determinant > 0.0F) == false)
    {
        return MathStatus::Degenerate;
    }
    if (determinant < MIN_LINEARLY_TRANSFORMED_COSINE_DETERMINANT)
    {
        return MathStatus::IllConditioned;
    }
    result.Transform = Transpose(Matrix3{{row0, row1, row2}});
    result.InverseDeterminant = determinant;
    outDistribution = result;
    return MathStatus::Success;
}

float32 EvaluateLinearlyTransformedCosine(const LinearlyTransformedCosine& distribution,
                                          Vector3 directionTangent) noexcept
{
    return EvaluateCosineJacobian(distribution, directionTangent) / kPiF;
}

MathStatus TryPrepareLinearlyTransformedCosinePolygon(const ProjectedPolygonInput& polygon,
                                                      const LinearlyTransformedCosine& distribution,
                                                      LinearlyTransformedCosinePolygon& outPolygon) noexcept
{
    if (polygon.VertexCount < MIN_PROJECTED_POLYGON_VERTICES ||
        polygon.VertexCount > MAX_PROJECTED_POLYGON_INPUT_VERTICES)
    {
        return MathStatus::InvalidArgument;
    }
    // Equation 3 of Heitz et al.: the LTC integral over P is the clamped-cosine integral over M^-1 P. Peters (section
    // 4.1) clips only the transformed polygon.
    ProjectedPolygonInput transformed;
    transformed.VertexCount = polygon.VertexCount;
    for (uint32 i = 0; i < polygon.VertexCount; ++i)
    {
        if (IsFinite(polygon.PositionsTangent[i]) == false)
        {
            return MathStatus::NonFiniteInput;
        }
        transformed.PositionsTangent[i] = distribution.InverseTransform * polygon.PositionsTangent[i];
    }
    LinearlyTransformedCosinePolygon result;
    const MathStatus status = TryPrepareProjectedSolidAnglePolygon(transformed, result.TransformedPolygon);
    if (status != MathStatus::Success)
    {
        return status;
    }
    result.Distribution = distribution;
    outPolygon = result;
    return MathStatus::Success;
}

float32 GetLinearlyTransformedCosinePolygonIntegral(const LinearlyTransformedCosinePolygon& polygon) noexcept
{
    return polygon.TransformedPolygon.ProjectedSolidAngle / kPiF;
}

MathStatus TryApproximateClampedCosinePolygonIntegral(const ProjectedPolygonInput& polygon,
                                                      ApproximatePolygonIntegral& outIntegral) noexcept
{
    if (polygon.VertexCount < MIN_PROJECTED_POLYGON_VERTICES ||
        polygon.VertexCount > MAX_PROJECTED_POLYGON_INPUT_VERTICES)
    {
        return MathStatus::InvalidArgument;
    }
    float32 largestMagnitude = 0.0F;
    for (uint32 i = 0; i < polygon.VertexCount; ++i)
    {
        if (IsFinite(polygon.PositionsTangent[i]) == false)
        {
            return MathStatus::NonFiniteInput;
        }
        largestMagnitude = Max(largestMagnitude, GetLargestMagnitude(polygon.PositionsTangent[i]));
    }
    ApproximatePolygonIntegral result;
    if ((largestMagnitude > 0.0F) == false)
    {
        outIntegral = result;
        return MathStatus::Success;
    }
    // Only directions matter; the common scale keeps the largest vertex's squared length normal.
    Vector3 directions[MAX_PROJECTED_POLYGON_INPUT_VERTICES]{};
    bool hasVertexAboveHorizon = false;
    bool hasVertexBelowHorizon = false;
    for (uint32 i = 0; i < polygon.VertexCount; ++i)
    {
        directions[i] = NormalizeVertexDirection(polygon.PositionsTangent[i] / largestMagnitude);
        hasVertexAboveHorizon = hasVertexAboveHorizon || directions[i].Z > 0.0F;
        hasVertexBelowHorizon = hasVertexBelowHorizon || directions[i].Z < 0.0F;
    }
    // The sign of the triple product is the polygon's winding as seen from the origin; zero means the polygon's plane
    // contains the origin (or a vertex is the origin), where the clipped integral is zero too.
    const float32 tripleProduct = Dot(directions[0], Cross(directions[1], directions[2]));
    if ((tripleProduct > 0.0F || tripleProduct < 0.0F) == false)
    {
        outIntegral = result;
        return MathStatus::Success;
    }
    // Slides 90-92: the edge integrals without the projection onto the normal, oriented towards the polygon. The
    // fitted edge ratio can carry an integral near one slightly past it, so integrals are clamped to [0, 1].
    Vector3 vectorFormFactor{};
    for (uint32 i = 0; i < polygon.VertexCount; ++i)
    {
        const uint32 next = i + 1 < polygon.VertexCount ? i + 1 : 0;
        vectorFormFactor = vectorFormFactor + IntegrateApproximateEdge(directions[i], directions[next]);
    }
    result.VectorFormFactor = vectorFormFactor * (tripleProduct > 0.0F ? INVERSE_TWO_PI : -INVERSE_TWO_PI);
    result.UnitTripleProduct = Abs(tripleProduct);
    if (hasVertexAboveHorizon == true && hasVertexBelowHorizon == false)
    {
        result.Integral = Min(Max(result.VectorFormFactor.Z, 0.0F), 1.0F);
    }
    else if (hasVertexAboveHorizon == true)
    {
        // Slide 102: the horizon-clipped form factor of the sphere with the same vector form factor.
        const float32 length = std::sqrt(Dot(result.VectorFormFactor, result.VectorFormFactor));
        result.Integral = Min(Max((length * length + result.VectorFormFactor.Z) / (length + 1.0F), 0.0F), 1.0F);
    }
    outIntegral = result;
    return MathStatus::Success;
}

MathStatus TryApproximateLinearlyTransformedCosinePolygonIntegral(const ProjectedPolygonInput& polygon,
                                                                  const LinearlyTransformedCosine& distribution,
                                                                  ApproximatePolygonIntegral& outIntegral) noexcept
{
    if (polygon.VertexCount < MIN_PROJECTED_POLYGON_VERTICES ||
        polygon.VertexCount > MAX_PROJECTED_POLYGON_INPUT_VERTICES)
    {
        return MathStatus::InvalidArgument;
    }
    // Equation 3 of Heitz et al. 2016: the LTC integral over P is the clamped-cosine integral over M^-1 P.
    ProjectedPolygonInput transformed;
    transformed.VertexCount = polygon.VertexCount;
    for (uint32 i = 0; i < polygon.VertexCount; ++i)
    {
        if (IsFinite(polygon.PositionsTangent[i]) == false)
        {
            return MathStatus::NonFiniteInput;
        }
        transformed.PositionsTangent[i] = distribution.InverseTransform * polygon.PositionsTangent[i];
    }
    return TryApproximateClampedCosinePolygonIntegral(transformed, outIntegral);
}

MathStatus TrySampleLinearlyTransformedCosinePolygon(const LinearlyTransformedCosinePolygon& polygon,
                                                     Vector2 randomNumbers,
                                                     Vector3& outDirectionTangent) noexcept
{
    Vector3 cosineDirection;
    const MathStatus status =
        TrySampleProjectedSolidAnglePolygon(polygon.TransformedPolygon, randomNumbers, cosineDirection);
    if (status != MathStatus::Success)
    {
        return status;
    }
    // Algorithm 1 of Heitz et al.: w = M w_o / |M w_o|. The adjugate is a positive multiple of M.
    const Vector3 direction = polygon.Distribution.Transform * cosineDirection;
    const float32 lengthSquared = Dot(direction, direction);
    if (IsPositiveNormalBits(lengthSquared) == false)
    {
        return MathStatus::IllConditioned;
    }
    const Vector3 result = direction * (1.0F / std::sqrt(lengthSquared));
    if (IsFinite(result) == false)
    {
        return MathStatus::IllConditioned;
    }
    outDirectionTangent = result;
    return MathStatus::Success;
}

float32 EvaluateLinearlyTransformedCosinePolygonDensity(const LinearlyTransformedCosinePolygon& polygon,
                                                        Vector3 directionTangent) noexcept
{
    const float32 projectedSolidAngle = polygon.TransformedPolygon.ProjectedSolidAngle;
    if ((projectedSolidAngle > 0.0F) == false)
    {
        return 0.0F;
    }
    // p(w) / (A / pi), where A is the transformed polygon's projected solid angle: the factors of pi cancel.
    const float32 result = EvaluateCosineJacobian(polygon.Distribution, directionTangent) / projectedSolidAngle;
    return IsFiniteBits(result) == true ? result : 0.0F;
}
} // namespace ludus::foundation::math
