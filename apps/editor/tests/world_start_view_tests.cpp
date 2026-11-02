#include "internal/level_workspace.h"
#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <QAction>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;
namespace math = ludus::foundation::math;

constexpr int32 SETTLE_TIMEOUT_MILLISECONDS = 10000;
/// Largest distance, in world units, between a camera position and the start view it was set from.
constexpr float64 POSITION_TOLERANCE = 0.0001;

void WriteFixtureFile(const QString& path, const QByteArray& bytes)
{
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()) == true);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
}

void WaitForWorld(WorldWorkspace& inoutWorkspace)
{
    REQUIRE(QTest::qWaitFor([&inoutWorkspace]() { return inoutWorkspace.CanSave() == true; },
                            SETTLE_TIMEOUT_MILLISECONDS) == true);
}

PreviewStartView GetStoredStartView(const WorldWorkspace& workspace)
{
    PreviewStartView view;
    REQUIRE(TryParseStartView(workspace.Document().SceneLevel().value(QStringLiteral("camera")).toObject(), view) ==
            true);
    return view;
}
} // namespace

TEST_CASE("An imported pbrt camera becomes the World start view and Set start view stores the viewport",
          "[editor][world][start-view]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString projectPath = QDir(directory.path()).filePath(QStringLiteral("project"));
    REQUIRE(QDir().mkpath(projectPath) == true);
    const QString root = QFileInfo(projectPath).canonicalFilePath();
    const QString source = directory.filePath(QStringLiteral("room/scene.pbrt"));
    WriteFixtureFile(source,
                     QByteArrayLiteral("LookAt 0 2 4  0 0 0  0 1 0\n"
                                       "Camera \"perspective\" \"float fov\" [45]\nWorldBegin\n"
                                       "LightSource \"point\" \"rgb I\" [1 1 1] \"point3 from\" [0 2 0]\n"
                                       "Material \"diffuse\" \"rgb reflectance\" [.6 .5 .4]\n"
                                       "Shape \"trianglemesh\" \"point3 P\" [-1 0 -1  1 0 -1  0 0 1]\n"
                                       "\"integer indices\" [0 1 2]\n"));
    WriteFixtureFile(directory.filePath(QStringLiteral("room/LICENSE.txt")),
                     QByteArrayLiteral("CC0 synthetic fixture\n"));

    ToolingPaths tools;
    tools.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    WorldWorkspace workspace(tools, nullptr);
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("room.world.json"))) == true);
    REQUIRE(workspace.ImportScene(source) == true);
    WaitForWorld(workspace);
    LevelWorkspace* levelOrNull = workspace.findChild<LevelWorkspace*>();
    REQUIRE(levelOrNull != nullptr);
    RenderPreview& preview = levelOrNull->GetRenderPreview();
    INFO(levelOrNull->StatusText().toStdString());

    // The import stored the pbrt camera in the level and the viewport opened from it.
    const PreviewStartView imported = GetStoredStartView(workspace);
    REQUIRE(preview.HasStartView() == true);
    CHECK(math::Distance(imported.EyeWorld, imported.TargetWorld) > 1);
    CHECK(math::Distance(preview.GetVisibleCameraSample().Pose.Position, imported.EyeWorld) ==
          Catch::Approx(0).margin(POSITION_TOLERANCE));
    CHECK(preview.GetVisibleCameraSample().Lens.VerticalFov == Catch::Approx(imported.VerticalFov));

    // Navigate elsewhere, then store that view through the scene viewport's View menu action.
    preview.Fly({1, 0, 0}, 1, 30);
    PreviewStartView described;
    REQUIRE(preview.TryDescribeStartView(described) == true);
    REQUIRE(math::Distance(described.EyeWorld, imported.EyeWorld) > 1);
    QAction& saveStartView = levelOrNull->GetViewport().GetStartViewSaveAction();
    CHECK(saveStartView.isVisible() == true);
    REQUIRE(saveStartView.isEnabled() == true);
    saveStartView.trigger();
    WaitForWorld(workspace);
    const PreviewStartView stored = GetStoredStartView(workspace);
    CHECK(math::Distance(stored.EyeWorld, described.EyeWorld) == Catch::Approx(0).margin(POSITION_TOLERANCE));
    CHECK(math::Distance(stored.TargetWorld, described.TargetWorld) == Catch::Approx(0).margin(POSITION_TOLERANCE));
    CHECK(workspace.Dirty() == true);
    CHECK(workspace.CanUndo() == true);

    // The viewport picks up the new start view once the World has reassembled.
    preview.Fly({0, 0, 1}, 1, 30);
    REQUIRE(preview.GoToStartView() == true);
    CHECK(math::Distance(preview.GetVisibleCameraSample().Pose.Position, described.EyeWorld) ==
          Catch::Approx(0).margin(POSITION_TOLERANCE));
    workspace.StopPreview();
}
