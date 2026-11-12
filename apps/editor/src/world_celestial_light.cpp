#include "internal/world_celestial_light.h"

#include <ludus/foundation/base/types.h>
#include <ludus/gameplay/world/celestial_transform.hpp>

#include <QJsonValue>
#include <QString>

namespace ludus::editor
{
namespace
{
using foundation::float64;
/// Initial spin of a new custom orbit in the editor, in degrees per hour.
constexpr float64 DEFAULT_WORLD_SPIN_DEGREES_PER_HOUR = 15;
constexpr QLatin1StringView EARTH_SUN{"Earth Sun"};
constexpr QLatin1StringView EARTH_MOON{"Earth Moon"};
constexpr QLatin1StringView ORBIT_TIME{"celestialOrbitTime"};
constexpr QLatin1StringView OBSERVER_SPIN{"celestialObserverSpin"};
constexpr QLatin1StringView ORBIT_RATE{"celestialOrbitRate"};
constexpr QLatin1StringView ORBIT_PHASE{"celestialOrbitPhase"};
constexpr QLatin1StringView ORBIT_INCLINATION{"celestialOrbitInclination"};
constexpr QLatin1StringView ORBIT_ASCENDING_NODE{"celestialOrbitAscendingNode"};
constexpr QLatin1StringView OBSERVER_RADIUS_RATIO{"celestialObserverRadiusRatio"};

struct WorldEarthObserver final
{
    float64 DaysSinceJ2000;
    float64 LatitudeDegrees;
    float64 LongitudeDegrees;
};

WorldEarthObserver ReadWorldEarthObserver(const QJsonObject& light)
{
    return {light.value(WORLD_CELESTIAL_TIME).toDouble(),
            light.value(WORLD_CELESTIAL_LATITUDE).toDouble(),
            light.value(WORLD_CELESTIAL_LONGITUDE).toDouble()};
}

QJsonObject
MakeChoiceProperty(QLatin1StringView id, const QString& label, const QString& help, const QJsonArray& choices)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("label"), label},
            {QStringLiteral("type"), QStringLiteral("enum")},
            {QStringLiteral("choices"), choices},
            {QStringLiteral("default"), choices.first()},
            {QStringLiteral("help"), help},
            {QStringLiteral("unit"), QString()}};
}

struct CelestialNumberProperty final
{
    QLatin1StringView Id;
    QString Label;
    QString Unit;
    QString Help;
    float64 Minimum;
    float64 Maximum;
    float64 Default = 0;
};

QJsonObject MakeCelestialNumberProperty(const CelestialNumberProperty& property)
{
    return {{QStringLiteral("id"), property.Id},
            {QStringLiteral("label"), property.Label},
            {QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("default"), property.Default},
            {QStringLiteral("min"), property.Minimum},
            {QStringLiteral("max"), property.Maximum},
            {QStringLiteral("help"), property.Help},
            {QStringLiteral("unit"), property.Unit}};
}

bool TryResolveCustomOrbit(const QJsonObject& light, gameplay::world::CelestialTransform& outSample)
{
    using namespace gameplay::world;
    CelestialObserver observer;
    observer.LatitudeDegrees = light.value(WORLD_CELESTIAL_LATITUDE).toDouble(observer.LatitudeDegrees);
    observer.LongitudeDegrees = light.value(WORLD_CELESTIAL_LONGITUDE).toDouble(observer.LongitudeDegrees);
    observer.SpinDegreesPerHour = light.value(OBSERVER_SPIN).toDouble(DEFAULT_WORLD_SPIN_DEGREES_PER_HOUR);
    const CelestialOrbit orbit
    {
        .DegreesPerHour = light.value(ORBIT_RATE).toDouble(),
        .PhaseDegrees = light.value(ORBIT_PHASE).toDouble(),
        .InclinationDegrees = light.value(ORBIT_INCLINATION).toDouble(),
        .AscendingNodeDegrees = light.value(ORBIT_ASCENDING_NODE).toDouble(),
        .ObserverRadiusRatio = light.value(OBSERVER_RADIUS_RATIO).toDouble(),
    };
    return TrySampleCelestialOrbitTransform(orbit, observer, light.value(ORBIT_TIME).toDouble(), outSample);
}
} // namespace

QJsonArray MakeWorldCelestialProperties()
{
    using namespace gameplay::world;
    const QString degrees = QStringLiteral("°");
    const QString degreesPerHour = QStringLiteral("°/h");
    return {
        MakeChoiceProperty(WORLD_CELESTIAL_ROLE,
                           QStringLiteral("Celestial Role"),
                           QStringLiteral("Static Sun and Static Moon use the authored Transform. Dynamic Celestial "
                                          "can use a rule to drive its rotation."),
                           {QStringLiteral("None"),
                            WORLD_CELESTIAL_STATIC_SUN,
                            QStringLiteral("Static Moon"),
                            WORLD_CELESTIAL_DYNAMIC}),
        MakeChoiceProperty(WORLD_CELESTIAL_DRIVER,
                           QStringLiteral("Rotation Driver"),
                           QStringLiteral("Use the Transform directly, sample an Earth sky rule, or author a custom "
                                          "orbit. +X is east, +Y is up and -Z is north."),
                           {WORLD_CELESTIAL_AUTHORED, EARTH_SUN, EARTH_MOON, WORLD_CELESTIAL_ORBIT}),
        MakeCelestialNumberProperty(
        {
            .Id = WORLD_CELESTIAL_TIME,
            .Label = QStringLiteral("Earth Time"),
            .Unit = QStringLiteral("days"),
            .Help = QStringLiteral("Fractional UTC days since noon on January 1, 2000. Add 0.25 to advance six hours."),
            .Minimum = -MAX_EARTH_CELESTIAL_DAYS,
            .Maximum = MAX_EARTH_CELESTIAL_DAYS,
        }),
        MakeCelestialNumberProperty(
        {
            .Id = WORLD_CELESTIAL_LATITUDE,
            .Label = QStringLiteral("Observer Latitude"),
            .Unit = degrees,
            .Help = QStringLiteral("Latitude on the observer world. North is positive."),
            .Minimum = -MAX_CELESTIAL_LATITUDE_DEGREES,
            .Maximum = MAX_CELESTIAL_LATITUDE_DEGREES,
        }),
        MakeCelestialNumberProperty(
        {
            .Id = WORLD_CELESTIAL_LONGITUDE,
            .Label = QStringLiteral("Observer Longitude"),
            .Unit = degrees,
            .Help = QStringLiteral("Observer longitude at the reference time. East is positive."),
            .Minimum = -MAX_CELESTIAL_LONGITUDE_DEGREES,
            .Maximum = MAX_CELESTIAL_LONGITUDE_DEGREES,
        }),
        MakeCelestialNumberProperty(
        {
            .Id = ORBIT_TIME,
            .Label = QStringLiteral("Orbit Time"),
            .Unit = QStringLiteral("h"),
            .Help = QStringLiteral("Simulation hours since the shared reference time. Negative values sample earlier."),
            .Minimum = -MAX_CELESTIAL_ORBIT_HOURS,
            .Maximum = MAX_CELESTIAL_ORBIT_HOURS,
        }),
        MakeCelestialNumberProperty(
        {
            .Id = OBSERVER_SPIN,
            .Label = QStringLiteral("World Spin"),
            .Unit = degreesPerHour,
            .Help = QStringLiteral("Rotation of the observer world. Positive turns east. Zero holds it fixed."),
            .Minimum = -MAX_CELESTIAL_RATE_DEGREES_PER_HOUR,
            .Maximum = MAX_CELESTIAL_RATE_DEGREES_PER_HOUR,
            .Default = DEFAULT_WORLD_SPIN_DEGREES_PER_HOUR,
        }),
        MakeCelestialNumberProperty(
        {
            .Id = ORBIT_RATE,
            .Label = QStringLiteral("Body Orbit Rate"),
            .Unit = degreesPerHour,
            .Help = QStringLiteral("How fast the source moves around its circular path. Negative reverses the path."),
            .Minimum = -MAX_CELESTIAL_RATE_DEGREES_PER_HOUR,
            .Maximum = MAX_CELESTIAL_RATE_DEGREES_PER_HOUR,
        }),
        MakeCelestialNumberProperty(
        {
            .Id = ORBIT_PHASE,
            .Label = QStringLiteral("Body Phase"),
            .Unit = degrees,
            .Help = QStringLiteral("Source position along the orbit at time zero, measured from its ascending node."),
            .Minimum = -MAX_CELESTIAL_ORBIT_ANGLE_DEGREES,
            .Maximum = MAX_CELESTIAL_ORBIT_ANGLE_DEGREES,
        }),
        MakeCelestialNumberProperty(
        {
            .Id = ORBIT_INCLINATION,
            .Label = QStringLiteral("Orbit Inclination"),
            .Unit = degrees,
            .Help = QStringLiteral("Tilt of the source's orbital plane relative to the observer world's equator."),
            .Minimum = 0,
            .Maximum = MAX_CELESTIAL_ORBIT_ANGLE_DEGREES,
        }),
        MakeCelestialNumberProperty(
        {
            .Id = ORBIT_ASCENDING_NODE,
            .Label = QStringLiteral("Ascending Node"),
            .Unit = degrees,
            .Help = QStringLiteral(
                "Angle east to the point where the source's positive orbit crosses the equator northward."),
            .Minimum = -MAX_CELESTIAL_ORBIT_ANGLE_DEGREES,
            .Maximum = MAX_CELESTIAL_ORBIT_ANGLE_DEGREES,
        }),
        MakeCelestialNumberProperty(
        {
            .Id = OBSERVER_RADIUS_RATIO,
            .Label = QStringLiteral("World Radius / Orbit Radius"),
            .Unit = QString(),
            .Help = QStringLiteral("World radius divided by orbit radius. Use zero for a distant source or a nonzero "
                                   "ratio to account for a nearby moon's parallax."),
            .Minimum = 0,
            .Maximum = MAX_CELESTIAL_OBSERVER_RADIUS_RATIO,
        }),
    };
}

WorldCelestialFieldGroup GetWorldCelestialFieldGroup(QStringView property)
{
    if (property == WORLD_CELESTIAL_ROLE)
    {
        return WorldCelestialFieldGroup::Role;
    }
    if (property == WORLD_CELESTIAL_DRIVER)
    {
        return WorldCelestialFieldGroup::Driver;
    }
    if (property == WORLD_CELESTIAL_LATITUDE || property == WORLD_CELESTIAL_LONGITUDE)
    {
        return WorldCelestialFieldGroup::Observer;
    }
    if (property == WORLD_CELESTIAL_TIME)
    {
        return WorldCelestialFieldGroup::Earth;
    }
    if (property == ORBIT_TIME || property == OBSERVER_SPIN || property == ORBIT_RATE || property == ORBIT_PHASE ||
        property == ORBIT_INCLINATION || property == ORBIT_ASCENDING_NODE || property == OBSERVER_RADIUS_RATIO)
    {
        return WorldCelestialFieldGroup::Orbit;
    }
    return WorldCelestialFieldGroup::Other;
}

bool IsWorldCelestialProperty(QStringView property)
{
    return GetWorldCelestialFieldGroup(property) != WorldCelestialFieldGroup::Other;
}

bool TryResolveWorldCelestialRotation(const QJsonObject& light,
                                      foundation::math::Quaternion& inoutRotation,
                                      bool& outIsAboveHorizon)
{
    using namespace gameplay::world;
    const QString driver = light.value(WORLD_CELESTIAL_DRIVER).toString();
    if (light.value(WORLD_CELESTIAL_ROLE).toString() != WORLD_CELESTIAL_DYNAMIC ||
        (driver != EARTH_SUN && driver != EARTH_MOON && driver != WORLD_CELESTIAL_ORBIT))
    {
        outIsAboveHorizon = true;
        return true;
    }

    CelestialTransform sample;
    const WorldEarthObserver observer = ReadWorldEarthObserver(light);
    const bool isSampled =
        driver == WORLD_CELESTIAL_ORBIT
            ? TryResolveCustomOrbit(light, sample)
            : TrySampleEarthCelestialTransform(driver == EARTH_SUN ? EarthCelestialBody::Sun : EarthCelestialBody::Moon,
                                               observer.DaysSinceJ2000,
                                               observer.LatitudeDegrees,
                                               observer.LongitudeDegrees,
                                               sample);
    if (isSampled == false)
    {
        return false;
    }
    inoutRotation = sample.Rotation;
    outIsAboveHorizon = sample.IsAboveHorizon;
    return true;
}

bool TrySampleWorldEarthNightSky(const QJsonObject& light, gameplay::world::EarthNightSkySample& outSky)
{
    const QString driver = light.value(WORLD_CELESTIAL_DRIVER).toString();
    if (light.value(WORLD_CELESTIAL_ROLE).toString() != WORLD_CELESTIAL_DYNAMIC ||
        (driver != EARTH_SUN && driver != EARTH_MOON))
    {
        return false;
    }
    const WorldEarthObserver observer = ReadWorldEarthObserver(light);
    return gameplay::world::TrySampleEarthNightSky(observer.DaysSinceJ2000,
                                                   observer.LatitudeDegrees,
                                                   observer.LongitudeDegrees,
                                                   outSky);
}
} // namespace ludus::editor
