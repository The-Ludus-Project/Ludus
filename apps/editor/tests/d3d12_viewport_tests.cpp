#include "internal/preview_shading.h"
#include "internal/preview_viewport.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/renderer_workspace.h"
#include "internal/world_document.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSize>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
constexpr int32 WIDTH = 640;
constexpr int32 HEIGHT = 480;
constexpr int32 FRAME_TIMEOUT_MILLISECONDS = 30000;
constexpr int32 POLL_MILLISECONDS = 10;

[[nodiscard]] QByteArray ReadFixture(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

[[nodiscard]] QQuickWindow* FindGpuWindowOrNull()
{
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport"))
        {
            return qobject_cast<QQuickWindow*>(window);
        }
    }
    return nullptr;
}

template <typename Predicate>
[[nodiscard]] bool WaitForPresentation(Predicate isReady)
{
    QElapsedTimer timer;
    timer.start();
    while (isReady() == false && timer.elapsed() < FRAME_TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    return isReady();
}

void MoveCube(WorldDocument& inoutWorld)
{
    QJsonObject draft = inoutWorld.Draft();
    QJsonArray entities = draft.value(QStringLiteral("entities")).toArray();
    for (qsizetype index = 0; index < entities.size(); ++index)
    {
        QJsonObject entity = entities.at(index).toObject();
        if (entity.value(QStringLiteral("id")).toString() != QStringLiteral("starter.cube"))
        {
            continue;
        }
        QJsonObject components = entity.value(QStringLiteral("components")).toObject();
        QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
        transform.insert(QStringLiteral("x"), -1);
        components.insert(QStringLiteral("transform"), transform);
        entity.insert(QStringLiteral("components"), components);
        entities.replace(index, entity);
        draft.insert(QStringLiteral("entities"), entities);
        QString error;
        REQUIRE(inoutWorld.Load(QJsonDocument(draft).toJson(), error) == true);
        return;
    }
    FAIL("Missing starter.cube in the authored World fixture");
}
} // namespace

TEST_CASE("Windows viewport presents the authored World and retains its frame through graph replacement",
          "[.][windows-native-viewport]")
{
    REQUIRE(QGuiApplication::platformName() == QStringLiteral("windows"));
    REQUIRE(PreviewViewport::GetPresentation() == ViewportPresentation::SharedDevice);

    QTemporaryDir directory(QDir::temp().filePath(QStringLiteral("Ludus Windows viewport-XXXXXX")));
    REQUIRE(directory.isValid() == true);
    const QString root = directory.path();
    REQUIRE(QDir(root).mkpath(QStringLiteral("authoring")) == true);
    for (const QString& name : {QStringLiteral("world.schema.json"),
                                QStringLiteral("rendering.world.json"),
                                QStringLiteral("render.preview.json"),
                                QStringLiteral("cube.mesh.json"),
                                QStringLiteral("ludus.png")})
    {
        const QByteArray bytes = ReadFixture(QStringLiteral(":/samples/live-edit-game/authoring/") + name);
        QFile file(QDir(root).filePath(QStringLiteral("authoring/") + name));
        REQUIRE(file.open(QIODevice::WriteOnly) == true);
        REQUIRE(file.write(bytes) == bytes.size());
    }
    WorldDocument world;
    QString error;
    REQUIRE(world.LoadSchema(ReadFixture(QDir(root).filePath(QStringLiteral("authoring/world.schema.json"))), error) ==
            true);
    REQUIRE(world.Load(ReadFixture(QDir(root).filePath(QStringLiteral("authoring/rendering.world.json"))), error) ==
            true);
    RenderScene scene;
    REQUIRE(RenderScene::Load(root, world, scene, error) == true);

    RendererWorkspace workspace({});
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(WIDTH, HEIGHT);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport) == true);
    REQUIRE(preview.SetScene(scene) == true);
    preview.SetShading(PreviewShading::Solid);
    REQUIRE(preview.Start() == true);
    REQUIRE(WaitForPresentation([&viewport]() { return viewport.PresentedFrames() > 0; }) == true);

    QQuickWindow* surfaceOrNull = FindGpuWindowOrNull();
    REQUIRE(surfaceOrNull != nullptr);
    CHECK(surfaceOrNull->rendererInterface()->graphicsApi() == QSGRendererInterface::Direct3D12);
    const QImage original = surfaceOrNull->grabWindow();
    REQUIRE(original.isNull() == false);

    uint64 previousFrame = viewport.PresentedFrames();
    preview.Navigate(0, {}, 2, false);
    REQUIRE(WaitForPresentation([&viewport, previousFrame]() { return viewport.PresentedFrames() > previousFrame; }) ==
            true);
    QImage navigated;
    REQUIRE(WaitForPresentation([surfaceOrNull, &original, &navigated]() {
                navigated = surfaceOrNull->grabWindow();
                return navigated.isNull() == false && navigated != original;
            }) == true);

    MoveCube(world);
    REQUIRE(RenderScene::Load(root, world, scene, error) == true);
    previousFrame = viewport.PresentedFrames();
    REQUIRE(preview.SetScene(scene) == true);
    REQUIRE(WaitForPresentation([surfaceOrNull, &viewport, previousFrame, &navigated]() {
                if (viewport.PresentedFrames() <= previousFrame)
                {
                    return false;
                }
                const QImage edited = surfaceOrNull->grabWindow();
                return edited.isNull() == false && edited != navigated;
            }) == true);

    previousFrame = viewport.PresentedFrames();
    const QSize beforeResize = surfaceOrNull->size();
    viewport.resize(WIDTH / 2, HEIGHT / 2);
    REQUIRE(WaitForPresentation([surfaceOrNull, &viewport, previousFrame, beforeResize]() {
                return viewport.PresentedFrames() > previousFrame && surfaceOrNull->size() != beforeResize;
            }) == true);

    workspace.SetPreviewSource(&preview);
    workspace.SetProject(root, 1);
    workspace.EnableWorldRendering();
    workspace.SetLivePreview(false);
    viewport.UseProjectGraph(workspace);
    RenderGraphPreview* graphOrNull = workspace.GetPreviewOrNull();
    REQUIRE(graphOrNull != nullptr);

    const QString fixture = QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE);
    const RenderGraphPreviewLoad initial = graphOrNull->Load(fixture);
    REQUIRE((initial == RenderGraphPreviewLoad::Staged || initial == RenderGraphPreviewLoad::Waiting));
    REQUIRE(WaitForPresentation([graphOrNull, &viewport]() {
                return graphOrNull->GetPresentedGeneration() != 0 && viewport.GetPresentedGraphFrameSerial() != 0;
            }) == true);
    const uint64 generation = graphOrNull->GetPresentedGeneration();
    CHECK(viewport.HasRetainedGraphFrame() == true);

    CHECK(graphOrNull->Load(QDir(root).filePath(QStringLiteral("missing.lurgrt"))) ==
          RenderGraphPreviewLoad::Unreadable);
    CHECK(graphOrNull->GetPresentedGeneration() == generation);
    CHECK(viewport.HasRetainedGraphFrame() == true);
    REQUIRE(graphOrNull->Load(fixture) == RenderGraphPreviewLoad::Staged);
    CHECK(graphOrNull->GetPresentedGeneration() == generation);
    CHECK(viewport.HasRetainedGraphFrame() == true);
    const uint64 previousSerial = viewport.GetPresentedGraphFrameSerial();
    REQUIRE(WaitForPresentation([graphOrNull, &viewport, generation, previousSerial]() {
                return graphOrNull->GetPresentedGeneration() > generation &&
                       viewport.GetPresentedGraphFrameSerial() > previousSerial;
            }) == true);
    CHECK(viewport.HasRetainedGraphFrame() == true);
    preview.Stop();
    CHECK(FindGpuWindowOrNull() == nullptr);
}
