// Import preview and report summaries, and the World import preview/report panels.

#include "internal/import_report.h"
#include "internal/level_workspace.h"
#include "internal/preview_viewport.h"
#include "internal/render_panel.h"
#include "internal/workspace_style.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

#include <functional>

using namespace ludus::editor;

namespace
{
/// Write a small general pbrt-v4 scene outside the reference profile.
void WriteGeneralScene(const QString& directory, const QString& name)
{
    REQUIRE(QDir().mkpath(directory) == true);
    QFile scene(QDir(directory).filePath(name));
    REQUIRE(scene.open(QIODevice::WriteOnly) == true);
    scene.write("LookAt 0 1 4  0 0 0  0 1 0\nCamera \"perspective\" \"float fov\" [ 45 ]\nWorldBegin\n"
                "LightSource \"infinite\" \"rgb L\" [ .2 .2 .2 ]\n"
                "Material \"diffuse\" \"rgb reflectance\" [ .6 .5 .4 ]\n"
                "Shape \"trianglemesh\" \"point3 P\" [ -1 0 -1  1 0 -1  1 0 1  -1 0 1 ]\n"
                "  \"integer indices\" [ 0 1 2  0 2 3 ]\n"
                "AttributeBegin\nAreaLightSource \"diffuse\" \"rgb L\" [ 4 4 4 ]\n"
                "Shape \"sphere\" \"float radius\" [ .25 ]\nAttributeEnd\n");
    QFile license(QDir(directory).filePath(QStringLiteral("LICENSE.txt")));
    REQUIRE(license.open(QIODevice::WriteOnly) == true);
    license.write("CC0 synthetic fixture\n");
}

/// Process events until isDone holds or ten seconds pass.
[[nodiscard]] bool WaitUntil(const std::function<bool()>& isDone)
{
    QElapsedTimer timer;
    timer.start();
    while (isDone() == false && timer.elapsed() < 10000)
    {
        QTest::qWait(10);
    }
    return isDone();
}

[[nodiscard]] ToolingPaths MakeSceneTools()
{
    ToolingPaths tools;
    tools.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    return tools;
}

[[nodiscard]] QJsonObject MakeBestEffortReport(bool isDryRun)
{
    return QJsonDocument::fromJson(QByteArray(R"({"importer":"pbrt-best-effort-v1","entry":"room.pbrt","dry_run":)") +
                                   (isDryRun == true ? "true" : "false") +
                                   R"(,"source":{"files":2,"placements":40,"vertices":1200,"materials":12,"textures":3,
               "lights":{"area":2,"infinite":1,"point":3,"spot":0,"distant":1,"other":0}},
               "cooked":{"meshes":6,"materials":6,"textures":6,"objects":6,"vertices":1200,
               "bundle_bytes":2097152,"dropped_placements":0},
               "diagnostics":[{"code":"DIRECTIVE_IGNORED","severity":"info","count":1,"message":"Integrator"},
               {"code":"MATERIALS_MERGED","severity":"warning","count":6,"message":"merged into slot 0"}]})")
        .object();
}
} // namespace

TEST_CASE("import sources split into a directory and a pbrt entry", "[editor][import]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const ImportSource reference = SplitImportSource(directory.path());
    CHECK(reference.Entry.isEmpty() == true);
    CHECK(reference.Directory == QDir(directory.path()).absolutePath());
    const ImportSource general = SplitImportSource(QDir(directory.path()).filePath(QStringLiteral("room.pbrt")));
    CHECK(general.Entry == QStringLiteral("room.pbrt"));
    CHECK(general.Directory == QDir(directory.path()).absolutePath());
    const ImportSource gltf = SplitImportSource(QDir(directory.path()).filePath(QStringLiteral("scene.glb")));
    CHECK(gltf.Entry == QStringLiteral("scene.glb"));
}

TEST_CASE("import previews list source and cooked counts, budget and fallbacks", "[editor][import]")
{
    const QJsonObject response{{QStringLiteral("profile"), QStringLiteral("pbrt-best-effort-v1")},
                               {QStringLiteral("report"), MakeBestEffortReport(true)},
                               {QStringLiteral("reference_rejection"), QStringLiteral("unsupported PixelFilter")}};
    const QString text = DescribeImportPreview(response);
    CHECK(text.contains(QStringLiteral("Importer: pbrt-best-effort-v1 (room.pbrt)")) == true);
    CHECK(text.contains(QStringLiteral("40 placements, 1200 vertices, 12 materials, 3 textures")) == true);
    CHECK(text.contains(QStringLiteral("2 area (emissive), 1 infinite (environment fallback), 4 skipped")) == true);
    CHECK(text.contains(QStringLiteral("Cooked: 6/6 meshes, 6/8 materials, 6/8 textures, 6 objects")) == true);
    CHECK(text.contains(QStringLiteral("bundle size measured on import")) == true);
    CHECK(text.contains(QStringLiteral("MATERIALS_MERGED ×6: merged into slot 0")) == true);
    CHECK(text.contains(QStringLiteral("DIRECTIVE_IGNORED")) == false); // Informational only.
    CHECK(text.contains(QStringLiteral("unsupported PixelFilter")) == true);

    const QString stored = DescribeImportReport(MakeBestEffortReport(false));
    CHECK(stored.contains(QStringLiteral("2.0 MiB of 64.0 MiB")) == true);

    // glTF and OBJ imports share the best-effort report schema and name their profile.
    QJsonObject gltf = MakeBestEffortReport(false);
    gltf.insert(QStringLiteral("importer"), QStringLiteral("gltf-best-effort-v1"));
    gltf.insert(QStringLiteral("entry"), QStringLiteral("scene.glb"));
    const QString gltfText = DescribeImportReport(gltf);
    CHECK(gltfText.contains(QStringLiteral("Importer: gltf-best-effort-v1 (scene.glb)")) == true);
    CHECK(gltfText.contains(QStringLiteral("2.0 MiB of 64.0 MiB")) == true);

    // Version 5 reports show the import profile's limits.
    QJsonObject imported = MakeBestEffortReport(false);
    QJsonObject cooked = imported.value(QStringLiteral("cooked")).toObject();
    cooked.insert(QStringLiteral("scene_format_version"), 5);
    imported.insert(QStringLiteral("cooked"), cooked);
    const QString importedText = DescribeImportReport(imported);
    CHECK(importedText.contains(QStringLiteral("Cooked: 6/256 meshes, 6/256 materials, 6/256 textures")) == true);
    CHECK(importedText.contains(QStringLiteral("1200 of 4000000 vertices")) == true);
    CHECK(importedText.contains(QStringLiteral("2.0 MiB of 384.0 MiB")) == true);

    const QJsonObject manifest{{QStringLiteral("source"), QStringLiteral("Modern Hall / staircase2")},
                               {QStringLiteral("meshes"), 6},
                               {QStringLiteral("materials"), 8},
                               {QStringLiteral("items"), 17},
                               {QStringLiteral("approximations"), QJsonArray{QStringLiteral("fog skipped")}}};
    const QString reference = DescribeImportReport(manifest);
    CHECK(reference.contains(QStringLiteral("Importer: pbrt-reference-v1 (Modern Hall / staircase2)")) == true);
    CHECK(reference.contains(QStringLiteral("  fog skipped")) == true);

    CHECK(DescribeImportReports({}).isEmpty() == true);
    const QJsonObject reports{
        {QStringLiteral("import_reports"), QJsonObject{{QStringLiteral("scene/1"), MakeBestEffortReport(false)}}}};
    CHECK(DescribeImportReports(reports).startsWith(QStringLiteral("Import scene/1\nImporter:")) == true);
}

TEST_CASE("Level preview of a general pbrt scene writes nothing, then imports with a stored report", "[editor][import]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QDir(directory.path()).filePath(QStringLiteral("project"));
    const QString source = QDir(directory.path()).filePath(QStringLiteral("external"));
    REQUIRE(QDir().mkpath(root) == true);
    WriteGeneralScene(source, QStringLiteral("room.pbrt"));
    const QString scene = QDir(source).filePath(QStringLiteral("room.pbrt"));
    LevelWorkspace workspace(MakeSceneTools());
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("scenes/main.lulevel"))) == true);

    REQUIRE(workspace.PreviewImport(scene) == true);
    CHECK(workspace.CanCancel() == true);
    REQUIRE(WaitUntil([&workspace]() { return workspace.Busy() == false; }) == true);
    const QJsonObject preview = workspace.ImportPreview();
    INFO(workspace.StatusText().toStdString());
    REQUIRE(preview.value(QStringLiteral("profile")).toString() == QStringLiteral("pbrt-best-effort-v1"));
    CHECK(preview.value(QStringLiteral("report")).toObject().value(QStringLiteral("dry_run")).toBool() == true);
    CHECK(QFileInfo::exists(QDir(root).filePath(QStringLiteral("assets"))) == false);

    REQUIRE(workspace.Import(scene) == true);
    REQUIRE(WaitUntil([&workspace]() { return workspace.Busy() == false; }) == true);
    INFO(workspace.StatusText().toStdString());
    CHECK(workspace.Document().value(QStringLiteral("objects")).toArray().isEmpty() == false);
    REQUIRE(workspace.ImportReports().isEmpty() == false);
    CHECK(DescribeImportReports(QJsonObject{{QStringLiteral("import_reports"), workspace.ImportReports()}})
              .contains(QStringLiteral("ENVIRONMENT_FALLBACK")) == true);
}

TEST_CASE("A failed Level preview leaves no preview", "[editor][import]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    WriteGeneralScene(directory.path(), QStringLiteral("room.pbrt"));
    LevelWorkspace workspace(MakeSceneTools());
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Create(QDir(directory.path()).filePath(QStringLiteral("main.lulevel"))) == true);
    REQUIRE(workspace.PreviewImport(QDir(directory.path()).filePath(QStringLiteral("missing.pbrt"))) == true);
    REQUIRE(WaitUntil([&workspace]() { return workspace.Busy() == false; }) == true);
    CHECK(workspace.ImportPreview().isEmpty() == true);
}

TEST_CASE("World shows the import preview, imports on confirmation and shows the stored report",
          "[editor][import][world]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString source = QDir(root).filePath(QStringLiteral("external"));
    WriteGeneralScene(source, QStringLiteral("room.pbrt"));
    WorldWorkspace workspace(MakeSceneTools(), nullptr);
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("main.world.json"))) == true);
    QPlainTextEdit* preview = workspace.findChild<QPlainTextEdit*>(QStringLiteral("worldImportPreview"));
    QPlainTextEdit* report = workspace.findChild<QPlainTextEdit*>(QStringLiteral("worldImportReport"));
    QToolButton* reportToggle = workspace.findChild<QToolButton*>(QStringLiteral("worldImportReportToggle"));
    REQUIRE(preview != nullptr);
    REQUIRE(report != nullptr);
    REQUIRE(reportToggle != nullptr);

    REQUIRE(workspace.PreviewImportScene(QDir(source).filePath(QStringLiteral("room.pbrt"))) == true);
    REQUIRE(WaitUntil([preview]() { return preview->isHidden() == false; }) == true);
    CHECK(preview->toPlainText().contains(QStringLiteral("pbrt-best-effort-v1")) == true);
    CHECK(report->isHidden() == true);

    REQUIRE(workspace.ConfirmImportPreview() == true);
    CHECK(preview->isHidden() == true);
    REQUIRE(WaitUntil([reportToggle]() { return reportToggle->isHidden() == false; }) == true);
    CHECK(report->isHidden() == true);
    reportToggle->click();
    CHECK(report->isHidden() == false);
    CHECK(report->toPlainText().contains(QStringLiteral("Cooked:")) == true);
    reportToggle->click();
    CHECK(report->isHidden() == true);
    CHECK(workspace.ConfirmImportPreview() == false); // The preview is consumed.
    // The imported scene is shown in the World viewport, not left behind the empty World panel.
    QWidget* viewport = workspace.findChild<QWidget*>(QStringLiteral("worldSceneViewport"));
    REQUIRE(viewport != nullptr);
    CHECK(viewport->isHidden() == false);
    QPushButton* cancel = workspace.findChild<QPushButton*>(QStringLiteral("worldCancelScene"));
    REQUIRE(cancel != nullptr);
    // The stored report publishes before the last candidate operation releases
    // its cancel row. Measure report expansion after that operation completes.
    REQUIRE(WaitUntil([cancel]() { return cancel->isHidden() == true; }) == true);
    ApplyWorkspaceStyle(&workspace);
    workspace.show();
    QScrollArea* hierarchy = workspace.findChild<QScrollArea*>(QStringLiteral("worldHierarchyScroll"));
    REQUIRE(hierarchy != nullptr);
    const QJsonObject original = workspace.Document().Draft();
    for (const QSize size : {QSize(1100, 520), QSize(800, 400)})
    {
        workspace.resize(size);
        reportToggle->setChecked(true);
        QTest::qWait(40);
        CHECK(workspace.size() == size);
        CHECK(viewport->height() >= 160);
        CHECK(workspace.rect().contains(viewport->mapTo(&workspace, viewport->rect().bottomRight())) == true);
        const ludus::foundation::int32 viewportHeight = viewport->height();
        for (const QString& name :
             {QStringLiteral("worldAdd"), QStringLiteral("worldDuplicate"), QStringLiteral("worldDelete")})
        {
            QPushButton* button = workspace.findChild<QPushButton*>(name);
            REQUIRE(button != nullptr);
            hierarchy->ensureWidgetVisible(button);
            QApplication::processEvents();
            CHECK(button->height() >= button->sizeHint().height());
            CHECK(hierarchy->viewport()->rect().contains(
                      button->mapTo(hierarchy->viewport(), button->rect().center())) == true);
        }
        reportToggle->setChecked(false);
        QTest::qWait(40);
        CHECK(viewport->height() == viewportHeight);
    }
    CHECK(workspace.Document().Draft() == original);
    if (PreviewViewport::GetPresentation() == ViewportPresentation::OffscreenReadback)
    {
        // The engine-owned device renders offscreen and the copied frame replaces the placeholder.
        const auto getPresentedFrames = [viewport]() {
            const PreviewViewport* presenter = dynamic_cast<const PreviewViewport*>(viewport);
            return presenter != nullptr ? presenter->PresentedFrames() : 0;
        };
        REQUIRE(WaitUntil([&getPresentedFrames]() { return getPresentedFrames() > 0; }) == true);
        const QWidget* canvas = viewport->findChild<QWidget*>(QStringLiteral("LudusOffscreenViewport"));
        REQUIRE(canvas != nullptr);
        CHECK(canvas->isHidden() == false);
        return;
    }
    // Without any presentation path (offscreen Qt) the viewport explains itself instead of the placeholder.
    bool hasExplanation = false;
    for (const QLabel* label : viewport->findChildren<QLabel*>())
    {
        hasExplanation = hasExplanation == true || label->text().contains(QStringLiteral("Scene ready")) == true;
        CHECK(label->text().contains(QStringLiteral("Import a reference scene")) == false);
    }
    CHECK(hasExplanation == true);
}

TEST_CASE("World import without an open World explains why nothing happened", "[editor][import][world]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    WorldWorkspace workspace(MakeSceneTools(), nullptr);
    workspace.SetProject(root, 1);
    QLabel* status = workspace.findChild<QLabel*>(QStringLiteral("worldStatus"));
    REQUIRE(status != nullptr);
    CHECK(workspace.PreviewImportScene(QDir(root).filePath(QStringLiteral("room.pbrt"))) == false);
    CHECK(status->text().contains(QStringLiteral("open a World")) == true);
    CHECK(workspace.ConfirmImportPreview() == false);
    CHECK(status->text().contains(QStringLiteral("choose Import scene")) == true);
}

TEST_CASE("A failed World import keeps the previous World view", "[editor][import][world]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    WorldWorkspace workspace(MakeSceneTools(), nullptr);
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("main.world.json"))) == true);
    QWidget* viewport = workspace.findChild<QWidget*>(QStringLiteral("worldSceneViewport"));
    QLabel* status = workspace.findChild<QLabel*>(QStringLiteral("worldStatus"));
    REQUIRE(viewport != nullptr);
    REQUIRE(status != nullptr);
    REQUIRE(workspace.ImportScene(QDir(root).filePath(QStringLiteral("missing.pbrt"))) == true);
    REQUIRE(WaitUntil([status]() { return status->text().startsWith(QStringLiteral("Importing")) == false; }) == true);
    CHECK(viewport->isHidden() == true);
    CHECK(status->text().isEmpty() == false);
}

TEST_CASE("An empty default World supports bundled geometry without requiring an import", "[editor][import][world]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    REQUIRE(QDir().mkpath(QDir(root).filePath(QStringLiteral("assets"))) == true);
    WorldWorkspace workspace(MakeSceneTools(), nullptr);
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("assets/staircase2.world.json"))) == true);
    QTest::qWait(200);
    bool hasImportHint = false;
    for (const QLabel* label : workspace.findChildren<QLabel*>())
    {
        CHECK(label->text().contains(QStringLiteral("Asset must be a regular file")) == false);
        hasImportHint = hasImportHint == true || label->text().contains(QStringLiteral("Use Import scene")) == true;
    }
    CHECK(hasImportHint == false);
}

TEST_CASE("A failed scene import is shown in the empty World viewport", "[editor][import][world]")
{
    RenderPanel panel;
    panel.SetWorld(QString(), nullptr);
    const QString error = QStringLiteral("Project catalog exceeds 64 MiB (MAX_CATALOG_BYTES)");
    panel.ShowSceneFailure(error);
    bool hasStatus = false;
    for (const QLabel* label : panel.findChildren<QLabel*>())
    {
        hasStatus = hasStatus == true || label->text() == QStringLiteral("Scene import failed: ") + error;
    }
    CHECK(hasStatus == true);
}
