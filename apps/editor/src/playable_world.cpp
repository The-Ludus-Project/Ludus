#include "internal/playable_world.h"

#include "internal/builtin_world.h"
#include "internal/project_files.h"
#include "internal/render_scene.h"
#include "internal/world_celestial_light.h"
#include "internal/world_climate.h"
#include "internal/world_document.h"
#include "internal/world_terrain.h"

#include <ludus/content/scene_cook/scene_assets.h>

#include <ludus/content/scene_format/playable_world.h>
#include <ludus/graphics/renderer/atmosphere.hpp>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>

namespace ludus::editor
{
namespace
{
using namespace Qt::StringLiterals;
namespace cook = content::scene_cook;
using namespace foundation;

bool CheckCancellation(const cook::Cancellation* cancelOrNull, QString& outError)
{
    if (cancelOrNull != nullptr && cancelOrNull->Requested != nullptr &&
        cancelOrNull->Requested(cancelOrNull->ContextOrNull) == true)
    {
        outError = QStringLiteral("World preparation cancelled.");
        return false;
    }
    return true;
}
QString ComputeDigest(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool AssembleImports(const QString& root,
                     const QJsonObject& level,
                     const cook::Cancellation* cancelOrNull,
                     cook::EditorSceneResult& outResult,
                     QString& outError)
{
    const QByteArray project = root.toUtf8();
    const QByteArray encoded = QJsonDocument(level).toJson(QJsonDocument::Compact);
    cook::EditorSceneRequest request;
    request.Root = {project.constData(), static_cast<usize>(project.size())};
    request.Level = {reinterpret_cast<const uint8*>(encoded.constData()), static_cast<usize>(encoded.size())};
    request.ShouldVerifySourceInputs = true;
    cook::AssetOptions options;
    options.CancelOrNull = cancelOrNull;
    cook::AssetError error;
    if (cook::OperateEditorScene(request, options, outResult, error) == false)
    {
        outError =
            QStringLiteral("Check the World imports %1 and reimport the affected asset. %2")
                .arg(QString::fromUtf8(
                         QJsonDocument(level.value("imports"_L1).toArray()).toJson(QJsonDocument::Compact)),
                     // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): the message length is passed.
                     QString::fromUtf8(error.Message.View().data(), static_cast<qsizetype>(error.Message.Length)));
        return false;
    }
    return true;
}
QJsonArray MakeVector(math::Vector3 value)
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}
QJsonObject EncodeAtmosphere(const graphics::renderer::AtmosphereDescription& sky)
{
    const graphics::renderer::CelestialBackgroundDescription& celestial = sky.Celestial;
    QJsonArray planets;
    for (const graphics::renderer::PlanetBackgroundDescription& planet : celestial.Planets)
    {
        planets.append(QJsonObject{{"directionWorld"_L1, MakeVector(planet.DirectionWorld)},
                                   {"illuminance"_L1, MakeVector(planet.Illuminance)}});
    }
    const graphics::renderer::CloudLayerDescription& clouds = sky.Clouds;
    const QJsonObject cloudLayer{{"enabled"_L1, clouds.IsEnabled},
                                 {"baseMetres"_L1, clouds.BaseHeightMetres},
                                 {"thicknessMetres"_L1, clouds.ThicknessMetres},
                                 {"sizeXMetres"_L1, clouds.SizeXMetres},
                                 {"sizeZMetres"_L1, clouds.SizeZMetres},
                                 {"edgeFadeMetres"_L1, clouds.EdgeFadeMetres},
                                 {"density"_L1, clouds.Density},
                                 {"extinctionPerMetre"_L1, clouds.ExtinctionPerMetre},
                                 {"albedo"_L1, clouds.SingleScatteringAlbedo},
                                 {"phaseAsymmetry"_L1, clouds.PhaseAsymmetry},
                                 {"axisXWorld"_L1, MakeVector(clouds.AxisXWorld)}};
    const graphics::renderer::AuroraLayerDescription& aurora = sky.Aurora;
    const QJsonObject auroraLayer{{"enabled"_L1, aurora.IsEnabled},
                                  {"emissionPerMetre"_L1, aurora.EmissionPerMetre},
                                  {"timeSeconds"_L1, aurora.TimeSeconds},
                                  {"axisXWorld"_L1, MakeVector(aurora.AxisXWorld)}};
    return {{"enabled"_L1, sky.IsEnabled},
            {"anchorWorld"_L1, MakeVector(sky.SurfaceAnchorWorld)},
            {"upWorld"_L1, MakeVector(sky.SurfaceUpWorld)},
            {"toSunWorld"_L1, MakeVector(sky.DirectionToSunWorld)},
            {"solarIlluminance"_L1, MakeVector(sky.SolarIlluminance)},
            {"solarRadiusRadians"_L1, sky.SolarAngularRadiusRadians},
            {"sunSourceId"_L1, QString::number(sky.SunSourceId)},
            {"lightScene"_L1, sky.ShouldLightScene},
            {"celestialEnabled"_L1, celestial.IsEnabled},
            {"starRadiance"_L1, celestial.StarRadiance},
            {"toMoonWorld"_L1, MakeVector(celestial.DirectionToMoonWorld)},
            {"lunarPeakRadiance"_L1, MakeVector(celestial.LunarPeakRadiance)},
            {"lunarRadiusRadians"_L1, celestial.LunarAngularRadiusRadians},
            {"milkyWayRadiance"_L1, celestial.MilkyWayRadiance},
            {"galacticNorthWorld"_L1, MakeVector(celestial.GalacticNorthWorld)},
            {"galacticCenterWorld"_L1, MakeVector(celestial.GalacticCenterWorld)},
            {"toSunAtMoonWorld"_L1, MakeVector(celestial.DirectionToSunAtMoonWorld)},
            {"planets"_L1, planets},
            {"clouds"_L1, cloudLayer},
            {"aurora"_L1, auroraLayer}};
}
/// The manifest's climate member: the World climate cooked to LUWTH, as lowercase hex with its SHA-256, or
/// null when the World has no climate. The player admits the bytes with the runtime's validating loader.
bool EncodeClimate(const WorldDocument& world, const QString& relativeWorld, QJsonValue& outClimate, QString& outError)
{
    const QJsonValue authored = world.Draft().value(WORLD_CLIMATE);
    if (authored.isUndefined() == true)
    {
        outClimate = QJsonValue::Null;
        return true;
    }
    foundation::Array<uint8> cooked;
    QString error;
    if (TryCookWorldClimate(authored, cooked, error) == false)
    {
        outError = QStringLiteral("%1: %2").arg(relativeWorld, error);
        return false;
    }
    const QByteArray bytes(reinterpret_cast<const char*>(cooked.GetData()), static_cast<qsizetype>(cooked.GetSize()));
    outClimate = QJsonObject{{"bytes"_L1, QString::fromLatin1(bytes.toHex())}, {"sha256"_L1, ComputeDigest(bytes)}};
    return true;
}

bool ExtractEntities(const WorldDocument& world, QJsonArray& outEntities, QJsonArray& outWarnings, QString& outError)
{
    bool hasSpawn = false;
    const QSet<QString> supported{"transform"_L1,
                                  "light"_L1,
                                  "renderable"_L1,
                                  "firstPersonPlayer"_L1,
                                  GetStaticColliderComponentId(),
                                  GetWorldAtmosphereComponentId()};
    for (const QJsonValue value : world.Draft().value("entities"_L1).toArray())
    {
        const QJsonObject entity = value.toObject();
        const QString key = world.GetEntityKey(entity);
        const QJsonObject components = entity.value("components"_L1).toObject();
        QJsonObject record{{"id"_L1, key}, {"name"_L1, entity.value("name"_L1)}};
        for (const QString& component : components.keys())
        {
            if (supported.contains(component) == false)
            {
                outWarnings.append(
                    QStringLiteral("%1 / %2 is outside the built-in Raster player profile.").arg(key, component));
            }
        }
        if (components.contains("firstPersonPlayer"_L1) == true)
        {
            gameplay::camera::CameraSample camera;
            float32 speed = 0;
            if (ReadWorldFirstPersonPlayer(world, key, camera, speed, outError) == false)
            {
                outError.prepend(key + QStringLiteral(": "));
                return false;
            }
            const math::Quaternion rotation = camera.Pose.Orientation;
            record.insert(
                "spawn"_L1,
                QJsonObject{
                    {"eyeWorld"_L1, QJsonArray{camera.Pose.Position.X, camera.Pose.Position.Y, camera.Pose.Position.Z}},
                    {"orientation"_L1,
                     QJsonArray{static_cast<float64>(rotation.X),
                                static_cast<float64>(rotation.Y),
                                static_cast<float64>(rotation.Z),
                                static_cast<float64>(rotation.W)}},
                    {"speed"_L1, speed}});
            hasSpawn = true;
        }
        if (components.contains("light"_L1) == true)
        {
            record.insert("light"_L1, MakeWorldLightValues(components.value("light"_L1).toObject(), world.Draft()));
            record.insert("lightSourceId"_L1, QString::number(ComputeEntitySourceId(key)));
            record.insert("transform"_L1, components.value("transform"_L1));
        }
        if (components.contains(GetWorldAtmosphereComponentId()) == true)
        {
            record.insert("atmosphere"_L1, components.value(GetWorldAtmosphereComponentId()));
        }
        outEntities.append(record);
    }
    if (hasSpawn == false)
    {
        outError = QStringLiteral("Add a First Person Player with a Transform to this World.");
    }
    return hasSpawn;
}
} // namespace

// NOLINTBEGIN(bugprone-easily-swappable-parameters): named project paths and renderer, matching the header.
bool PreparePlayableWorld(const QString& root,
                          const QString& relativeWorld,
                          const WorldDocument& world,
                          const QString& renderer,
                          const QString& destination,
                          const cook::Cancellation* cancelOrNull,
                          QString& outError,
                          QStringList* warningsOrNull)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    if (relativeWorld.isEmpty() == true || QDir::isAbsolutePath(relativeWorld) == true ||
        relativeWorld != QDir::cleanPath(relativeWorld) || relativeWorld == ".."_L1 ||
        relativeWorld.startsWith("../"_L1) == true)
    {
        outError = QStringLiteral("Choose a World using its path relative to the project root.");
        return false;
    }
    if (world.HasBuiltInSchema() == false ||
        renderer != QString::fromLatin1(content::scene_format::PLAYABLE_RASTER_RENDERER.data()))
    {
        outError = QStringLiteral("%1: choose a built-in World and the raster renderer for Play.").arg(relativeWorld);
        return false;
    }
    QJsonArray entities;
    QJsonArray warnings;
    QJsonValue climate;
    RenderScene authored;
    if (CheckCancellation(cancelOrNull, outError) == false ||
        EncodeClimate(world, relativeWorld, climate, outError) == false ||
        ExtractEntities(world, entities, warnings, outError) == false ||
        RenderScene::Load(root, world, authored, outError, false) == false)
    {
        return false;
    }
    if (authored.AtmosphereStatus.isEmpty() == false)
    {
        QStringList owners;
        for (const QJsonValue entity : FindWorldAtmosphereEntities(world.Draft()))
        {
            owners.append(world.GetEntityKey(entity.toObject()));
        }
        outError = owners.join(QStringLiteral(", ")) + QStringLiteral(" / Atmosphere: ") + authored.AtmosphereStatus;
        return false;
    }
    QJsonObject level = world.SceneLevel();
    cook::EditorSceneResult assembly;
    if (level.isEmpty() == false && AssembleImports(root, level, cancelOrNull, assembly, outError) == false)
    {
        return false;
    }
    const QJsonObject response =
        QJsonDocument::fromJson(QByteArray::fromRawData(reinterpret_cast<const char*>(assembly.Response.GetData()),
                                                        static_cast<qsizetype>(assembly.Response.GetSize())))
            .object();
    const QJsonObject artifacts = response.value("artifacts"_L1).toObject();
    for (const QJsonValue warning : response.value("warnings"_L1).toArray())
    {
        warnings.append(warning);
    }
    const QString id = world.HasUuidRecords() == true ? world.Draft().value("document"_L1).toString()
                                                      : level.value("id"_L1).toString(relativeWorld);
    if (level.isEmpty() == true)
    {
        level.insert("id"_L1, id);
        level.insert("camera"_L1, MakeStartViewJson({ .EyeWorld = {0, 0, 1}, .TargetWorld = {}, .VerticalFov = 1 }));
    }
    const QByteArray imported = QByteArray::fromRawData(reinterpret_cast<const char*>(assembly.Scene.GetData()),
                                                        static_cast<qsizetype>(assembly.Scene.GetSize()));
    RenderScene composed;
    QByteArray bundle;
    if (CheckCancellation(cancelOrNull, outError) == false ||
        RenderScene::Merge(authored, imported, level, composed, outError, artifacts, &bundle) == false)
    {
        return false;
    }
    QHash<QString, QString> objectOwners;
    for (const QJsonValue entry : world.Draft().value("entities"_L1).toArray())
    {
        const QJsonObject entity = entry.toObject();
        const QString key = world.GetEntityKey(entity);
        objectOwners.insert(key, key);
        if (entity.value("components"_L1).toObject().contains(GetTerrainComponentId()) == true)
        {
            objectOwners.insert(GetTerrainWaterObjectId(key), key);
        }
    }
    QJsonArray objects;
    QSet<QString> colliders = GetWorldStaticColliderEntityIds(world);
    for (qsizetype i = 0; i < composed.Objects.size(); ++i)
    {
        const PreviewObject& object = composed.Objects[i];
        const QString owner = objectOwners.value(object.Id);
        if (owner.isEmpty() == true)
        {
            outError = QStringLiteral("%1: assign the render object %2 to a World entity before preparing Play.")
                           .arg(relativeWorld, object.Id);
            return false;
        }
        const uint64 sourceId = composed.Definition->Objects[static_cast<usize>(i)].SourceId;
        objects.append(QJsonObject{{"entity"_L1, owner},
                                   {"sourceId"_L1, QString::number(sourceId)},
                                   {"staticCollider"_L1, colliders.remove(object.Id) == true}});
        if (composed.Surfaces[object.Surface].HasUnresolvedMaterial == true)
        {
            outError = QStringLiteral("%1: map the unresolved material before preparing Play.").arg(object.Id);
            return false;
        }
    }
    if (colliders.isEmpty() == false)
    {
        outError = QStringLiteral("%1: attach Static Collider to a visible mesh or remove the component.")
                       .arg(*colliders.cbegin());
        return false;
    }
    QJsonArray dependencies;
    for (QJsonObject::const_iterator entry = artifacts.constBegin(); entry != artifacts.constEnd(); ++entry)
    {
        dependencies.append(QJsonObject{{"id"_L1, entry.key()}, {"key"_L1, entry.value()}});
    }
    const QJsonObject manifest{
        {"version"_L1, static_cast<int32>(content::scene_format::PLAYABLE_WORLD_VERSION)},
        {"world"_L1, id},
        {"source"_L1, relativeWorld},
        {"worldSha256"_L1, ComputeDigest(world.Encode())},
        {"sceneSha256"_L1,
         bundle.isEmpty() == true ? ComputeDigest(bundle) : QString::fromLatin1(composed.PacketIdentity.toHex())},
        {"renderer"_L1, renderer},
        {"entities"_L1, entities},
        {"objects"_L1, objects},
        {"earthObserver"_L1, GetWorldEarthObserver(world.Draft())},
        {"atmosphere"_L1, EncodeAtmosphere(authored.GetAtmosphereForView())},
        {"climate"_L1, climate},
        {"artifacts"_L1, dependencies},
        {"warnings"_L1, warnings}};
    const QByteArray header = QJsonDocument(manifest).toJson(QJsonDocument::Compact) + '\n';
    if (header.size() - 1 > static_cast<qsizetype>(content::scene_format::MAX_PLAYABLE_MANIFEST_BYTES))
    {
        outError = QStringLiteral("World manifest exceeds its byte budget. Split the World into smaller imports.");
        return false;
    }
    if (destination.endsWith(".luplay"_L1) == false)
    {
        outError = QStringLiteral("Choose a .luplay output file.");
        return false;
    }
    QSaveFile file(destination);
    if (file.open(QIODevice::WriteOnly) == false || file.write(header) != header.size() ||
        file.write(bundle) != bundle.size())
    {
        outError = QStringLiteral("Choose a writable snapshot destination. %1").arg(file.errorString());
        return false;
    }
    if (CheckCancellation(cancelOrNull, outError) == false)
    {
        return false;
    }
    if (file.commit() == false)
    {
        outError = QStringLiteral("Choose a writable snapshot destination. %1").arg(file.errorString());
        return false;
    }
    if (warningsOrNull != nullptr)
    {
        warningsOrNull->clear();
        for (const QJsonValue warning : warnings)
        {
            warningsOrNull->append(warning.toString());
        }
    }
    outError.clear();
    return true;
}
namespace
{
bool ReadProjectInput(const QString& root, const QString& path, int64 maximum, QByteArray& outBytes, QString& outError)
{
    QFile file(path);
    if (ProjectFiles::IsProjectFile(root, path) == true && file.open(QIODevice::ReadOnly) == true)
    {
        const QByteArray bytes = file.read(maximum + 1);
        if (bytes.size() <= maximum && file.error() == QFileDevice::NoError && file.atEnd() == true)
        {
            outBytes = bytes;
            return true;
        }
    }
    outError = QStringLiteral("Check that %1 is a readable project file of at most %2 bytes.").arg(path).arg(maximum);
    return false;
}
} // namespace
bool PrepareSavedPlayableWorld(const QString& root,
                               const QString& relativeWorld,
                               const QString& renderer,
                               const QString& destination,
                               QString& outError,
                               QStringList* warningsOrNull)
{
    const QString path = QDir(root).filePath(relativeWorld);
    const QString schemaPath = QDir(root).filePath(QStringLiteral("authoring/world.schema.json"));
    const QFileInfo schemaFile(schemaPath);
    QByteArray schema;
    QByteArray bytes;
    WorldDocument world;
    const bool hasSchema = schemaFile.exists() == true || schemaFile.isSymLink() == true;
    if ((hasSchema == true ? ReadProjectInput(root, schemaPath, WorldDocument::MAX_SCHEMA_BYTES, schema, outError)
                           : ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, outError)) == false ||
        world.LoadSchema(schema, outError) == false ||
        ReadProjectInput(root, path, WorldDocument::MAX_BYTES, bytes, outError) == false ||
        world.Load(bytes, outError) == false)
    {
        return false;
    }
    return PreparePlayableWorld(root, relativeWorld, world, renderer, destination, nullptr, outError, warningsOrNull);
}

namespace
{
/// Name of the resolved scattering model a cooked material carries, for the export manifest.
QString MaterialModelName(content::scene_format::MaterialModel model)
{
    switch (model)
    {
        case content::scene_format::MaterialModel::HairCard:
            return QStringLiteral("hair_card");
        case content::scene_format::MaterialModel::Skin:
            return QStringLiteral("skin");
        case content::scene_format::MaterialModel::MetallicRoughness:
            return QStringLiteral("metallic_roughness");
        case content::scene_format::MaterialModel::Diffuse:
            return QStringLiteral("diffuse");
        default:
            return QStringLiteral("other");
    }
}

/// Describe each imported material slot of a cooked definition: its name, resolved model, alpha mask
/// and whether it binds a texture, so a consumer can prove the hair opacity map and skin inputs survived.
QJsonArray DescribeMaterialSlots(const resources::SceneDefinition& scene)
{
    QJsonArray slots;
    for (const resources::SceneMaterial& material : scene.Materials)
    {
        const std::string_view name = material.Name.View();
        slots.append(QJsonObject{{"name"_L1, QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()))},
                                 {"model"_L1, MaterialModelName(material.Scattering.Model)},
                                 {"alphaMasked"_L1, material.IsAlphaMasked},
                                 {"alphaCutoff"_L1, static_cast<double>(material.AlphaCutoff)},
                                 {"hasTexture"_L1, material.Texture != ~uint32{0}}});
    }
    return slots;
}
} // namespace

bool ExportCookedWorldScene(const QString& root,
                            const WorldDocument& world,
                            const QString& sceneDestination,
                            const QString& manifestDestination,
                            QString& outError)
{
    if (sceneDestination.endsWith(".luscn"_L1) == false || manifestDestination.endsWith(".json"_L1) == false)
    {
        outError = QStringLiteral("Choose a .luscn scene output and a .json manifest output.");
        return false;
    }
    RenderScene authored;
    if (RenderScene::Load(root, world, authored, outError, false) == false)
    {
        return false;
    }
    if (authored.AtmosphereStatus.isEmpty() == false)
    {
        outError = QStringLiteral("Resolve the World atmosphere before exporting its scene: %1")
                       .arg(authored.AtmosphereStatus);
        return false;
    }
    QJsonObject level = world.SceneLevel();
    if (level.isEmpty() == true)
    {
        outError = QStringLiteral("Import a scene into this World before exporting its cooked scene.");
        return false;
    }
    cook::EditorSceneResult assembly;
    if (AssembleImports(root, level, nullptr, assembly, outError) == false)
    {
        return false;
    }
    const QJsonObject response =
        QJsonDocument::fromJson(QByteArray::fromRawData(reinterpret_cast<const char*>(assembly.Response.GetData()),
                                                        static_cast<qsizetype>(assembly.Response.GetSize())))
            .object();
    const QJsonObject artifacts = response.value("artifacts"_L1).toObject();
    const QByteArray imported = QByteArray::fromRawData(reinterpret_cast<const char*>(assembly.Scene.GetData()),
                                                        static_cast<qsizetype>(assembly.Scene.GetSize()));
    RenderScene composed;
    QByteArray bundle;
    if (RenderScene::Merge(authored, imported, level, composed, outError, artifacts, &bundle) == false)
    {
        return false;
    }
    if (bundle.isEmpty() == true || composed.Definition == nullptr)
    {
        outError = QStringLiteral("The cooked World scene is empty; import a scene with geometry.");
        return false;
    }
    for (const PreviewSurface& surface : composed.Surfaces)
    {
        if (surface.HasUnresolvedMaterial == true)
        {
            outError = QStringLiteral("Map every imported material before exporting the cooked scene.");
            return false;
        }
    }
    QJsonArray dependencies;
    for (QJsonObject::const_iterator entry = artifacts.constBegin(); entry != artifacts.constEnd(); ++entry)
    {
        dependencies.append(QJsonObject{{"id"_L1, entry.key()}, {"key"_L1, entry.value()}});
    }
    const QJsonObject manifest{{"version"_L1, 1},
                               {"worldSha256"_L1, ComputeDigest(world.Encode())},
                               {"sceneSha256"_L1, ComputeDigest(bundle)},
                               {"camera"_L1, level.value("camera"_L1)},
                               {"materialSlots"_L1, DescribeMaterialSlots(*composed.Definition)},
                               {"objects"_L1, static_cast<int>(composed.Definition->Objects.GetSize())},
                               {"meshes"_L1, static_cast<int>(composed.Definition->Meshes.GetSize())},
                               {"textures"_L1, static_cast<int>(composed.Definition->Textures.GetSize())},
                               {"lights"_L1, static_cast<int>(composed.Definition->Lights.GetSize())},
                               {"artifacts"_L1, dependencies}};
    QSaveFile sceneFile(sceneDestination);
    if (sceneFile.open(QIODevice::WriteOnly) == false || sceneFile.write(bundle) != bundle.size() ||
        sceneFile.commit() == false)
    {
        outError = QStringLiteral("Choose a writable scene destination. %1").arg(sceneFile.errorString());
        return false;
    }
    const QByteArray manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    QSaveFile manifestFile(manifestDestination);
    if (manifestFile.open(QIODevice::WriteOnly) == false || manifestFile.write(manifestBytes) != manifestBytes.size() ||
        manifestFile.commit() == false)
    {
        outError = QStringLiteral("Choose a writable manifest destination. %1").arg(manifestFile.errorString());
        return false;
    }
    outError.clear();
    return true;
}
} // namespace ludus::editor
