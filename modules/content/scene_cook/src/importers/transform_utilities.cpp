#include "transform_utilities.h"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::scene_cook::importers
{
namespace
{
/// Rows and columns of math::Matrix4d (column-major, Elements[4 * column + row]).
constexpr int MATRIX_DIMENSION = 4;
/// Rows and columns of the linear (rotation/scale) block of an affine matrix.
constexpr int LINEAR_DIMENSION = 3;
/// Column that holds an affine matrix's translation.
constexpr int TRANSLATION_COLUMN = 3;
/// Values of a pbrt LookAt directive: eye, look and up points, three components each.
constexpr foundation::core::usize LOOK_AT_EYE_OFFSET = 0;
constexpr foundation::core::usize LOOK_AT_TARGET_OFFSET = 3;
constexpr foundation::core::usize LOOK_AT_UP_OFFSET = 6;
constexpr foundation::core::usize LOOK_AT_VALUE_COUNT = 9;

[[nodiscard]] float64 GetElement(const math::Matrix4d& matrix, int row, int column) noexcept
{
    return matrix.Elements[MATRIX_DIMENSION * column + row];
}

void SetElement(math::Matrix4d& inoutMatrix, int row, int column, float64 value) noexcept
{
    inoutMatrix.Elements[MATRIX_DIMENSION * column + row] = value;
}
[[nodiscard]] math::Vector3d ReadPoint(std::span<const float64> values, foundation::core::usize offset) noexcept
{
    return math::Vector3d{values[offset], values[offset + 1], values[offset + 2]};
}
} // namespace

math::Matrix4d MultiplyMatrices(const math::Matrix4d& left, const math::Matrix4d& right) noexcept
{
    math::Matrix4d result;
    for (int row = 0; row < MATRIX_DIMENSION; ++row)
    {
        for (int column = 0; column < MATRIX_DIMENSION; ++column)
        {
            float64 sum = 0.0;
            for (int k = 0; k < MATRIX_DIMENSION; ++k)
            {
                sum += GetElement(left, row, k) * GetElement(right, k, column);
            }
            SetElement(result, row, column, sum);
        }
    }
    return result;
}

math::Matrix4d MakeTranslation(const math::Vector3d& offset) noexcept
{
    math::Matrix4d result = math::Matrix4d::Identity();
    SetElement(result, 0, TRANSLATION_COLUMN, offset.X);
    SetElement(result, 1, TRANSLATION_COLUMN, offset.Y);
    SetElement(result, 2, TRANSLATION_COLUMN, offset.Z);
    return result;
}

math::Matrix4d MakeScale(const math::Vector3d& factors) noexcept
{
    math::Matrix4d result = math::Matrix4d::Identity();
    SetElement(result, 0, 0, factors.X);
    SetElement(result, 1, 1, factors.Y);
    SetElement(result, 2, 2, factors.Z);
    return result;
}

bool MakeRotation(float64 angleDegrees, const math::Vector3d& axis, math::Matrix4d& outMatrix) noexcept
{
    const float64 length = math::Length(axis);
    if (length == 0.0 || math::IsFinite(length) == false)
    {
        return false;
    }
    const float64 x = axis.X / length;
    const float64 y = axis.Y / length;
    const float64 z = axis.Z / length;
    const float64 radians = math::DegreesToRadians(angleDegrees);
    const float64 sine = math::Sin(radians);
    const float64 cosine = math::Cos(radians);
    const float64 rows[LINEAR_DIMENSION][LINEAR_DIMENSION] = {
        {x * x + (1 - x * x) * cosine, x * y * (1 - cosine) - z * sine, x * z * (1 - cosine) + y * sine},
        {x * y * (1 - cosine) + z * sine, y * y + (1 - y * y) * cosine, y * z * (1 - cosine) - x * sine},
        {x * z * (1 - cosine) - y * sine, y * z * (1 - cosine) + x * sine, z * z + (1 - z * z) * cosine},
    };
    outMatrix = math::Matrix4d::Identity();
    for (int row = 0; row < LINEAR_DIMENSION; ++row)
    {
        for (int column = 0; column < LINEAR_DIMENSION; ++column)
        {
            SetElement(outMatrix, row, column, rows[row][column]);
        }
    }
    return true;
}

bool MakeLookAt(std::span<const float64> values, math::Matrix4d& outMatrix) noexcept
{
    if (values.size() < LOOK_AT_VALUE_COUNT)
    {
        return false;
    }
    const math::Vector3d eye = ReadPoint(values, LOOK_AT_EYE_OFFSET);
    const math::Vector3d look = ReadPoint(values, LOOK_AT_TARGET_OFFSET);
    const math::Vector3d up = ReadPoint(values, LOOK_AT_UP_OFFSET);
    const math::Vector3d forward = look - eye;
    const float64 forwardLength = math::Length(forward);
    if (forwardLength == 0.0)
    {
        return false;
    }
    const math::Vector3d direction = forward * (1.0 / forwardLength);
    const float64 upLength = math::Length(up);
    if (upLength == 0.0)
    {
        return false;
    }
    const math::Vector3d rightRaw = math::Cross(up * (1.0 / upLength), direction);
    const float64 rightLength = math::Length(rightRaw);
    if (rightLength == 0.0)
    {
        return false;
    }
    const math::Vector3d right = rightRaw * (1.0 / rightLength);
    const math::Vector3d newUp = math::Cross(direction, right);
    // World-from-camera has columns right, newUp, direction, eye; pbrt stores its inverse.
    math::Matrix4d worldFromCamera = math::Matrix4d::Identity();
    const math::Vector3d columns[MATRIX_DIMENSION] = {right, newUp, direction, eye};
    for (int column = 0; column < MATRIX_DIMENSION; ++column)
    {
        SetElement(worldFromCamera, 0, column, columns[column].X);
        SetElement(worldFromCamera, 1, column, columns[column].Y);
        SetElement(worldFromCamera, 2, column, columns[column].Z);
    }
    return TryInvertAffine(worldFromCamera, outMatrix);
}

float64 ComputeLinearDeterminant(const math::Matrix4d& matrix) noexcept
{
    const auto at = [&](int row, int column) noexcept { return GetElement(matrix, row, column); };
    return at(0, 0) * (at(1, 1) * at(2, 2) - at(1, 2) * at(2, 1)) -
           at(0, 1) * (at(1, 0) * at(2, 2) - at(1, 2) * at(2, 0)) +
           at(0, 2) * (at(1, 0) * at(2, 1) - at(1, 1) * at(2, 0));
}

bool TryInvertAffine(const math::Matrix4d& matrix, math::Matrix4d& outInverse) noexcept
{
    const float64 determinant = ComputeLinearDeterminant(matrix);
    if (determinant == 0.0 || math::IsFinite(determinant) == false)
    {
        return false;
    }
    const auto at = [&](int row, int column) noexcept { return GetElement(matrix, row, column); };
    const float64 inverseDeterminant = 1.0 / determinant;
    math::Matrix4d result = math::Matrix4d::Identity();
    for (int row = 0; row < LINEAR_DIMENSION; ++row)
    {
        for (int column = 0; column < LINEAR_DIMENSION; ++column)
        {
            // Cofactor of (column, row) for the adjugate.
            const int r0 = column == 0 ? 1 : 0;
            const int r1 = column == 2 ? 1 : 2;
            const int c0 = row == 0 ? 1 : 0;
            const int c1 = row == 2 ? 1 : 2;
            const float64 minor = at(r0, c0) * at(r1, c1) - at(r0, c1) * at(r1, c0);
            const float64 sign = ((row + column) % 2) == 0 ? 1.0 : -1.0;
            SetElement(result, row, column, sign * minor * inverseDeterminant);
        }
    }
    for (int row = 0; row < LINEAR_DIMENSION; ++row)
    {
        float64 sum = 0.0;
        for (int k = 0; k < LINEAR_DIMENSION; ++k)
        {
            sum += GetElement(result, row, k) * at(k, TRANSLATION_COLUMN);
        }
        SetElement(result, row, TRANSLATION_COLUMN, -sum);
    }
    for (const float64 value : result.Elements)
    {
        if (math::IsFinite(value) == false)
        {
            return false;
        }
    }
    outInverse = result;
    return true;
}

math::Vector3d TransformPoint(const math::Matrix4d& matrix, math::Vector3d point) noexcept
{
    const math::Vector3d direction = TransformDirection(matrix, point);
    return math::Vector3d{direction.X + GetElement(matrix, 0, TRANSLATION_COLUMN),
                          direction.Y + GetElement(matrix, 1, TRANSLATION_COLUMN),
                          direction.Z + GetElement(matrix, 2, TRANSLATION_COLUMN)};
}

math::Vector3d TransformDirection(const math::Matrix4d& matrix, math::Vector3d direction) noexcept
{
    const float64 input[LINEAR_DIMENSION] = {direction.X, direction.Y, direction.Z};
    float64 output[LINEAR_DIMENSION] = {0.0, 0.0, 0.0};
    for (int row = 0; row < LINEAR_DIMENSION; ++row)
    {
        for (int k = 0; k < LINEAR_DIMENSION; ++k)
        {
            output[row] += GetElement(matrix, row, k) * input[k];
        }
    }
    return math::Vector3d{output[0], output[1], output[2]};
}
} // namespace ludus::content::scene_cook::importers
