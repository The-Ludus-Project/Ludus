#include "internal/builtin_world.h"
#include "internal/render_scene_limits.h"
#include "internal/world_aperture.h"
#include "internal/world_document.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace ludus::editor
{
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
} // namespace
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
    light.insert(QStringLiteral("intensity"), static_cast<foundation::float64>(placement.IlluminanceLux));
    return inoutWorld.CreateEntity(
        entropy,
        outError,
        QStringLiteral("Sun"),
        outId,
        QJsonObject{{QStringLiteral("transform"), transform}, {QStringLiteral("light"), light}});
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
        outError = QStringLiteral("Geometry and light presets require the default World schema.");
        return false;
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
