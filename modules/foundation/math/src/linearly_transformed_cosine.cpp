// Linearly transformed cosines over convex polygons; see linearly_transformed_cosine.hpp
// for the Heitz et al. 2016 and Peters 2021 references. Compiled with precise FP
// flags (-ffp-contract=off, no fast math). <cmath> is permitted here (ADR 0003, ADR 0010).

#include <ludus/foundation/math/linearly_transformed_cosine.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <cmath>

namespace ludus::foundation::math
{
namespace
{
using core::float64;
using core::uint32;

[[nodiscard]] Vector3 Narrow(Vector3d value) noexcept
{
    return Vector3{static_cast<float32>(value.X), static_cast<float32>(value.Y), static_cast<float32>(value.Z)};
}
[[nodiscard]] bool IsFiniteMatrix(const Matrix3& matrix) noexcept
{
    return IsFinite(matrix.Columns[0]) == true && IsFinite(matrix.Columns[1]) == true &&
           IsFinite(matrix.Columns[2]) == true;
}

[[nodiscard]] Vector3d Widen(Vector3 value) noexcept
{
    return Vector3d{static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}
[[nodiscard]] float64 ComputeDeterminant(const Matrix3& matrix) noexcept
{
    return Dot(Widen(matrix.Columns[0]), Cross(Widen(matrix.Columns[1]), Widen(matrix.Columns[2])));
}

/// The inverse through the adjugate, in float64. LTC matrices for low roughness are strongly anisotropic, so this
/// accepts any finite, invertible matrix instead of applying TryInverse's conditioning policy.
[[nodiscard]] bool TryInvert(const Matrix3& matrix, Matrix3& outInverse) noexcept
{
    const Vector3d column0 = Widen(matrix.Columns[0]);
    const Vector3d column1 = Widen(matrix.Columns[1]);
    const Vector3d column2 = Widen(matrix.Columns[2]);
    const float64 determinant = Dot(column0, Cross(column1, column2));
    if ((determinant > 0.0 || determinant < 0.0) == false || IsFinite(determinant) == false)
    {
        return false;
    }
    // The rows of the inverse are the cross products of column pairs divided by the determinant.
    const Vector3d row0 = Cross(column1, column2) * (1.0 / determinant);
    const Vector3d row1 = Cross(column2, column0) * (1.0 / determinant);
    const Vector3d row2 = Cross(column0, column1) * (1.0 / determinant);
    const Matrix3 inverse{{
        Narrow(Vector3d{row0.X, row1.X, row2.X}),
        Narrow(Vector3d{row0.Y, row1.Y, row2.Y}),
        Narrow(Vector3d{row0.Z, row1.Z, row2.Z}),
    }};
    if (IsFiniteMatrix(inverse) == false)
    {
        return false;
    }
    outInverse = inverse;
    return true;
}

} // namespace

float32 EvaluateLtcDensity(const Matrix3& inverseTransform, Vector3 directionTangent) noexcept
{
    if (IsFiniteMatrix(inverseTransform) == false || IsFinite(directionTangent) == false)
    {
        return 0.0F;
    }
    const Vector3 cosineDirection = inverseTransform * directionTangent;
    const float32 lengthSquared = Dot(cosineDirection, cosineDirection);
    const float32 inverseDeterminant = static_cast<float32>(std::abs(ComputeDeterminant(inverseTransform)));
    if (cosineDirection.Z <= 0.0F || (lengthSquared > 0.0F) == false)
    {
        return 0.0F;
    }
    return cosineDirection.Z * inverseDeterminant / (kPiF * lengthSquared * lengthSquared);
}

MathStatus TryPrepareLtcPolygon(const ProjectedPolygonInput& polygon,
                                const Matrix3& inverseTransform,
                                LtcPolygon& outPolygon) noexcept
{
    if (IsFiniteMatrix(inverseTransform) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    LtcPolygon result;
    result.InverseTransform = inverseTransform;
    if (TryInvert(inverseTransform, result.Transform) == false)
    {
        return MathStatus::Degenerate;
    }
    result.InverseDeterminant = static_cast<float32>(std::abs(ComputeDeterminant(inverseTransform)));
    ProjectedPolygonInput transformed = polygon;
    for (uint32 i = 0; i < polygon.VertexCount && i < MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++i)
    {
        transformed.PositionsTangent[i] = inverseTransform * polygon.PositionsTangent[i];
    }
    const MathStatus status = TryPrepareProjectedSolidAnglePolygon(transformed, result.Polygon);
    if (status != MathStatus::Success)
    {
        return status;
    }
    outPolygon = result;
    return MathStatus::Success;
}

float32 GetLtcPolygonIntegral(const LtcPolygon& polygon) noexcept
{
    return polygon.Polygon.ProjectedSolidAngle / kPiF;
}

MathStatus TrySampleLtcPolygon(const LtcPolygon& polygon, Vector2 randomNumbers, Vector3& outDirectionTangent) noexcept
{
    Vector3 cosineDirection;
    const MathStatus status = TrySampleProjectedSolidAnglePolygon(polygon.Polygon, randomNumbers, cosineDirection);
    if (status != MathStatus::Success)
    {
        return status;
    }
    Vector3 direction;
    if (TryNormalize(polygon.Transform * cosineDirection, direction) != MathStatus::Success)
    {
        return MathStatus::IllConditioned;
    }
    outDirectionTangent = direction;
    return MathStatus::Success;
}

float32 EvaluateLtcPolygonDensity(const LtcPolygon& polygon, Vector3 directionTangent) noexcept
{
    if (IsFinite(directionTangent) == false || (polygon.Polygon.ProjectedSolidAngle > 0.0F) == false)
    {
        return 0.0F;
    }
    const Vector3 cosineDirection = polygon.InverseTransform * directionTangent;
    const float32 lengthSquared = Dot(cosineDirection, cosineDirection);
    if (cosineDirection.Z <= 0.0F || (lengthSquared > 0.0F) == false)
    {
        return 0.0F;
    }
    // D(omega) / (A / pi): the factors of pi cancel.
    return cosineDirection.Z * polygon.InverseDeterminant /
           (polygon.Polygon.ProjectedSolidAngle * lengthSquared * lengthSquared);
}
} // namespace ludus::foundation::math
