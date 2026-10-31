#include "internal/level_workspace.h"
#include "internal/preview_lighting.h"
#include "internal/render_graph_preview.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace renderer = ludus::graphics::renderer;

constexpr int32 SETTLE_TIMEOUT_MILLISECONDS = 10000;

void WriteFixtureFile(const QString& path, const QByteArray& bytes)
{
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()) == true);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
}

void WriteLightingSource(const QString& path, bool shouldIncludeSecondLight)
{
    QByteArray source = QByteArrayLiteral("LookAt 0 2 4  0 0 0  0 1 0\n"
                                          "Camera \"perspective\" \"float fov\" [45]\nWorldBegin\n");
    if (shouldIncludeSecondLight == true)
    {
        source += QByteArrayLiteral("LightSource \"point\" \"rgb I\" [3 1 2] \"point3 from\" [0 2 0]\n"
                                    "LightSource \"point\" \"rgb I\" [4 2 1] \"point3 from\" [1 2 0]\n");
    }
    else
    {
        source += QByteArrayLiteral("LightSource \"point\" \"rgb I\" [1 2 3] \"point3 from\" [0 2 0]\n");
    }
    source += QByteArrayLiteral("Material \"diffuse\" \"rgb reflectance\" [.6 .5 .4]\n"
                                "Shape \"trianglemesh\" \"point3 P\" [-1 0 -1  1 0 -1  0 0 1]\n"
                                "\"integer indices\" [0 1 2]\n");
    WriteFixtureFile(path, source);
    WriteFixtureFile(QDir(QFileInfo(path).absolutePath()).filePath(QStringLiteral("LICENSE.txt")),
                     QByteArrayLiteral("CC0 synthetic fixture\n"));
}

void WaitForWorld(WorldWorkspace& inoutWorkspace)
{
    REQUIRE(QTest::qWaitFor([&inoutWorkspace]() { return inoutWorkspace.CanSave() == true; },
                            SETTLE_TIMEOUT_MILLISECONDS) == true);
}

ToolingPaths MakeSceneTools()
{
    ToolingPaths tools;
    tools.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    return tools;
}

void CreateImportedWorld(const QString& root, const QString& path, const QString& source)
{
    WorldWorkspace workspace(MakeSceneTools(), nullptr);
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(path) == true);
    REQUIRE(workspace.ImportScene(source) == true);
    WaitForWorld(workspace);
    REQUIRE(workspace.Save() == true);
    WaitForWorld(workspace);
    REQUIRE(workspace.Dirty() == false);
    workspace.StopPreview();
}
} // namespace

TEST_CASE("Switching imported Worlds replaces every direct light and preserves failed replacements",
          "[editor][world][lighting][world-light-switch]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString projectPath = QDir(directory.path()).filePath(QStringLiteral("project"));
    REQUIRE(QDir().mkpath(projectPath) == true);
    const QString root = QFileInfo(projectPath).canonicalFilePath();
    const QString firstSource = directory.filePath(QStringLiteral("first/scene.pbrt"));
    const QString secondSource = directory.filePath(QStringLiteral("second/scene.pbrt"));
    WriteLightingSource(firstSource, false);
    WriteLightingSource(secondSource, true);
    const QString firstPath = QDir(root).filePath(QStringLiteral("first.world.json"));
    const QString secondPath = QDir(root).filePath(QStringLiteral("second.world.json"));
    CreateImportedWorld(root, firstPath, firstSource);
    CreateImportedWorld(root, secondPath, secondSource);

    WorldWorkspace workspace(MakeSceneTools(), nullptr);
    workspace.SetProject(root, 1);
    LevelWorkspace* levelOrNull = workspace.findChild<LevelWorkspace*>();
    REQUIRE(levelOrNull != nullptr);
    preview_lighting::Inputs direct;
    preview_lighting::Inputs shadowed;
    renderer::LightSelection firstLights;
    QByteArray firstKey;
    for (const QString& path : {firstPath, secondPath, firstPath})
    {
        REQUIRE(workspace.Open(path) == true);
        WaitForWorld(workspace);
        INFO(levelOrNull->StatusText().toStdString());
        const RenderScene& scene = levelOrNull->PreviewScene();
        const QByteArray key = preview_lighting::MakeKey(scene);
        REQUIRE(scene.Definition != nullptr);
        const uint32 expectedLights = path == firstPath ? 1 : 2;
        REQUIRE(scene.Definition->Lights.GetSize() == expectedLights);
        RenderGraphExtraction extraction;
        QString extractionError;
        REQUIRE(MakeRenderGraphExtraction(scene, {}, 1, extraction, extractionError) == true);
        REQUIRE(extraction.ShaderInputs.Lights.GetSize() == expectedLights);
        REQUIRE(scene.AuthoredLights.Count == expectedLights);
        for (uint32 index = 0; index < expectedLights; ++index)
        {
            const renderer::DirectLight& extracted = extraction.ShaderInputs.Lights[index];
            const ludus::resources::SceneLight& stored = scene.Definition->Lights[index];
            CHECK(extracted.SourceId == stored.SourceId);
            CHECK(extracted.Color * extracted.Intensity == stored.IntensityRgb);
            CHECK(extracted.Position == stored.PositionWorld);
            CHECK(extracted.Direction == stored.DirectionWorld);
            CHECK(extracted.Range == stored.Range);
        }
        REQUIRE(preview_lighting::BuildInputs(scene, key, direct, false) == rhi::RasterStatus::Ready);
        REQUIRE(preview_lighting::BuildInputs(scene, key, shadowed) == rhi::RasterStatus::Ready);
        REQUIRE(direct.Lights.Count == expectedLights);
        REQUIRE(shadowed.Lights.Count == expectedLights);
        CHECK(direct.ShadowCount == 0);
        CHECK(shadowed.ShadowCount == expectedLights * renderer::POINT_SHADOW_FACES);
        for (uint32 index = 0; index < expectedLights; ++index)
        {
            CHECK(direct.Lights.Lights[index].SourceId == shadowed.Lights.Lights[index].SourceId);
            CHECK(direct.Lights.Lights[index].Color == shadowed.Lights.Lights[index].Color);
            CHECK(direct.Lights.Lights[index].Intensity == shadowed.Lights.Lights[index].Intensity);
        }
        if (firstKey.isEmpty() == true)
        {
            firstKey = key;
            firstLights = direct.Lights;
        }
        else if (path == firstPath)
        {
            CHECK(key == firstKey);
            CHECK(direct.Lights.Lights[0].SourceId == firstLights.Lights[0].SourceId);
            CHECK(direct.Lights.Lights[0].Color == firstLights.Lights[0].Color);
            CHECK(direct.Lights.Lights[0].Intensity == firstLights.Lights[0].Intensity);
        }
        else
        {
            CHECK(key != firstKey);
            CHECK((direct.Lights.Lights[0].Color == firstLights.Lights[0].Color) == false);
        }
    }

    const QJsonObject accepted = workspace.Document().Draft();
    const QByteArray acceptedIdentity = levelOrNull->PreviewScene().Identity;
    const QString invalidPath = QDir(root).filePath(QStringLiteral("invalid.world.json"));
    WriteFixtureFile(invalidPath, QByteArrayLiteral("{}"));
    CHECK(workspace.Open(invalidPath) == false);
    CHECK(workspace.Document().Draft() == accepted);
    CHECK(levelOrNull->PreviewScene().Identity == acceptedIdentity);
    const RenderScene& retained = levelOrNull->PreviewScene();
    CHECK(preview_lighting::MakeKey(retained) == firstKey);
    REQUIRE(preview_lighting::BuildInputs(retained, firstKey, shadowed) == rhi::RasterStatus::Ready);
    REQUIRE(shadowed.Lights.Count == 1);
    CHECK(shadowed.Lights.Lights[0].SourceId == firstLights.Lights[0].SourceId);
    workspace.StopPreview();
}
