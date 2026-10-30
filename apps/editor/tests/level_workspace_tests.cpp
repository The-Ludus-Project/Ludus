#include "entropy_fixture.h"
#include "internal/editor_identity.h"
#include "internal/level_workspace.h"
#include "internal/preview_viewport.h"
#include "scene_fixture.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMetaObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/renderer_workspace.h"
#    include "internal/world_graph_view.h"
#    include <QQuickWindow>
#endif

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
namespace
{
ToolingPaths SceneTools()
{
    ToolingPaths tools;
    tools.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    return tools;
}
void Drain(LevelWorkspace& workspace)
{
    QElapsedTimer timer;
    timer.start();
    while (workspace.CanSave() == false && timer.elapsed() < 10000)
    {
        QTest::qWait(10);
    }
    REQUIRE(workspace.CanSave());
}
} // namespace
TEST_CASE("3D level workflow saves and opens a fresh scene without an authoring schema", "[editor][level]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = QDir(directory.path()).filePath(QStringLiteral("scenes/test.lulevel"));
    LevelWorkspace workspace(SceneTools());
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Create(path));
    REQUIRE(workspace.Dirty());
    REQUIRE(workspace.Save());
    Drain(workspace);
    REQUIRE_FALSE(workspace.Dirty());
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const auto saved = file.readAll();
    const auto value = QJsonDocument::fromJson(saved).object();
    REQUIRE(value.value(QStringLiteral("objects")).toArray().isEmpty());
    LevelWorkspace reopened(SceneTools());
    reopened.SetProject(directory.path(), 2);
    REQUIRE(reopened.Open(path));
    Drain(reopened);
    REQUIRE(reopened.Document() == value);
    REQUIRE_FALSE(reopened.Dirty());
    REQUIRE_FALSE(QFileInfo::exists(QDir(directory.path()).filePath(QStringLiteral("authoring/world.schema.json"))));
}
TEST_CASE("unsupported scene import and saved-file conflicts retain the active scene", "[editor][level]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = QDir(directory.path()).filePath(QStringLiteral("scene.lulevel"));
    LevelWorkspace workspace(SceneTools());
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Create(path));
    REQUIRE(workspace.Save());
    Drain(workspace);
    const auto original = workspace.Document();
    REQUIRE(workspace.Import(directory.path()));
    Drain(workspace);
    REQUIRE(workspace.Document() == original);
    REQUIRE_FALSE(workspace.Dirty());
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write("external edit") == 13);
    file.close();
    REQUIRE(workspace.Save());
    Drain(workspace);
    REQUIRE(workspace.Document() == original);
    REQUIRE(file.open(QIODevice::ReadOnly));
    REQUIRE(file.readAll() == QByteArray("external edit"));
}
TEST_CASE("new 3D level IDs come from FoundationIdentity and fail without entropy", "[editor][level][identity]")
{
    namespace fixture = ludus::editor::tests;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = QDir(directory.path()).filePath(QStringLiteral("scenes/test.lulevel"));
    LevelWorkspace workspace(SceneTools());
    workspace.SetProject(directory.path(), 1);
    workspace.SetEntropySource(fixture::MakeUnavailableEntropySource());
    CHECK_FALSE(workspace.Create(path));
    CHECK(workspace.StatusText() == DescribeEntropyUnavailable());
    CHECK(workspace.Document().isEmpty());
    CHECK_FALSE(workspace.Dirty());
    workspace.SetEntropySource(fixture::MakeFixedEntropySource());
    REQUIRE(workspace.Create(path));
    CHECK(workspace.Document().value(QStringLiteral("id")) == QJsonValue(QLatin1String(fixture::FIXED_UUID_TEXT)));
    CHECK(workspace.Dirty());
}

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
TEST_CASE("Delegated graph failures finish scene opening and permit a raster retry",
          "[.level-native][editor][level][world-graph]")
{
    using namespace ludus::foundation;
    constexpr int32 TIMEOUT_MILLISECONDS = 30000;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QDir(directory.path()).filePath(QStringLiteral("project"));
    const QString source = QDir(directory.path()).filePath(QStringLiteral("source"));
    const QString relative = QStringLiteral("scenes/main.lulevel");
    REQUIRE(ludus::editor::tests::CreateFixtureSource(source) == true);
    QString error;
    REQUIRE(ludus::editor::tests::PrepareImportedLevel(root, relative, source, error) == true);
    RendererWorkspace renderer({});
    renderer.SetProject(root, 1);
    LevelWorkspace scene(SceneTools());
    scene.SetProject(root, 1);
    PreviewViewport& viewport = scene.GetViewport();
    viewport.UseProjectGraph(renderer);
    WorldGraphView* graphViewOrNull = viewport.findChild<WorldGraphView*>();
    REQUIRE(graphViewOrNull != nullptr);
    renderer.SetPreviewSource(&scene.GetRenderPreview());
    scene.resize(1000, 700);
    scene.show();
    const QString problem = QStringLiteral("RenderGraphSchema: missing required output flag");
    QSignalSpy finished(&scene, &LevelWorkspace::OperationFinished);
    const auto waitFor = [&](const auto& isReady) {
        QElapsedTimer timer;
        timer.start();
        while (isReady() == false && timer.elapsed() < TIMEOUT_MILLISECONDS)
        {
            QTest::qWait(1);
            renderer.ServicePreview();
        }
        return isReady();
    };
    QImage previousImage;
    QJsonObject previousDocument;
    QMetaObject::Connection failureConnection;
    SECTION("failure before the candidate starts")
    {
        scene.GetRenderPreview().SetProjectGraphProblem(problem);
    }
    SECTION("failure while waiting for the first frame")
    {
        failureConnection = QObject::connect(&scene.GetRenderPreview(), &RenderPreview::DeviceRequested, &scene, [&]() {
            QTimer::singleShot(0, &scene, [&]() { scene.GetRenderPreview().SetProjectGraphProblem(problem); });
        });
    }
    SECTION("failure retains the last presented scene")
    {
        REQUIRE(renderer.BuildAndPreview() == true);
        REQUIRE(scene.Open(QDir(root).filePath(relative)) == true);
        REQUIRE(waitFor([&]() { return scene.Busy() == false; }) == true);
        REQUIRE(scene.GetRenderPreview().Frames() != 0);
        previousImage = graphViewOrNull->GetImage();
        REQUIRE(previousImage.isNull() == false);
        previousDocument = scene.Document();
        QJsonObject replacement = previousDocument;
        QJsonObject camera = replacement.value(QStringLiteral("camera")).toObject();
        camera.insert(QStringLiteral("fov"), 0.5);
        replacement.insert(QStringLiteral("camera"), camera);
        QFile file(QDir(root).filePath(relative));
        REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
        const QByteArray bytes = QJsonDocument(replacement).toJson();
        REQUIRE(file.write(bytes) == bytes.size());
        file.close();
        scene.GetRenderPreview().SetProjectGraphProblem(problem);
        finished.clear();
    }
    REQUIRE(scene.Open(QDir(root).filePath(relative)) == true);
    REQUIRE(waitFor([&]() { return scene.Busy() == false; }) == true);
    REQUIRE(finished.size() == 1);
    CHECK(finished.at(0).at(0).toBool() == false);
    CHECK(scene.StatusText().contains(problem) == true);
    CHECK(scene.Document() == previousDocument);
    if (previousImage.isNull() == false)
    {
        // WorldWorkspace forwards the failed operation's diagnostic through this production path.
        scene.ShowSceneFailure(problem);
        CHECK(graphViewOrNull->GetImage() == previousImage);
    }
    QObject::disconnect(failureConnection);
    scene.GetRenderPreview().SetProjectGraphProblem({});
    REQUIRE(renderer.BuildAndPreview() == true);
    finished.clear();
    REQUIRE(scene.Open(QDir(root).filePath(relative)) == true);
    REQUIRE(waitFor([&]() { return scene.Busy() == false; }) == true);
    REQUIRE(finished.size() == 1);
    CHECK(finished.at(0).at(0).toBool() == true);
    CHECK(scene.GetRenderPreview().Frames() != 0);
    CHECK(graphViewOrNull->GetImage().isNull() == false);
    scene.StopPreview();
    renderer.SetPreviewSource(nullptr);
}

TEST_CASE("saved cooked level renders after moving the project and removing source inputs", "[.level-native]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto root = QDir(directory.path()).filePath(QStringLiteral("project"));
    const auto source = QDir(directory.path()).filePath(QStringLiteral("external"));
    const auto tools = SceneTools();
    REQUIRE(ludus::editor::tests::CreateFixtureSource(source));
    QString error;
    INFO(error.toStdString());
    REQUIRE(ludus::editor::tests::PrepareImportedLevel(root, QStringLiteral("scenes/main.lulevel"), source, error));
    REQUIRE(QDir(source).removeRecursively());
    const auto moved = QDir(directory.path()).filePath(QStringLiteral("moved"));
    REQUIRE(QDir().rename(root, moved));
    LevelWorkspace workspace(tools);
    workspace.resize(1000, 700);
    workspace.show();
    workspace.SetProject(moved, 1);
    REQUIRE(workspace.Open(QDir(moved).filePath(QStringLiteral("scenes/main.lulevel"))));
    Drain(workspace);
    REQUIRE(workspace.Document().value(QStringLiteral("objects")).toArray().size() == 1);
    QWidget* viewport = workspace.findChild<QWidget*>(QStringLiteral("levelViewport"));
    REQUIRE(viewport != nullptr);
    QTest::qWait(100);
    CHECK(static_cast<PreviewViewport*>(viewport)->PresentedFrames() > 0);
    QImage image;
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport"))
        {
            image = static_cast<QQuickWindow*>(window)->grabWindow();
        }
    }
    REQUIRE_FALSE(image.isNull());
    bool hasEmission = false;
    for (ludus::foundation::int32 y = 0; y < image.height(); ++y)
    {
        for (ludus::foundation::int32 x = 0; x < image.width(); ++x)
        {
            hasEmission = hasEmission || qRed(image.pixel(x, y)) > 0;
        }
    }
    CHECK(hasEmission);
    const QJsonObject document = workspace.Document();
    QComboBox* shading = viewport->findChild<QComboBox*>(QStringLiteral("viewportShading"));
    REQUIRE(shading != nullptr);
    CHECK(shading->currentIndex() == 0);
    const PreviewViewport* gpuViewport = static_cast<PreviewViewport*>(viewport);
    const ludus::foundation::uint64 targetFrames = gpuViewport->PresentedFrames() + 2;
    shading->setCurrentIndex(2);
    QElapsedTimer timer;
    timer.start();
    while (gpuViewport->PresentedFrames() < targetFrames && timer.elapsed() < 10000)
    {
        QTest::qWait(20);
    }
    REQUIRE(gpuViewport->PresentedFrames() >= targetFrames);
    QImage litImage;
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport"))
        {
            litImage = static_cast<QQuickWindow*>(window)->grabWindow();
        }
    }
    REQUIRE_FALSE(litImage.isNull());
    CHECK(litImage != image);
    CHECK(workspace.Document() == document);
    CHECK(workspace.Navigation().value(QStringLiteral("shading")).toString() == QStringLiteral("lit"));
    workspace.StopPreview();
}
#endif

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
TEST_CASE("staircase2 imports, saves and renders from a moved project without its source", "[editor][staircase2]")
{
    const auto reference = qEnvironmentVariable("LUDUS_STAGE5_REFERENCE_SOURCE");
    if (reference.isEmpty())
    {
        SUCCEED("Set LUDUS_STAGE5_REFERENCE_SOURCE to run the supplied-scene acceptance test.");
        return;
    }
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto root = QDir(directory.path()).filePath(QStringLiteral("project"));
    const auto source = QDir(directory.path()).filePath(QStringLiteral("external"));
    REQUIRE(QDir().mkpath(root));
    const auto tools = SceneTools();
    REQUIRE(ludus::editor::tests::CopyTree(reference, source));
    LevelWorkspace workspace(tools);
    workspace.SetProject(root, 1);
    const auto path = QDir(root).filePath(QStringLiteral("scenes/main.lulevel"));
    REQUIRE(workspace.Create(path));
    REQUIRE(workspace.Import(source));
    Drain(workspace);
    REQUIRE(workspace.Dirty());
    const auto original = workspace.Document();
    REQUIRE(original.value(QStringLiteral("objects")).toArray().size() == 21);
    REQUIRE(workspace.Save());
    Drain(workspace);
    REQUIRE_FALSE(workspace.Dirty());
    workspace.StopPreview();
    REQUIRE(QDir(source).removeRecursively());
    const auto moved = QDir(directory.path()).filePath(QStringLiteral("moved"));
    REQUIRE(QDir().rename(root, moved));
    LevelWorkspace reopened(tools);
    reopened.resize(1000, 700);
    reopened.show();
    reopened.SetProject(moved, 2);
    REQUIRE(reopened.Open(QDir(moved).filePath(QStringLiteral("scenes/main.lulevel"))));
    Drain(reopened);
    REQUIRE(reopened.Document() == original);
    QWidget* viewport = reopened.findChild<QWidget*>(QStringLiteral("levelViewport"));
    REQUIRE(viewport != nullptr);
    QTest::qWait(100);
    CHECK(static_cast<PreviewViewport*>(viewport)->PresentedFrames() > 0);
    const QString output = qEnvironmentVariable("LUDUS_STAGE5_REFERENCE_IMAGE");
    if (output.isEmpty() == false)
    {
        for (QWindow* window : QGuiApplication::allWindows())
        {
            if (window->objectName() == QStringLiteral("LudusGpuViewport"))
            {
                REQUIRE(static_cast<QQuickWindow*>(window)->grabWindow().save(output));
            }
        }
    }
    reopened.StopPreview();
}
#endif
