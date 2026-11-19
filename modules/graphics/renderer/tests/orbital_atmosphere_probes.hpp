#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer::testing
{
// Shared camera and lighting inputs for the renderer oracle and native World viewport sweep.
inline constexpr foundation::float64 EARTH_GROUND_RADIUS_KILOMETRES = 6360;
inline constexpr foundation::float64 EARTH_TOP_RADIUS_KILOMETRES = 6460;
inline constexpr foundation::float64 ORBIT_ALTITUDE_KILOMETRES = 500;
inline constexpr foundation::float32 ORBIT_HEIGHT_METRES =
    static_cast<foundation::float32>(ORBIT_ALTITUDE_KILOMETRES * 1000);
inline constexpr foundation::float32 ORBIT_VERTICAL_FOV_RADIANS = 0.01F;

enum class OrbitalRay
{
    Miss,
    OuterChord,
    AboveGroundTangent,
    GroundHit,
};

struct OrbitalProbe
{
    OrbitalRay Ray;
    foundation::float64 TangentAltitudeKilometres;
    foundation::float32 DayIlluminance;
    foundation::float32 TwilightIlluminance;
};

// Fixed illuminances put even the thin chord above the linear UNORM8 readback floor.
inline constexpr foundation::usize ORBITAL_PROBE_COUNT = 4;
inline constexpr foundation::StaticArray<OrbitalProbe, ORBITAL_PROBE_COUNT> ORBITAL_PROBES{
    {{OrbitalRay::Miss, 103, 32768, 32768},
     {OrbitalRay::OuterChord, 97, 32768, 32768},
     {OrbitalRay::AboveGroundTangent, 1, 8, 256},
     {OrbitalRay::GroundHit, -1, 8, 256}}};

inline foundation::math::Vector3d GetOrbitalProbeDirection(foundation::float64 tangentAltitudeKilometres)
{
    const foundation::float64 horizontal = (EARTH_GROUND_RADIUS_KILOMETRES + tangentAltitudeKilometres) /
                                           (EARTH_GROUND_RADIUS_KILOMETRES + ORBIT_ALTITUDE_KILOMETRES);
    return {0, -foundation::math::Sqrt(1 - horizontal * horizontal), -horizontal};
}

inline foundation::math::Vector3 GetOrbitalSunDirection(bool isTwilight)
{
    constexpr foundation::float32 TWILIGHT_SUN_Y = -0.43F;
    return isTwilight == true ? foundation::math::Vector3{0,
                                                          TWILIGHT_SUN_Y,
                                                          -foundation::math::Sqrt(1 - TWILIGHT_SUN_Y * TWILIGHT_SUN_Y)}
                              : foundation::math::Vector3{0, 1, 0};
}
} // namespace ludus::graphics::renderer::testing
