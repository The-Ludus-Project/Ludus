#include "entropy_fixture.h"
#include "internal/builtin_world.h"
#include "internal/level_workspace.h"
#include "internal/render_scene.h"
#include "internal/world_aperture.h"
#include "internal/world_document.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <catch2/catch_test_macros.hpp>

namespace ludus::editor
{
namespace
{
namespace math = foundation::math;
using foundation::float32;
using foundation::int32;
using foundation::usize;

/// Positions agree after a float32 Euler round trip through the transform component to within this, in metres.
constexpr float32 PLACEMENT_TOLERANCE_METRES = 1e-5F;
/// Imports and reassemblies of the small aperture scene finish well within this.
constexpr int32 SCENE_TIMEOUT_MILLISECONDS = 30000;

WorldDocument MakeDefaultWorld()
{
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument world;
    REQUIRE(world.LoadSchema(schema, error) == true);
    REQUIRE(world.New(error) == true);
    return world;
}

QByteArray ReadFile(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

QByteArray ReadSingleSlit()
{
    return ReadFile(QDir(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT))
                        .filePath(QStringLiteral("tests/scenes/diffraction/single-slit.luscene.json")));
}

/// The largest |x| of a mesh vertex lying inside the slit's length: half the opening.
float32 GetSlitHalfOpening(const PreviewMesh& mesh)
{
    float32 halfOpening = 1e9F;
    for (const math::Vector3& position : mesh.Positions)
    {
        halfOpening = math::Min(halfOpening, math::Abs(position.X));
    }
    return halfOpening;
}

/// Wait until no scene operation or placement cook runs, so the World accepts edits again.
void WaitForSettledScene(const WorldWorkspace& workspace, const LevelWorkspace& level)
{
    QElapsedTimer timer;
    timer.start();
    const auto isSettled = [&workspace, &level]() {
        return workspace.CanSave() == true && level.CanStartOperation() == true &&
               level.GetPlacementValidation() == PlacementValidation::Ready;
    };
    while (isSettled() == false && timer.elapsed() < SCENE_TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(10);
    }
    REQUIRE(isSettled() == true);
}

QJsonObject GetAperture(const WorldDocument& world, const QString& key)
{
    return world.Entity(key).value(QStringLiteral("components")).toObject().value(GetApertureComponentId()).toObject();
}
} // namespace

TEST_CASE("Aperture Slit creates a parameterized screen that extraction regenerates", "[editor][world][aperture]")
{
    WorldDocument world = MakeDefaultWorld();
    REQUIRE(world.HasBuiltInAperture() == true);
    QString key;
    QString error;
    INFO(error.toStdString());
    REQUIRE(CreateWorldObject(world, tests::MakeFixedEntropySource(), QStringLiteral("Aperture Slit"), key, error) ==
            true);
    QJsonObject aperture = GetAperture(world, key);
    CHECK(aperture.value(QStringLiteral("opening")).toDouble() == 0.05);
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    REQUIRE(RenderScene::Load(root.path(), world, scene, error, false) == true);
    REQUIRE(scene.Objects.size() == 1);
    // Beside, above and below the slit: four quads of two triangles.
    const PreviewMesh& mesh = scene.Meshes[scene.Objects[0].Mesh];
    CHECK(mesh.Positions.size() == 16);
    CHECK(mesh.Indices.size() == 24);
    CHECK(GetSlitHalfOpening(mesh) == 0.025F);
    for (const math::Vector3& normal : mesh.Normals)
    {
        CHECK(normal == math::Vector3{0, 0, 1});
    }
    // The default screen absorbs, like the knife-edge screen of the diffraction scene.
    CHECK(scene.Objects[0].Tint == math::Vector4{0, 0, 0, 1});

    // Changing the opening in Properties is a component edit; the next extraction regenerates the screen.
    const QJsonObject entity = world.Entity(key);
    QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    aperture.insert(QStringLiteral("opening"), 0.2);
    components.insert(GetApertureComponentId(), aperture);
    REQUIRE(world.Edit(key, components, entity.value(QStringLiteral("name")).toString(), error) == true);
    RenderScene widened;
    REQUIRE(RenderScene::Load(root.path(), world, widened, error, false) == true);
    CHECK(GetSlitHalfOpening(widened.Meshes[widened.Objects[0].Mesh]) == 0.1F);
    CHECK(widened.AssetsIdentity != scene.AssetsIdentity);

    // A closed slit leaves the two halves of the screen.
    aperture.insert(QStringLiteral("opening"), 0);
    components.insert(GetApertureComponentId(), aperture);
    REQUIRE(world.Edit(key, components, entity.value(QStringLiteral("name")).toString(), error) == true);
    RenderScene closed;
    REQUIRE(RenderScene::Load(root.path(), world, closed, error, false) == true);
    CHECK(closed.Meshes[closed.Objects[0].Mesh].Positions.size() == 8);
}

TEST_CASE("Aperture edits that cannot form a screen are refused with the field to change", "[editor][world][aperture]")
{
    WorldDocument world = MakeDefaultWorld();
    QString key;
    QString error;
    REQUIRE(CreateWorldObject(world, tests::MakeFixedEntropySource(), QStringLiteral("Aperture Slit"), key, error) ==
            true);
    const QJsonObject applied = world.Draft();
    const QJsonObject entity = world.Entity(key);
    const QString name = entity.value(QStringLiteral("name")).toString();
    QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    QJsonObject aperture = components.value(GetApertureComponentId()).toObject();

    aperture.insert(QStringLiteral("opening"), 1);
    components.insert(GetApertureComponentId(), aperture);
    CHECK(world.Edit(key, components, name, error) == false);
    CHECK(error.contains(QStringLiteral("Opening")) == true);
    CHECK(error.contains(QStringLiteral("Screen width")) == true);
    CHECK(world.Draft() == applied);

    aperture.insert(QStringLiteral("opening"), 0.05);
    aperture.insert(QStringLiteral("slitLength"), 2);
    components.insert(GetApertureComponentId(), aperture);
    CHECK(world.Edit(key, components, name, error) == false);
    CHECK(error.contains(QStringLiteral("Slit length")) == true);
    CHECK(world.Draft() == applied);

    // The aperture draws its own screen; a Renderable on the same entity is refused with the repair.
    REQUIRE(world.Attach(key, error, QStringLiteral("renderable")) == false);
    CHECK(error.contains(QStringLiteral("Aperture Slit")) == true);
    CHECK(world.Draft() == applied);
}

TEST_CASE("Scene source apertures become World entities that keep their frame and sizes", "[editor][world][aperture]")
{
    WorldDocument world = MakeDefaultWorld();
    ImportedApertures apertures;
    QString error;
    INFO(error.toStdString());
    REQUIRE(MakeImportedApertures(ReadSingleSlit(),
                                  world,
                                  QStringLiteral("level"),
                                  QStringLiteral("single-slit.luscene.json"),
                                  apertures,
                                  error) == true);
    REQUIRE(apertures.Entities.size() == 1);
    CHECK(apertures.Warnings.isEmpty() == true);
    const QJsonObject entity = apertures.Entities[0].toObject();
    CHECK(entity.value(QStringLiteral("name")).toString() == QStringLiteral("Slit screen"));
    const QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    const QJsonObject aperture = components.value(GetApertureComponentId()).toObject();
    CHECK(aperture.value(QStringLiteral("opening")).toDouble() == 0.05);
    CHECK(aperture.value(QStringLiteral("slitLength")).toDouble() == 0.8);
    CHECK(aperture.value(QStringLiteral("screenWidth")).toDouble() == 3);
    CHECK(aperture.value(QStringLiteral("r")).toDouble() == 0);
    const QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
    for (const QString& member :
         {QStringLiteral("x"), QStringLiteral("rx"), QStringLiteral("ry"), QStringLiteral("rz")})
    {
        CHECK(transform.value(member).toDouble() == 0);
    }
    // The ID depends only on the level, the source file and the object, so a reimport finds the entity.
    ImportedApertures again;
    REQUIRE(MakeImportedApertures(ReadSingleSlit(),
                                  world,
                                  QStringLiteral("level"),
                                  QStringLiteral("single-slit.luscene.json"),
                                  again,
                                  error) == true);
    CHECK(again.Entities == apertures.Entities);

    // A tilted frame: facing +X, with the slit along +Z, centred at (1, 2, 3).
    const QByteArray tilted = R"({"format": "ludus.scene", "version": 1, "name": "Tilted",
        "materials": [{"name": "Grey", "model": "metallic_roughness", "base_color": [0.5, 0.5, 0.5]}],
        "objects": [{"name": "Gate", "material": "Grey",
                     "aperture": {"center": [1, 2, 3], "normal": [1, 0, 0], "up": [0, 0, 1], "screen_width": 2,
                                  "screen_height": 1, "slit_length": 0.5, "opening": 0.1}}]})";
    REQUIRE(MakeImportedApertures(tilted,
                                  world,
                                  QStringLiteral("level"),
                                  QStringLiteral("tilted.luscene.json"),
                                  apertures,
                                  error) == true);
    REQUIRE(apertures.Entities.size() == 1);
    CHECK(apertures.Warnings.size() == 1); // metallic_roughness reflects as Lambertian.
    const QJsonObject gate = apertures.Entities[0].toObject();
    QString key;
    REQUIRE(world.CreateEntity(tests::MakeFixedEntropySource(),
                               error,
                               gate.value(QStringLiteral("name")).toString(),
                               key,
                               gate.value(QStringLiteral("components")).toObject()) == true);
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    REQUIRE(RenderScene::Load(root.path(), world, scene, error, false) == true);
    REQUIRE(scene.Objects.size() == 1);
    const PreviewObject& object = scene.Objects[0];
    const PreviewMesh& mesh = scene.Meshes[object.Mesh];
    // The first corner of the left quad is (-1, -0.5) in the frame: across = Cross(up, normal) = +Y.
    const math::Vector3 cornerWorld = math::TransformPoint(object.Transform, mesh.Positions[0]);
    CHECK(math::Abs(cornerWorld.X - 1) < PLACEMENT_TOLERANCE_METRES);
    CHECK(math::Abs(cornerWorld.Y - 1) < PLACEMENT_TOLERANCE_METRES);
    CHECK(math::Abs(cornerWorld.Z - 2.5F) < PLACEMENT_TOLERANCE_METRES);
    // The screen's front face, the frame's +Z, looks along the source normal.
    const math::Vector3 normalWorld = object.Transform.Columns[2];
    CHECK(math::Abs(normalWorld.X - 1) < PLACEMENT_TOLERANCE_METRES);
}

TEST_CASE("Importing the single slit makes its opening editable in the World", "[editor][world][aperture]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString sourceDirectory = QDir(root).filePath(QStringLiteral("source"));
    REQUIRE(QDir().mkpath(sourceDirectory) == true);
    const QString source = QDir(sourceDirectory).filePath(QStringLiteral("single-slit.luscene.json"));
    {
        QFile file(source);
        REQUIRE(file.open(QIODevice::WriteOnly) == true);
        const QByteArray bytes = ReadSingleSlit();
        REQUIRE(file.write(bytes) == bytes.size());
    }
    ToolingPaths tooling;
    tooling.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    // The import mints one UUID per cooked object, so it needs real entropy rather than the fixed fixture.
    WorldWorkspace workspace(tooling, nullptr);
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("slit.world.json"))) == true);
    LevelWorkspace* levelOrNull = workspace.findChild<LevelWorkspace*>();
    REQUIRE(levelOrNull != nullptr);
    QSignalSpy finished(levelOrNull, &LevelWorkspace::OperationFinished);
    REQUIRE(workspace.ImportScene(source) == true);
    REQUIRE(finished.wait(SCENE_TIMEOUT_MILLISECONDS) == true);
    INFO(levelOrNull->StatusText().toStdString());
    REQUIRE(finished.last().first().toBool() == true);

    // The wall and light are cooked; the screen is an aperture entity, not cooked geometry.
    QString key;
    for (const QJsonValue value : workspace.Document().Draft().value(QStringLiteral("entities")).toArray())
    {
        const QJsonObject entity = value.toObject();
        if (entity.value(QStringLiteral("components")).toObject().contains(GetApertureComponentId()) == true)
        {
            key = workspace.Document().GetEntityKey(entity);
        }
    }
    REQUIRE(key.isEmpty() == false);
    CHECK(workspace.Document().Entity(key).contains(QStringLiteral("scene")) == false);
    const auto getHalfOpening = [&levelOrNull, &key]() {
        const RenderScene& scene = levelOrNull->PreviewScene();
        for (const PreviewObject& object : scene.Objects)
        {
            if (object.Id == key)
            {
                const resources::SceneMesh& mesh = scene.Definition->Meshes[static_cast<usize>(object.Mesh)];
                float32 halfOpening = 1e9F;
                for (const math::Vector3& position : mesh.Positions)
                {
                    halfOpening = math::Min(halfOpening, math::Abs(position.X));
                }
                return halfOpening;
            }
        }
        FAIL("The presented scene lacks the aperture");
        return 0.0F;
    };
    CHECK(getHalfOpening() == 0.025F);

    // Widen the opening in Properties and apply it: the composed scene presents the new screen.
    WaitForSettledScene(workspace, *levelOrNull);
    REQUIRE(workspace.SelectEntity(key) == true);
    QLineEdit* openingOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:aperture/opening"));
    REQUIRE(openingOrNull != nullptr);
    openingOrNull->setFocus();
    openingOrNull->selectAll();
    QTest::keyClicks(openingOrNull, QStringLiteral("0.1"));
    REQUIRE(workspace.Apply() == true);
    // Properties parses numbers at float32 precision, like every World number field.
    CHECK(static_cast<float32>(GetAperture(workspace.Document(), key).value(QStringLiteral("opening")).toDouble()) ==
          0.1F);
    QElapsedTimer timer;
    timer.start();
    while (getHalfOpening() != 0.05F && timer.elapsed() < SCENE_TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(10);
    }
    CHECK(getHalfOpening() == 0.05F);
    // Undo restores the applied opening and its screen.
    WaitForSettledScene(workspace, *levelOrNull);
    workspace.Undo();
    CHECK(GetAperture(workspace.Document(), key).value(QStringLiteral("opening")).toDouble() == 0.05);
    timer.restart();
    while (getHalfOpening() != 0.025F && timer.elapsed() < SCENE_TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(10);
    }
    CHECK(getHalfOpening() == 0.025F);
    WaitForSettledScene(workspace, *levelOrNull);
    workspace.StopPreview();
}
} // namespace ludus::editor
