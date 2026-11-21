#include "internal/rigid_math.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/physics/collision/queries.h>

namespace ludus::physics::rigid::internal
{
namespace
{
using foundation::StaticArray;

constexpr usize MATRIX_ELEMENT_COUNT = 9;
/// Most cyclic Jacobi sweeps; a symmetric 3x3 matrix converges quadratically within a handful.
constexpr uint32 MAX_JACOBI_SWEEPS = 64;
/// Off-diagonal magnitude, relative to the matrix scale, at which Jacobi iteration stops.
constexpr float64 JACOBI_RELATIVE_TOLERANCE = 1.0e-15;
/// Smallest determinant, relative to the cube of the matrix scale, that TrySolve accepts.
constexpr float64 MIN_RELATIVE_DETERMINANT = 1.0e-14;
constexpr float64 QUARTER = 0.25;

using Elements = StaticArray<float64, MATRIX_ELEMENT_COUNT>;

/// The matrix being diagonalized and the accumulated rotation whose columns become its eigenvectors.
struct JacobiState final
{
    Elements Matrix{};
    Elements Vectors{};
};

constexpr usize GetIndex(uint32 row, uint32 column) noexcept
{
    return static_cast<usize>(row) * AXIS_COUNT + column;
}

Elements ToElements(const Matrix3x3& matrix) noexcept
{
    Elements elements{};
    const StaticArray<math::Vector3d, AXIS_COUNT> columns{matrix.ColumnX, matrix.ColumnY, matrix.ColumnZ};
    for (uint32 column = 0; column < AXIS_COUNT; ++column)
    {
        for (uint32 row = 0; row < AXIS_COUNT; ++row)
        {
            elements[GetIndex(row, column)] = GetComponent(columns[column], row);
        }
    }
    return elements;
}

Matrix3x3 FromElements(const Elements& elements) noexcept
{
    return {
        {elements[GetIndex(X_AXIS, X_AXIS)], elements[GetIndex(Y_AXIS, X_AXIS)], elements[GetIndex(Z_AXIS, X_AXIS)]},
        {elements[GetIndex(X_AXIS, Y_AXIS)], elements[GetIndex(Y_AXIS, Y_AXIS)], elements[GetIndex(Z_AXIS, Y_AXIS)]},
        {elements[GetIndex(X_AXIS, Z_AXIS)], elements[GetIndex(Y_AXIS, Z_AXIS)], elements[GetIndex(Z_AXIS, Z_AXIS)]}};
}

float64 GetScale(const Elements& elements) noexcept
{
    float64 scale = 0.0;
    for (const float64 element : elements)
    {
        scale = math::Max(scale, math::Abs(element));
    }
    return scale;
}

float64 GetOffDiagonalMagnitude(const Elements& elements) noexcept
{
    return math::Abs(elements[GetIndex(X_AXIS, Y_AXIS)]) + math::Abs(elements[GetIndex(X_AXIS, Z_AXIS)]) +
           math::Abs(elements[GetIndex(Y_AXIS, Z_AXIS)]);
}

/// One Jacobi rotation in the (p, q) plane that zeroes element (p, q) of the symmetric matrix, accumulated into the
/// columns of the vectors.
void RotateJacobi(uint32 p, uint32 q, JacobiState& inoutState) noexcept
{
    Elements& matrix = inoutState.Matrix;
    Elements& vectors = inoutState.Vectors;
    const float64 offDiagonal = matrix[GetIndex(p, q)];
    if (offDiagonal == 0.0)
    {
        return;
    }
    const float64 theta = (matrix[GetIndex(q, q)] - matrix[GetIndex(p, p)]) / (2.0 * offDiagonal);
    const float64 tangent = (theta >= 0.0 ? 1.0 : -1.0) / (math::Abs(theta) + math::Sqrt(theta * theta + 1.0));
    const float64 cosine = 1.0 / math::Sqrt(tangent * tangent + 1.0);
    const float64 sine = tangent * cosine;
    for (uint32 k = 0; k < AXIS_COUNT; ++k)
    {
        const float64 kp = matrix[GetIndex(k, p)];
        const float64 kq = matrix[GetIndex(k, q)];
        matrix[GetIndex(k, p)] = cosine * kp - sine * kq;
        matrix[GetIndex(k, q)] = sine * kp + cosine * kq;
    }
    for (uint32 k = 0; k < AXIS_COUNT; ++k)
    {
        const float64 pk = matrix[GetIndex(p, k)];
        const float64 qk = matrix[GetIndex(q, k)];
        matrix[GetIndex(p, k)] = cosine * pk - sine * qk;
        matrix[GetIndex(q, k)] = sine * pk + cosine * qk;
    }
    // The rotation zeroes (p, q) in exact arithmetic; store the exact zero.
    matrix[GetIndex(p, q)] = 0.0;
    matrix[GetIndex(q, p)] = 0.0;
    for (uint32 k = 0; k < AXIS_COUNT; ++k)
    {
        const float64 kp = vectors[GetIndex(k, p)];
        const float64 kq = vectors[GetIndex(k, q)];
        vectors[GetIndex(k, p)] = cosine * kp - sine * kq;
        vectors[GetIndex(k, q)] = sine * kp + cosine * kq;
    }
}

Rotation MakeRotation(float64 x, float64 y, float64 z, float64 w) noexcept
{
    return { .X = x, .Y = y, .Z = z, .W = w };
}

float64 GetDeterminant(const Matrix3x3& matrix) noexcept
{
    return math::Dot(matrix.ColumnX, math::Cross(matrix.ColumnY, matrix.ColumnZ));
}
} // namespace

Rotation Normalize(Rotation rotation) noexcept
{
    const float64 norm = math::Sqrt(GetNormSquared(rotation));
    LUDUS_ASSERT(norm > 0.0);
    return {rotation.X / norm, rotation.Y / norm, rotation.Z / norm, rotation.W / norm};
}

math::Vector3d Rotate(Rotation rotation, math::Vector3d value) noexcept
{
    // v' = v + 2 w (u x v) + 2 u x (u x v), with u the vector part.
    const math::Vector3d axis{rotation.X, rotation.Y, rotation.Z};
    const math::Vector3d twiceCross = math::Cross(axis, value) * 2.0;
    return value + twiceCross * rotation.W + math::Cross(axis, twiceCross);
}

Rotation IntegrateRotation(Rotation rotation, math::Vector3d angularVelocity, float64 seconds) noexcept
{
    const float64 speed = math::Length(angularVelocity);
    const float64 halfAngle = 0.5 * speed * seconds;
    if (halfAngle == 0.0)
    {
        return rotation;
    }
    const math::Vector3d axis = angularVelocity * (math::Sin(halfAngle) / speed);
    const Rotation increment{axis.X, axis.Y, axis.Z, math::Cos(halfAngle)};
    return Normalize(Multiply(increment, rotation));
}

math::Vector3d GetShortestArcVelocity(Rotation from, Rotation to, float64 seconds) noexcept
{
    Rotation delta = Multiply(to, Conjugate(from));
    // q and -q are the same rotation; the nonnegative scalar part is the shorter arc.
    if (delta.W < 0.0)
    {
        delta = {-delta.X, -delta.Y, -delta.Z, -delta.W};
    }
    const math::Vector3d axis{delta.X, delta.Y, delta.Z};
    const float64 sine = math::Length(axis);
    if (sine == 0.0)
    {
        return {};
    }
    const float64 angle = 2.0 * math::Atan2(sine, delta.W);
    return axis * (angle / (sine * seconds));
}

Matrix3x3 ToMatrix(Rotation rotation) noexcept
{
    return {Rotate(rotation, {1.0, 0.0, 0.0}), Rotate(rotation, {0.0, 1.0, 0.0}), Rotate(rotation, {0.0, 0.0, 1.0})};
}

Rotation FromMatrix(const Matrix3x3& matrix) noexcept
{
    const float64 xx = matrix.ColumnX.X;
    const float64 yy = matrix.ColumnY.Y;
    const float64 zz = matrix.ColumnZ.Z;
    const float64 trace = xx + yy + zz;
    // Four times the squared components less one are trace, xx - yy - zz, yy - xx - zz and zz - xx - yy; divide by
    // the largest component for accuracy.
    const float64 xCandidate = xx - yy - zz;
    const float64 yCandidate = yy - xx - zz;
    const float64 zCandidate = zz - xx - yy;
    Rotation rotation;
    if (trace >= xCandidate && trace >= yCandidate && trace >= zCandidate)
    {
        const float64 scale = math::Sqrt(1.0 + trace) * 2.0;
        rotation = MakeRotation((matrix.ColumnY.Z - matrix.ColumnZ.Y) / scale,
                                (matrix.ColumnZ.X - matrix.ColumnX.Z) / scale,
                                (matrix.ColumnX.Y - matrix.ColumnY.X) / scale,
                                scale * QUARTER);
    }
    else if (xCandidate >= yCandidate && xCandidate >= zCandidate)
    {
        const float64 scale = math::Sqrt(1.0 + xx - yy - zz) * 2.0;
        rotation = MakeRotation(scale * QUARTER,
                                (matrix.ColumnY.X + matrix.ColumnX.Y) / scale,
                                (matrix.ColumnZ.X + matrix.ColumnX.Z) / scale,
                                (matrix.ColumnY.Z - matrix.ColumnZ.Y) / scale);
    }
    else if (yCandidate >= zCandidate)
    {
        const float64 scale = math::Sqrt(1.0 + yy - xx - zz) * 2.0;
        rotation = MakeRotation((matrix.ColumnY.X + matrix.ColumnX.Y) / scale,
                                scale * QUARTER,
                                (matrix.ColumnZ.Y + matrix.ColumnY.Z) / scale,
                                (matrix.ColumnZ.X - matrix.ColumnX.Z) / scale);
    }
    else
    {
        const float64 scale = math::Sqrt(1.0 + zz - xx - yy) * 2.0;
        rotation = MakeRotation((matrix.ColumnZ.X + matrix.ColumnX.Z) / scale,
                                (matrix.ColumnZ.Y + matrix.ColumnY.Z) / scale,
                                scale * QUARTER,
                                (matrix.ColumnX.Y - matrix.ColumnY.X) / scale);
    }
    return Normalize(rotation);
}

bool TrySolve(const Matrix3x3& matrix, math::Vector3d value, math::Vector3d& outSolution) noexcept
{
    const float64 scale = GetScale(ToElements(matrix));
    const float64 determinant = GetDeterminant(matrix);
    if (scale == 0.0 || math::IsFinite(determinant) == false ||
        math::Abs(determinant) <= MIN_RELATIVE_DETERMINANT * scale * scale * scale)
    {
        return false;
    }
    const math::Vector3d solution{GetDeterminant({value, matrix.ColumnY, matrix.ColumnZ}) / determinant,
                                  GetDeterminant({matrix.ColumnX, value, matrix.ColumnZ}) / determinant,
                                  GetDeterminant({matrix.ColumnX, matrix.ColumnY, value}) / determinant};
    if (math::IsFinite(solution) == false)
    {
        return false;
    }
    outSolution = solution;
    return true;
}

InertiaTensor ToTensor(const Matrix3x3& matrix) noexcept
{
    return
    {
        .Xx = matrix.ColumnX.X,
        .Yy = matrix.ColumnY.Y,
        .Zz = matrix.ColumnZ.Z,
        .Xy = 0.5 * (matrix.ColumnY.X + matrix.ColumnX.Y),
        .Xz = 0.5 * (matrix.ColumnZ.X + matrix.ColumnX.Z),
        .Yz = 0.5 * (matrix.ColumnZ.Y + matrix.ColumnY.Z),
    };
}

bool TryDecomposeSymmetric(const Matrix3x3& matrix, SymmetricEigen& outEigen) noexcept
{
    JacobiState state{ .Matrix = ToElements(matrix), .Vectors = ToElements(MakeDiagonal({1.0, 1.0, 1.0})) };
    const Elements& elements = state.Matrix;
    const float64 tolerance = JACOBI_RELATIVE_TOLERANCE * GetScale(elements);
    bool hasConverged = false;
    for (uint32 sweep = 0; sweep < MAX_JACOBI_SWEEPS; ++sweep)
    {
        if (GetOffDiagonalMagnitude(elements) <= tolerance)
        {
            hasConverged = true;
            break;
        }
        RotateJacobi(X_AXIS, Y_AXIS, state);
        RotateJacobi(X_AXIS, Z_AXIS, state);
        RotateJacobi(Y_AXIS, Z_AXIS, state);
    }
    if (hasConverged == false && GetOffDiagonalMagnitude(elements) > tolerance)
    {
        return false;
    }
    Matrix3x3 eigenvectors = FromElements(state.Vectors);
    // Jacobi rotations preserve the determinant, so the columns are already right-handed; renormalize rounding.
    if (GetDeterminant(eigenvectors) < 0.0)
    {
        eigenvectors.ColumnZ = -eigenvectors.ColumnZ;
    }
    outEigen.Values = {elements[GetIndex(X_AXIS, X_AXIS)],
                       elements[GetIndex(Y_AXIS, Y_AXIS)],
                       elements[GetIndex(Z_AXIS, Z_AXIS)]};
    outEigen.Vectors = eigenvectors;
    return true;
}

Status ValidateOrientation(math::Quaternion orientation) noexcept
{
    if (math::IsFinite(orientation) == false)
    {
        return Status::NonFiniteInput;
    }
    return math::Abs(GetNormSquared(Widen(orientation)) - 1.0) > collision::UNIT_QUATERNION_SQUARED_TOLERANCE
               ? Status::InvalidArgument
               : Status::Success;
}

Status ValidatePosition(math::Vector3d position) noexcept
{
    return ValidateBounded(position, static_cast<float64>(collision::MAX_COORDINATE_METERS));
}

Status ValidateLocalPose(const BodyLocalPose& pose) noexcept
{
    if (math::IsFinite(pose.PositionBody) == false || math::IsFinite(pose.OrientationBody) == false)
    {
        return Status::NonFiniteInput;
    }
    const Status position = ValidatePosition(pose.PositionBody);
    return position != Status::Success ? position : ValidateOrientation(pose.OrientationBody);
}

Status ValidateBounded(math::Vector3d value, float64 limit) noexcept
{
    if (math::IsFinite(value) == false)
    {
        return Status::NonFiniteInput;
    }
    return GetMaxMagnitude(value) > limit ? Status::OutOfRange : Status::Success;
}
} // namespace ludus::physics::rigid::internal
