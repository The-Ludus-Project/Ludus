#pragma once

#include "rigid_math.h"

#include <ludus/foundation/base/core.h>

#include <ludus/physics/rigid/mass_properties.h>
#include <ludus/physics/rigid/status.h>

namespace ludus::physics::rigid::internal
{
/// Principal frame of admitted mass properties with the rotation kept in float64.
struct PrincipalFrame final
{
    float64 MassKilograms = 0.0;
    math::Vector3d CenterOfMassBody{};
    Rotation BodyFromCenterOfMass{};
    math::Vector3d PrincipalMoments{};
};

/// Validate `properties` and diagonalize their inertia. @return As for TryComputePrincipalMassFrame.
[[nodiscard]] Status ComputePrincipalFrame(const MassProperties& properties, PrincipalFrame& outFrame) noexcept;
} // namespace ludus::physics::rigid::internal
