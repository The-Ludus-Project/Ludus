#include "internal/builtin_world.h"
#include "internal/playable_world.h"
#include "internal/render_scene.h"
#include "internal/world_document.h"
#include "playable_snapshot.h"
#include "scene_fixture.h"
#include "world_climate_fixture.h"

#include <ludus/content/scene_format/playable_world.h>
#include <ludus/graphics/renderer/atmosphere.hpp>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace ludus::editor::tests
{
namespace
{
using namespace Qt::StringLiterals;
QByteArray ReadBytes(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}
bool Decode(const QByteArray& bytes, first_person::PlayableSnapshot& outSnapshot)
{
    return first_person::DecodePlayableSnapshot(
        {reinterpret_cast<const foundation::uint8*>(bytes.constData()), static_cast<foundation::usize>(bytes.size())},
        outSnapshot);
}
first_person::PlayableSnapshotDiagnostic DecodeDiagnostic(const QByteArray& bytes,
                                                          first_person::PlayableSnapshot& outSnapshot)
{
    first_person::PlayableSnapshotDiagnostic diagnostic;
    first_person::DecodePlayableSnapshot(
        {reinterpret_cast<const foundation::uint8*>(bytes.constData()), static_cast<foundation::usize>(bytes.size())},
        outSnapshot,
        diagnostic);
    return diagnostic;
}
} // namespace
TEST_CASE("Playable World freezes an applied glTF placement and admits only a complete replacement",
          "[world][playable-world]")
{
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid() == true);
    const QString root = temporary.filePath("project"_L1);
    const QString source = temporary.filePath("source"_L1);
    const QString relative = QStringLiteral("room.world.json");
    const QString output = temporary.filePath("room.luplay"_L1);
    QString error;
    WorldDocument world;
    const auto prepare = [&] {
        const bool isPrepared =
            PreparePlayableWorld(root, relative, world, QStringLiteral("raster"), output, nullptr, error);
        if (isPrepared == false)
        {
            WARN(error.toStdString());
        }
        return isPrepared;
    };
    REQUIRE(CopyTree(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/modules/content/scene_cook/tests/fixtures/gltf/maps"),
                     source) == true);
    REQUIRE(PrepareImportedLevel(root, QStringLiteral("scenes/import.lulevel"), source, error, "scene.gltf") == true);
    const QJsonObject level =
        QJsonDocument::fromJson(ReadBytes(QDir(root).filePath("scenes/import.lulevel"_L1))).object();
    const QString objectId = level.value("objects"_L1).toArray().first().toObject().value("id"_L1).toString();
    QByteArray schema;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    REQUIRE(world.LoadSchema(schema, error) == true);
    REQUIRE(world.New(error) == true);
    REQUIRE(world.ImportScene(level, error) == true);
    REQUIRE(world.Attach(objectId, error, GetStaticColliderComponentId()) == true);
    const QJsonObject entity = world.Entity(objectId);
    QJsonObject placement = world.ScenePlacement(entity);
    placement.insert("position"_L1, QJsonArray{2, 0, 1});
    REQUIRE(world.Edit(objectId,
                       entity.value("components"_L1).toObject(),
                       entity.value("name"_L1).toString(),
                       error,
                       &placement) == true);
    const foundation::identity::EntropySource entropy = platform::GetSystemEntropySource();
    QString spawn;
    QString sun;
    QString sky;
    REQUIRE(CreateWorldObject(world, entropy, QStringLiteral("First Person Player"), spawn, error) == true);
    const QJsonObject player = world.Entity(spawn);
    QJsonObject playerComponents = player.value("components"_L1).toObject();
    QJsonObject spawnTransform = playerComponents.value("transform"_L1).toObject();
    spawnTransform.insert("y"_L1, 2);
    spawnTransform.insert("ry"_L1, 90);
    playerComponents.insert("transform"_L1, spawnTransform);
    REQUIRE(world.Edit(spawn, playerComponents, player.value("name"_L1).toString(), error) == true);
    gameplay::camera::CameraSample expectedCamera;
    foundation::float32 expectedSpeed = 0;
    REQUIRE(ReadWorldFirstPersonPlayer(world, spawn, expectedCamera, expectedSpeed, error) == true);
    REQUIRE(CreateWorldObject(world, entropy, QStringLiteral("Directional Light"), sun, error) == true);
    const QJsonObject sunEntity = world.Entity(sun);
    QJsonObject sunComponents = sunEntity.value("components"_L1).toObject();
    QJsonObject sunLight = sunComponents.value("light"_L1).toObject();
    sunLight.insert(WORLD_RASTER_SHADOWS, false);
    sunComponents.insert("light"_L1, sunLight);
    REQUIRE(world.Edit(sun, sunComponents, sunEntity.value("name"_L1).toString(), error) == true);
    REQUIRE(CreateWorldObject(world, entropy, QStringLiteral("Atmosphere"), sky, error) == true);
    const QJsonObject skyEntity = world.Entity(sky);
    QJsonObject components = skyEntity.value("components"_L1).toObject();
    QJsonObject atmosphere = components.value("atmosphere"_L1).toObject();
    atmosphere.insert("sun"_L1, sun);
    // The cloud layer travels with the Atmosphere into the player.
    constexpr foundation::float64 CLOUD_BASE_METRES = 2500;
    atmosphere.insert("clouds"_L1, true);
    atmosphere.insert("cloudBase"_L1, CLOUD_BASE_METRES);
    constexpr foundation::float64 AURORA_TIME_SECONDS = 19.5;
    atmosphere.insert("aurora"_L1, true);
    atmosphere.insert("auroraTime"_L1, AURORA_TIME_SECONDS);
    components.insert("atmosphere"_L1, atmosphere);
    REQUIRE(world.Edit(sky, components, skyEntity.value("name"_L1).toString(), error) == true);
    const QByteArray catalog = ReadBytes(QDir(root).filePath("assets/catalog.json"_L1));
    REQUIRE(prepare() == true);
    const QByteArray acceptedBytes = ReadBytes(output);
    first_person::PlayableSnapshot accepted;
    REQUIRE(Decode(acceptedBytes, accepted) == true);
    REQUIRE(accepted.Spawns.GetSize() == 1);
    const QByteArray spawnKey = spawn.toUtf8();
    CHECK(accepted.Spawns[0].Entity == std::string_view(spawnKey.constData()));
    CHECK(accepted.Spawns[0].Pose.Position == expectedCamera.Pose.Position);
    CHECK(accepted.Spawns[0].Pose.Orientation.Y == Catch::Approx(expectedCamera.Pose.Orientation.Y));
    CHECK(accepted.Spawns[0].Pose.Orientation.W == Catch::Approx(expectedCamera.Pose.Orientation.W));
    CHECK(accepted.Spawns[0].Speed == expectedSpeed);
    std::string_view worldDigest;
    REQUIRE(accepted.Manifest.Root().Get("worldSha256").String(worldDigest) == true);
    CHECK(worldDigest == QCryptographicHash::hash(world.Encode(), QCryptographicHash::Sha256).toHex().toStdString());
    REQUIRE(accepted.ColliderObjects.GetSize() == 1);
    const resources::SceneObject& object = accepted.Scene.Objects[accepted.ColliderObjects[0]];
    CHECK(object.Transform.Translation == foundation::math::Vector3{2, 0, 1});
    REQUIRE(accepted.Scene.Lights.GetSize() >= 1);
    const resources::SceneMaterial& material = accepted.Scene.Materials[object.Material];
    CHECK(material.SurfaceTexture < accepted.Scene.Textures.GetSize());
    CHECK(material.EmissionTexture < accepted.Scene.Textures.GetSize());
    CHECK(accepted.Atmosphere.IsEnabled == true);
    CHECK(accepted.Atmosphere.SunSourceId == ComputeEntitySourceId(sun));
    REQUIRE(accepted.RasterUnshadowedLightIds.GetSize() == 1);
    CHECK(accepted.RasterUnshadowedLightIds[0] == accepted.Atmosphere.SunSourceId);
    CHECK(accepted.Atmosphere.Clouds.IsEnabled == true);
    CHECK(static_cast<foundation::float64>(accepted.Atmosphere.Clouds.BaseHeightMetres) == CLOUD_BASE_METRES);
    CHECK(accepted.Atmosphere.Clouds.ExtinctionPerMetre ==
          graphics::renderer::CloudLayerDescription::DEFAULT_EXTINCTION_PER_METRE);
    CHECK(accepted.Atmosphere.Aurora.IsEnabled == true);
    CHECK(static_cast<foundation::float64>(accepted.Atmosphere.Aurora.TimeSeconds) == AURORA_TIME_SECONDS);
    CHECK(accepted.Atmosphere.Aurora.EmissionPerMetre ==
          graphics::renderer::AuroraLayerDescription::DEFAULT_EMISSION_PER_METRE);
    REQUIRE(prepare() == true);
    CHECK(ReadBytes(output) == acceptedBytes);
    CHECK(ReadBytes(QDir(root).filePath("assets/catalog.json"_L1)) == catalog);
    CHECK(accepted.Manifest.Root().Get("climate").Null() == true);
    CHECK(accepted.Climate.IsEmpty() == true);
    SECTION("the World climate reaches Play as its cooked bytes")
    {
        REQUIRE(world.SetClimate(MakeGoldenWorldClimate(), error) == true);
        foundation::Array<foundation::uint8> cooked;
        REQUIRE(TryCookWorldClimate(world.Draft().value(WORLD_CLIMATE), cooked, error) == true);
        gameplay::weather::Climate expected;
        REQUIRE(gameplay::weather::TryLoadClimate({cooked.GetData(), cooked.GetSize()}, expected) ==
                content::weather_format::WeatherFormatStatus::Success);
        REQUIRE(prepare() == true);
        const QByteArray withClimate = ReadBytes(output);
        first_person::PlayableSnapshot played;
        REQUIRE(Decode(withClimate, played) == true);
        REQUIRE(played.Climate.IsEmpty() == false);
        CHECK(played.Climate.GetFingerprint() == expected.GetFingerprint());
        // A tampered climate byte fails the digest and preserves the accepted generation.
        const QByteArray bytesKey = QByteArrayLiteral("\"bytes\":\"");
        const qsizetype climateAt = withClimate.indexOf(bytesKey);
        REQUIRE(climateAt > 0);
        QByteArray tampered = withClimate;
        const qsizetype digit = climateAt + bytesKey.size();
        tampered[digit] = tampered[digit] == '0' ? '1' : '0';
        CHECK(Decode(tampered, played) == false);
        CHECK(played.Climate.GetFingerprint() == expected.GetFingerprint());
        // An invalid climate never reaches Play: preparation names the record to repair.
        QJsonObject invalid = MakeGoldenWorldClimate();
        QJsonArray regions = invalid.value("regions"_L1).toArray();
        QJsonObject ridge = regions[1].toObject();
        ridge.insert("feather"_L1, -1.0);
        regions[1] = ridge;
        invalid.insert("regions"_L1, regions);
        CHECK(world.SetClimate(invalid, error) == false);
        CHECK(error.contains(u"Region “Ridge”"_s) == true);
        // Removing the climate prepares the original package again, byte for byte.
        REQUIRE(world.SetClimate(QJsonValue::Undefined, error) == true);
        REQUIRE(prepare() == true);
    }
    SECTION("failed runtime decode preserves the accepted generation")
    {
        const QByteArray oversizedManifest(static_cast<qsizetype>(content::scene_format::MAX_PLAYABLE_MANIFEST_BYTES) +
                                               1,
                                           ' ');
        REQUIRE(Decode(oversizedManifest + '\n', accepted) == false);
        REQUIRE(Decode(acceptedBytes.chopped(1), accepted) == false);
        CHECK(accepted.Spawns[0].Entity == std::string_view(spawnKey.constData()));
        QJsonObject header = QJsonDocument::fromJson(acceptedBytes.first(acceptedBytes.indexOf('\n'))).object();
        QJsonArray objects = header.value("objects"_L1).toArray();
        QJsonObject binding = objects.first().toObject();
        binding.insert("entity"_L1, QStringLiteral("missing-entity"));
        objects[0] = binding;
        header.insert("objects"_L1, objects);
        REQUIRE(Decode(QJsonDocument(header).toJson(QJsonDocument::Compact) +
                           acceptedBytes.sliced(acceptedBytes.indexOf('\n')),
                       accepted) == false);
        REQUIRE(Decode(QByteArrayLiteral("{\"version\":2}\n"), accepted) == false);
    }
    SECTION("late cancellation preserves the accepted file")
    {
        struct Polls final
        {
            foundation::usize Count = 0;
            foundation::usize CancelAt = 0;
        } polls;
        const content::scene_cook::Cancellation cancellation
        {
            .Requested =
                [](void* context) noexcept {
                    Polls& progress = *static_cast<Polls*>(context);
                    ++progress.Count;
                    return progress.Count == progress.CancelAt;
                },
            .ContextOrNull = &polls,
        };
        REQUIRE(PreparePlayableWorld(root, relative, world, QStringLiteral("raster"), output, &cancellation, error) ==
                true);
        polls.CancelAt = polls.Count;
        polls.Count = 0;
        REQUIRE(PreparePlayableWorld(root, relative, world, QStringLiteral("raster"), output, &cancellation, error) ==
                false);
        CHECK(error.contains("cancelled"_L1) == true);
    }
    SECTION("stale raw input names the repair and preserves the accepted file")
    {
        QFile file(QDir(source).filePath("scene.gltf"_L1));
        REQUIRE(file.open(QIODevice::Append) == true);
        REQUIRE(file.write(" ") == 1);
        file.close();
        REQUIRE(PreparePlayableWorld(root, relative, world, QStringLiteral("raster"), output, nullptr, error) == false);
        CHECK(error.contains("scene.gltf"_L1) == true);
    }
    SECTION("deleted imported input retains the accepted file")
    {
        REQUIRE(QFile::remove(QDir(source).filePath("scene.gltf"_L1)) == true);
        REQUIRE(PreparePlayableWorld(root, relative, world, QStringLiteral("raster"), output, nullptr, error) == false);
        CHECK(error.contains("scene.gltf"_L1) == true);
    }
    SECTION("missing cooked dependency retains the accepted file")
    {
        const QJsonObject records = QJsonDocument::fromJson(catalog).object().value("resources"_L1).toObject();
        const QString mesh = level.value("objects"_L1).toArray().first().toObject().value("mesh"_L1).toString();
        REQUIRE(QFile::remove(QDir(root).filePath(records.value(mesh).toObject().value("path"_L1).toString())) == true);
        REQUIRE(PreparePlayableWorld(root, relative, world, QStringLiteral("raster"), output, nullptr, error) == false);
    }
    SECTION("unassigned Sun identifies the Atmosphere and preserves the accepted file")
    {
        atmosphere.insert("sun"_L1, QString{});
        components.insert("atmosphere"_L1, atmosphere);
        REQUIRE(world.Edit(sky, components, skyEntity.value("name"_L1).toString(), error) == true);
        REQUIRE(PreparePlayableWorld(root, relative, world, QStringLiteral("raster"), output, nullptr, error) == false);
        CHECK(error.contains(sky) == true);
    }
    SECTION("unsupported renderer keeps the accepted file")
    {
        REQUIRE(PreparePlayableWorld(root, relative, world, QStringLiteral("reference"), output, nullptr, error) ==
                false);
    }
    SECTION("the player names an unsupported renderer and still launches the raster snapshot")
    {
        // Change only the renderer in the otherwise valid accepted manifest; everything else is byte for byte.
        const qsizetype newline = acceptedBytes.indexOf('\n');
        QJsonObject header = QJsonDocument::fromJson(acceptedBytes.first(newline)).object();
        REQUIRE(header.value("renderer"_L1).toString() == QStringLiteral("raster"));
        header.insert("renderer"_L1, QStringLiteral("reference"));
        const QByteArray reskinned =
            QJsonDocument(header).toJson(QJsonDocument::Compact) + acceptedBytes.sliced(newline);
        first_person::PlayableSnapshot rejected;
        const first_person::PlayableSnapshotDiagnostic diagnostic = DecodeDiagnostic(reskinned, rejected);
        // An unsupported renderer is a distinct, named outcome, not the generic unreadable-file rejection.
        CHECK(diagnostic.Status == first_person::PlayableSnapshotStatus::UnsupportedRenderer);
        CHECK(diagnostic.Renderer.GetView() == std::string_view("reference"));
        // The original raster snapshot still decodes, so a supported World launches unchanged.
        first_person::PlayableSnapshot launched;
        const first_person::PlayableSnapshotDiagnostic rasterDiagnostic = DecodeDiagnostic(acceptedBytes, launched);
        CHECK(rasterDiagnostic.Status == first_person::PlayableSnapshotStatus::Ok);
        CHECK(rasterDiagnostic.Renderer.IsEmpty() == true);
        REQUIRE(launched.Spawns.GetSize() == 1);
    }
    CHECK(ReadBytes(output) == acceptedBytes);
}
} // namespace ludus::editor::tests
