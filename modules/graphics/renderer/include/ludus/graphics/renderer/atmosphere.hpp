#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/graphics/renderer/lighting_layout.h>

namespace ludus::graphics::renderer
{
/// An unresolved planet on the distant celestial boundary.
struct PlanetBackgroundDescription final
{
    /// Nonzero finite world direction toward a visible source. Normalized during preparation.
    foundation::math::Vector3 DirectionWorld{};
    /// Finite nonnegative linear RGB illuminance in lux before extinction. Zero omits this point.
    foundation::math::Vector3 Illuminance{};
};

/// Stars, planets, the Milky Way and an optional Moon supply scene-linear radiance beyond the atmosphere shell.
struct CelestialBackgroundDescription final
{
    /// Enable the celestial boundary and perspective views from orbit.
    bool IsEnabled = false;
    /// Finite nonnegative procedural star peak luminance in scene-linear nits. Zero disables stars.
    foundation::float32 StarRadiance = 0;
    /// Nonzero finite world direction toward the Moon, normalized when its disk is present.
    foundation::math::Vector3 DirectionToMoonWorld{0, 0, 1};
    /// Finite nonnegative linear RGB peak luminance in nits at full phase.
    foundation::math::Vector3 LunarPeakRadiance{};
    /// Apparent lunar radius in radians. Zero omits the disk. A positive radius must be less than half pi.
    foundation::float32 LunarAngularRadiusRadians = 0;
    /// Procedural Milky Way brightness scale in scene-linear nits. Finite and nonnegative. Zero omits the band.
    foundation::float32 MilkyWayRadiance = 0;
    /// Finite nonzero world direction toward the galactic north pole, used when the Milky Way is enabled.
    foundation::math::Vector3 GalacticNorthWorld{0, 1, 0};
    /// Finite nonzero world direction toward the galactic center. Its projection onto the galactic plane must
    /// be nonzero when the Milky Way is enabled. The procedural stars then also use this celestial frame.
    foundation::math::Vector3 GalacticCenterWorld{0, 0, -1};
    /// Optional background points. They receive the same view transmittance and occlusion as the stars.
    foundation::StaticArray<PlanetBackgroundDescription, ATMOSPHERE_PLANET_CAPACITY> Planets{};
    /// Nominal apparent lunar radius of one-quarter degree, from NASA Science's Hubble Glossary.
    static constexpr foundation::float32 DEFAULT_LUNAR_RADIUS_RADIANS = 0.004363323F;
    /// Maximum perspective observer altitude in metres for the orbital background.
    static constexpr foundation::float64 MAXIMUM_ORBITAL_ALTITUDE_METRES = 1'000'000'000;
};

/// Spherical Earth atmosphere resolved from one World entity and directional light.
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
    /// Stable source identity of the linked directional light in the scene. Zero supplies no direct light.
    foundation::uint64 SunSourceId = 0;
    /// Enable multiple scattering, skylight and aerial perspective for opaque deferred surfaces.
    /// Only applies when the view is admitted to the atmosphere shell.
    bool ShouldLightScene = false;
    /// Optional boundary provider. Its phase uses DirectionToSunWorld, even when SolarIlluminance is zero.
    CelestialBackgroundDescription Celestial{};
};
/// Compute the signed altitude in metres above an Earth clear-sky surface.
/// Invalid anchor, up or eye coordinates preserve outAltitudeMetres.
[[nodiscard]] bool TryGetAtmosphereAltitude(const AtmosphereDescription& atmosphere,
                                            foundation::math::Vector3 eyeWorld,
                                            foundation::float64& outAltitudeMetres) noexcept;
/// Top boundary altitude in metres for the Earth clear-sky profile.
[[nodiscard]] foundation::float64 GetAtmosphereTopAltitude() noexcept;
/// Maximum background observer altitude in metres for the given projection and celestial settings.
[[nodiscard]] foundation::float64 GetAtmosphereMaximumViewAltitude(const AtmosphereDescription& atmosphere,
                                                                   bool isOrthographic) noexcept;
} // namespace ludus::graphics::renderer
