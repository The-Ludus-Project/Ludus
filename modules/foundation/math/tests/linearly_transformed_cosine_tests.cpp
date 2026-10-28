// Linearly transformed cosines over polygons (Heitz et al. 2016, Peters 2021 section 4.1),
// checked against independent float64 quadrature over the sphere.

#include <ludus/foundation/math/linearly_transformed_cosine.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <initializer_list>
#include <limits>

using namespace ludus::foundation::math;
using ludus::foundation::core::float32;
using ludus::foundation::core::float64;
using ludus::foundation::core::uint32;

namespace
{
// Quadrature resolution over the sphere: uniform in cos(theta) and azimuth, so every cell has equal solid angle.
constexpr uint32 QUADRATURE_ROWS = 1024;
constexpr uint32 QUADRATURE_COLUMNS = 2048;
constexpr uint32 SAMPLE_GRID = 128;

/// M = [[a, 0, b], [0, c, 0], [d, 0, 1]], the isotropic form of Heitz et al. Equation (6).
[[nodiscard]] Matrix3 MakeIsotropicTransform(float32 a, float32 b, float32 c, float32 d) noexcept
{
    return Matrix3{{Vector3{a, 0, d}, Vector3{0, c, 0}, Vector3{b, 0, 1}}};
}

[[nodiscard]] Matrix3 Invert(const Matrix3& matrix)
{
    Matrix3 inverse;
    REQUIRE(TryInverse(matrix, inverse) == MathStatus::Success);
    return inverse;
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

/// Whether a direction lies in the convex polygon's solid angle, for either winding.
[[nodiscard]] bool IsInside(const ProjectedPolygonInput& polygon, Vector3 direction) noexcept
{
    float64 orientation = 0.0;
    bool isInside = true;
    for (uint32 i = 0; i < polygon.VertexCount; ++i)
    {
        const Vector3 normal =
            Cross(polygon.PositionsTangent[i], polygon.PositionsTangent[(i + 1) % polygon.VertexCount]);
        const float64 side = static_cast<float64>(Dot(normal, direction));
        orientation = i == 0 ? side : orientation;
        isInside = isInside == true && side * orientation >= 0.0;
    }
    return isInside;
}

/// Quadrature of g(omega) over the sphere, restricted to the polygon when one is given.
template <typename Integrand>
[[nodiscard]] float64 IntegrateOverSphere(const ProjectedPolygonInput* polygonOrNull, const Integrand& integrand)
{
    float64 sum = 0.0;
    for (uint32 i = 0; i < QUADRATURE_ROWS; ++i)
    {
        const float64 z = -1.0 + 2.0 * (static_cast<float64>(i) + 0.5) / QUADRATURE_ROWS;
        const float64 radius = std::sqrt(1.0 - z * z);
        for (uint32 j = 0; j < QUADRATURE_COLUMNS; ++j)
        {
            const float64 azimuth = (static_cast<float64>(j) + 0.5) / QUADRATURE_COLUMNS * kTau;
            const Vector3 direction{static_cast<float32>(radius * std::cos(azimuth)),
                                    static_cast<float32>(radius * std::sin(azimuth)),
                                    static_cast<float32>(z)};
            if (polygonOrNull == nullptr || IsInside(*polygonOrNull, direction) == true)
            {
                sum += integrand(direction);
            }
        }
    }
    return sum * 2.0 * kTau / (static_cast<float64>(QUADRATURE_ROWS) * QUADRATURE_COLUMNS);
}

// A glossy-looking transform and a light above and to the side of the shading point.
const Matrix3 TRANSFORM = MakeIsotropicTransform(0.35F, 0.25F, 0.45F, -0.15F);
const ProjectedPolygonInput LIGHT =
    MakePolygon({{-0.2F, -0.6F, 1.0F}, {0.9F, -0.4F, 1.2F}, {1.1F, 0.5F, 1.3F}, {0.0F, 0.7F, 1.1F}});
} // namespace

TEST_CASE("The LTC density integrates to one over the sphere", "[math][ltc]")
{
    const Matrix3 inverse = Invert(TRANSFORM);
    const float64 total = IntegrateOverSphere(nullptr, [&](Vector3 direction) {
        return static_cast<float64>(EvaluateLtcDensity(inverse, direction));
    });
    REQUIRE(std::abs(total - 1.0) <= 2.0e-3);
    // The identity transform is the clamped cosine.
    REQUIRE(std::abs(EvaluateLtcDensity(Matrix3::Identity(), Vector3{0, 0, 1}) - 1.0F / kPiF) <= 1.0e-7F);
    REQUIRE(EvaluateLtcDensity(Matrix3::Identity(), Vector3{0, 0, -1}) == 0.0F);
}

TEST_CASE("The polygon integral matches quadrature of the LTC over the polygon", "[math][ltc]")
{
    const Matrix3 inverse = Invert(TRANSFORM);
    LtcPolygon polygon;
    REQUIRE(TryPrepareLtcPolygon(LIGHT, inverse, polygon) == MathStatus::Success);
    const float64 quadrature = IntegrateOverSphere(&LIGHT, [&](Vector3 direction) {
        return static_cast<float64>(EvaluateLtcDensity(inverse, direction));
    });
    REQUIRE(quadrature > 0.01);
    REQUIRE(std::abs(static_cast<float64>(GetLtcPolygonIntegral(polygon)) - quadrature) <= 5.0e-3 * quadrature);

    // With the identity, the LTC technique is projected solid angle sampling.
    LtcPolygon cosine;
    ProjectedSolidAnglePolygon projected;
    REQUIRE(TryPrepareLtcPolygon(LIGHT, Matrix3::Identity(), cosine) == MathStatus::Success);
    REQUIRE(TryPrepareProjectedSolidAnglePolygon(LIGHT, projected) == MathStatus::Success);
    REQUIRE(GetLtcPolygonIntegral(cosine) == projected.ProjectedSolidAngle / kPiF);
    Vector3 fromLtc;
    Vector3 fromProjected;
    REQUIRE(TrySampleLtcPolygon(cosine, Vector2{0.3F, 0.7F}, fromLtc) == MathStatus::Success);
    REQUIRE(TrySampleProjectedSolidAnglePolygon(projected, Vector2{0.3F, 0.7F}, fromProjected) == MathStatus::Success);
    REQUIRE(Length(fromLtc - fromProjected) <= 1.0e-6F);
    REQUIRE(std::abs(EvaluateLtcPolygonDensity(cosine, fromLtc) -
                     EvaluateProjectedSolidAnglePolygonDensity(projected, fromProjected)) <=
            1.0e-5F * EvaluateLtcPolygonDensity(cosine, fromLtc));
}

TEST_CASE("LTC samples lie in the polygon and give unbiased estimates", "[math][ltc]")
{
    // E[g / p] over samples of the LTC equals the integral of g over the polygon (Peters section 4.1). The bounded
    // integrand g = D(omega) (1 + x) keeps the estimator's variance small.
    const Matrix3 inverse = Invert(TRANSFORM);
    LtcPolygon polygon;
    REQUIRE(TryPrepareLtcPolygon(LIGHT, inverse, polygon) == MathStatus::Success);
    const auto integrand = [&](Vector3 direction) {
        return static_cast<float64>(EvaluateLtcDensity(inverse, direction)) * (1.0 + static_cast<float64>(direction.X));
    };
    const float64 expected = IntegrateOverSphere(&LIGHT, integrand);
    float64 sum = 0.0;
    for (uint32 i = 0; i < SAMPLE_GRID; ++i)
    {
        for (uint32 j = 0; j < SAMPLE_GRID; ++j)
        {
            const Vector2 randomNumbers{(static_cast<float32>(i) + 0.5F) / SAMPLE_GRID,
                                        (static_cast<float32>(j) + 0.5F) / SAMPLE_GRID};
            Vector3 direction;
            REQUIRE(TrySampleLtcPolygon(polygon, randomNumbers, direction) == MathStatus::Success);
            REQUIRE(std::abs(static_cast<float64>(Length(direction)) - 1.0) <= 1.0e-5);
            REQUIRE(IsInside(LIGHT, direction) == true);
            const float32 density = EvaluateLtcPolygonDensity(polygon, direction);
            REQUIRE(density > 0.0F);
            sum += integrand(direction) / static_cast<float64>(density);
        }
    }
    const float64 estimate = sum / (static_cast<float64>(SAMPLE_GRID) * SAMPLE_GRID);
    REQUIRE(std::abs(estimate - expected) <= 5.0e-3 * expected);
}

TEST_CASE("LTC edge cases", "[math][ltc]")
{
    // A polygon that the transform moves below the cosine's horizon disables the technique (Peters section 4.1).
    const Matrix3 tilt = MakeIsotropicTransform(1.0F, 0.0F, 1.0F, 3.0F);
    const ProjectedPolygonInput grazing = MakePolygon({{1.0F, -0.1F, 0.1F}, {1.0F, 0.1F, 0.1F}, {1.0F, 0.0F, 0.2F}});
    LtcPolygon empty;
    REQUIRE(TryPrepareLtcPolygon(grazing, Invert(tilt), empty) == MathStatus::Success);
    REQUIRE(GetLtcPolygonIntegral(empty) == 0.0F);
    Vector3 unchanged{7, 7, 7};
    REQUIRE(TrySampleLtcPolygon(empty, Vector2{0.5F, 0.5F}, unchanged) == MathStatus::Degenerate);
    REQUIRE(unchanged == Vector3{7, 7, 7});
    REQUIRE(EvaluateLtcPolygonDensity(empty, Vector3{0, 0, 1}) == 0.0F);

    LtcPolygon polygon;
    polygon.InverseDeterminant = 42.0F;
    const Matrix3 singular{{Vector3{1, 0, 0}, Vector3{1, 0, 0}, Vector3{0, 0, 1}}};
    REQUIRE(TryPrepareLtcPolygon(LIGHT, singular, polygon) == MathStatus::Degenerate);
    Matrix3 nonFinite = Matrix3::Identity();
    nonFinite.Columns[1].Y = std::numeric_limits<float32>::infinity();
    REQUIRE(TryPrepareLtcPolygon(LIGHT, nonFinite, polygon) == MathStatus::NonFiniteInput);
    REQUIRE(TryPrepareLtcPolygon(MakePolygon({{0, 0, 1}, {1, 0, 1}}), Matrix3::Identity(), polygon) ==
            MathStatus::InvalidArgument);
    REQUIRE(polygon.InverseDeterminant == 42.0F);
    REQUIRE(EvaluateLtcDensity(singular, Vector3{0, 0, 1}) == 0.0F);
}
