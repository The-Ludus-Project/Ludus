#include "transform_utilities.h"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::scene_cook::importers
{
namespace
{
[[nodiscard]] float64 GetElement(const math::Matrix4d& matrix, int row, int column) noexcept
{
    return matrix.Elements[4 * column + row];
}

void SetElement(math::Matrix4d& inoutMatrix, int row, int column, float64 value) noexcept
{
    inoutMatrix.Elements[4 * column + row] = value;
}
} // namespace

math::Matrix4d MultiplyMatrices(const math::Matrix4d& left, const math::Matrix4d& right) noexcept
{
    math::Matrix4d result;
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            float64 sum = 0.0;
            for (int k = 0; k < 4; ++k)
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
    result.Elements[12] = offset.X;
    result.Elements[13] = offset.Y;
    result.Elements[14] = offset.Z;
    return result;
}

math::Matrix4d MakeScale(const math::Vector3d& factors) noexcept
{
    math::Matrix4d result = math::Matrix4d::Identity();
    result.Elements[0] = factors.X;
    result.Elements[5] = factors.Y;
    result.Elements[10] = factors.Z;
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
    const float64 rows[3][3] = {
        {x * x + (1 - x * x) * cosine, x * y * (1 - cosine) - z * sine, x * z * (1 - cosine) + y * sine},
        {x * y * (1 - cosine) + z * sine, y * y + (1 - y * y) * cosine, y * z * (1 - cosine) - x * sine},
        {x * z * (1 - cosine) - y * sine, y * z * (1 - cosine) + x * sine, z * z + (1 - z * z) * cosine},
    };
    outMatrix = math::Matrix4d::Identity();
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            SetElement(outMatrix, row, column, rows[row][column]);
        }
    }
    return true;
}

bool MakeLookAt(std::span<const float64> values, math::Matrix4d& outMatrix) noexcept
{
    if (values.size() < 9)
    {
        return false;
    }
    const math::Vector3d eye{values[0], values[1], values[2]};
    const math::Vector3d look{values[3], values[4], values[5]};
    const math::Vector3d up{values[6], values[7], values[8]};
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
    const math::Vector3d columns[4] = {right, newUp, direction, eye};
    for (int column = 0; column < 4; ++column)
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
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
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
    for (int row = 0; row < 3; ++row)
    {
        float64 sum = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            sum += GetElement(result, row, k) * at(k, 3);
        }
        SetElement(result, row, 3, -sum);
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
    return math::Vector3d{direction.X + matrix.Elements[12],
                          direction.Y + matrix.Elements[13],
                          direction.Z + matrix.Elements[14]};
}

math::Vector3d TransformDirection(const math::Matrix4d& matrix, math::Vector3d direction) noexcept
{
    const float64 input[3] = {direction.X, direction.Y, direction.Z};
    float64 output[3] = {0.0, 0.0, 0.0};
    for (int row = 0; row < 3; ++row)
    {
        for (int k = 0; k < 3; ++k)
        {
            output[row] += GetElement(matrix, row, k) * input[k];
        }
    }
    return math::Vector3d{output[0], output[1], output[2]};
}
} // namespace ludus::content::scene_cook::importers
