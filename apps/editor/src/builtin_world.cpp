#include "internal/builtin_world.h"
#include "internal/render_scene.h"
#include "internal/render_scene_limits.h"
#include "internal/world_aperture.h"
#include "internal/world_document.h"
#include "internal/world_probe_volume.h"
#include "internal/world_terrain.h"

#include "internal/world_celestial_light.h"

#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace ludus::editor
{
namespace math = foundation::math;
namespace camera = gameplay::camera;
bool IsBuiltInWorld(const WorldDocument& world)
{
    return world.Schema().value(QStringLiteral("id")) == QJsonValue(QStringLiteral("ludus.world.default.v1"));
}
bool ReadBuiltInWorldAsset(const QString& name, QByteArray& outBytes, QString& outError)
{
    QFile file(QStringLiteral(":/world/") + name);
    if (name.contains(QLatin1Char('/')) == true || file.open(QIODevice::ReadOnly) == false || file.size() <= 0 ||
        file.size() > static_cast<foundation::int64>(MAX_PREVIEW_DOCUMENT_BYTES))
    {
        outError = QStringLiteral("Bundled World asset is unavailable: %1").arg(name);
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || bytes.size() != file.size())
    {
        outError = QStringLiteral("Could not read bundled World asset: %1").arg(name);
        return false;
    }
    outBytes = bytes;
    return true;
}
namespace
{
constexpr foundation::float64 DEFAULT_PLAYER_EYE_HEIGHT_METRES = 1.65;
constexpr foundation::float32 FIRST_PERSON_MAXIMUM_PITCH_DEGREES = 89.9F;
constexpr foundation::float32 FIRST_PERSON_NEAR_METRES = 0.01F;
/// The schema defaults of one component's properties.
QJsonObject MakeDefaultValues(const WorldDocument& world, const QString& component)
{
    QJsonObject values;
    for (const QJsonValue property : world.Component(component).value(QStringLiteral("properties")).toArray())
    {
        const QJsonObject definition = property.toObject();
        values.insert(definition.value(QStringLiteral("id")).toString(), definition.value(QStringLiteral("default")));
    }
    return values;
}
QJsonObject MakeFirstPersonTransform(const WorldDocument& world)
{
    const QString transformKey = QStringLiteral("transform");
    QJsonObject transform = MakeDefaultValues(world, transformKey);
    transform.insert(QStringLiteral("y"), DEFAULT_PLAYER_EYE_HEIGHT_METRES);
    const QJsonObject scene = world.Draft().value(QStringLiteral("scene")).toObject();
    PreviewStartView start;
    if (TryParseStartView(scene.value(QStringLiteral("camera")).toObject(), start) == false)
    {
        return transform;
    }
    const QJsonObject position{{QStringLiteral("x"), start.EyeWorld.X},
                               {QStringLiteral("y"), start.EyeWorld.Y},
                               {QStringLiteral("z"), start.EyeWorld.Z}};
    // Imported camera coordinates can exceed the built-in entity Transform's bounds.
    for (const QJsonValue value : world.Component(transformKey).value(QStringLiteral("properties")).toArray())
    {
        const QJsonObject property = value.toObject();
        const QJsonValue coordinate = position.value(property.value(QStringLiteral("id")).toString());
        if (coordinate.isDouble() == true &&
            (coordinate.toDouble() < property.value(QStringLiteral("min")).toDouble() ||
             coordinate.toDouble() > property.value(QStringLiteral("max")).toDouble()))
        {
            return transform;
        }
    }
    const math::Vector3d directionWorld = start.TargetWorld - start.EyeWorld;
    const foundation::float64 horizontal =
        math::Sqrt(directionWorld.X * directionWorld.X + directionWorld.Z * directionWorld.Z);
    transform.insert(QStringLiteral("x"), start.EyeWorld.X);
    transform.insert(QStringLiteral("y"), start.EyeWorld.Y);
    transform.insert(QStringLiteral("z"), start.EyeWorld.Z);
    transform.insert(QStringLiteral("rx"), math::RadiansToDegrees(math::Atan2(directionWorld.Y, horizontal)));
    transform.insert(QStringLiteral("ry"),
                     horizontal > 0 ? math::RadiansToDegrees(math::Atan2(-directionWorld.X, -directionWorld.Z)) : 0);
    return transform;
}
} // namespace
bool HasWorldFirstPersonPlayer(const WorldDocument& world, const QString& id)
{
    return IsBuiltInWorld(world) == true && world.Entity(id)
                                                .value(QStringLiteral("components"))
                                                .toObject()
                                                .contains(QStringLiteral("firstPersonPlayer"));
}
QString GetStaticColliderComponentId()
{
    return QStringLiteral("staticCollider");
}
bool CanAttachWorldStaticCollider(const QJsonObject& entity)
{
    const QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    return components.contains(GetStaticColliderComponentId()) == false &&
           (components.contains(QStringLiteral("renderable")) == true ||
            components.contains(GetTerrainComponentId()) == true ||
            components.contains(GetApertureComponentId()) == true ||
            entity.value(QStringLiteral("scene")).isObject() == true);
}
QSet<QString> GetWorldStaticColliderEntityIds(const WorldDocument& world)
{
    QSet<QString> ids;
    if (world.HasBuiltInSchema() == false)
    {
        return ids;
    }
    const QString component = GetStaticColliderComponentId();
    for (const QJsonValue value : world.Draft().value(QStringLiteral("entities")).toArray())
    {
        const QJsonObject entity = value.toObject();
        if (entity.value(QStringLiteral("components")).toObject().contains(component) == true)
        {
            ids.insert(world.GetEntityKey(entity));
        }
    }
    return ids;
}
bool ReadWorldFirstPersonPlayer(const WorldDocument& world,
                                const QString& id,
                                camera::CameraSample& outCamera,
                                foundation::float32& outSpeed,
                                QString& outError)
{
    if (HasWorldFirstPersonPlayer(world, id) == false)
    {
        outError = QStringLiteral("Select a First Person Player in the World hierarchy.");
        return false;
    }
    const QJsonObject components = world.Entity(id).value(QStringLiteral("components")).toObject();
    const QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
    if (transform.isEmpty() == true)
    {
        outError = QStringLiteral("Add a Transform to the selected First Person Player to set its spawn position.");
        return false;
    }
    const foundation::float32 pitchDegrees =
        static_cast<foundation::float32>(transform.value(QStringLiteral("rx")).toDouble());
    const foundation::float32 yawDegrees =
        static_cast<foundation::float32>(transform.value(QStringLiteral("ry")).toDouble());
    math::Quaternion pitch;
    math::Quaternion yaw;
    if (math::TryFromAxisAngle(
            {1, 0, 0},
            math::DegreesToRadians(
                math::Clamp(pitchDegrees, -FIRST_PERSON_MAXIMUM_PITCH_DEGREES, FIRST_PERSON_MAXIMUM_PITCH_DEGREES)),
            pitch) != math::MathStatus::Success ||
        math::TryFromAxisAngle({0, 1, 0}, math::DegreesToRadians(yawDegrees), yaw) != math::MathStatus::Success)
    {
        outError = QStringLiteral("Set valid X and Y rotations on the selected First Person Player.");
        return false;
    }
    camera::CameraSample spawn;
    spawn.Pose.Position = {transform.value(QStringLiteral("x")).toDouble(),
                           transform.value(QStringLiteral("y")).toDouble(),
                           transform.value(QStringLiteral("z")).toDouble()};
    spawn.Pose.Orientation = yaw * pitch;
    spawn.Lens.Projection = camera::CameraProjection::PerspectiveInfinite;
    spawn.Lens.NearPlane = FIRST_PERSON_NEAR_METRES;
    outCamera = spawn;
    const QJsonObject player = components.value(QStringLiteral("firstPersonPlayer")).toObject();
    outSpeed = static_cast<foundation::float32>(player.value(QStringLiteral("speed")).toDouble());
    outError.clear();
    return true;
}
bool CreateWorldLuminaire(WorldDocument& inoutWorld,
                          const foundation::identity::EntropySource& entropy,
                          const WorldLuminairePlacement& placement,
                          QString& outId,
                          QString& outError)
{
    if (IsBuiltInWorld(inoutWorld) == false)
    {
        outError = QStringLiteral("Luminaire presets require the default World schema.");
        return false;
    }
    QJsonObject transform = MakeDefaultValues(inoutWorld, QStringLiteral("transform"));
    transform.insert(QStringLiteral("x"), static_cast<foundation::float64>(placement.PositionWorld.X));
    transform.insert(QStringLiteral("y"), static_cast<foundation::float64>(placement.PositionWorld.Y));
    transform.insert(QStringLiteral("z"), static_cast<foundation::float64>(placement.PositionWorld.Z));
    transform.insert(QStringLiteral("rx"), static_cast<foundation::float64>(placement.RotationDegrees.X));
    transform.insert(QStringLiteral("ry"), static_cast<foundation::float64>(placement.RotationDegrees.Y));
    transform.insert(QStringLiteral("rz"), static_cast<foundation::float64>(placement.RotationDegrees.Z));
    QJsonObject luminaire = MakeDefaultValues(inoutWorld, QStringLiteral("luminaire"));
    luminaire.insert(QStringLiteral("halfExtent"), static_cast<foundation::float64>(placement.HalfExtentWorld));
    return inoutWorld.CreateEntity(
        entropy,
        outError,
        QStringLiteral("Luminaire"),
        outId,
        QJsonObject{{QStringLiteral("transform"), transform}, {QStringLiteral("luminaire"), luminaire}});
}
bool CreateWorldSun(WorldDocument& inoutWorld,
                    const foundation::identity::EntropySource& entropy,
                    const WorldSunPlacement& placement,
                    QString& outId,
                    QString& outError)
{
    if (IsBuiltInWorld(inoutWorld) == false)
    {
        outError = QStringLiteral("Light presets require the default World schema.");
        return false;
    }
    QJsonObject transform = MakeDefaultValues(inoutWorld, QStringLiteral("transform"));
    transform.insert(QStringLiteral("x"), static_cast<foundation::float64>(placement.PositionWorld.X));
    transform.insert(QStringLiteral("y"), static_cast<foundation::float64>(placement.PositionWorld.Y));
    transform.insert(QStringLiteral("z"), static_cast<foundation::float64>(placement.PositionWorld.Z));
    transform.insert(QStringLiteral("rx"), static_cast<foundation::float64>(placement.RotationDegrees.X));
    transform.insert(QStringLiteral("ry"), static_cast<foundation::float64>(placement.RotationDegrees.Y));
    transform.insert(QStringLiteral("rz"), static_cast<foundation::float64>(placement.RotationDegrees.Z));
    QJsonObject light = MakeDefaultValues(inoutWorld, QStringLiteral("light"));
    light.insert(QStringLiteral("kind"), QStringLiteral("directional"));
    if (inoutWorld.HasBuiltInSchema() == true)
    {
        light.insert(WORLD_CELESTIAL_ROLE, WORLD_CELESTIAL_STATIC_SUN);
    }
    light.insert(QStringLiteral("intensity"), static_cast<foundation::float64>(placement.IlluminanceLux));
    return inoutWorld.CreateEntity(
        entropy,
        outError,
        QStringLiteral("Sun"),
        outId,
        QJsonObject{{QStringLiteral("transform"), transform}, {QStringLiteral("light"), light}});
}
QString GetWorldAtmosphereComponentId()
{
    return QStringLiteral("atmosphere");
}
bool IsWorldAtmosphereLightField(const QString& component, const QString& field)
{
    return component == GetWorldAtmosphereComponentId() &&
           (field == QStringLiteral("sun") || field == QStringLiteral("moon"));
}
QJsonArray FindWorldAtmosphereEntities(const QJsonObject& document)
{
    const QString component = GetWorldAtmosphereComponentId();
    QJsonArray atmospheres;
    for (const QJsonValue value : document.value(QStringLiteral("entities")).toArray())
    {
        if (value.toObject().value(QStringLiteral("components")).toObject().contains(component) == true)
        {
            atmospheres.append(value);
        }
    }
    return atmospheres;
}
bool IsWorldAtmosphereLight(const QJsonObject& entity)
{
    return entity.value(QStringLiteral("components"))
               .toObject()
               .value(QStringLiteral("light"))
               .toObject()
               .value(QStringLiteral("kind"))
               .toString() == QStringLiteral("directional");
}
bool CreateWorldObject(WorldDocument& inoutWorld,
                       const foundation::identity::EntropySource& entropy,
                       const QString& preset,
                       QString& outId,
                       QString& outError)
{
    if (preset == QStringLiteral("Luminaire"))
    {
        // Facing down above a primitive at the origin.
        constexpr WorldLuminairePlacement ABOVE_ORIGIN
        {
            .PositionWorld = {0, 2, 0},
            .RotationDegrees = {WORLD_LUMINAIRE_DOWNWARD_DEGREES, 0, 0},
            .HalfExtentWorld = WORLD_LUMINAIRE_DEFAULT_HALF_EXTENT,
        };
        return CreateWorldLuminaire(inoutWorld, entropy, ABOVE_ORIGIN, outId, outError);
    }
    if (preset == QStringLiteral("Aperture Slit"))
    {
        if (inoutWorld.HasBuiltInAperture() == false)
        {
            outError = QStringLiteral("Aperture presets require the default World schema.");
            return false;
        }
        // A screen at the origin facing +Z, with the schema's default opening.
        const QString component = GetApertureComponentId();
        return inoutWorld.CreateEntity(
            entropy,
            outError,
            preset,
            outId,
            QJsonObject{{QStringLiteral("transform"), MakeDefaultValues(inoutWorld, QStringLiteral("transform"))},
                        {component, MakeDefaultValues(inoutWorld, component)}});
    }
    if (IsBuiltInWorld(inoutWorld) == false)
    {
        outError = QStringLiteral("World object presets require the default World schema.");
        return false;
    }
    if (preset == QStringLiteral("Atmosphere"))
    {
        const QString component = GetWorldAtmosphereComponentId();
        const QString transform = QStringLiteral("transform");
        return inoutWorld.CreateEntity(entropy,
                                       outError,
                                       preset,
                                       outId,
                                       QJsonObject{{transform, MakeDefaultValues(inoutWorld, transform)},
                                                   {component, MakeDefaultValues(inoutWorld, component)}});
    }
    if (preset == QStringLiteral("GI Probe Volume"))
    {
        const QString component = GetGiProbeVolumeComponentId();
        return inoutWorld.CreateEntity(
            entropy,
            outError,
            preset,
            outId,
            QJsonObject{{QStringLiteral("transform"), MakeDefaultValues(inoutWorld, QStringLiteral("transform"))},
                        {component, MakeDefaultValues(inoutWorld, component)}});
    }
    if (preset == QStringLiteral("First Person Player"))
    {
        const QString component = QStringLiteral("firstPersonPlayer");
        return inoutWorld.CreateEntity(entropy,
                                       outError,
                                       preset,
                                       outId,
                                       QJsonObject{{QStringLiteral("transform"), MakeFirstPersonTransform(inoutWorld)},
                                                   {component, MakeDefaultValues(inoutWorld, component)}});
    }
    const bool isGeometry =
        preset == QStringLiteral("Cube") || preset == QStringLiteral("Plane") || preset == QStringLiteral("Sphere");
    const bool isLight = preset == QStringLiteral("Point Light") || preset == QStringLiteral("Spot Light") ||
                         preset == QStringLiteral("Directional Light");
    if (isGeometry == false && isLight == false)
    {
        outError = QStringLiteral("Unknown World object preset.");
        return false;
    }
    const QString component = isGeometry == true ? QStringLiteral("renderable") : QStringLiteral("light");
    QJsonObject values = MakeDefaultValues(inoutWorld, component);
    if (isGeometry == true)
    {
        values.insert(QStringLiteral("mesh"), QStringLiteral("mesh.") + preset.toLower());
    }
    else
    {
        values.insert(QStringLiteral("kind"), preset.section(QLatin1Char(' '), 0, 0).toLower());
    }
    QJsonObject transform = MakeDefaultValues(inoutWorld, QStringLiteral("transform"));
    if (isLight == true)
    {
        // Place the light above and in front of a primitive at the origin.
        transform.insert(QStringLiteral("y"), 2);
        transform.insert(QStringLiteral("z"), 2);
        constexpr foundation::int32 LIGHT_DOWNWARD_DEGREES = -45;
        transform.insert(QStringLiteral("rx"), LIGHT_DOWNWARD_DEGREES);
    }
    return inoutWorld.CreateEntity(entropy,
                                   outError,
                                   preset,
                                   outId,
                                   QJsonObject{{QStringLiteral("transform"), transform}, {component, values}});
}
} // namespace ludus::editor
