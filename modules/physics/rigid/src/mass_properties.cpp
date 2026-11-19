// Mass-frame contracts follow Eric Lengyel's chapter 14 of Game Engine Gems 1 as recorded in
// docs/architecture/physics-systems-gems-review.md: the COM frame is stored apart from the authored body frame and
// compound parts are composed with rotated tensors and the parallel-axis theorem.

#include <ludus/physics/rigid/mass_properties.h>

#include "internal/hull_mass.h"
#include "internal/mass_frame.h"
#include "internal/rigid_math.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/physics/collision/queries.h>

namespace ludus::physics::rigid
{
namespace
{
namespace math = foundation::math;
using internal::Matrix3x3;

constexpr float64 BALL_VOLUME_FACTOR = 4.0 / 3.0;       ///< Ball volume over pi r^3.
constexpr float64 HEMISPHERE_VOLUME_FACTOR = 2.0 / 3.0; ///< Hemisphere volume over pi r^3.
constexpr float64 SOLID_BALL_INERTIA_FACTOR = 0.4;      ///< Ball moment over m r^2.
constexpr float64 BOX_VOLUME_FROM_HALF_EXTENTS = 8.0;   ///< Box volume over the product of half extents.
constexpr float64 BOX_INERTIA_FACTOR = 1.0 / 3.0;       ///< Box moment over m (b^2 + c^2) for half extents.
constexpr float64 CYLINDER_RADIAL_FACTOR = 0.25;        ///< Cylinder transverse moment term over m r^2.
constexpr float64 CYLINDER_LENGTH_FACTOR = 1.0 / 12.0;  ///< Cylinder transverse moment term over m h^2.
constexpr float64 HEMISPHERE_LENGTH_FACTOR = 0.25;      ///< Capsule cap term over m h^2.
constexpr float64 HEMISPHERE_CROSS_FACTOR = 3.0 / 8.0;  ///< Capsule cap term over m h r.

Status ValidateDensity(float64 density) noexcept
{
    if (math::IsFinite(density) == false)
    {
        return Status::NonFiniteInput;
    }
    if (density <= 0.0)
    {
        return Status::InvalidArgument;
    }
    return density > MAX_DENSITY_KILOGRAMS_PER_CUBIC_METER ? Status::OutOfRange : Status::Success;
}

/// Validate one primitive dimension: finite, at most the query distance limit, and positive unless `canBeZero`.
Status ValidateDimension(float64 meters, bool canBeZero) noexcept
{
    if (math::IsFinite(meters) == false)
    {
        return Status::NonFiniteInput;
    }
    if (meters < 0.0 || (meters == 0.0 && canBeZero == false))
    {
        return Status::InvalidArgument;
    }
    return meters > collision::MAX_QUERY_DISTANCE_METERS ? Status::OutOfRange : Status::Success;
}

Status ComputeSphere(float64 radius, float64 density, MassProperties& outProperties) noexcept
{
    const Status dimension = ValidateDimension(radius, false);
    if (dimension != Status::Success)
    {
        return dimension;
    }
    const float64 mass = density * BALL_VOLUME_FACTOR * math::kPi * radius * radius * radius;
    const float64 moment = SOLID_BALL_INERTIA_FACTOR * mass * radius * radius;
    outProperties =
    {
        .MassKilograms = mass,
        .InertiaAboutCenterOfMass = { .Xx = moment, .Yy = moment, .Zz = moment },
    };
    return Status::Success;
}

Status ComputeBox(math::Vector3d halfExtents, float64 density, MassProperties& outProperties) noexcept
{
    for (uint32 axis = 0; axis < internal::AXIS_COUNT; ++axis)
    {
        const Status dimension = ValidateDimension(internal::GetComponent(halfExtents, axis), false);
        if (dimension != Status::Success)
        {
            return dimension;
        }
    }
    const float64 mass = density * BOX_VOLUME_FROM_HALF_EXTENTS * halfExtents.X * halfExtents.Y * halfExtents.Z;
    const float64 xx = halfExtents.X * halfExtents.X;
    const float64 yy = halfExtents.Y * halfExtents.Y;
    const float64 zz = halfExtents.Z * halfExtents.Z;
    outProperties =
    {
        .MassKilograms = mass,
        .InertiaAboutCenterOfMass =
        {
            .Xx = BOX_INERTIA_FACTOR * mass * (yy + zz),
            .Yy = BOX_INERTIA_FACTOR * mass * (xx + zz),
            .Zz = BOX_INERTIA_FACTOR * mass * (xx + yy),
        },
    };
    return Status::Success;
}

/// A Y-axis cylinder of height 2 * halfHeight capped by two hemispheres of the same radius.
Status ComputeCapsule(float64 halfHeight, float64 radius, float64 density, MassProperties& outProperties) noexcept
{
    const Status radiusStatus = ValidateDimension(radius, false);
    if (radiusStatus != Status::Success)
    {
        return radiusStatus;
    }
    const Status heightStatus = ValidateDimension(halfHeight, true);
    if (heightStatus != Status::Success)
    {
        return heightStatus;
    }
    const float64 height = 2.0 * halfHeight;
    const float64 radiusSquared = radius * radius;
    const float64 cylinderMass = density * math::kPi * radiusSquared * height;
    const float64 capMass = density * HEMISPHERE_VOLUME_FACTOR * math::kPi * radiusSquared * radius;
    const float64 axial =
        0.5 * cylinderMass * radiusSquared + 2.0 * SOLID_BALL_INERTIA_FACTOR * capMass * radiusSquared;
    // Each cap: its moment about the capsule centre, from the hemisphere's COM at 3r/8 by the parallel-axis theorem.
    const float64 capTransverse =
        capMass * (SOLID_BALL_INERTIA_FACTOR * radiusSquared + HEMISPHERE_LENGTH_FACTOR * height * height +
                   HEMISPHERE_CROSS_FACTOR * height * radius);
    const float64 transverse =
        cylinderMass * (CYLINDER_RADIAL_FACTOR * radiusSquared + CYLINDER_LENGTH_FACTOR * height * height) +
        2.0 * capTransverse;
    outProperties =
    {
        .MassKilograms = cylinderMass + 2.0 * capMass,
        .InertiaAboutCenterOfMass = { .Xx = transverse, .Yy = axial, .Zz = transverse },
    };
    return Status::Success;
}

Status ComputeHull(const collision::ConvexHull* hullOrNull, float64 density, MassProperties& outProperties) noexcept
{
    if (hullOrNull == nullptr || hullOrNull->IsCooked() == false)
    {
        return Status::InvalidArgument;
    }
    internal::UnitDensityMass unit;
    const Status integrated = internal::ComputeHullUnitMass(hullOrNull->GetPointsShape(), unit);
    if (integrated != Status::Success)
    {
        return integrated;
    }
    outProperties =
    {
        .MassKilograms = density * unit.VolumeCubicMeters,
        .CenterOfMassLocal = unit.CenterOfMassShape,
        .InertiaAboutCenterOfMass = internal::ToTensor(Scale(internal::FromTensor(unit.UnitInertia), density)),
    };
    return Status::Success;
}

bool IsTensorFinite(const InertiaTensor& tensor) noexcept
{
    return math::IsFinite(tensor.Xx) == true && math::IsFinite(tensor.Yy) == true &&
           math::IsFinite(tensor.Zz) == true && math::IsFinite(tensor.Xy) == true &&
           math::IsFinite(tensor.Xz) == true && math::IsFinite(tensor.Yz) == true;
}

/// Validate and diagonalize; shared by validation and principal-frame computation.
Status ValidateAndDecompose(const MassProperties& properties, internal::SymmetricEigen& outEigen) noexcept
{
    if (math::IsFinite(properties.MassKilograms) == false || math::IsFinite(properties.CenterOfMassLocal) == false ||
        IsTensorFinite(properties.InertiaAboutCenterOfMass) == false)
    {
        return Status::NonFiniteInput;
    }
    if (properties.MassKilograms < MIN_MASS_KILOGRAMS || properties.MassKilograms > MAX_MASS_KILOGRAMS)
    {
        return Status::InvalidMassProperties;
    }
    const Status center = internal::ValidatePosition(properties.CenterOfMassLocal);
    if (center != Status::Success)
    {
        return center;
    }
    internal::SymmetricEigen eigen;
    if (internal::TryDecomposeSymmetric(internal::FromTensor(properties.InertiaAboutCenterOfMass), eigen) == false)
    {
        return Status::InvalidMassProperties;
    }
    const math::Vector3d moments = eigen.Values;
    const float64 smallest = math::Min(moments.X, math::Min(moments.Y, moments.Z));
    const float64 largest = math::Max(moments.X, math::Max(moments.Y, moments.Z));
    const float64 sum = moments.X + moments.Y + moments.Z;
    if (smallest <= 0.0 || smallest < MIN_INERTIA_CONDITION_RATIO * largest ||
        largest > (sum - largest) * (1.0 + INERTIA_TRIANGLE_RELATIVE_TOLERANCE))
    {
        return Status::InvalidMassProperties;
    }
    outEigen = eigen;
    return Status::Success;
}
} // namespace

Status TryComputeShapeMassProperties(const collision::ConvexShape& shape,
                                     float64 densityKilogramsPerCubicMeter,
                                     MassProperties& outProperties) noexcept
{
    const Status density = ValidateDensity(densityKilogramsPerCubicMeter);
    if (density != Status::Success)
    {
        return density;
    }
    MassProperties properties;
    Status computed = Status::InvalidArgument;
    switch (shape.Kind)
    {
        case collision::ConvexShapeKind::Sphere:
            computed = ComputeSphere(shape.RadiusMeters, densityKilogramsPerCubicMeter, properties);
            break;
        case collision::ConvexShapeKind::Box:
            computed = ComputeBox(shape.HalfExtentsMeters, densityKilogramsPerCubicMeter, properties);
            break;
        case collision::ConvexShapeKind::Capsule:
            computed =
                ComputeCapsule(shape.HalfHeightMeters, shape.RadiusMeters, densityKilogramsPerCubicMeter, properties);
            break;
        case collision::ConvexShapeKind::Hull:
            computed = ComputeHull(shape.HullOrNull, densityKilogramsPerCubicMeter, properties);
            break;
    }
    if (computed != Status::Success)
    {
        return computed;
    }
    internal::SymmetricEigen unused;
    const Status validation = ValidateAndDecompose(properties, unused);
    if (validation != Status::Success)
    {
        // A valid shape and density can still exceed the mass limit or produce a degenerate thin tensor.
        return validation == Status::OutOfRange ? Status::OutOfRange : Status::InvalidMassProperties;
    }
    outProperties = properties;
    return Status::Success;
}

Status TryComposeMassProperties(foundation::BorrowedView<const MassPart> parts, MassProperties& outProperties) noexcept
{
    if (parts.IsValid() == false || parts.IsEmpty() == true)
    {
        return Status::InvalidArgument;
    }
    if (parts.GetSize() > MAX_MASS_PARTS)
    {
        return Status::CapacityExceeded;
    }
    float64 mass = 0.0;
    math::Vector3d weightedCenter{};
    for (const MassPart& part : parts)
    {
        const Status pose = internal::ValidateLocalPose(part.Pose);
        if (pose != Status::Success)
        {
            return pose;
        }
        internal::SymmetricEigen unused;
        const Status properties = ValidateAndDecompose(part.Properties, unused);
        if (properties != Status::Success)
        {
            return properties;
        }
        const internal::Rotation rotation = internal::WidenNormalized(part.Pose.OrientationBody);
        const math::Vector3d centerBody =
            part.Pose.PositionBody + internal::Rotate(rotation, part.Properties.CenterOfMassLocal);
        mass += part.Properties.MassKilograms;
        weightedCenter = weightedCenter + centerBody * part.Properties.MassKilograms;
    }
    const math::Vector3d center = weightedCenter * (1.0 / mass);
    Matrix3x3 inertia{};
    for (const MassPart& part : parts)
    {
        const internal::Rotation rotation = internal::WidenNormalized(part.Pose.OrientationBody);
        const Matrix3x3 rotationMatrix = internal::ToMatrix(rotation);
        const math::Vector3d offset =
            part.Pose.PositionBody + internal::Rotate(rotation, part.Properties.CenterOfMassLocal) - center;
        const float64 partMass = part.Properties.MassKilograms;
        const float64 distanceSquared = math::LengthSquared(offset);
        // Parallel-axis theorem: I = R I_part R^T + m (|d|^2 E - d d^T).
        const Matrix3x3 shift = Add(internal::MakeDiagonal({distanceSquared, distanceSquared, distanceSquared}),
                                    Scale(internal::MakeOuterProduct(offset, offset), -1.0));
        inertia = Add(
            inertia,
            Add(internal::RotateTensor(rotationMatrix, internal::FromTensor(part.Properties.InertiaAboutCenterOfMass)),
                Scale(shift, partMass)));
    }
    const MassProperties composed
    {
        .MassKilograms = mass,
        .CenterOfMassLocal = center,
        .InertiaAboutCenterOfMass = internal::ToTensor(inertia),
    };
    internal::SymmetricEigen unused;
    const Status validation = ValidateAndDecompose(composed, unused);
    if (validation != Status::Success)
    {
        return validation;
    }
    outProperties = composed;
    return Status::Success;
}

Status TryValidateMassProperties(const MassProperties& properties) noexcept
{
    internal::SymmetricEigen unused;
    return ValidateAndDecompose(properties, unused);
}

Status TryComputePrincipalMassFrame(const MassProperties& properties, PrincipalMassFrame& outFrame) noexcept
{
    internal::PrincipalFrame frame;
    const Status computed = internal::ComputePrincipalFrame(properties, frame);
    if (computed != Status::Success)
    {
        return computed;
    }
    outFrame =
    {
        .MassKilograms = frame.MassKilograms,
        .BodyFromCenterOfMass =
        {
            .PositionBody = frame.CenterOfMassBody,
            .OrientationBody = internal::Narrow(frame.BodyFromCenterOfMass),
        },
        .PrincipalMomentsKilogramMetersSquared = frame.PrincipalMoments,
    };
    return Status::Success;
}

namespace internal
{
Status ComputePrincipalFrame(const MassProperties& properties, PrincipalFrame& outFrame) noexcept
{
    SymmetricEigen eigen;
    const Status validation = ValidateAndDecompose(properties, eigen);
    if (validation != Status::Success)
    {
        return validation;
    }
    outFrame =
    {
        .MassKilograms = properties.MassKilograms,
        .CenterOfMassBody = properties.CenterOfMassLocal,
        .BodyFromCenterOfMass = FromMatrix(eigen.Vectors),
        .PrincipalMoments = eigen.Values,
    };
    return Status::Success;
}
} // namespace internal
} // namespace ludus::physics::rigid
