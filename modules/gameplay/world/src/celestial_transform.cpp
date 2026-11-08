#include <ludus/gameplay/world/celestial_transform.hpp>

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::gameplay::world
{
namespace math = foundation::math;
using foundation::float32;
using foundation::float64;
namespace
{
struct EclipticCoordinates final
{
    float64 LongitudeRadians;
    float64 LatitudeRadians;
    float64 ParallaxRadians;
};

float64 ComputeSineDegrees(float64 degrees) noexcept
{
    return math::Sin(math::DegreesToRadians(degrees));
}

// Thanks to the U.S. Naval Observatory, "Computing Approximate Solar Coordinates"
// (https://aa.usno.navy.mil/faq/sun_approx), for the ecliptic longitude approximation and mean obliquity.
EclipticCoordinates ComputeSolarCoordinates(float64 days) noexcept
{
    constexpr float64 ANOMALY_AT_EPOCH_DEGREES = 357.529;
    constexpr float64 ANOMALY_DEGREES_PER_DAY = 0.98560028;
    constexpr float64 LONGITUDE_AT_EPOCH_DEGREES = 280.459;
    constexpr float64 LONGITUDE_DEGREES_PER_DAY = 0.98564736;
    constexpr float64 FIRST_HARMONIC_DEGREES = 1.915;
    constexpr float64 SECOND_HARMONIC_DEGREES = 0.020;
    const float64 anomalyDegrees = ANOMALY_AT_EPOCH_DEGREES + ANOMALY_DEGREES_PER_DAY * days;
    const float64 longitudeDegrees = LONGITUDE_AT_EPOCH_DEGREES + LONGITUDE_DEGREES_PER_DAY * days +
                                     FIRST_HARMONIC_DEGREES * ComputeSineDegrees(anomalyDegrees) +
                                     SECOND_HARMONIC_DEGREES * ComputeSineDegrees(2 * anomalyDegrees);
    return {math::DegreesToRadians(longitudeDegrees), 0, 0};
}

// Thanks to the U.S. Naval Observatory and Royal Greenwich Observatory, The Astronomical Almanac for the
// Year 1984, p. D46, and Eagle Engineering's PLANECHG technical documentation, Appendix H, pp. 80–81
// (https://ntrs.nasa.gov/citations/19890004071), for the lunar ecliptic and horizontal parallax series.
EclipticCoordinates ComputeLunarCoordinates(float64 centuries) noexcept
{
    constexpr float64 LONGITUDE_AT_EPOCH_DEGREES = 218.32;
    constexpr float64 LONGITUDE_DEGREES_PER_CENTURY = 481267.833;
    constexpr float64 ANOMALY_AT_EPOCH_DEGREES = 134.9;
    constexpr float64 ANOMALY_DEGREES_PER_CENTURY = 477198.85;
    constexpr float64 EVECTION_AT_EPOCH_DEGREES = 259.2;
    constexpr float64 EVECTION_DEGREES_PER_CENTURY = -413335.38;
    constexpr float64 VARIATION_AT_EPOCH_DEGREES = 235.7;
    constexpr float64 VARIATION_DEGREES_PER_CENTURY = 890534.23;
    constexpr float64 SECOND_ANOMALY_AT_EPOCH_DEGREES = 269.9;
    constexpr float64 SECOND_ANOMALY_DEGREES_PER_CENTURY = 954397.70;
    constexpr float64 SOLAR_ANOMALY_AT_EPOCH_DEGREES = 357.5;
    constexpr float64 SOLAR_ANOMALY_DEGREES_PER_CENTURY = 35999.05;
    constexpr float64 LONGITUDE_PERTURBATION_AT_EPOCH_DEGREES = 186.6;
    constexpr float64 LONGITUDE_PERTURBATION_DEGREES_PER_CENTURY = 966404.05;
    constexpr float64 LATITUDE_ARGUMENT_AT_EPOCH_DEGREES = 93.3;
    constexpr float64 LATITUDE_ARGUMENT_DEGREES_PER_CENTURY = 483202.03;
    constexpr float64 LATITUDE_HARMONIC_AT_EPOCH_DEGREES = 228.2;
    constexpr float64 LATITUDE_HARMONIC_DEGREES_PER_CENTURY = 960400.87;
    constexpr float64 LATITUDE_NODE_AT_EPOCH_DEGREES = 318.3;
    constexpr float64 LATITUDE_NODE_DEGREES_PER_CENTURY = 6003.18;
    constexpr float64 LATITUDE_PERTURBATION_AT_EPOCH_DEGREES = 217.6;
    constexpr float64 LATITUDE_PERTURBATION_DEGREES_PER_CENTURY = -407332.20;

    const float64 anomalyDegrees = ANOMALY_AT_EPOCH_DEGREES + ANOMALY_DEGREES_PER_CENTURY * centuries;
    const float64 evectionDegrees = EVECTION_AT_EPOCH_DEGREES + EVECTION_DEGREES_PER_CENTURY * centuries;
    const float64 variationDegrees = VARIATION_AT_EPOCH_DEGREES + VARIATION_DEGREES_PER_CENTURY * centuries;
    const float64 secondAnomalyDegrees =
        SECOND_ANOMALY_AT_EPOCH_DEGREES + SECOND_ANOMALY_DEGREES_PER_CENTURY * centuries;
    const float64 solarAnomalyDegrees = SOLAR_ANOMALY_AT_EPOCH_DEGREES + SOLAR_ANOMALY_DEGREES_PER_CENTURY * centuries;
    const float64 longitudePerturbationDegrees =
        LONGITUDE_PERTURBATION_AT_EPOCH_DEGREES + LONGITUDE_PERTURBATION_DEGREES_PER_CENTURY * centuries;
    const float64 latitudeArgumentDegrees =
        LATITUDE_ARGUMENT_AT_EPOCH_DEGREES + LATITUDE_ARGUMENT_DEGREES_PER_CENTURY * centuries;
    const float64 latitudeHarmonicDegrees =
        LATITUDE_HARMONIC_AT_EPOCH_DEGREES + LATITUDE_HARMONIC_DEGREES_PER_CENTURY * centuries;
    const float64 latitudeNodeDegrees = LATITUDE_NODE_AT_EPOCH_DEGREES + LATITUDE_NODE_DEGREES_PER_CENTURY * centuries;
    const float64 latitudePerturbationDegrees =
        LATITUDE_PERTURBATION_AT_EPOCH_DEGREES + LATITUDE_PERTURBATION_DEGREES_PER_CENTURY * centuries;

    constexpr float64 LONGITUDE_ANOMALY_DEGREES = 6.29;
    constexpr float64 LONGITUDE_EVECTION_DEGREES = -1.27;
    constexpr float64 LONGITUDE_VARIATION_DEGREES = 0.66;
    constexpr float64 LONGITUDE_SECOND_ANOMALY_DEGREES = 0.21;
    constexpr float64 LONGITUDE_SOLAR_ANOMALY_DEGREES = -0.19;
    constexpr float64 LONGITUDE_PERTURBATION_DEGREES = -0.11;
    constexpr float64 LATITUDE_INCLINATION_DEGREES = 5.13;
    constexpr float64 LATITUDE_HARMONIC_DEGREES = 0.28;
    constexpr float64 LATITUDE_NODE_DEGREES = -0.28;
    constexpr float64 LATITUDE_PERTURBATION_DEGREES = -0.17;
    constexpr float64 MEAN_PARALLAX_DEGREES = 0.9508;
    constexpr float64 PARALLAX_ANOMALY_DEGREES = 0.0518;
    constexpr float64 PARALLAX_EVECTION_DEGREES = 0.0095;
    constexpr float64 PARALLAX_VARIATION_DEGREES = 0.0078;
    constexpr float64 PARALLAX_SECOND_ANOMALY_DEGREES = 0.0028;

    const float64 longitudeDegrees = LONGITUDE_AT_EPOCH_DEGREES + LONGITUDE_DEGREES_PER_CENTURY * centuries +
                                     LONGITUDE_ANOMALY_DEGREES * ComputeSineDegrees(anomalyDegrees) +
                                     LONGITUDE_EVECTION_DEGREES * ComputeSineDegrees(evectionDegrees) +
                                     LONGITUDE_VARIATION_DEGREES * ComputeSineDegrees(variationDegrees) +
                                     LONGITUDE_SECOND_ANOMALY_DEGREES * ComputeSineDegrees(secondAnomalyDegrees) +
                                     LONGITUDE_SOLAR_ANOMALY_DEGREES * ComputeSineDegrees(solarAnomalyDegrees) +
                                     LONGITUDE_PERTURBATION_DEGREES * ComputeSineDegrees(longitudePerturbationDegrees);
    const float64 latitudeDegrees = LATITUDE_INCLINATION_DEGREES * ComputeSineDegrees(latitudeArgumentDegrees) +
                                    LATITUDE_HARMONIC_DEGREES * ComputeSineDegrees(latitudeHarmonicDegrees) +
                                    LATITUDE_NODE_DEGREES * ComputeSineDegrees(latitudeNodeDegrees) +
                                    LATITUDE_PERTURBATION_DEGREES * ComputeSineDegrees(latitudePerturbationDegrees);
    const float64 parallaxDegrees =
        MEAN_PARALLAX_DEGREES + PARALLAX_ANOMALY_DEGREES * math::Cos(math::DegreesToRadians(anomalyDegrees)) +
        PARALLAX_EVECTION_DEGREES * math::Cos(math::DegreesToRadians(evectionDegrees)) +
        PARALLAX_VARIATION_DEGREES * math::Cos(math::DegreesToRadians(variationDegrees)) +
        PARALLAX_SECOND_ANOMALY_DEGREES * math::Cos(math::DegreesToRadians(secondAnomalyDegrees));
    return {math::DegreesToRadians(longitudeDegrees),
            math::DegreesToRadians(latitudeDegrees),
            math::DegreesToRadians(parallaxDegrees)};
}
} // namespace

bool TrySampleEarthCelestialTransform(EarthCelestialBody body,
                                      float64 daysSinceJ2000,
                                      float64 latitudeDegrees,
                                      float64 longitudeDegrees,
                                      EarthCelestialTransform& outTransform) noexcept
{
    if ((body != EarthCelestialBody::Sun && body != EarthCelestialBody::Moon) ||
        math::IsFinite(daysSinceJ2000) == false || math::Abs(daysSinceJ2000) > MAX_EARTH_CELESTIAL_DAYS ||
        math::IsFinite(latitudeDegrees) == false || math::Abs(latitudeDegrees) > MAX_EARTH_LATITUDE_DEGREES ||
        math::IsFinite(longitudeDegrees) == false || math::Abs(longitudeDegrees) > MAX_EARTH_LONGITUDE_DEGREES)
    {
        return false;
    }

    constexpr float64 DAYS_PER_JULIAN_CENTURY = 36525;
    constexpr float64 OBLIQUITY_AT_EPOCH_DEGREES = 23.439;
    constexpr float64 OBLIQUITY_DEGREES_PER_DAY = -0.00000036;
    const EclipticCoordinates coordinates = body == EarthCelestialBody::Sun
                                                ? ComputeSolarCoordinates(daysSinceJ2000)
                                                : ComputeLunarCoordinates(daysSinceJ2000 / DAYS_PER_JULIAN_CENTURY);
    const float64 obliquityRadians =
        math::DegreesToRadians(OBLIQUITY_AT_EPOCH_DEGREES + OBLIQUITY_DEGREES_PER_DAY * daysSinceJ2000);
    const float64 eclipticHorizontal = math::Cos(coordinates.LatitudeRadians);
    const float64 xEcliptic = eclipticHorizontal * math::Cos(coordinates.LongitudeRadians);
    const float64 yEcliptic = eclipticHorizontal * math::Sin(coordinates.LongitudeRadians);
    const float64 zEcliptic = math::Sin(coordinates.LatitudeRadians);
    const float64 yEquatorial = yEcliptic * math::Cos(obliquityRadians) - zEcliptic * math::Sin(obliquityRadians);
    const float64 zEquatorial = yEcliptic * math::Sin(obliquityRadians) + zEcliptic * math::Cos(obliquityRadians);

    // Thanks to the U.S. Naval Observatory, "Computing Approximate Sidereal Time" and "Computing Altitude
    // and Azimuth from Greenwich Apparent Sidereal Time" (https://aa.usno.navy.mil/faq/GAST and
    // https://aa.usno.navy.mil/faq/alt_az). Use mean sidereal time and project the equatorial vector directly
    // into the local horizon basis, avoiding the singularity of azimuth at zenith.
    constexpr float64 SIDEREAL_AT_EPOCH_HOURS = 18.697375;
    constexpr float64 SIDEREAL_HOURS_PER_DAY = 24.065709824279;
    constexpr float64 DEGREES_PER_SIDEREAL_HOUR = 15;
    const float64 siderealRadians = math::DegreesToRadians(
        DEGREES_PER_SIDEREAL_HOUR * (SIDEREAL_AT_EPOCH_HOURS + SIDEREAL_HOURS_PER_DAY * daysSinceJ2000) +
        longitudeDegrees);
    const float64 latitudeRadians = math::DegreesToRadians(latitudeDegrees);
    const float64 equatorialMeridian =
        xEcliptic * math::Cos(siderealRadians) + yEquatorial * math::Sin(siderealRadians);
    const float64 east = yEquatorial * math::Cos(siderealRadians) - xEcliptic * math::Sin(siderealRadians);
    const float64 north = zEquatorial * math::Cos(latitudeRadians) - equatorialMeridian * math::Sin(latitudeRadians);
    // The geocentric unit direction is in body-distance units. Subtract one Earth radius in those units.
    const float64 up = equatorialMeridian * math::Cos(latitudeRadians) + zEquatorial * math::Sin(latitudeRadians) -
                       math::Sin(coordinates.ParallaxRadians);
    const math::Vector3 incomingWorld{-static_cast<float32>(east),
                                      -static_cast<float32>(up),
                                      static_cast<float32>(north)};
    EarthCelestialTransform transform;
    if (math::TryRotationBetween({0, 0, -1}, incomingWorld, transform.Rotation) != math::MathStatus::Success)
    {
        return false;
    }
    transform.IsAboveHorizon = up > 0;
    outTransform = transform;
    return true;
}
} // namespace ludus::gameplay::world
