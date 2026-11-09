#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer
{
/// Spherical Earth clear-sky background resolved from one World atmosphere and directional light.
struct AtmosphereDescription final
{
    /// False preserves the graph's authored background. Other fields are ignored when false.
    bool IsEnabled = false;
    /// Surface anchor in world metres. The ground sphere is tangent to this point.
    foundation::math::Vector3 SurfaceAnchorWorld{};
    /// Nonzero finite outward direction at the anchor, normalized during preparation.
    foundation::math::Vector3 SurfaceUpWorld{0, 1, 0};
    /// Nonzero finite direction toward the Sun, normalized during preparation.
    foundation::math::Vector3 DirectionToSunWorld{0, 1, 0};
    /// Finite nonnegative linear RGB illuminance in lux on a plane perpendicular to the Sun.
    foundation::math::Vector3 SolarIlluminance{};
    /// Earth profile's nominal solar radius in radians, from Hillaire's accompanying sample.
    static constexpr foundation::float32 DEFAULT_SOLAR_RADIUS_RADIANS = 0.004675F;
    /// Solar disk radius in radians, finite and strictly between zero and half pi.
    foundation::float32 SolarAngularRadiusRadians = DEFAULT_SOLAR_RADIUS_RADIANS;
};
/// Compute the signed altitude in metres above an Earth clear-sky surface.
/// Invalid anchor, up or eye coordinates preserve outAltitudeMetres.
[[nodiscard]] bool TryGetAtmosphereAltitude(const AtmosphereDescription& atmosphere,
                                            foundation::math::Vector3 eyeWorld,
                                            foundation::float64& outAltitudeMetres) noexcept;
/// Top boundary altitude in metres for the Earth clear-sky profile.
[[nodiscard]] foundation::float64 GetAtmosphereTopAltitude() noexcept;
} // namespace ludus::graphics::renderer
