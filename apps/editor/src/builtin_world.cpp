#include "internal/builtin_world.h"
#include "internal/render_scene_limits.h"
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
bool CreateWorldObject(WorldDocument& inoutWorld,
                       const foundation::identity::EntropySource& entropy,
                       const QString& preset,
                       QString& outId,
                       QString& outError)
{
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
    QJsonObject values;
    for (const QJsonValue property : inoutWorld.Component(component).value(QStringLiteral("properties")).toArray())
    {
        const QJsonObject definition = property.toObject();
        values.insert(definition.value(QStringLiteral("id")).toString(), definition.value(QStringLiteral("default")));
    }
    if (isGeometry == true)
    {
        values.insert(QStringLiteral("mesh"), QStringLiteral("mesh.") + preset.toLower());
    }
    else
    {
        values.insert(QStringLiteral("kind"), preset.section(QLatin1Char(' '), 0, 0).toLower());
    }
    QJsonObject transform;
    for (const QJsonValue property :
         inoutWorld.Component(QStringLiteral("transform")).value(QStringLiteral("properties")).toArray())
    {
        const QJsonObject definition = property.toObject();
        transform.insert(definition.value(QStringLiteral("id")).toString(),
                         definition.value(QStringLiteral("default")));
    }
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
