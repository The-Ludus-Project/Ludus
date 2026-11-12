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
constexpr float64 DAYS_PER_JULIAN_CENTURY = 36525;

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
// (https://aa.usno.navy.mil/faq/sun_approx), for mean anomaly, ecliptic longitude, distance and apparent size.
float64 ComputeSolarAnomalyDegrees(float64 days) noexcept
{
    constexpr float64 ANOMALY_AT_EPOCH_DEGREES = 357.529;
    constexpr float64 ANOMALY_DEGREES_PER_DAY = 0.98560028;
    return ANOMALY_AT_EPOCH_DEGREES + ANOMALY_DEGREES_PER_DAY * days;
}

EclipticCoordinates ComputeSolarCoordinates(float64 days) noexcept
{
    constexpr float64 LONGITUDE_AT_EPOCH_DEGREES = 280.459;
    constexpr float64 LONGITUDE_DEGREES_PER_DAY = 0.98564736;
    constexpr float64 FIRST_HARMONIC_DEGREES = 1.915;
    constexpr float64 SECOND_HARMONIC_DEGREES = 0.020;
    const float64 anomalyDegrees = ComputeSolarAnomalyDegrees(days);
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
bool IsWithinCelestialRange(float64 value, float64 limit) noexcept
{
    return math::IsFinite(value) == true && math::Abs(value) <= limit;
}

/// An angle that starts at EpochDegrees and advances DegreesPerHour.
struct CyclicAngle
{
    float64 EpochDegrees = 0;
    float64 DegreesPerHour = 0;
};

float64 ComputeCyclicRadians(CyclicAngle angle, float64 elapsedHours) noexcept
{
    constexpr float64 DEGREES_PER_TURN = 360;
    const float64 elapsedDegrees = angle.DegreesPerHour * elapsedHours;
    const float64 wrappedDegrees = elapsedDegrees - DEGREES_PER_TURN * math::Floor(elapsedDegrees / DEGREES_PER_TURN);
    return math::DegreesToRadians(angle.EpochDegrees + wrappedDegrees);
}

/// The observer's local horizon: its meridian and latitude in radians, and its
/// distance from the planet's centre in units of the body's distance.
struct HorizonObserver
{
    float64 MeridianRadians = 0;
    float64 LatitudeRadians = 0;
    float64 RadiusRatio = 0;
};

math::Vector3d ProjectToHorizon(math::Vector3d directionEquatorial, const HorizonObserver& observer) noexcept
{
    const float64 cosMeridian = math::Cos(observer.MeridianRadians);
    const float64 sinMeridian = math::Sin(observer.MeridianRadians);
    const float64 cosLatitude = math::Cos(observer.LatitudeRadians);
    const float64 sinLatitude = math::Sin(observer.LatitudeRadians);
    const float64 equatorialMeridian = directionEquatorial.X * cosMeridian + directionEquatorial.Y * sinMeridian;
    const float64 east = directionEquatorial.Y * cosMeridian - directionEquatorial.X * sinMeridian;
    const float64 north = directionEquatorial.Z * cosLatitude - equatorialMeridian * sinLatitude;
    // The body direction has unit length. Subtract the observer's radius in body-distance units.
    const float64 up = equatorialMeridian * cosLatitude + directionEquatorial.Z * sinLatitude - observer.RadiusRatio;
    return {east, up, -north};
}

math::Vector3 NarrowDirection(math::Vector3d directionWorld) noexcept
{
    return {static_cast<float32>(directionWorld.X),
            static_cast<float32>(directionWorld.Y),
            static_cast<float32>(directionWorld.Z)};
}

bool TryMakeCelestialTransform(math::Vector3d directionToBodyWorld, CelestialTransform& outTransform) noexcept
{
    CelestialTransform transform;
    if (math::TryRotationBetween({0, 0, -1}, -NarrowDirection(directionToBodyWorld), transform.Rotation) !=
        math::MathStatus::Success)
    {
        return false;
    }
    // Dimensionless roundoff allowance in the vertical direction component at the exact horizon.
    constexpr float64 HORIZON_COMPONENT_TOLERANCE = 1e-12;
    transform.IsAboveHorizon = directionToBodyWorld.Y > HORIZON_COMPONENT_TOLERANCE;
    outTransform = transform;
    return true;
}
struct EarthObservation final
{
    float64 DaysSinceJ2000;
    float64 LatitudeDegrees;
    float64 LongitudeDegrees;
};

bool IsValidEarthObserver(const EarthObservation& observation) noexcept
{
    return IsWithinCelestialRange(observation.DaysSinceJ2000, MAX_EARTH_CELESTIAL_DAYS) == true &&
           IsWithinCelestialRange(observation.LatitudeDegrees, MAX_EARTH_LATITUDE_DEGREES) == true &&
           IsWithinCelestialRange(observation.LongitudeDegrees, MAX_EARTH_LONGITUDE_DEGREES) == true;
}

HorizonObserver MakeEarthHorizon(const EarthObservation& observation) noexcept
{
    // Thanks to the U.S. Naval Observatory, "Computing Approximate Sidereal Time" and "Computing Altitude
    // and Azimuth from Greenwich Apparent Sidereal Time" (https://aa.usno.navy.mil/faq/GAST and
    // https://aa.usno.navy.mil/faq/alt_az). Project into the mean local horizon without computing azimuth.
    constexpr float64 SIDEREAL_AT_EPOCH_HOURS = 18.697375;
    constexpr float64 SIDEREAL_HOURS_PER_DAY = 24.065709824279;
    constexpr float64 DEGREES_PER_SIDEREAL_HOUR = 15;
    return
    {
        .MeridianRadians =
            math::DegreesToRadians(DEGREES_PER_SIDEREAL_HOUR *
                                       (SIDEREAL_AT_EPOCH_HOURS + SIDEREAL_HOURS_PER_DAY * observation.DaysSinceJ2000) +
                                   observation.LongitudeDegrees),
        .LatitudeRadians = math::DegreesToRadians(observation.LatitudeDegrees),
    };
}

struct SecularElement final
{
    float64 AtEpoch;
    float64 PerJulianCentury;
};

struct PlanetOrbit final
{
    SecularElement SemimajorAxisAu;
    SecularElement Eccentricity;
    SecularElement InclinationDegrees;
    SecularElement MeanLongitudeDegrees;
    SecularElement PerihelionLongitudeDegrees;
    SecularElement AscendingNodeDegrees;
};

struct VisiblePlanet final
{
    PlanetOrbit Orbit;
    float64 FullPhaseMagnitudeAtUnitDistances;
    math::Vector3 Tint;
};

// Thanks to E.M. Standish, J.G. Williams and NASA JPL Solar System Dynamics, "Approximate Positions of
// the Planets", Table 1 and section "Formulae for using the Keplerian elements"
// (https://ssd.jpl.nasa.gov/planets/approx_pos.html). Use the Earth–Moon barycenter as the observer's center.
// Thanks to NASA NSSDC's "Planetary Fact Sheets" (https://nssdc.gsfc.nasa.gov/planetary/planetfact.html)
// for the V(1,0) magnitudes. The RGB tints are representative display colors.
constexpr PlanetOrbit EARTH_MOON_ORBIT{
    {1.00000261, 0.00000562},
    {0.01671123, -0.00004392},
    {-0.00001531, -0.01294668},
    {100.46457166, 35999.37244981},
    {102.93768193, 0.32327364},
    {0, 0},
};

constexpr foundation::StaticArray<VisiblePlanet, EARTH_VISIBLE_PLANET_COUNT> VISIBLE_PLANETS{{
    {
        .Orbit = {{0.38709927, 0.00000037},
                  {0.20563593, 0.00001906},
                  {7.00497902, -0.00594749},
                  {252.25032350, 149472.67411175},
                  {77.45779628, 0.16047689},
                  {48.33076593, -0.12534081}},
        .FullPhaseMagnitudeAtUnitDistances = -0.613,
        .Tint = {1, 0.94F, 0.86F},
    }, // Mercury
    {
        .Orbit = {{0.72333566, 0.00000390},
                  {0.00677672, -0.00004107},
                  {3.39467605, -0.00078890},
                  {181.97909950, 58517.81538729},
                  {131.60246718, 0.00268329},
                  {76.67984255, -0.27769418}},
        .FullPhaseMagnitudeAtUnitDistances = -4.38,
        .Tint = {1, 0.98F, 0.92F},
    }, // Venus
    {
        .Orbit = {{1.52371034, 0.00001847},
                  {0.09339410, 0.00007882},
                  {1.84969142, -0.00813131},
                  {-4.55343205, 19140.30268499},
                  {-23.94362959, 0.44441088},
                  {49.55953891, -0.29257343}},
        .FullPhaseMagnitudeAtUnitDistances = -1.60,
        .Tint = {1, 0.54F, 0.32F},
    }, // Mars
    {
        .Orbit = {{5.20288700, -0.00011607},
                  {0.04838624, -0.00013253},
                  {1.30439695, -0.00183714},
                  {34.39644051, 3034.74612775},
                  {14.72847983, 0.21252668},
                  {100.47390909, 0.20469106}},
        .FullPhaseMagnitudeAtUnitDistances = -9.40,
        .Tint = {1, 0.93F, 0.81F},
    }, // Jupiter
    {
        .Orbit = {{9.53667594, -0.00125060},
                  {0.05386179, -0.00050991},
                  {2.48599187, 0.00193609},
                  {49.95424423, 1222.49362201},
                  {92.59887831, -0.41897216},
                  {113.66242448, -0.28867794}},
        .FullPhaseMagnitudeAtUnitDistances = -8.91,
        .Tint = {1, 0.87F, 0.65F},
    }, // Saturn
}};

float64 EvaluateElement(SecularElement element, float64 centuries) noexcept
{
    return element.AtEpoch + element.PerJulianCentury * centuries;
}

math::Vector3d ComputeHeliocentricPositionEcliptic(const PlanetOrbit& orbit, float64 centuries) noexcept
{
    const float64 semimajorAxis = EvaluateElement(orbit.SemimajorAxisAu, centuries);
    const float64 eccentricity = EvaluateElement(orbit.Eccentricity, centuries);
    const float64 inclination = math::DegreesToRadians(EvaluateElement(orbit.InclinationDegrees, centuries));
    const float64 perihelionLongitude = EvaluateElement(orbit.PerihelionLongitudeDegrees, centuries);
    const float64 nodeLongitude = EvaluateElement(orbit.AscendingNodeDegrees, centuries);
    const float64 perihelionArgument = math::DegreesToRadians(perihelionLongitude - nodeLongitude);
    const float64 node = math::DegreesToRadians(nodeLongitude);
    const float64 unwrappedAnomaly =
        math::DegreesToRadians(EvaluateElement(orbit.MeanLongitudeDegrees, centuries) - perihelionLongitude);
    const float64 meanAnomaly = unwrappedAnomaly - math::kTau * math::Floor(unwrappedAnomaly / math::kTau + 0.5);
    float64 anomaly = meanAnomaly + eccentricity * math::Sin(meanAnomaly);
    // The largest eccentricity in Table 1's subset is Mercury's ~0.206.
    // Four Newton iterations suffice for all admitted dates at JPL's angular tolerance.
    constexpr foundation::usize KEPLER_ITERATIONS = 4;
    for (foundation::usize i = 0; i < KEPLER_ITERATIONS; ++i)
    {
        anomaly +=
            (meanAnomaly - anomaly + eccentricity * math::Sin(anomaly)) / (1 - eccentricity * math::Cos(anomaly));
    }
    const float64 xOrbit = semimajorAxis * (math::Cos(anomaly) - eccentricity);
    const float64 yOrbit = semimajorAxis * math::Sqrt(1 - eccentricity * eccentricity) * math::Sin(anomaly);
    const float64 xNode = xOrbit * math::Cos(perihelionArgument) - yOrbit * math::Sin(perihelionArgument);
    const float64 yNode = xOrbit * math::Sin(perihelionArgument) + yOrbit * math::Cos(perihelionArgument);
    const float64 yInclined = yNode * math::Cos(inclination);
    return {xNode * math::Cos(node) - yInclined * math::Sin(node),
            xNode * math::Sin(node) + yInclined * math::Cos(node),
            yNode * math::Sin(inclination)};
}

math::Vector3d RotateEclipticToEquatorial(math::Vector3d vectorEcliptic, float64 obliquityRadians) noexcept
{
    return {vectorEcliptic.X,
            vectorEcliptic.Y * math::Cos(obliquityRadians) - vectorEcliptic.Z * math::Sin(obliquityRadians),
            vectorEcliptic.Y * math::Sin(obliquityRadians) + vectorEcliptic.Z * math::Cos(obliquityRadians)};
}

float32 ComputePlanetIlluminance(float64 fullPhaseMagnitude,
                                 math::Vector3d positionEcliptic,
                                 math::Vector3d displacementEcliptic,
                                 float64 observerDistance) noexcept
{
    // Thanks to Andrew Crumey, "Human contrast threshold and astronomical visibility", section 3
    // (https://doi.org/10.1093/mnras/stu992), for the V-band zero point and illuminance relation.
    constexpr float64 MAGNITUDE_ZERO_LUX = 2.54e-6;
    constexpr float64 MAGNITUDE_FLUX_BASE = 10;
    constexpr float64 MAGNITUDE_FLUX_EXPONENT = -0.4;
    const float64 sunDistanceSquared = math::LengthSquared(positionEcliptic);
    const float64 distanceProduct = math::Sqrt(sunDistanceSquared) * observerDistance;
    const float64 cosine = math::Dot(positionEcliptic, displacementEcliptic) / distanceProduct;
    const float64 sine = math::Length(math::Cross(positionEcliptic, displacementEcliptic)) / distanceProduct;
    const float64 phaseAngle = math::Atan2(sine, cosine);
    // Use a Lambertian sphere for the visible phase, including the crescent falloff near conjunction.
    const float64 phase = math::Max(0.0, (sine + (math::kPi - phaseAngle) * cosine) / math::kPi);
    const float64 fullPhaseLux =
        MAGNITUDE_ZERO_LUX * math::Pow(MAGNITUDE_FLUX_BASE, MAGNITUDE_FLUX_EXPONENT * fullPhaseMagnitude);
    return static_cast<float32>(fullPhaseLux * phase / (distanceProduct * distanceProduct));
}
} // namespace

bool TrySampleCelestialOrbitTransform(const CelestialOrbit& orbit,
                                      const CelestialObserver& observer,
                                      float64 elapsedHours,
                                      CelestialTransform& outTransform) noexcept
{
    if (IsWithinCelestialRange(elapsedHours, MAX_CELESTIAL_ORBIT_HOURS) == false ||
        IsWithinCelestialRange(observer.LatitudeDegrees, MAX_CELESTIAL_LATITUDE_DEGREES) == false ||
        IsWithinCelestialRange(observer.LongitudeDegrees, MAX_CELESTIAL_LONGITUDE_DEGREES) == false ||
        IsWithinCelestialRange(observer.SpinDegreesPerHour, MAX_CELESTIAL_RATE_DEGREES_PER_HOUR) == false ||
        IsWithinCelestialRange(orbit.DegreesPerHour, MAX_CELESTIAL_RATE_DEGREES_PER_HOUR) == false ||
        IsWithinCelestialRange(orbit.PhaseDegrees, MAX_CELESTIAL_ORBIT_ANGLE_DEGREES) == false ||
        IsWithinCelestialRange(orbit.AscendingNodeDegrees, MAX_CELESTIAL_ORBIT_ANGLE_DEGREES) == false ||
        IsWithinCelestialRange(orbit.InclinationDegrees, MAX_CELESTIAL_ORBIT_ANGLE_DEGREES) == false ||
        orbit.InclinationDegrees < 0 ||
        IsWithinCelestialRange(orbit.ObserverRadiusRatio, MAX_CELESTIAL_OBSERVER_RADIUS_RATIO) == false ||
        orbit.ObserverRadiusRatio < 0)
    {
        return false;
    }

    // Thanks to NASA JPL Solar System Dynamics, "Approximate Positions of the Planets", section 2,
    // https://ssd.jpl.nasa.gov/planets/approx_pos.html. Use its orbital-plane rotation with a circular
    // unit path, a fixed world equator as the reference plane, and phase measured from the node.
    const float64 phaseRadians =
        ComputeCyclicRadians({ .EpochDegrees = orbit.PhaseDegrees, .DegreesPerHour = orbit.DegreesPerHour },
                             elapsedHours);
    const float64 inclinationRadians = math::DegreesToRadians(orbit.InclinationDegrees);
    const float64 nodeRadians = math::DegreesToRadians(orbit.AscendingNodeDegrees);
    const float64 xOrbit = math::Cos(phaseRadians);
    const float64 yOrbit = math::Sin(phaseRadians);
    const float64 inclinedYEquatorial = yOrbit * math::Cos(inclinationRadians);
    const math::Vector3d directionEquatorial{
        xOrbit * math::Cos(nodeRadians) - inclinedYEquatorial * math::Sin(nodeRadians),
        xOrbit * math::Sin(nodeRadians) + inclinedYEquatorial * math::Cos(nodeRadians),
        yOrbit * math::Sin(inclinationRadians),
    };
    const HorizonObserver horizon
    {
        .MeridianRadians = ComputeCyclicRadians(
            { .EpochDegrees = observer.LongitudeDegrees, .DegreesPerHour = observer.SpinDegreesPerHour },
            elapsedHours),
        .LatitudeRadians = math::DegreesToRadians(observer.LatitudeDegrees),
        .RadiusRatio = orbit.ObserverRadiusRatio,
    };
    return TryMakeCelestialTransform(ProjectToHorizon(directionEquatorial, horizon), outTransform);
}

namespace
{
math::Vector3d ProjectEarthDirection(const EclipticCoordinates& coordinates,
                                     const EarthObservation& observation) noexcept
{
    constexpr float64 OBLIQUITY_AT_EPOCH_DEGREES = 23.439;
    constexpr float64 OBLIQUITY_DEGREES_PER_DAY = -0.00000036;
    const float64 obliquityRadians =
        math::DegreesToRadians(OBLIQUITY_AT_EPOCH_DEGREES + OBLIQUITY_DEGREES_PER_DAY * observation.DaysSinceJ2000);
    const float64 eclipticHorizontal = math::Cos(coordinates.LatitudeRadians);
    const float64 xEcliptic = eclipticHorizontal * math::Cos(coordinates.LongitudeRadians);
    const float64 yEcliptic = eclipticHorizontal * math::Sin(coordinates.LongitudeRadians);
    const float64 zEcliptic = math::Sin(coordinates.LatitudeRadians);
    HorizonObserver horizon = MakeEarthHorizon(observation);
    horizon.RadiusRatio = math::Sin(coordinates.ParallaxRadians);
    return ProjectToHorizon(RotateEclipticToEquatorial({xEcliptic, yEcliptic, zEcliptic}, obliquityRadians), horizon);
}

// Thanks to Kasten and Young, "Revised optical air mass tables and approximation formula",
// Applied Optics 28 (1989), eq. 4, https://doi.org/10.1364/AO.28.004735.
// Use geometric elevation and a representative clear-sky RGB optical depth for the direct beam.
math::Vector3 ComputeGroundIlluminance(math::Vector3 boundary, math::Vector3d directionToBodyWorld) noexcept
{
    constexpr float64 AIR_MASS_COEFFICIENT = 0.50572;
    constexpr float64 AIR_MASS_OFFSET_DEGREES = 6.07995;
    constexpr float64 AIR_MASS_EXPONENT = -1.6364;
    constexpr math::Vector3d ZENITH_OPTICAL_DEPTH{0.10, 0.15, 0.24};
    const float64 elevationDegrees = math::RadiansToDegrees(math::Atan2(
        directionToBodyWorld.Y,
        math::Sqrt(directionToBodyWorld.X * directionToBodyWorld.X + directionToBodyWorld.Z * directionToBodyWorld.Z)));
    const float64 airMass =
        1 / (directionToBodyWorld.Y +
             AIR_MASS_COEFFICIENT * math::Pow(elevationDegrees + AIR_MASS_OFFSET_DEGREES, AIR_MASS_EXPONENT));
    return {boundary.X * static_cast<float32>(math::Exp(-airMass * ZENITH_OPTICAL_DEPTH.X)),
            boundary.Y * static_cast<float32>(math::Exp(-airMass * ZENITH_OPTICAL_DEPTH.Y)),
            boundary.Z * static_cast<float32>(math::Exp(-airMass * ZENITH_OPTICAL_DEPTH.Z))};
}

float64 ComputeSolarDistanceAu(float64 days) noexcept
{
    constexpr float64 DISTANCE_AT_EPOCH_AU = 1.00014; // USNO approximate solar coordinates, mean term.
    constexpr float64 FIRST_DISTANCE_HARMONIC_AU = 0.01671;
    constexpr float64 SECOND_DISTANCE_HARMONIC_AU = 0.00014;
    const float64 anomalyRadians = math::DegreesToRadians(ComputeSolarAnomalyDegrees(days));
    return DISTANCE_AT_EPOCH_AU - FIRST_DISTANCE_HARMONIC_AU * math::Cos(anomalyRadians) -
           SECOND_DISTANCE_HARMONIC_AU * math::Cos(2 * anomalyRadians);
}

struct LunarPhotometryGeometry final
{
    // The Moon offset is in geocentric lunar distance units. The Sun direction has unit length.
    math::Vector3d MoonOffsetWorld;
    math::Vector3d SunDirectionWorld;
    float64 MoonObserverRadiusRatio;
    float64 SunDistanceAu;
};

void SampleLunarPhotometry(const LunarPhotometryGeometry& geometry, EarthCelestialLighting& outLighting) noexcept
{
    constexpr float64 EARTH_RADIUS_METRES = 6'371'000;
    constexpr float64 ASTRONOMICAL_UNIT_METRES = 149'597'870'700;
    constexpr float64 LUNAR_RADIUS_METRES = 1'737'400;
    constexpr float64 REFERENCE_LUNAR_DISTANCE_METRES = 384'400'000;
    const float64 relativeMoonDistance = math::Length(geometry.MoonOffsetWorld);
    const float64 moonDistanceMetres = relativeMoonDistance * EARTH_RADIUS_METRES / geometry.MoonObserverRadiusRatio;
    const math::Vector3d directionToMoonWorld = geometry.MoonOffsetWorld * (1 / relativeMoonDistance);
    const math::Vector3d moonToSunWorld =
        geometry.SunDirectionWorld -
        directionToMoonWorld * (moonDistanceMetres / (geometry.SunDistanceAu * ASTRONOMICAL_UNIT_METRES));
    const math::Vector3d directionToSunAtMoonWorld = moonToSunWorld * (1 / math::Length(moonToSunWorld));
    const float64 phaseCosine = math::Clamp(-math::Dot(directionToMoonWorld, directionToSunAtMoonWorld), -1.0, 1.0);
    const float64 phaseSine = math::Length(math::Cross(directionToMoonWorld, directionToSunAtMoonWorld));
    const float64 phaseRadians = math::Atan2(phaseSine, phaseCosine);
    const float64 phaseDegrees = math::RadiansToDegrees(phaseRadians);

    // Thanks to Krisciunas and Schaefer, "A model of the brightness of moonlight", PASP 103 (1991),
    // eqs. 8–9, https://doi.org/10.1086/132921. Use their relative visual phase curve and full-Moon scale.
    // Normalize the lit Lambertian disk to that curve. At a very thin crescent the geometric disk
    // integral tends to zero, so cap the gain and let the direct beam tend to zero with the disk.
    constexpr float64 FULL_MOON_LUX = 0.32;
    constexpr float64 LINEAR_MAGNITUDE_PER_DEGREE = 0.026;
    constexpr float64 QUARTIC_MAGNITUDE_PER_DEGREE = 4e-9;
    constexpr float64 LOG10_FLUX_PER_MAGNITUDE = -0.4;
    constexpr float64 MAXIMUM_DISK_GAIN = 1;
    const float64 phaseDegreesSquared = phaseDegrees * phaseDegrees;
    const float64 relativeMagnitude = LINEAR_MAGNITUDE_PER_DEGREE * phaseDegrees +
                                      QUARTIC_MAGNITUDE_PER_DEGREE * phaseDegreesSquared * phaseDegreesSquared;
    constexpr float64 MAGNITUDE_BASE = 10;
    const float64 phaseFlux = math::Pow(MAGNITUDE_BASE, LOG10_FLUX_PER_MAGNITUDE * relativeMagnitude);
    const float64 diskPhase = math::Max((phaseSine + (math::kPi - phaseRadians) * phaseCosine) / math::kPi, 0.0);
    const float64 diskGain = diskPhase > phaseFlux ? phaseFlux / diskPhase : MAXIMUM_DISK_GAIN;
    const float64 distanceScale = REFERENCE_LUNAR_DISTANCE_METRES / (moonDistanceMetres * geometry.SunDistanceAu);
    const float64 fullPhaseLux = FULL_MOON_LUX * distanceScale * distanceScale;
    const float64 sineRadius = LUNAR_RADIUS_METRES / moonDistanceMetres;
    constexpr float64 LAMBERTIAN_FULL_PHASE_SCALE = 1.5; // The mean cosine over a projected full disk is two thirds.
    const float32 peak = static_cast<float32>(fullPhaseLux * LAMBERTIAN_FULL_PHASE_SCALE * diskGain /
                                              (math::kPi * sineRadius * sineRadius));
    const float32 illuminance = static_cast<float32>(fullPhaseLux * math::Min(phaseFlux, diskPhase));
    outLighting.BoundaryIlluminance = {illuminance, illuminance, illuminance};
    outLighting.LunarPeakRadiance = {peak, peak, peak};
    outLighting.AngularRadiusRadians =
        static_cast<float32>(math::Atan2(sineRadius, math::Sqrt(1 - sineRadius * sineRadius)));
    outLighting.DirectionToSunAtMoonWorld = NarrowDirection(directionToSunAtMoonWorld);
}
} // namespace

bool TrySampleEarthCelestialTransform(EarthCelestialBody body,
                                      float64 daysSinceJ2000,
                                      float64 latitudeDegrees,
                                      float64 longitudeDegrees,
                                      EarthCelestialTransform& outTransform) noexcept
{
    const EarthObservation observation
    {
        .DaysSinceJ2000 = daysSinceJ2000,
        .LatitudeDegrees = latitudeDegrees,
        .LongitudeDegrees = longitudeDegrees,
    };
    if ((body != EarthCelestialBody::Sun && body != EarthCelestialBody::Moon) ||
        IsValidEarthObserver(observation) == false)
    {
        return false;
    }
    const EclipticCoordinates coordinates = body == EarthCelestialBody::Sun
                                                ? ComputeSolarCoordinates(daysSinceJ2000)
                                                : ComputeLunarCoordinates(daysSinceJ2000 / DAYS_PER_JULIAN_CENTURY);
    return TryMakeCelestialTransform(ProjectEarthDirection(coordinates, observation), outTransform);
}

bool TrySampleEarthCelestialLighting(EarthCelestialBody body,
                                     float64 daysSinceJ2000,
                                     float64 latitudeDegrees,
                                     float64 longitudeDegrees,
                                     EarthCelestialLighting& outLighting) noexcept
{
    const EarthObservation observation
    {
        .DaysSinceJ2000 = daysSinceJ2000,
        .LatitudeDegrees = latitudeDegrees,
        .LongitudeDegrees = longitudeDegrees,
    };
    if ((body != EarthCelestialBody::Sun && body != EarthCelestialBody::Moon) ||
        IsValidEarthObserver(observation) == false)
    {
        return false;
    }

    const EclipticCoordinates solarCoordinates = ComputeSolarCoordinates(daysSinceJ2000);
    const math::Vector3d sunWorld = ProjectEarthDirection(solarCoordinates, observation);
    const float64 sunDistanceAu = ComputeSolarDistanceAu(daysSinceJ2000);
    EarthCelestialLighting lighting;
    math::Vector3d directionToBodyWorld = sunWorld;
    if (body == EarthCelestialBody::Sun)
    {
        // Thanks to Darula, Kittler and Gueymard, "Reference luminous solar constant and solar luminance
        // for illuminance calculations", Solar Energy 79 (2005), https://doi.org/10.1016/j.solener.2005.01.004.
        constexpr float64 MEAN_SOLAR_LUX = 133'800;
        constexpr float64 SOLAR_SEMIDIAMETER_DEGREES_AT_ONE_AU = 0.2666; // USNO apparent solar coordinates.
        const float32 illuminance = static_cast<float32>(MEAN_SOLAR_LUX / (sunDistanceAu * sunDistanceAu));
        lighting.BoundaryIlluminance = {illuminance, illuminance, illuminance};
        lighting.AngularRadiusRadians =
            static_cast<float32>(math::DegreesToRadians(SOLAR_SEMIDIAMETER_DEGREES_AT_ONE_AU / sunDistanceAu));
    }
    else
    {
        const EclipticCoordinates lunarCoordinates = ComputeLunarCoordinates(daysSinceJ2000 / DAYS_PER_JULIAN_CENTURY);
        const math::Vector3d moonWorld = ProjectEarthDirection(lunarCoordinates, observation);
        const LunarPhotometryGeometry geometry
        {
            .MoonOffsetWorld = moonWorld,
            .SunDirectionWorld = sunWorld,
            .MoonObserverRadiusRatio = math::Sin(lunarCoordinates.ParallaxRadians),
            .SunDistanceAu = sunDistanceAu,
        };
        SampleLunarPhotometry(geometry, lighting);
        directionToBodyWorld = moonWorld;
    }
    if (TryMakeCelestialTransform(directionToBodyWorld, lighting.Transform) == false)
    {
        return false;
    }
    if (lighting.Transform.IsAboveHorizon == true)
    {
        lighting.GroundIlluminance =
            ComputeGroundIlluminance(lighting.BoundaryIlluminance,
                                     directionToBodyWorld * (1 / math::Length(directionToBodyWorld)));
    }
    outLighting = lighting;
    return true;
}
bool TrySampleEarthNightSky(float64 daysSinceJ2000,
                            float64 latitudeDegrees,
                            float64 longitudeDegrees,
                            EarthNightSkySample& outSky) noexcept
{
    const EarthObservation observation
    {
        .DaysSinceJ2000 = daysSinceJ2000,
        .LatitudeDegrees = latitudeDegrees,
        .LongitudeDegrees = longitudeDegrees,
    };
    if (IsValidEarthObserver(observation) == false)
    {
        return false;
    }
    const HorizonObserver horizon = MakeEarthHorizon(observation);
    // Thanks to the ESA Gaia Data Release 3 Documentation, section 4.1.7, for the Hipparcos convention.
    // https://gea.esac.esa.int/archive/documentation/GDR3/Data_processing/chap_cu3ast/sec_cu3ast_intro/ssec_cu3ast_intro_transforms.html
    // These are the Galactic rotation matrix's first and third rows in ICRS.
    constexpr math::Vector3d GALACTIC_CENTER_EQUATORIAL{-0.05487556, -0.87343709, -0.48383502};
    constexpr math::Vector3d GALACTIC_NORTH_EQUATORIAL{-0.86766615, -0.19807637, 0.45598378};
    constexpr float64 J2000_OBLIQUITY_RADIANS = math::DegreesToRadians(23.43928);
    EarthNightSkySample sky;
    sky.GalacticCenterWorld = NarrowDirection(ProjectToHorizon(GALACTIC_CENTER_EQUATORIAL, horizon));
    sky.GalacticNorthWorld = NarrowDirection(ProjectToHorizon(GALACTIC_NORTH_EQUATORIAL, horizon));
    const float64 centuries = daysSinceJ2000 / DAYS_PER_JULIAN_CENTURY;
    const math::Vector3d earthEcliptic = ComputeHeliocentricPositionEcliptic(EARTH_MOON_ORBIT, centuries);
    for (foundation::usize i = 0; i < VISIBLE_PLANETS.GetSize(); ++i)
    {
        const VisiblePlanet& planet = VISIBLE_PLANETS[i];
        const math::Vector3d positionEcliptic = ComputeHeliocentricPositionEcliptic(planet.Orbit, centuries);
        const math::Vector3d displacementEcliptic = positionEcliptic - earthEcliptic;
        const float64 distance = math::Length(displacementEcliptic);
        const math::Vector3d directionEquatorial =
            RotateEclipticToEquatorial(displacementEcliptic * (1 / distance), J2000_OBLIQUITY_RADIANS);
        sky.Planets[i].DirectionWorld = NarrowDirection(ProjectToHorizon(directionEquatorial, horizon));
        sky.Planets[i].Illuminance = planet.Tint * ComputePlanetIlluminance(planet.FullPhaseMagnitudeAtUnitDistances,
                                                                            positionEcliptic,
                                                                            displacementEcliptic,
                                                                            distance);
    }
    outSky = sky;
    return true;
}
} // namespace ludus::gameplay::world
