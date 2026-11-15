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
        MakeChoiceProperty(
            WORLD_CELESTIAL_DRIVER,
            QStringLiteral("Celestial Preset"),
            QStringLiteral("Use the Transform, choose a physical Earth light, or author a custom orbit. "
                           "+X is east, +Y is up and -Z is north."),
            {WORLD_CELESTIAL_AUTHORED, WORLD_CELESTIAL_EARTH_SUN, WORLD_CELESTIAL_EARTH_MOON, WORLD_CELESTIAL_ORBIT}),
        QJsonObject{
            {QStringLiteral("id"), WORLD_CELESTIAL_MANUAL_PHOTOMETRY},
            {QStringLiteral("label"), QStringLiteral("Override Earth Colour and Intensity")},
            {QStringLiteral("type"), QStringLiteral("boolean")},
            {QStringLiteral("default"), false},
            {QStringLiteral("help"), QStringLiteral("Use the authored colour and intensity with Earth motion.")},
            {QStringLiteral("unit"), QString()}},
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
    if (property == WORLD_CELESTIAL_TIME || property == WORLD_CELESTIAL_MANUAL_PHOTOMETRY)
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

bool IsWorldEarthLight(const QJsonObject& light)
{
    const QString driver = light.value(WORLD_CELESTIAL_DRIVER).toString();
    return light.value(QStringLiteral("kind")).toString() == QStringLiteral("directional") &&
           light.value(WORLD_CELESTIAL_ROLE).toString() == WORLD_CELESTIAL_DYNAMIC &&
           (driver == WORLD_CELESTIAL_EARTH_SUN || driver == WORLD_CELESTIAL_EARTH_MOON);
}

bool AreWorldEarthObserversEqual(const QJsonObject& first, const QJsonObject& second)
{
    for (const QLatin1StringView property : {WORLD_CELESTIAL_TIME, WORLD_CELESTIAL_LATITUDE, WORLD_CELESTIAL_LONGITUDE})
    {
        const QJsonValue firstValue = first.value(property);
        const QJsonValue secondValue = second.value(property);
        if ((firstValue.isUndefined() == true ? QJsonValue(0) : firstValue) !=
            (secondValue.isUndefined() == true ? QJsonValue(0) : secondValue))
        {
            return false;
        }
    }
    return true;
}

void CopyWorldEarthObserver(const QJsonObject& source, QJsonObject& inoutLight)
{
    for (const QLatin1StringView property : {WORLD_CELESTIAL_TIME, WORLD_CELESTIAL_LATITUDE, WORLD_CELESTIAL_LONGITUDE})
    {
        const QJsonValue value = source.value(property);
        if (value.isUndefined() == true)
        {
            inoutLight.remove(property);
        }
        else
        {
            inoutLight.insert(property, value);
        }
    }
}

namespace
{
QJsonObject GetEntityLight(const QJsonObject& entity)
{
    return entity.value(QStringLiteral("components")).toObject().value(QStringLiteral("light")).toObject();
}

void RemoveWorldEarthObserver(QJsonObject& inoutEntity)
{
    QJsonObject components = inoutEntity.value(QStringLiteral("components")).toObject();
    QJsonObject light = components.value(QStringLiteral("light")).toObject();
    for (const QLatin1StringView property : {WORLD_CELESTIAL_TIME, WORLD_CELESTIAL_LATITUDE, WORLD_CELESTIAL_LONGITUDE})
    {
        light.remove(property);
    }
    components.insert(QStringLiteral("light"), light);
    inoutEntity.insert(QStringLiteral("components"), components);
}
} // namespace

bool IsWorldCustomOrbitLight(const QJsonObject& light)
{
    return light.value(QStringLiteral("kind")).toString() == QStringLiteral("directional") &&
           light.value(WORLD_CELESTIAL_ROLE).toString() == WORLD_CELESTIAL_DYNAMIC &&
           light.value(WORLD_CELESTIAL_DRIVER).toString() == WORLD_CELESTIAL_ORBIT;
}

QJsonObject GetWorldEarthObserver(const QJsonObject& world)
{
    const QJsonValue shared = world.value(WORLD_EARTH_OBSERVER);
    if (shared.isUndefined() == false)
    {
        return shared.toObject();
    }
    for (const QJsonValue entity : world.value(QStringLiteral("entities")).toArray())
    {
        const QJsonObject light = GetEntityLight(entity.toObject());
        if (IsWorldEarthLight(light) == true)
        {
            QJsonObject observer;
            CopyWorldEarthObserver(light, observer);
            return observer;
        }
    }
    return {};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a light component, then the World that owns it.
QJsonObject MakeWorldLightValues(const QJsonObject& light, const QJsonObject& world)
{
    const bool hasSharedObserver = world.contains(WORLD_EARTH_OBSERVER);
    if (hasSharedObserver == false && IsWorldEarthLight(light) == true)
    {
        return light;
    }
    const QJsonObject observer = GetWorldEarthObserver(world);
    if (hasSharedObserver == false && observer.isEmpty() == true)
    {
        return light;
    }
    QJsonObject values = light;
    if (IsWorldCustomOrbitLight(light) == true)
    {
        // An orbit keeps its own coordinates; only the Earth date is shared, for a later switch to Earth.
        const QJsonValue time = observer.value(WORLD_CELESTIAL_TIME);
        if (time.isUndefined() == false)
        {
            values.insert(WORLD_CELESTIAL_TIME, time);
        }
        return values;
    }
    CopyWorldEarthObserver(observer, values);
    return values;
}

void LiftWorldEarthObserver(const QJsonObject& previousWorld, qsizetype editedIndex, QJsonObject& inoutWorld)
{
    QJsonArray entities = inoutWorld.value(QStringLiteral("entities")).toArray();
    const QJsonArray previousEntities = previousWorld.value(QStringLiteral("entities")).toArray();
    if (editedIndex < 0 || editedIndex >= entities.size() || editedIndex >= previousEntities.size())
    {
        return;
    }
    const QJsonObject light = GetEntityLight(entities[editedIndex].toObject());
    if (IsWorldEarthLight(light) == false)
    {
        return;
    }
    const QJsonObject previousLight = GetEntityLight(previousEntities[editedIndex].toObject());
    // Without the root member, an Earth light's own saved values are what it showed; they become the World's.
    QJsonObject observer =
        previousWorld.contains(WORLD_EARTH_OBSERVER) == false && IsWorldEarthLight(previousLight) == true
            ? QJsonObject{}
            : GetWorldEarthObserver(previousWorld);
    const QJsonObject shown = MakeWorldLightValues(previousLight, previousWorld);
    for (const QLatin1StringView property : {WORLD_CELESTIAL_TIME, WORLD_CELESTIAL_LATITUDE, WORLD_CELESTIAL_LONGITUDE})
    {
        const QJsonValue proposed = light.value(property);
        if (proposed.isUndefined() == false &&
            (observer.contains(property) == false || proposed != shown.value(property)))
        {
            observer.insert(property, proposed);
        }
    }
    inoutWorld.insert(WORLD_EARTH_OBSERVER, observer);
    for (qsizetype i = 0; i < entities.size(); ++i)
    {
        QJsonObject entity = entities[i].toObject();
        if (IsWorldEarthLight(GetEntityLight(entity)) == true)
        {
            RemoveWorldEarthObserver(entity);
            entities[i] = entity;
        }
    }
    inoutWorld.insert(QStringLiteral("entities"), entities);
}

bool IsWorldEarthObserverValid(const QJsonValue& value)
{
    using namespace gameplay::world;
    if (value.isObject() == false)
    {
        return false;
    }
    const QJsonObject observer = value.toObject();
    qsizetype known = 0;
    for (const QLatin1StringView property : {WORLD_CELESTIAL_TIME, WORLD_CELESTIAL_LATITUDE, WORLD_CELESTIAL_LONGITUDE})
    {
        const QJsonValue member = observer.value(property);
        if (member.isUndefined() == true)
        {
            continue;
        }
        const float64 limit = property == WORLD_CELESTIAL_TIME       ? MAX_EARTH_CELESTIAL_DAYS
                              : property == WORLD_CELESTIAL_LATITUDE ? MAX_CELESTIAL_LATITUDE_DEGREES
                                                                     : MAX_CELESTIAL_LONGITUDE_DEGREES;
        const float64 number = member.toDouble();
        if (member.isDouble() == false || number < -limit || number > limit)
        {
            return false;
        }
        ++known;
    }
    return known == observer.size();
}

bool TrySampleWorldEarthLight(const QJsonObject& light, gameplay::world::EarthCelestialLighting& outLighting)
{
    using namespace gameplay::world;
    if (IsWorldEarthLight(light) == false)
    {
        return false;
    }
    const EarthCelestialBody body = light.value(WORLD_CELESTIAL_DRIVER).toString() == WORLD_CELESTIAL_EARTH_SUN
                                        ? EarthCelestialBody::Sun
                                        : EarthCelestialBody::Moon;
    const WorldEarthObserver observer = ReadWorldEarthObserver(light);
    return TrySampleEarthCelestialLighting(body,
                                           observer.DaysSinceJ2000,
                                           observer.LatitudeDegrees,
                                           observer.LongitudeDegrees,
                                           outLighting);
}

bool TryResolveWorldCelestialRotation(const QJsonObject& light,
                                      foundation::math::Quaternion& inoutRotation,
                                      bool& outIsAboveHorizon,
                                      gameplay::world::EarthCelestialLighting& outEarthLighting)
{
    using namespace gameplay::world;
    CelestialTransform sample;
    EarthCelestialLighting earthLighting;
    if (IsWorldEarthLight(light) == true)
    {
        if (TrySampleWorldEarthLight(light, earthLighting) == false)
        {
            return false;
        }
        sample = earthLighting.Transform;
    }
    else if (light.value(WORLD_CELESTIAL_ROLE).toString() == WORLD_CELESTIAL_DYNAMIC &&
             light.value(WORLD_CELESTIAL_DRIVER).toString() == WORLD_CELESTIAL_ORBIT)
    {
        if (TryResolveCustomOrbit(light, sample) == false)
        {
            return false;
        }
    }
    else
    {
        sample.Rotation = inoutRotation;
        sample.IsAboveHorizon = true;
    }
    inoutRotation = sample.Rotation;
    outIsAboveHorizon = sample.IsAboveHorizon;
    outEarthLighting = earthLighting;
    return true;
}

bool TrySampleWorldEarthNightSky(const QJsonObject& light, gameplay::world::EarthNightSkySample& outSky)
{
    if (IsWorldEarthLight(light) == false)
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
