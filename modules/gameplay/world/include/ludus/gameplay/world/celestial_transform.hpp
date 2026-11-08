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
inline constexpr foundation::float64 MAX_EARTH_LATITUDE_DEGREES = 90;
/// Largest magnitude of an observer's longitude, in degrees.
inline constexpr foundation::float64 MAX_EARTH_LONGITUDE_DEGREES = 180;

/// A sample of a celestial transform driver in the observer's world frame.
struct EarthCelestialTransform final
{
    /// Rotation that points local -Z along incoming light, with world +X east, +Y up and -Z north.
    foundation::math::Quaternion Rotation{};
    /// True when the center of the body is strictly above the geometric horizon.
    bool IsAboveHorizon = false;
};

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
