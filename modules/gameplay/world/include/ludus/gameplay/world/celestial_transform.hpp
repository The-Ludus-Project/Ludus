#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/quaternion.hpp>

namespace ludus::gameplay::world
{
/// Built-in Earth sky rules for driving a transform's rotation.
enum class EarthCelestialBody : foundation::uint8
{
    Sun,  ///< Approximate the Sun's apparent path.
    Moon, ///< Approximate the Moon's apparent path, including parallax at the observer.
};

/// Largest magnitude of a supported date offset from J2000.0, in days (approximately 1950–2050).
inline constexpr foundation::float64 MAX_EARTH_CELESTIAL_DAYS = 18262.5;
/// Largest magnitude of an observer's latitude, in degrees.
inline constexpr foundation::float64 MAX_CELESTIAL_LATITUDE_DEGREES = 90;
/// Largest magnitude of an observer's longitude, in degrees.
inline constexpr foundation::float64 MAX_CELESTIAL_LONGITUDE_DEGREES = 180;
/// Earth sky latitude limit, in degrees.
inline constexpr foundation::float64 MAX_EARTH_LATITUDE_DEGREES = MAX_CELESTIAL_LATITUDE_DEGREES;
/// Earth sky longitude limit, in degrees.
inline constexpr foundation::float64 MAX_EARTH_LONGITUDE_DEGREES = MAX_CELESTIAL_LONGITUDE_DEGREES;
/// Largest magnitude of a custom orbit clock offset, in simulation hours.
inline constexpr foundation::float64 MAX_CELESTIAL_ORBIT_HOURS = 100000000;
/// Largest magnitude of a spin or orbit rate, in degrees per simulation hour.
inline constexpr foundation::float64 MAX_CELESTIAL_RATE_DEGREES_PER_HOUR = 360000;
/// Largest signed angle magnitude for a custom orbit's phase and ascending node, in degrees.
inline constexpr foundation::float64 MAX_CELESTIAL_ORBIT_ANGLE_DEGREES = 180;
/// Largest observer radius divided by orbit radius. Leaves at least 1/256 of the orbit radius as clearance.
inline constexpr foundation::float64 MAX_CELESTIAL_OBSERVER_RADIUS_RATIO = 0.99609375;

/// A sample of a celestial transform driver in the observer's world frame.
struct CelestialTransform final
{
    /// Rotation that points local -Z along incoming light, with world +X east, +Y up and -Z north.
    foundation::math::Quaternion Rotation{};
    /// True when the center of the body is above the geometric horizon to numerical precision.
    bool IsAboveHorizon = false;
};

/// Result of the Earth sky sampler, in the same frame as a custom orbit sample.
using EarthCelestialTransform = CelestialTransform;

/// An observer on a spherical world with a fixed spin axis. Zero longitude uses the reference meridian at time zero.
struct CelestialObserver final
{
    /// Latitude in degrees, north positive.
    foundation::float64 LatitudeDegrees = 0;
    /// Longitude in degrees at time zero, east positive.
    foundation::float64 LongitudeDegrees = 0;
    /// Signed world spin in degrees per simulation hour. Positive turns the observer east. Zero fixes the meridian.
    foundation::float64 SpinDegreesPerHour = 0;
};

/// Prescribed circular path of a star or satellite relative to the observer world's center and equator.
struct CelestialOrbit final
{
    /// Signed motion in degrees per simulation hour. Positive advances from the ascending node. Zero fixes the phase.
    foundation::float64 DegreesPerHour = 0;
    /// Position on the orbit at time zero, in degrees measured from its ascending node.
    foundation::float64 PhaseDegrees = 0;
    /// Angle in degrees between orbit and equatorial planes, from zero to MAX_CELESTIAL_ORBIT_ANGLE_DEGREES.
    foundation::float64 InclinationDegrees = 0;
    /// Angle east in degrees from the reference meridian at time zero to the ascending node.
    foundation::float64 AscendingNodeDegrees = 0;
    /// Observer distance from the world's center divided by orbit radius. Zero gives a distant source.
    foundation::float64 ObserverRadiusRatio = 0;
};

/// Sample a circular sky orbit using a caller-owned clock shared by its observer and any other bodies.
/// @param orbit Bounded signed rate, phase and node, nonnegative inclination and observer radius ratio.
/// @param observer Bounded latitude, longitude and signed spin rate.
/// @param elapsedHours Fractional simulation hours relative to the authored epoch. Negative times are valid.
/// @param outTransform Replaced on success. Invalid inputs preserve it.
/// @return True on success. False for nonfinite inputs or values outside the documented limits.
/// @note Allocation-free, with no thread affinity. Includes geometric parallax on a spherical world.
[[nodiscard]] bool TrySampleCelestialOrbitTransform(const CelestialOrbit& orbit,
                                                    const CelestialObserver& observer,
                                                    foundation::float64 elapsedHours,
                                                    CelestialTransform& outTransform) noexcept;

/// Sample an Earth sky rule at a caller-supplied time. The caller owns the clock and applies the rotation.
/// @param body Sun or Moon.
/// @param daysSinceJ2000 Fractional UTC days since 2000-01-01 12:00 UTC, within MAX_EARTH_CELESTIAL_DAYS.
/// @param latitudeDegrees Observer latitude in degrees, north positive.
/// @param longitudeDegrees Observer longitude in degrees, east positive.
/// @param outTransform Replaced on success. Invalid inputs preserve it.
/// @return True on success. False for an unknown body, a nonfinite value or an input outside the documented range.
/// @note Allocation-free, with no thread affinity. Uses a spherical Earth and a geometric horizon at sea level.
[[nodiscard]] bool TrySampleEarthCelestialTransform(EarthCelestialBody body,
                                                    foundation::float64 daysSinceJ2000,
                                                    foundation::float64 latitudeDegrees,
                                                    foundation::float64 longitudeDegrees,
                                                    EarthCelestialTransform& outTransform) noexcept;
} // namespace ludus::gameplay::world
