// Version-2 World records in the Editor: open, edit and save documents whose entities
// carry SourceObjectId UUIDs and labels, while version-1 documents stay version 1
// (docs/architecture/object-handles-uuid.md#stage-3-uuid-authoring-records).
#include "entropy_fixture.h"
#include "internal/editor_identity.h"
#include "internal/render_scene.h"
#include "internal/world_demo_adapter.h"
#include "internal/world_document.h"
#include "internal/world_workspace.h"

#include <ludus/content/identity_records/migration_manifest.h>
#include <ludus/content/identity_records/world_records.h>
#include <ludus/foundation/containers/array.hpp>

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <ludus/platform/entropy.h>

#include <string_view>

using namespace ludus::editor;
namespace
{
namespace fixture = ludus::editor::tests;
namespace identity_records = ludus::content::identity_records;

/// Project-relative path of the starter World in STARTER_MANIFEST.
constexpr std::string_view STARTER_PATH = "authoring/starter.world.json";
constexpr const char* STARTER_DOCUMENT = "5a1b2c3d-4e5f-4a6b-9c7d-8e9f0a1b2c3d";
constexpr const char* STARTER_CUBE = "3f2b8c1e-6d4a-4f0b-9a7c-1e2d3c4b5a60";
constexpr const char* STARTER_TARGET = "7c9e2a4b-1f3d-4e5a-8b6c-2d4e6f8a0b1c";
/// Test-owned migration manifest for the live-edit starter World.
constexpr std::string_view STARTER_MANIFEST = R"json({
  "format": "ludus.identity-migration",
  "version": 1,
  "documents": [
    {
      "path": "authoring/starter.world.json",
      "document": "5a1b2c3d-4e5f-4a6b-9c7d-8e9f0a1b2c3d",
      "objects": [
        { "legacy_id": "starter.cube", "object": "3f2b8c1e-6d4a-4f0b-9a7c-1e2d3c4b5a60" },
        { "legacy_id": "starter.target", "object": "7c9e2a4b-1f3d-4e5a-8b6c-2d4e6f8a0b1c" }
      ]
    }
  ]
})json";
/// Path of the world-demo room in the committed world-demo manifest.
constexpr std::string_view FIRST_ROOM_PATH = "levels/first-room.world.json";

QByteArray Read(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
void Write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes) == bytes.size());
}
QByteArray ReadSource(const char* relativePath)
{
    return Read(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT) + QLatin1Char('/') + QLatin1String(relativePath));
}
std::string_view View(const QByteArray& bytes)
{
    return {bytes.constData(), static_cast<ludus::foundation::usize>(bytes.size())};
}
/// Replay `manifestJson` through Ludus::ContentIdentity to turn `source`, a version-1 World, into its version-2 record.
QByteArray
Migrate(std::string_view manifestJson, const identity_records::MigrationSource& source, const QByteArray& schema)
{
    identity_records::IdentityRecordError error;
    identity_records::MigrationManifest manifest;
    REQUIRE(manifest.Read(manifestJson, error) == identity_records::IdentityRecordStatus::Success);
    ludus::foundation::Array<identity_records::EntityReferenceProperty> references;
    REQUIRE(identity_records::ReadEntityReferenceProperties(View(schema), references, error) ==
            identity_records::IdentityRecordStatus::Success);
    ludus::content::Bytes output;
    REQUIRE(identity_records::ApplyWorldMigration(manifest,
                                                  source,
                                                  {references.GetData(), references.GetSize()},
                                                  output,
                                                  error) == identity_records::IdentityRecordStatus::Success);
    const std::string_view text = output.String();
    return QByteArray(text.data(), static_cast<qsizetype>(text.size()));
}
QByteArray StarterSchema()
{
    return ReadSource("examples/live-edit-game/authoring/world.schema.json");
}
QByteArray StarterVersion1()
{
    return ReadSource("examples/live-edit-game/authoring/starter.world.json");
}
QByteArray StarterVersion2()
{
    const QByteArray world = StarterVersion1();
    return Migrate(STARTER_MANIFEST, { .Path = STARTER_PATH, .WorldJson = View(world) }, StarterSchema());
}
QByteArray FirstRoomVersion2()
{
    const QByteArray manifest = ReadSource("apps/world_demo/levels/identity-migration.json");
    const QByteArray world = ReadSource("apps/world_demo/levels/first-room.world.json");
    return Migrate(View(manifest),
                   { .Path = FIRST_ROOM_PATH, .WorldJson = View(world) },
                   ReadSource("apps/world_demo/authoring/world.schema.json"));
}
/// UUID of the committed first-room entity whose label is `label`.
QString FirstRoomUuid(const QString& label)
{
    const QJsonObject world = QJsonDocument::fromJson(FirstRoomVersion2()).object();
    for (const QJsonValue value : world.value(QStringLiteral("entities")).toArray())
    {
        if (value.toObject().value(QStringLiteral("label")) == QJsonValue(label))
        {
            return value.toObject().value(QStringLiteral("uuid")).toString();
        }
    }
    FAIL("Missing first-room label");
    return {};
}
WorldDocument OpenStarter(const QByteArray& world)
{
    WorldDocument document;
    QString error;
    REQUIRE(document.LoadSchema(StarterSchema(), error));
    REQUIRE(document.Load(world, error));
    return document;
}
QJsonValue Property(const QJsonObject& entity, const QString& component, const QString& property)
{
    return entity.value(QStringLiteral("components")).toObject().value(component).toObject().value(property);
}
/// Write `world` and the starter schema into a new project at `directory`; returns the World path.
QString WriteStarterProject(QTemporaryDir& directory, const QByteArray& world)
{
    REQUIRE(directory.isValid());
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    REQUIRE(root.mkdir(QStringLiteral("authoring")));
    Write(root.filePath(QStringLiteral("authoring/world.schema.json")), StarterSchema());
    const QString path = root.filePath(QStringLiteral("authoring/starter.world.json"));
    Write(path, world);
    return path;
}
void Select(WorldWorkspace& view, const QString& key)
{
    QListWidget* list = view.findChild<QListWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(list != nullptr);
    for (ludus::foundation::int32 row = 0; row < list->count(); ++row)
    {
        if (list->item(row)->data(Qt::UserRole).toString() == key)
        {
            list->setCurrentRow(row);
            return;
        }
    }
    FAIL("Missing World entity");
}
} // namespace
TEST_CASE("Version-2 Worlds open with UUID keys, labels and UUID references and round-trip on save",
          "[editor][world][identity]")
{
    WorldDocument document = OpenStarter(StarterVersion2());
    REQUIRE(document.HasUuidRecords());
    const QJsonObject target = document.Entity(QLatin1String(STARTER_TARGET));
    REQUIRE(target.isEmpty() == false);
    CHECK(document.GetEntityKey(target) == QLatin1String(STARTER_TARGET));
    CHECK(document.GetEntityLabel(target) == QStringLiteral("starter.target"));
    CHECK(document.Entity(QStringLiteral("starter.target")).isEmpty());
    CHECK(Property(target, QStringLiteral("behavior"), QStringLiteral("target")) ==
          QJsonValue(QLatin1String(STARTER_CUBE)));

    const QJsonObject saved = QJsonDocument::fromJson(document.Encode()).object();
    CHECK(saved.value(QStringLiteral("version")) == QJsonValue(2));
    CHECK(saved.value(QStringLiteral("document")) == QJsonValue(QLatin1String(STARTER_DOCUMENT)));
    CHECK(saved == document.Draft());
    WorldDocument reopened = OpenStarter(document.Encode());
    CHECK(reopened.Draft() == document.Draft());
}
TEST_CASE("Version-2 Worlds reject invalid identities, legacy references and implicit conversion",
          "[editor][world][identity]")
{
    const QJsonObject valid = QJsonDocument::fromJson(StarterVersion2()).object();
    WorldDocument document = OpenStarter(StarterVersion2());
    QString error;
    const auto withEntity = [&valid](ludus::foundation::int32 index, const QString& member, const QJsonValue& value) {
        QJsonObject candidate = valid;
        QJsonArray entities = candidate.value(QStringLiteral("entities")).toArray();
        QJsonObject entity = entities[index].toObject();
        entity.insert(member, value);
        entities[index] = entity;
        candidate.insert(QStringLiteral("entities"), entities);
        return candidate;
    };
    // Uppercase, version-1, nil and repeated UUIDs; a label repeated by another entity.
    CHECK_FALSE(
        document.Validate(withEntity(0, QStringLiteral("uuid"), QStringLiteral("3F2B8C1E-6D4A-4F0B-9A7C-1E2D3C4B5A60")),
                          error));
    CHECK_FALSE(
        document.Validate(withEntity(0, QStringLiteral("uuid"), QStringLiteral("3f2b8c1e-6d4a-1f0b-9a7c-1e2d3c4b5a60")),
                          error));
    CHECK_FALSE(
        document.Validate(withEntity(0, QStringLiteral("uuid"), QStringLiteral("00000000-0000-0000-0000-000000000000")),
                          error));
    CHECK_FALSE(document.Validate(withEntity(0, QStringLiteral("uuid"), QLatin1String(STARTER_TARGET)), error));
    CHECK_FALSE(document.Validate(withEntity(0, QStringLiteral("uuid"), QLatin1String(STARTER_DOCUMENT)), error));
    CHECK_FALSE(document.Validate(withEntity(0, QStringLiteral("label"), QStringLiteral("starter.target")), error));
    // Version-2 entities have no `id`, and the document identity is required.
    CHECK_FALSE(document.Validate(withEntity(0, QStringLiteral("id"), QStringLiteral("starter.cube")), error));
    QJsonObject withoutDocument = valid;
    withoutDocument.remove(QStringLiteral("document"));
    CHECK_FALSE(document.Validate(withoutDocument, error));
    QJsonObject withScene = valid;
    withScene.insert(QStringLiteral("scene"), QJsonObject{});
    CHECK_FALSE(document.Validate(withScene, error));
    CHECK(error.contains(QStringLiteral("scene extension")));

    // Entity references hold target UUIDs; a legacy ID no longer resolves.
    QJsonObject target = document.Entity(QLatin1String(STARTER_TARGET));
    QJsonObject components = target.value(QStringLiteral("components")).toObject();
    QJsonObject behavior = components.value(QStringLiteral("behavior")).toObject();
    behavior.insert(QStringLiteral("target"), QStringLiteral("starter.cube"));
    components.insert(QStringLiteral("behavior"), behavior);
    const QJsonObject before = document.Draft();
    CHECK_FALSE(document.Edit(QLatin1String(STARTER_TARGET), components, QStringLiteral("Target Marker"), error));
    CHECK(document.Draft() == before);

    // An open document keeps its version and AssetId; a version-1 recovery cannot replace it.
    WorldDocument version1 = OpenStarter(StarterVersion1());
    CHECK_FALSE(document.Restore(version1.Draft(), error));
    QJsonObject otherDocument = valid;
    otherDocument.insert(QStringLiteral("document"), QStringLiteral("6b2c3d4e-5f60-4b7c-8d9e-0f1a2b3c4d5e"));
    CHECK_FALSE(document.Restore(otherDocument, error));
    CHECK_FALSE(version1.Restore(valid, error));
    CHECK_FALSE(document.ImportScene(QJsonObject{{QStringLiteral("objects"), QJsonArray{}}}, error));
    CHECK(document.Draft() == before);
    CHECK_FALSE(document.CanUndo());
}
TEST_CASE("Version-2 Add and Duplicate mint UUIDs and unique labels; Rename keeps identity",
          "[editor][world][identity]")
{
    WorldDocument document = OpenStarter(StarterVersion2());
    const QJsonObject before = document.Draft();
    QString error;
    QString key = QStringLiteral("unchanged");
    CHECK_FALSE(document.CreateEntity(fixture::MakeUnavailableEntropySource(), error, QStringLiteral("Entity"), key));
    CHECK(error == DescribeEntropyUnavailable());
    CHECK(key == QStringLiteral("unchanged"));
    CHECK(document.Draft() == before);

    fixture::SequentialEntropy sequence;
    const ludus::foundation::identity::EntropySource entropy = fixture::MakeSequentialEntropySource(sequence);
    REQUIRE(document.CreateEntity(entropy, error, QStringLiteral("Key Light"), key));
    const QJsonObject created = document.Entity(key);
    CHECK(key == QStringLiteral("01010101-0101-4101-8101-010101010101"));
    CHECK(document.GetEntityLabel(created) == QStringLiteral("key-light"));
    CHECK(created.contains(QStringLiteral("id")) == false);
    QString second;
    REQUIRE(document.CreateEntity(entropy, error, QStringLiteral("Key Light"), second));
    CHECK(second != key);
    CHECK(document.GetEntityLabel(document.Entity(second)) == QStringLiteral("key-light-2"));
    QString unnamed;
    REQUIRE(document.CreateEntity(entropy, error, QStringLiteral("灯光"), unnamed));
    CHECK(document.GetEntityLabel(document.Entity(unnamed)) == QStringLiteral("entity"));

    // Every candidate repeats an identity of this World: nothing changes.
    REQUIRE(document.CreateEntity(fixture::MakeFixedEntropySource(), error, QStringLiteral("Fixed"), key));
    CHECK(key == QLatin1String(fixture::FIXED_UUID_TEXT));
    const QJsonObject beforeConflict = document.Draft();
    QString conflict = QStringLiteral("unchanged");
    CHECK_FALSE(document.CreateEntity(fixture::MakeFixedEntropySource(), error, QStringLiteral("Fixed"), conflict));
    CHECK(error.contains(QStringLiteral("unique")));
    CHECK_FALSE(
        document.DuplicateEntity(fixture::MakeFixedEntropySource(), error, QLatin1String(STARTER_TARGET), conflict));
    CHECK(conflict == QStringLiteral("unchanged"));
    CHECK(document.Draft() == beforeConflict);

    // Duplicate remaps self-references to the copy and keeps other references.
    QJsonObject target = document.Entity(QLatin1String(STARTER_TARGET));
    QJsonObject components = target.value(QStringLiteral("components")).toObject();
    QJsonObject behavior = components.value(QStringLiteral("behavior")).toObject();
    behavior.insert(QStringLiteral("target"), QLatin1String(STARTER_TARGET));
    components.insert(QStringLiteral("behavior"), behavior);
    REQUIRE(document.Edit(QLatin1String(STARTER_TARGET), components, QStringLiteral("Target Marker"), error));
    QString copy;
    REQUIRE(document.DuplicateEntity(entropy, error, QLatin1String(STARTER_TARGET), copy));
    const QJsonObject duplicate = document.Entity(copy);
    CHECK(copy != QLatin1String(STARTER_TARGET));
    CHECK(document.GetEntityLabel(duplicate) == QStringLiteral("target-marker-copy"));
    CHECK(duplicate.value(QStringLiteral("name")) == QJsonValue(QStringLiteral("Target Marker copy")));
    CHECK(Property(duplicate, QStringLiteral("behavior"), QStringLiteral("target")) == QJsonValue(copy));
    REQUIRE(document.Undo());
    REQUIRE(document.Undo());
    REQUIRE(document.DuplicateEntity(entropy, error, QLatin1String(STARTER_TARGET), copy));
    CHECK(Property(document.Entity(copy), QStringLiteral("behavior"), QStringLiteral("target")) ==
          QJsonValue(QLatin1String(STARTER_CUBE)));

    // Rename changes only the display name.
    REQUIRE(document.RenameEntity(QLatin1String(STARTER_CUBE), error, QStringLiteral("Hero Cube")));
    const QJsonObject renamed = document.Entity(QLatin1String(STARTER_CUBE));
    CHECK(renamed.value(QStringLiteral("name")) == QJsonValue(QStringLiteral("Hero Cube")));
    CHECK(document.GetEntityLabel(renamed) == QStringLiteral("starter.cube"));
    CHECK(document.GetEntityKey(renamed) == QLatin1String(STARTER_CUBE));
    const QJsonObject saved = QJsonDocument::fromJson(document.Encode()).object();
    CHECK(saved.value(QStringLiteral("version")) == QJsonValue(2));
    CHECK(saved.value(QStringLiteral("document")) == QJsonValue(QLatin1String(STARTER_DOCUMENT)));
}
TEST_CASE("Version-1 Worlds keep opening, editing and saving as version 1", "[editor][world][identity]")
{
    WorldDocument document = OpenStarter(StarterVersion1());
    CHECK_FALSE(document.HasUuidRecords());
    const QJsonObject cube = document.Entity(QStringLiteral("starter.cube"));
    CHECK(document.GetEntityKey(cube) == QStringLiteral("starter.cube"));
    CHECK(document.GetEntityLabel(cube) == QStringLiteral("starter.cube"));
    QString error;
    QString key;
    REQUIRE(document.CreateEntity(fixture::MakeFixedEntropySource(), error, QStringLiteral("Entity"), key));
    CHECK(key == QLatin1String(fixture::FIXED_UUID_TEXT));
    QString copy;
    REQUIRE(document.DuplicateEntity(ludus::platform::GetSystemEntropySource(), error, key, copy));
    REQUIRE(document.RenameEntity(QStringLiteral("starter.cube"), error, QStringLiteral("Renamed")));
    const QJsonObject saved = QJsonDocument::fromJson(document.Encode()).object();
    CHECK(saved.value(QStringLiteral("version")) == QJsonValue(1));
    CHECK(saved.contains(QStringLiteral("document")) == false);
    for (const QJsonValue value : saved.value(QStringLiteral("entities")).toArray())
    {
        const QJsonObject entity = value.toObject();
        CHECK(entity.contains(QStringLiteral("id")));
        CHECK(entity.contains(QStringLiteral("uuid")) == false);
        CHECK(entity.contains(QStringLiteral("label")) == false);
    }
}
TEST_CASE("World workspace saves version-2 and version-1 documents in their own versions", "[editor][world][identity]")
{
    for (const bool hasUuidRecords : {true, false})
    {
        QTemporaryDir directory;
        const QString path =
            WriteStarterProject(directory, hasUuidRecords == true ? StarterVersion2() : StarterVersion1());
        WorldWorkspace view;
        view.SetProject(directory.path(), 1);
        fixture::SequentialEntropy sequence;
        view.SetEntropySource(fixture::MakeSequentialEntropySource(sequence));
        REQUIRE(view.Open(path));
        CHECK(view.Document().HasUuidRecords() == hasUuidRecords);
        const QString target =
            hasUuidRecords == true ? QLatin1String(STARTER_TARGET) : QStringLiteral("starter.target");
        Select(view, target);
        QComboBox* reference = view.findChild<QComboBox*>(QStringLiteral("worldField:behavior/target"));
        REQUIRE(reference != nullptr);
        CHECK(reference->currentData().toString() ==
              (hasUuidRecords == true ? QLatin1String(STARTER_CUBE) : QStringLiteral("starter.cube")));
        view.findChild<QPushButton*>(QStringLiteral("worldAdd"))->click();
        Select(view, target);
        view.findChild<QPushButton*>(QStringLiteral("worldDuplicate"))->click();
        REQUIRE(view.Save());

        const QJsonObject saved = QJsonDocument::fromJson(Read(path)).object();
        const QJsonArray entities = saved.value(QStringLiteral("entities")).toArray();
        REQUIRE(entities.size() == 4);
        CHECK(saved.value(QStringLiteral("version")) == QJsonValue(hasUuidRecords == true ? 2 : 1));
        CHECK(saved.contains(QStringLiteral("document")) == hasUuidRecords);
        if (hasUuidRecords == true)
        {
            CHECK(saved.value(QStringLiteral("document")) == QJsonValue(QLatin1String(STARTER_DOCUMENT)));
            CHECK(entities[2].toObject().value(QStringLiteral("label")) == QJsonValue(QStringLiteral("entity")));
            CHECK(entities[3].toObject().value(QStringLiteral("label")) ==
                  QJsonValue(QStringLiteral("target-marker-copy")));
        }
        for (const QJsonValue value : entities)
        {
            CHECK(value.toObject().contains(QStringLiteral("uuid")) == hasUuidRecords);
            CHECK(value.toObject().contains(QStringLiteral("id")) == (hasUuidRecords == false));
        }
        WorldWorkspace reopened;
        reopened.SetProject(directory.path(), 1);
        REQUIRE(reopened.Open(path));
        CHECK(reopened.Document().Draft() == view.Document().Draft());
    }
}
TEST_CASE("World demo opens, edits and saves the committed manifest's version-2 room",
          "[editor][world][world-demo][identity]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path =
        QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(QStringLiteral("first-room.world.json"));
    Write(path, FirstRoomVersion2());
    WorldWorkspace view;
    view.SetProject(directory.path(), 1);
    fixture::SequentialEntropy sequence;
    view.SetEntropySource(fixture::MakeSequentialEntropySource(sequence));
    REQUIRE(view.Open(path));
    REQUIRE(IsWorldDemo(view.Document()));
    REQUIRE(view.Document().HasUuidRecords());

    // The runtime still names recipes by label, and enemy targets resolve through UUIDs.
    ludus::world_demo::Level version2;
    QString error;
    REQUIRE(ProjectWorldDemo(view.Document(), version2, error));
    ludus::world_demo::Level version1;
    const QByteArray legacy = ReadSource("apps/world_demo/levels/first-room.world.json");
    REQUIRE(ludus::world_demo::ReadWorldLevel(View(legacy), version1).Error == ludus::world_demo::LevelError::None);
    CHECK(version2.EntityCount == version1.EntityCount);
    CHECK(version2.Document.Value != ludus::foundation::identity::Uuid{});

    const QString player = FirstRoomUuid(QStringLiteral("player-start"));
    Select(view, player);
    CHECK(view.Document().FindEntityKey(QStringLiteral("player-start")) == player);
    CHECK(view.Document().FindEntityKey(QStringLiteral("missing")).isEmpty());
    QLineEdit* x = view.findChild<QLineEdit*>(QStringLiteral("worldField:transform/x"));
    REQUIRE(x != nullptr);
    x->setFocus();
    x->selectAll();
    QTest::keyClicks(x, QStringLiteral("0.5"));
    REQUIRE(view.Apply());

    // The World viewport's objects carry the UUID key of the entity whose label names the recipe.
    RenderScene scene;
    REQUIRE(RenderScene::Load(directory.path(), view.Document(), scene, error));
    bool hasPlayer = false;
    for (const PreviewObject& object : scene.Objects)
    {
        if (object.Name == QStringLiteral("player-start"))
        {
            hasPlayer = true;
            CHECK(object.Id == player);
            CHECK(object.Transform.Translation.X == .5F);
        }
    }
    CHECK(hasPlayer);

    const QString guard = FirstRoomUuid(QStringLiteral("guard-a"));
    Select(view, guard);
    view.findChild<QPushButton*>(QStringLiteral("worldDuplicate"))->click();
    REQUIRE(view.Save());

    const QJsonObject saved = QJsonDocument::fromJson(Read(path)).object();
    CHECK(saved.value(QStringLiteral("version")) == QJsonValue(2));
    const QJsonArray entities = saved.value(QStringLiteral("entities")).toArray();
    const QJsonObject copy = entities.last().toObject();
    CHECK(copy.value(QStringLiteral("label")) == QJsonValue(QStringLiteral("guard-a-copy")));
    CHECK(copy.value(QStringLiteral("uuid")) != QJsonValue(guard));
    CHECK(Property(copy, QStringLiteral("enemy"), QStringLiteral("target")) == QJsonValue(player));
    ludus::world_demo::Level runtime;
    const QByteArray bytes = Read(path);
    REQUIRE(ludus::world_demo::ReadWorldLevel(View(bytes), runtime).Error == ludus::world_demo::LevelError::None);
    CHECK(runtime.EntityCount == version1.EntityCount + 1);
    CHECK(runtime.Entities[0].Position.X == .5F);
}
