// Linearly transformed cosines over polygons (Heitz et al. 2016; Peters 2021,
// section 4.1) checked against an independent float64 oracle. The oracle never
// calls the production transform: it evaluates Equation 1 of Heitz et al.
// directly, integrates the LTC over the polygon by area sampling, transforms and
// clips the polygon itself for Lambert's edge formula, and measures solid angles
// with Van Oosterom and Strackee's triangle formula.

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/linearly_transformed_cosine.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <initializer_list>
#include <limits>

using namespace ludus::foundation::math;
using ludus::foundation::Array;
using ludus::foundation::core::float32;
using ludus::foundation::core::float64;
using ludus::foundation::core::uint32;
using ludus::foundation::core::uint64;
using ludus::foundation::core::usize;

namespace
{
// Room for an input polygon clipped by one plane.
constexpr uint32 MAX_ORACLE_VERTICES = MAX_PROJECTED_POLYGON_INPUT_VERTICES + 1;
constexpr uint64 RANDOM_SEED = 0x17C0516EULL;
constexpr uint32 POLYGON_COUNT = 2000;
constexpr uint32 SAMPLES_PER_POLYGON = 16;
// The sharpest specular lobe the renderer admits: perceptual roughness 0.045, alpha = 0.045^2.
constexpr float64 MINIMUM_ALPHA = 0.045 * 0.045;
// Moderate lobes for the Monte Carlo checks, whose variance grows with the lobe's sharpness.
constexpr float64 MODERATE_MINIMUM_ALPHA = 0.25;
constexpr uint32 STRATA_PER_AXIS = 64;
constexpr uint32 AREA_SAMPLE_COUNT = 1u << 16;
// Inside tests allow this much angular slack (radians) for single-precision samples on a polygon edge.
constexpr float64 EDGE_SLACK = 1.0e-5;
// LTC integrals below this may sample inaccurately in single precision, like the paper's tiny distant polygons.
constexpr float64 NEGLIGIBLE_INTEGRAL = 1.0e-6;
// Polygons per Monte Carlo test; only those that pass each test's conditioning filter are compared.
constexpr uint32 MONTE_CARLO_POLYGON_COUNT = 160;
// The cosine of the support boundary's distance from a polygon whose E[1 / pdf] is checked; keeps 1 / pdf bounded.
constexpr float64 SUPPORT_MARGIN = 0.05;

struct OracleMatrix final
{
    Vector3d Columns[3]{};
};

struct OraclePolygon final
{
    Vector3d Positions[MAX_ORACLE_VERTICES]{};
    uint32 Count = 0;
};

[[nodiscard]] float64 Draw(RandomStream& inoutRandom, float64 low, float64 high) noexcept
{
    return low + (high - low) * static_cast<float64>(inoutRandom.NextFloat01());
}
[[nodiscard]] Vector3d Widen(Vector3 value) noexcept
{
    return Vector3d{static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}
[[nodiscard]] Vector3 Narrow(Vector3d value) noexcept
{
    return Vector3{static_cast<float32>(value.X), static_cast<float32>(value.Y), static_cast<float32>(value.Z)};
}
[[nodiscard]] float64 GetLength(Vector3d value) noexcept
{
    return std::sqrt(Dot(value, value));
}
[[nodiscard]] Vector3d Normalize(Vector3d value) noexcept
{
    return value * (1.0 / GetLength(value));
}
[[nodiscard]] Vector3d Apply(const OracleMatrix& matrix, Vector3d value) noexcept
{
    return matrix.Columns[0] * value.X + matrix.Columns[1] * value.Y + matrix.Columns[2] * value.Z;
}
[[nodiscard]] float64 GetDeterminant(const OracleMatrix& matrix) noexcept
{
    return Dot(matrix.Columns[0], Cross(matrix.Columns[1], matrix.Columns[2]));
}
[[nodiscard]] OracleMatrix Widen(const Matrix3& matrix) noexcept
{
    return OracleMatrix{{Widen(matrix.Columns[0]), Widen(matrix.Columns[1]), Widen(matrix.Columns[2])}};
}
[[nodiscard]] Matrix3 Narrow(const OracleMatrix& matrix) noexcept
{
    return Matrix3{{Narrow(matrix.Columns[0]), Narrow(matrix.Columns[1]), Narrow(matrix.Columns[2])}};
}

/// Equation 1 of Heitz et al. with the clamped cosine of Equation 4, in float64.
[[nodiscard]] float64 EvaluateOracleDensity(const OracleMatrix& inverseTransform, Vector3d direction) noexcept
{
    const Vector3d cosineDirection = Apply(inverseTransform, Normalize(direction));
    const float64 length = GetLength(cosineDirection);
    if (cosineDirection.Z <= 0.0)
    {
        return 0.0;
    }
    return cosineDirection.Z / length * GetDeterminant(inverseTransform) / (length * length * length) / kPi;
}

/// An isotropic BRDF fit's inverse transform: the planar form of Heitz et al. Equation 6 for a view direction in the
/// x-z plane, rotated by a random azimuth about the normal and multiplied by a random positive scale.
[[nodiscard]] Matrix3 MakeRandomInverseTransform(RandomStream& inoutRandom, float64 minimumAlpha) noexcept
{
    const float64 inverseAlphaX = std::exp(Draw(inoutRandom, 0.0, -std::log(minimumAlpha)));
    const float64 inverseAlphaY = std::exp(Draw(inoutRandom, 0.0, -std::log(minimumAlpha)));
    const float64 skewX = Draw(inoutRandom, -0.7, 0.7) * inverseAlphaX;
    const float64 skewZ = Draw(inoutRandom, -0.7, 0.7);
    const OracleMatrix canonical{{
        Vector3d{inverseAlphaX, 0.0, skewZ},
        Vector3d{0.0, inverseAlphaY, 0.0},
        Vector3d{skewX, 0.0, 1.0},
    }};
    // M^-1 = canonical * R^T, where R rotates the canonical frame about z into the tangent frame.
    const float64 azimuth = Draw(inoutRandom, 0.0, kTau);
    const float64 cosine = std::cos(azimuth);
    const float64 sine = std::sin(azimuth);
    const float64 scale = std::exp(Draw(inoutRandom, std::log(0.01), std::log(100.0)));
    const OracleMatrix rotationTransposed{{
        Vector3d{cosine, -sine, 0.0},
        Vector3d{sine, cosine, 0.0},
        Vector3d{0.0, 0.0, 1.0},
    }};
    OracleMatrix result;
    for (uint32 i = 0; i < 3; ++i)
    {
        result.Columns[i] = Apply(canonical, rotationTransposed.Columns[i]) * scale;
    }
    return Narrow(result);
}

/// A random convex polygon: points on an ellipse in a random plane, possibly behind the shading point.
[[nodiscard]] ProjectedPolygonInput MakeRandomPolygon(RandomStream& inoutRandom) noexcept
{
    ProjectedPolygonInput result;
    uint32 count = 0;
    static_cast<void>(inoutRandom.TryNextBounded(MAX_PROJECTED_POLYGON_INPUT_VERTICES - 2, count));
    result.VertexCount = count + 3;
    const float64 cosine = Draw(inoutRandom, -0.4, 1.0);
    const float64 azimuth = Draw(inoutRandom, 0.0, kTau);
    const float64 sine = std::sqrt(1.0 - cosine * cosine);
    const float64 distance = std::exp(Draw(inoutRandom, std::log(0.1), std::log(10.0)));
    const Vector3d center{sine * std::cos(azimuth) * distance, sine * std::sin(azimuth) * distance, cosine * distance};
    const float64 normalCosine = Draw(inoutRandom, -1.0, 1.0);
    const float64 normalAzimuth = Draw(inoutRandom, 0.0, kTau);
    const float64 normalSine = std::sqrt(1.0 - normalCosine * normalCosine);
    const Vector3d normal{normalSine * std::cos(normalAzimuth), normalSine * std::sin(normalAzimuth), normalCosine};
    const Vector3d helper = std::abs(normal.X) < 0.9 ? Vector3d{1.0, 0.0, 0.0} : Vector3d{0.0, 1.0, 0.0};
    const Vector3d tangent = Normalize(Cross(normal, helper));
    const Vector3d bitangent = Cross(normal, tangent);
    const float64 radius = distance * std::exp(Draw(inoutRandom, std::log(0.02), std::log(2.0)));
    const float64 secondRadius = radius * Draw(inoutRandom, 0.3, 1.0);
    const float64 phase = Draw(inoutRandom, 0.0, kTau);
    const bool isReversed = inoutRandom.NextFloat01() < 0.5F;
    for (uint32 i = 0; i < result.VertexCount; ++i)
    {
        const float64 angle =
            phase + (static_cast<float64>(i) + Draw(inoutRandom, 0.0, 0.8)) * kTau / result.VertexCount;
        const uint32 index = isReversed == true ? result.VertexCount - 1 - i : i;
        result.PositionsTangent[index] =
            Narrow(center + tangent * (std::cos(angle) * radius) + bitangent * (std::sin(angle) * secondRadius));
    }
    return result;
}

[[nodiscard]] ProjectedPolygonInput MakePolygon(std::initializer_list<Vector3> positions) noexcept
{
    ProjectedPolygonInput result;
    for (const Vector3& position : positions)
    {
        result.PositionsTangent[result.VertexCount] = position;
        ++result.VertexCount;
    }
    return result;
}

[[nodiscard]] OraclePolygon WidenPolygon(const ProjectedPolygonInput& input) noexcept
{
    OraclePolygon result;
    result.Count = input.VertexCount;
    for (uint32 i = 0; i < input.VertexCount; ++i)
    {
        result.Positions[i] = Widen(input.PositionsTangent[i]);
    }
    return result;
}

/// Keep the part of a planar polygon where Dot(normal, p) >= 0 (the plane passes through the origin).
[[nodiscard]] OraclePolygon ClipByPlane(const OraclePolygon& polygon, Vector3d normal) noexcept
{
    OraclePolygon result;
    for (uint32 i = 0; i < polygon.Count; ++i)
    {
        const Vector3d start = polygon.Positions[i];
        const Vector3d end = polygon.Positions[(i + 1) % polygon.Count];
        const float64 startDistance = Dot(normal, start);
        const float64 endDistance = Dot(normal, end);
        if (startDistance >= 0.0)
        {
            result.Positions[result.Count] = start;
            ++result.Count;
        }
        if ((startDistance > 0.0 && endDistance < 0.0) || (startDistance < 0.0 && endDistance > 0.0))
        {
            const float64 t = startDistance / (startDistance - endDistance);
            result.Positions[result.Count] = start + (end - start) * t;
            ++result.Count;
        }
    }
    result.Count = result.Count >= 3 ? result.Count : 0;
    return result;
}

/// Lambert's formula: the cosine-weighted solid angle of a spherical polygon in the upper hemisphere.
[[nodiscard]] float64 ComputeLambertProjectedSolidAngle(const OraclePolygon& polygon) noexcept
{
    float64 sum = 0.0;
    for (uint32 i = 0; i < polygon.Count; ++i)
    {
        const Vector3d start = Normalize(polygon.Positions[i]);
        const Vector3d end = Normalize(polygon.Positions[(i + 1) % polygon.Count]);
        const Vector3d normal = Cross(start, end);
        const float64 normalLength = GetLength(normal);
        if (normalLength > 0.0)
        {
            sum += std::atan2(normalLength, Dot(start, end)) * normal.Z / normalLength;
        }
    }
    return 0.5 * std::abs(sum);
}

/// Equation 3 of Heitz et al. evaluated independently: transform in float64, clip, Lambert, divide by pi.
[[nodiscard]] float64 ComputeOracleIntegral(const ProjectedPolygonInput& input, const OracleMatrix& inverseTransform)
{
    OraclePolygon transformed = WidenPolygon(input);
    for (uint32 i = 0; i < transformed.Count; ++i)
    {
        transformed.Positions[i] = Apply(inverseTransform, transformed.Positions[i]);
    }
    return ComputeLambertProjectedSolidAngle(ClipByPlane(transformed, Vector3d{0.0, 0.0, 1.0})) / kPi;
}

/// Van Oosterom and Strackee's solid angle of the convex polygon, as a fan of triangles.
[[nodiscard]] float64 ComputeOracleSolidAngle(const OraclePolygon& polygon) noexcept
{
    float64 sum = 0.0;
    for (uint32 i = 1; i + 1 < polygon.Count; ++i)
    {
        const Vector3d a = polygon.Positions[0];
        const Vector3d b = polygon.Positions[i];
        const Vector3d c = polygon.Positions[i + 1];
        const float64 lengthA = GetLength(a);
        const float64 lengthB = GetLength(b);
        const float64 lengthC = GetLength(c);
        sum += 2.0 * std::atan2(std::abs(Dot(a, Cross(b, c))),
                                lengthA * lengthB * lengthC + Dot(a, b) * lengthC + Dot(a, c) * lengthB +
                                    Dot(b, c) * lengthA);
    }
    return sum;
}

/// Whether the ray from the origin in this direction hits the convex polygon, with EDGE_SLACK radians of tolerance.
[[nodiscard]] bool IsInsidePolygon(const OraclePolygon& polygon, Vector3d direction) noexcept
{
    const float64 orientation = Dot(polygon.Positions[0], Cross(polygon.Positions[1], polygon.Positions[2]));
    const Vector3d unit = Normalize(direction);
    for (uint32 i = 0; i < polygon.Count; ++i)
    {
        const Vector3d normal = Cross(polygon.Positions[i], polygon.Positions[(i + 1) % polygon.Count]);
        if ((orientation > 0.0 ? 1.0 : -1.0) * Dot(unit, normal) / GetLength(normal) < -EDGE_SLACK)
        {
            return false;
        }
    }
    return true;
}

float64 GetPercentile(Array<float64>& inoutValues, float64 fraction) noexcept
{
    ludus::foundation::Sort(inoutValues.GetData(), inoutValues.GetSize(), [](float64 a, float64 b) noexcept {
        return a < b;
    });
    return inoutValues[static_cast<usize>(fraction * static_cast<float64>(inoutValues.GetSize() - 1))];
}
} // namespace

TEST_CASE("The identity LTC reproduces projected solid angle sampling", "[math][linearly_transformed_cosine]")
{
    LinearlyTransformedCosine identity;
    REQUIRE(TryMakeLinearlyTransformedCosine(Matrix3::Identity(), identity) == MathStatus::Success);
    REQUIRE(identity.InverseTransform == Matrix3::Identity());
    REQUIRE(identity.Transform == Matrix3::Identity());
    REQUIRE(identity.InverseDeterminant == 1.0F);
    RandomStream random;
    REQUIRE(random.TryReseed(RANDOM_SEED, 1) == true);
    for (uint32 polygonIndex = 0; polygonIndex < 200; ++polygonIndex)
    {
        const ProjectedPolygonInput input = MakeRandomPolygon(random);
        ProjectedSolidAnglePolygon expected;
        LinearlyTransformedCosinePolygon actual;
        REQUIRE(TryPrepareProjectedSolidAnglePolygon(input, expected) == MathStatus::Success);
        REQUIRE(TryPrepareLinearlyTransformedCosinePolygon(input, identity, actual) == MathStatus::Success);
        REQUIRE(actual.TransformedPolygon.ProjectedSolidAngle == expected.ProjectedSolidAngle);
        REQUIRE(GetLinearlyTransformedCosinePolygonIntegral(actual) == expected.ProjectedSolidAngle / kPiF);
        const Vector2 randomNumbers{random.NextFloat01(), random.NextFloat01()};
        Vector3 expectedDirection;
        Vector3 direction;
        const MathStatus expectedStatus =
            TrySampleProjectedSolidAnglePolygon(expected, randomNumbers, expectedDirection);
        REQUIRE(TrySampleLinearlyTransformedCosinePolygon(actual, randomNumbers, direction) == expectedStatus);
        if (expectedStatus == MathStatus::Success)
        {
            REQUIRE(Length(direction - expectedDirection) <= 1.0e-6F);
            const float32 expectedDensity = EvaluateProjectedSolidAnglePolygonDensity(expected, expectedDirection);
            REQUIRE(Abs(EvaluateLinearlyTransformedCosinePolygonDensity(actual, direction) - expectedDensity) <=
                    1.0e-5F * expectedDensity);
        }
    }
}

TEST_CASE("LTCs are invariant to a positive scale of the inverse transform", "[math][linearly_transformed_cosine]")
{
    RandomStream random;
    REQUIRE(random.TryReseed(RANDOM_SEED, 2) == true);
    const ProjectedPolygonInput input = MakePolygon({{1, -1, 0.5F}, {1, -1, 2}, {1, 1, 2}, {1, 1, 0.5F}});
    for (uint32 i = 0; i < 64; ++i)
    {
        const Matrix3 inverseTransform = MakeRandomInverseTransform(random, MINIMUM_ALPHA);
        LinearlyTransformedCosine distribution;
        REQUIRE(TryMakeLinearlyTransformedCosine(inverseTransform, distribution) == MathStatus::Success);
        // A power of two leaves the representation bit-identical.
        for (const float32 scale : {0x1.0p-40F, 0.5F, 8.0F, 0x1.0p+50F})
        {
            const Matrix3 scaled{{inverseTransform.Columns[0] * scale,
                                  inverseTransform.Columns[1] * scale,
                                  inverseTransform.Columns[2] * scale}};
            LinearlyTransformedCosine scaledDistribution;
            REQUIRE(TryMakeLinearlyTransformedCosine(scaled, scaledDistribution) == MathStatus::Success);
            REQUIRE(scaledDistribution.InverseTransform == distribution.InverseTransform);
            REQUIRE(scaledDistribution.Transform == distribution.Transform);
            REQUIRE(scaledDistribution.InverseDeterminant == distribution.InverseDeterminant);
        }
        // Other scales change only rounding.
        const Matrix3 tripled{{inverseTransform.Columns[0] * 3.0F,
                               inverseTransform.Columns[1] * 3.0F,
                               inverseTransform.Columns[2] * 3.0F}};
        LinearlyTransformedCosine tripledDistribution;
        REQUIRE(TryMakeLinearlyTransformedCosine(tripled, tripledDistribution) == MathStatus::Success);
        LinearlyTransformedCosinePolygon polygon;
        LinearlyTransformedCosinePolygon tripledPolygon;
        REQUIRE(TryPrepareLinearlyTransformedCosinePolygon(input, distribution, polygon) == MathStatus::Success);
        REQUIRE(TryPrepareLinearlyTransformedCosinePolygon(input, tripledDistribution, tripledPolygon) ==
                MathStatus::Success);
        const float32 integral = GetLinearlyTransformedCosinePolygonIntegral(polygon);
        REQUIRE(Abs(GetLinearlyTransformedCosinePolygonIntegral(tripledPolygon) - integral) <=
                1.0e-6F + 1.0e-4F * integral);
        const Vector3 direction = Vector3{1.0F, 0.1F, 1.0F} * (1.0F / Sqrt(2.01F));
        const float32 density = EvaluateLinearlyTransformedCosine(distribution, direction);
        REQUIRE(Abs(EvaluateLinearlyTransformedCosine(tripledDistribution, direction) - density) <= 1.0e-4F * density);
    }
}

TEST_CASE("LTC integrals over polygons match an independent float64 transform", "[math][linearly_transformed_cosine]")
{
    RandomStream random;
    REQUIRE(random.TryReseed(RANDOM_SEED, 3) == true);
    Array<float64> densityErrors;
    uint32 sampledCount = 0;
    uint32 belowHorizonCount = 0;
    uint32 outsideCount = 0;
    uint32 emptyCount = 0;
    uint32 negligibleCount = 0;
    for (uint32 polygonIndex = 0; polygonIndex < POLYGON_COUNT; ++polygonIndex)
    {
        const ProjectedPolygonInput input = MakeRandomPolygon(random);
        const Matrix3 inverseTransform = MakeRandomInverseTransform(random, MINIMUM_ALPHA);
        INFO("polygon " << polygonIndex);
        LinearlyTransformedCosine distribution;
        REQUIRE(TryMakeLinearlyTransformedCosine(inverseTransform, distribution) == MathStatus::Success);
        LinearlyTransformedCosinePolygon polygon;
        REQUIRE(TryPrepareLinearlyTransformedCosinePolygon(input, distribution, polygon) == MathStatus::Success);
        const OracleMatrix oracleInverse = Widen(inverseTransform);
        const float64 expected = ComputeOracleIntegral(input, oracleInverse);
        const float64 integral = static_cast<float64>(GetLinearlyTransformedCosinePolygonIntegral(polygon));
        REQUIRE(integral >= 0.0);
        REQUIRE(integral <= 1.0 + 1.0e-6);
        REQUIRE(std::abs(integral - expected) <= 1.0e-6 + 1.0e-3 * expected);
        if (integral == 0.0)
        {
            ++emptyCount;
            Vector3 unchanged{7, 7, 7};
            REQUIRE(TrySampleLinearlyTransformedCosinePolygon(polygon, Vector2{0.5F, 0.5F}, unchanged) ==
                    MathStatus::Degenerate);
            REQUIRE(unchanged == Vector3{7, 7, 7});
            continue;
        }
        // Single precision cannot sample transformed polygons with negligible projected solid angle reliably (Peters
        // 2021, section 3.7, Fig. 7b); their samples may miss the polygon.
        if (integral < NEGLIGIBLE_INTEGRAL)
        {
            ++negligibleCount;
            continue;
        }
        const OraclePolygon oraclePolygon = WidenPolygon(input);
        for (uint32 sampleIndex = 0; sampleIndex < SAMPLES_PER_POLYGON; ++sampleIndex)
        {
            const Vector2 randomNumbers{random.NextFloat01(), random.NextFloat01()};
            Vector3 direction;
            const MathStatus status = TrySampleLinearlyTransformedCosinePolygon(polygon, randomNumbers, direction);
            REQUIRE(status == MathStatus::Success);
            ++sampledCount;
            REQUIRE(Abs(Length(direction) - 1.0F) <= 1.0e-6F);
            belowHorizonCount += direction.Z < 0.0F ? 1 : 0;
            outsideCount += IsInsidePolygon(oraclePolygon, Widen(direction)) == true ? 0 : 1;
            // The density is the LTC divided by the integral (Peters 2021, section 4.1); the integral's own accuracy
            // is checked above, so this compares Equation 1.
            const float64 density =
                static_cast<float64>(EvaluateLinearlyTransformedCosinePolygonDensity(polygon, direction));
            const float64 expectedDensity = EvaluateOracleDensity(oracleInverse, Widen(direction)) / integral;
            REQUIRE(density >= 0.0);
            if (expectedDensity > 0.0)
            {
                REQUIRE(densityErrors.TryAdd(std::abs(density - expectedDensity) / expectedDensity) == true);
            }
        }
    }
    CAPTURE(sampledCount, belowHorizonCount, outsideCount, emptyCount, negligibleCount);
    REQUIRE(negligibleCount < POLYGON_COUNT / 2);
    REQUIRE(emptyCount > 0);
    REQUIRE(emptyCount < POLYGON_COUNT / 2);
    // Samples come from the polygon; only single-precision samples on its edges may fail the slack test.
    REQUIRE(outsideCount * 1000 <= sampledCount);
    // The LTC is not clipped to the shading horizon (Peters 2021, section 4.1).
    REQUIRE(belowHorizonCount > 0);
    REQUIRE(GetPercentile(densityErrors, 0.99) <= 2.0e-5);
    REQUIRE(GetPercentile(densityErrors, 1.0) <= 2.0e-4);
}

TEST_CASE("LTC polygon integrals match Monte Carlo integration of Equation 1", "[math][linearly_transformed_cosine]")
{
    RandomStream random;
    REQUIRE(random.TryReseed(RANDOM_SEED, 4) == true);
    uint32 checkedCount = 0;
    for (uint32 polygonIndex = 0; polygonIndex < MONTE_CARLO_POLYGON_COUNT; ++polygonIndex)
    {
        const ProjectedPolygonInput input = MakeRandomPolygon(random);
        const Matrix3 inverseTransform = MakeRandomInverseTransform(random, MODERATE_MINIMUM_ALPHA);
        LinearlyTransformedCosine distribution;
        REQUIRE(TryMakeLinearlyTransformedCosine(inverseTransform, distribution) == MathStatus::Success);
        LinearlyTransformedCosinePolygon polygon;
        REQUIRE(TryPrepareLinearlyTransformedCosinePolygon(input, distribution, polygon) == MathStatus::Success);
        // Integrate p over the polygon by uniform area sampling of its triangle fan: dw = cos(light) dA / d^2.
        const OraclePolygon oraclePolygon = WidenPolygon(input);
        const OracleMatrix oracleInverse = Widen(inverseTransform);
        const Vector3d origin = oraclePolygon.Positions[0];
        float64 areas[MAX_ORACLE_VERTICES]{};
        float64 totalArea = 0.0;
        for (uint32 i = 1; i + 1 < oraclePolygon.Count; ++i)
        {
            areas[i] =
                0.5 * GetLength(Cross(oraclePolygon.Positions[i] - origin, oraclePolygon.Positions[i + 1] - origin));
            totalArea += areas[i];
        }
        const Vector3d normal =
            Normalize(Cross(oraclePolygon.Positions[1] - origin, oraclePolygon.Positions[2] - origin));
        float64 sum = 0.0;
        for (uint32 sampleIndex = 0; sampleIndex < AREA_SAMPLE_COUNT; ++sampleIndex)
        {
            float64 target = Draw(random, 0.0, totalArea);
            uint32 triangle = 1;
            while (triangle + 2 < oraclePolygon.Count && target >= areas[triangle])
            {
                target -= areas[triangle];
                ++triangle;
            }
            float64 u = Draw(random, 0.0, 1.0);
            float64 v = Draw(random, 0.0, 1.0);
            if (u + v > 1.0)
            {
                u = 1.0 - u;
                v = 1.0 - v;
            }
            const Vector3d point = origin + (oraclePolygon.Positions[triangle] - origin) * u +
                                   (oraclePolygon.Positions[triangle + 1] - origin) * v;
            const float64 distanceSquared = Dot(point, point);
            const float64 lightCosine = std::abs(Dot(normal, point)) / std::sqrt(distanceSquared);
            sum += EvaluateOracleDensity(oracleInverse, point) * lightCosine / distanceSquared;
        }
        const float64 estimate = sum * totalArea / AREA_SAMPLE_COUNT;
        const float64 integral = static_cast<float64>(GetLinearlyTransformedCosinePolygonIntegral(polygon));
        INFO("polygon " << polygonIndex << ", estimate " << estimate << ", integral " << integral);
        REQUIRE(std::abs(integral - estimate) <= 2.0e-3 + 3.0e-2 * estimate);
        checkedCount += estimate > 1.0e-2 ? 1 : 0;
    }
    REQUIRE(checkedCount >= 16);
}

TEST_CASE("LTC samples cover the polygon's support with the stated density", "[math][linearly_transformed_cosine]")
{
    RandomStream random;
    REQUIRE(random.TryReseed(RANDOM_SEED, 5) == true);
    uint32 checkedCount = 0;
    for (uint32 polygonIndex = 0; polygonIndex < MONTE_CARLO_POLYGON_COUNT; ++polygonIndex)
    {
        const ProjectedPolygonInput input = MakeRandomPolygon(random);
        const Matrix3 inverseTransform = MakeRandomInverseTransform(random, MODERATE_MINIMUM_ALPHA);
        LinearlyTransformedCosine distribution;
        REQUIRE(TryMakeLinearlyTransformedCosine(inverseTransform, distribution) == MathStatus::Success);
        LinearlyTransformedCosinePolygon polygon;
        REQUIRE(TryPrepareLinearlyTransformedCosinePolygon(input, distribution, polygon) == MathStatus::Success);
        if (GetLinearlyTransformedCosinePolygonIntegral(polygon) < 1.0e-3F)
        {
            continue;
        }
        // E[1 / pdf] is the polygon's solid angle when the LTC is positive over all of it: every vertex lies in the
        // half-space whose normal is the last row of M^-1. Where the polygon crosses that plane the density
        // vanishes and 1 / pdf has infinite variance, so those polygons are skipped.
        const OracleMatrix oracleInverse = Widen(inverseTransform);
        const Vector3d supportNormal{oracleInverse.Columns[0].Z,
                                     oracleInverse.Columns[1].Z,
                                     oracleInverse.Columns[2].Z};
        const OraclePolygon oraclePolygon = WidenPolygon(input);
        bool isInsideSupport = true;
        for (uint32 i = 0; i < oraclePolygon.Count; ++i)
        {
            isInsideSupport = isInsideSupport == true && Dot(supportNormal, Normalize(oraclePolygon.Positions[i])) >
                                                             SUPPORT_MARGIN * GetLength(supportNormal);
        }
        if (isInsideSupport == false)
        {
            continue;
        }
        const float64 expected = ComputeOracleSolidAngle(oraclePolygon);
        float64 sum = 0.0;
        for (uint32 i = 0; i < STRATA_PER_AXIS * STRATA_PER_AXIS; ++i)
        {
            // Integer stratum coordinates; the division deliberately truncates to the row.
            const uint32 column = i % STRATA_PER_AXIS;
            const uint32 row = i / STRATA_PER_AXIS;
            const Vector2 randomNumbers{
                (static_cast<float32>(column) + random.NextFloat01()) / STRATA_PER_AXIS,
                (static_cast<float32>(row) + random.NextFloat01()) / STRATA_PER_AXIS,
            };
            Vector3 direction;
            REQUIRE(TrySampleLinearlyTransformedCosinePolygon(
                        polygon,
                        Vector2{Min(randomNumbers.X, 1.0F), Min(randomNumbers.Y, 1.0F)},
                        direction) == MathStatus::Success);
            const float32 density = EvaluateLinearlyTransformedCosinePolygonDensity(polygon, direction);
            REQUIRE(density > 0.0F);
            sum += 1.0 / static_cast<float64>(density);
        }
        const float64 estimate = sum / (STRATA_PER_AXIS * STRATA_PER_AXIS);
        INFO("polygon " << polygonIndex << ", estimate " << estimate << ", solid angle " << expected);
        REQUIRE(std::abs(estimate - expected) <= 1.0e-5 + 2.0e-2 * expected);
        ++checkedCount;
    }
    REQUIRE(checkedCount >= 16);
}

TEST_CASE("Moderate LTC densities integrate to one over the sphere", "[math][linearly_transformed_cosine]")
{
    RandomStream random;
    REQUIRE(random.TryReseed(RANDOM_SEED, 6) == true);
    constexpr uint32 CELLS_PER_AXIS = 1024;
    for (uint32 i = 0; i < 4; ++i)
    {
        LinearlyTransformedCosine distribution;
        REQUIRE(TryMakeLinearlyTransformedCosine(MakeRandomInverseTransform(random, MODERATE_MINIMUM_ALPHA),
                                                 distribution) == MathStatus::Success);
        // Midpoint rule in (cos theta, phi), whose area element is d(cos theta) d(phi).
        float64 sum = 0.0;
        for (uint32 row = 0; row < CELLS_PER_AXIS; ++row)
        {
            const float64 cosine = -1.0 + 2.0 * (row + 0.5) / CELLS_PER_AXIS;
            const float64 sine = std::sqrt(1.0 - cosine * cosine);
            for (uint32 column = 0; column < CELLS_PER_AXIS; ++column)
            {
                const float64 azimuth = kTau * (column + 0.5) / CELLS_PER_AXIS;
                const Vector3d direction{sine * std::cos(azimuth), sine * std::sin(azimuth), cosine};
                sum += static_cast<float64>(EvaluateLinearlyTransformedCosine(distribution, Narrow(direction)));
            }
        }
        REQUIRE(std::abs(sum * 2.0 * kTau / (CELLS_PER_AXIS * CELLS_PER_AXIS) - 1.0) <= 5.0e-3);
    }
}

TEST_CASE("Empty clips disable the LTC technique", "[math][linearly_transformed_cosine]")
{
    // A shear whose support excludes the polygon: (M^-1 w).z = z - 4x is negative over it.
    LinearlyTransformedCosine distribution;
    REQUIRE(TryMakeLinearlyTransformedCosine(Matrix3{{Vector3{1, 0, -4}, Vector3{0, 1, 0}, Vector3{0, 0, 1}}},
                                             distribution) == MathStatus::Success);
    LinearlyTransformedCosinePolygon polygon;
    REQUIRE(TryPrepareLinearlyTransformedCosinePolygon(
                MakePolygon({{1, -0.5F, 0.5F}, {1, 0.5F, 0.5F}, {1, 0.5F, 1}, {1, -0.5F, 1}}),
                distribution,
                polygon) == MathStatus::Success);
    REQUIRE(GetLinearlyTransformedCosinePolygonIntegral(polygon) == 0.0F);
    REQUIRE(EvaluateLinearlyTransformedCosinePolygonDensity(polygon, Vector3{0.8F, 0.0F, 0.6F}) == 0.0F);
    Vector3 unchanged{7, 7, 7};
    REQUIRE(TrySampleLinearlyTransformedCosinePolygon(polygon, Vector2{0.5F, 0.5F}, unchanged) ==
            MathStatus::Degenerate);
    REQUIRE(unchanged == Vector3{7, 7, 7});
    // Without the shear the same polygon is visible.
    LinearlyTransformedCosine identity;
    REQUIRE(TryMakeLinearlyTransformedCosine(Matrix3::Identity(), identity) == MathStatus::Success);
    REQUIRE(TryPrepareLinearlyTransformedCosinePolygon(
                MakePolygon({{1, -0.5F, 0.5F}, {1, 0.5F, 0.5F}, {1, 0.5F, 1}, {1, -0.5F, 1}}),
                identity,
                polygon) == MathStatus::Success);
    REQUIRE(GetLinearlyTransformedCosinePolygonIntegral(polygon) > 0.0F);
    // Directions outside the support evaluate to zero.
    REQUIRE(EvaluateLinearlyTransformedCosine(distribution, Vector3{0.8F, 0.0F, 0.6F}) == 0.0F);
    REQUIRE(EvaluateLinearlyTransformedCosine(identity, Vector3{0.0F, 0.0F, -1.0F}) == 0.0F);
    REQUIRE(EvaluateLinearlyTransformedCosine(identity, Vector3{0.0F, 0.0F, 0.0F}) == 0.0F);
    REQUIRE(EvaluateLinearlyTransformedCosine(identity,
                                              Vector3{0.0F, std::numeric_limits<float32>::quiet_NaN(), 1.0F}) == 0.0F);
    REQUIRE(Abs(EvaluateLinearlyTransformedCosine(identity, Vector3{0.0F, 0.0F, 1.0F}) - 1.0F / kPiF) <= 1.0e-7F);
}

TEST_CASE("Invalid LTC inputs leave outputs unchanged", "[math][linearly_transformed_cosine]")
{
    LinearlyTransformedCosine distribution;
    distribution.InverseDeterminant = 42.0F;
    const float32 infinity = std::numeric_limits<float32>::infinity();
    REQUIRE(TryMakeLinearlyTransformedCosine(Matrix3{{Vector3{1, 0, 0}, Vector3{0, infinity, 0}, Vector3{0, 0, 1}}},
                                             distribution) == MathStatus::NonFiniteInput);
    REQUIRE(TryMakeLinearlyTransformedCosine(Matrix3::Zero(), distribution) == MathStatus::Degenerate);
    // A mirror and a singular matrix have no positive determinant.
    REQUIRE(TryMakeLinearlyTransformedCosine(Matrix3{{Vector3{1, 0, 0}, Vector3{0, 1, 0}, Vector3{0, 0, -1}}},
                                             distribution) == MathStatus::Degenerate);
    REQUIRE(TryMakeLinearlyTransformedCosine(Matrix3{{Vector3{1, 0, 0}, Vector3{0, 1, 0}, Vector3{1, 1, 0}}},
                                             distribution) == MathStatus::Degenerate);
    // Entries at or above 2^127, or all subnormal, have no normal power-of-two scale.
    REQUIRE(TryMakeLinearlyTransformedCosine(
                Matrix3{{Vector3{0x1.0p+127F, 0, 0}, Vector3{0, 0x1.0p+127F, 0}, Vector3{0, 0, 0x1.0p+127F}}},
                distribution) == MathStatus::Degenerate);
    REQUIRE(TryMakeLinearlyTransformedCosine(
                Matrix3{{Vector3{0x1.0p-130F, 0, 0}, Vector3{0, 0x1.0p-130F, 0}, Vector3{0, 0, 0x1.0p-130F}}},
                distribution) == MathStatus::Degenerate);
    // A nearly flat distribution leaves single precision.
    REQUIRE(TryMakeLinearlyTransformedCosine(Matrix3{{Vector3{1, 0, 0}, Vector3{0, 1, 0}, Vector3{0, 0, 1.0e-12F}}},
                                             distribution) == MathStatus::IllConditioned);
    REQUIRE(distribution.InverseDeterminant == 42.0F);
    // The largest normal-scalable magnitude is accepted.
    REQUIRE(TryMakeLinearlyTransformedCosine(
                Matrix3{{Vector3{0x1.0p+126F, 0, 0}, Vector3{0, 0x1.0p+126F, 0}, Vector3{0, 0, 0x1.0p+126F}}},
                distribution) == MathStatus::Success);
    REQUIRE(distribution.InverseTransform == Matrix3::Identity());

    LinearlyTransformedCosinePolygon polygon;
    polygon.TransformedPolygon.ProjectedSolidAngle = 42.0F;
    ProjectedPolygonInput input = MakePolygon({{1, 0, 1}, {1, 1, 1}});
    REQUIRE(TryPrepareLinearlyTransformedCosinePolygon(input, distribution, polygon) == MathStatus::InvalidArgument);
    input.VertexCount = MAX_PROJECTED_POLYGON_INPUT_VERTICES + 1;
    REQUIRE(TryPrepareLinearlyTransformedCosinePolygon(input, distribution, polygon) == MathStatus::InvalidArgument);
    input = MakePolygon({{1, 0, 1}, {1, 1, infinity}, {0, 1, 1}});
    REQUIRE(TryPrepareLinearlyTransformedCosinePolygon(input, distribution, polygon) == MathStatus::NonFiniteInput);
    // Finite vertices whose transform overflows.
    LinearlyTransformedCosine stretched;
    REQUIRE(TryMakeLinearlyTransformedCosine(Matrix3{{Vector3{1.5F, 0, 0}, Vector3{0, 1.5F, 0}, Vector3{0, 0, 1.5F}}},
                                             stretched) == MathStatus::Success);
    input = MakePolygon({{3.0e38F, 0, 1}, {3.0e38F, 1, 1}, {0, 1, 1}});
    REQUIRE(TryPrepareLinearlyTransformedCosinePolygon(input, stretched, polygon) == MathStatus::NonFiniteInput);
    REQUIRE(polygon.TransformedPolygon.ProjectedSolidAngle == 42.0F);

    REQUIRE(TryPrepareLinearlyTransformedCosinePolygon(MakePolygon({{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}}),
                                                       distribution,
                                                       polygon) == MathStatus::Success);
    Vector3 unchanged{7, 7, 7};
    REQUIRE(TrySampleLinearlyTransformedCosinePolygon(polygon, Vector2{-0.1F, 0.5F}, unchanged) ==
            MathStatus::InvalidArgument);
    REQUIRE(TrySampleLinearlyTransformedCosinePolygon(polygon, Vector2{0.5F, 1.5F}, unchanged) ==
            MathStatus::InvalidArgument);
    REQUIRE(TrySampleLinearlyTransformedCosinePolygon(polygon,
                                                      Vector2{std::numeric_limits<float32>::quiet_NaN(), 0.5F},
                                                      unchanged) == MathStatus::NonFiniteInput);
    REQUIRE(unchanged == Vector3{7, 7, 7});
}
