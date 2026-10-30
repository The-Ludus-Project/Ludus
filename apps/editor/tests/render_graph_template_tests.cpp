// Built-in creation and canvas settings use the real shared backend and document history.
#include "internal/controller.h"
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_configuration_template.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_pipeline_node.h"
#include "internal/render_graph_quick_add.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
constexpr int32 TEMPLATE_JOB_TIMEOUT_MILLISECONDS = 60000;
constexpr int32 TEMPLATE_WORKSPACE_WIDTH = 1280;
constexpr int32 TEMPLATE_WORKSPACE_HEIGHT = 800;
constexpr int32 TEMPLATE_PROPERTIES_WIDTH = 320;
constexpr int32 TEMPLATE_PROPERTIES_HEIGHT = 700;
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");
QString FindTemplatePython()
{
    const QString managed = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/host-tools/venv/bin/python");
    return QFileInfo(managed).isExecutable() == true ? managed
                                                     : QStandardPaths::findExecutable(QStringLiteral("python3"));
}
} // namespace

TEST_CASE("ReSTIR template settings are editable from the canvas with Properties hidden",
          "[editor][render-graph][restir-template]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    QWidget properties;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.resize(TEMPLATE_WORKSPACE_WIDTH, TEMPLATE_WORKSPACE_HEIGHT);
    QWidget* inspector = workspace.TakeInspectorPanel(&properties);
    inspector->hide();
    properties.hide();
    workspace.show();
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    QTimer::singleShot(0, &workspace, []() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        QComboBox* templateOrNull = dialogOrNull->findChild<QComboBox*>(QStringLiteral("newRenderGraphTemplate"));
        REQUIRE(templateOrNull != nullptr);
        templateOrNull->setCurrentIndex(templateOrNull->findData(QStringLiteral("restir-di")));
        const QString capture = qEnvironmentVariable("LUDUS_EDITOR_RESTIR_CAPTURE");
        if (capture.isEmpty() == false)
        {
            REQUIRE(dialogOrNull->grab().save(capture + QStringLiteral("-template.png")) == true);
        }
        dialogOrNull->accept();
    });
    REQUIRE(workspace.PromptNewGraph() == true);
    REQUIRE(finished.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(workspace.GetLastResult().Message.toStdString());
    REQUIRE(workspace.GetDocument().GetDraft(QStringLiteral("library.lurender.json")).contains("bytes_per_pixel") ==
            true);
    const QString manifest = QStringLiteral("main.lushader.json");
    const QByteArray original = workspace.GetDocument().GetDraft(manifest);
    REQUIRE(original.contains("RESTIR_INITIAL_CANDIDATE_COUNT") == true);
    REQUIRE(workspace.GetCanvas().GetNodeCount() > 0);
    CHECK(inspector->isVisible() == false);
    const auto getCount = [&workspace, &manifest]() {
        return QJsonDocument::fromJson(workspace.GetDocument().GetDraft(manifest))
            .object()
            .value(QStringLiteral("settings"))
            .toArray()[0]
            .toObject()
            .value(QStringLiteral("value"))
            .toInt();
    };
    CHECK(getCount() == 8);
    bool hasOpenedSettings = false;
    QTimer::singleShot(0, &workspace, [&workspace, &hasOpenedSettings]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* settingsOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphEditSettingsAction"));
        REQUIRE(settingsOrNull != nullptr);
        REQUIRE(settingsOrNull->isEnabled() == true);
        menuOrNull->close();
        QTimer::singleShot(0, &workspace, [&hasOpenedSettings]() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialogOrNull != nullptr);
            QSpinBox* candidatesOrNull =
                dialogOrNull->findChild<QSpinBox*>(QStringLiteral("renderGraphSetting_RESTIR_INITIAL_CANDIDATE_COUNT"));
            REQUIRE(candidatesOrNull != nullptr);
            candidatesOrNull->setFocus();
            candidatesOrNull->selectAll();
            QTest::keyClicks(candidatesOrNull, QStringLiteral("12"));
            QSignalSpy committed(candidatesOrNull, &QSpinBox::editingFinished);
            QTest::keyClick(candidatesOrNull, Qt::Key_Return);
            CHECK(committed.count() > 0);
            hasOpenedSettings = true;
            const QString capture = qEnvironmentVariable("LUDUS_EDITOR_RESTIR_CAPTURE");
            if (capture.isEmpty() == false)
            {
                REQUIRE(dialogOrNull->grab().save(capture + QStringLiteral("-settings.png")) == true);
            }
            dialogOrNull->reject();
        });
        settingsOrNull->trigger();
    });
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const QPointF background = canvas.scene()->itemsBoundingRect().bottomRight() + QPointF(100, 100);
    canvas.centerOn(background);
    const QPoint position = canvas.mapFromScene(background);
    QContextMenuEvent context(QContextMenuEvent::Mouse, position, canvas.viewport()->mapToGlobal(position));
    QCoreApplication::sendEvent(canvas.viewport(), &context);
    REQUIRE(hasOpenedSettings == true);
    CHECK(getCount() == 12);
    CHECK(workspace.GetDocument().GetSaved(manifest) == original);
    REQUIRE(workspace.CanUndo() == true);
    workspace.Undo();
    CHECK(getCount() == 8);
    workspace.Redo();
    CHECK(getCount() == 12);
    REQUIRE(workspace.Save() == true);
    REQUIRE(workspace.Validate() == true);
    REQUIRE(finished.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(workspace.GetLastResult().Message.toStdString());
    CHECK(workspace.GetLastResult().IsSuccessful == true);
    const QString path = QDir(project.path()).filePath(QStringLiteral("graphs/Main/main.lurender.json"));
    RenderGraphDocument reloaded;
    REQUIRE(reloaded.Load(QFileInfo(path).absolutePath(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reloaded.GetDraft(manifest) == workspace.GetDocument().GetSaved(manifest));
}

TEST_CASE("Visibility templates are created from the editor picker with Properties hidden",
          "[editor][render-graph][visibility-template]")
{
    QString identity;
    SECTION("visibility buffer and depth")
    {
        identity = QStringLiteral("visibility-buffer");
    }
    SECTION("surface reconstruction and secondary rays")
    {
        identity = QStringLiteral("visibility-secondary-rays");
    }
    SECTION("tiled emitter tables and MIS direct lighting")
    {
        identity = QStringLiteral("tiled-vbuffer-nee");
    }
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    QWidget properties;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    QWidget* inspector = workspace.TakeInspectorPanel(&properties);
    inspector->hide();
    properties.hide();
    workspace.show();
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    QTimer::singleShot(0, &workspace, [&identity]() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        QComboBox* templateOrNull = dialogOrNull->findChild<QComboBox*>(QStringLiteral("newRenderGraphTemplate"));
        REQUIRE(templateOrNull != nullptr);
        const int32 selected = templateOrNull->findData(identity);
        REQUIRE(selected >= 0);
        templateOrNull->setCurrentIndex(selected);
        CHECK(templateOrNull->currentData(Qt::ToolTipRole).toString().isEmpty() == false);
        dialogOrNull->accept();
    });
    REQUIRE(workspace.PromptNewGraph() == true);
    REQUIRE(finished.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(workspace.GetLastResult().Message.toStdString());
    REQUIRE(workspace.GetCanvas().GetNodeCount() > 0);
    CHECK(inspector->isVisible() == false);
    const QByteArray manifest = workspace.GetDocument().GetDraft(QStringLiteral("main.lushader.json"));
    CHECK(manifest.contains("Flat") == true);
    const bool hasRays = identity != QStringLiteral("visibility-buffer");
    CHECK(manifest.contains("visibility_buffer") == hasRays);
    const QByteArray graph = workspace.GetDocument().GetDraft(QStringLiteral("library.lurender.json"));
    CHECK(graph.contains("GraphicsPipeline") == true);
    CHECK(graph.contains("RayTracingPipeline") == hasRays);
    if (identity == QStringLiteral("tiled-vbuffer-nee"))
    {
        CHECK(graph.contains("bytes_per_pixel") == true);
        CHECK(workspace.GetDocument().GetDraft(QStringLiteral("main_shaders/tile_table.slang")).isEmpty() == false);
    }
    CHECK(graph.contains("visibility") == true);
    CHECK(graph.contains("depth") == true);
    REQUIRE(workspace.Validate() == true);
    REQUIRE(finished.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(workspace.GetLastResult().Message.toStdString());
    CHECK(workspace.GetLastResult().IsSuccessful == true);
}

TEST_CASE("ReSTIR configuration template fills an existing graph library from the canvas",
          "[editor][render-graph][restir-library]")
{
    QTemporaryDir project;
    QWidget properties;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    workspace.resize(TEMPLATE_WORKSPACE_WIDTH, TEMPLATE_WORKSPACE_HEIGHT);
    QWidget* inspector = workspace.TakeInspectorPanel(&properties);
    inspector->hide();
    properties.hide();
    workspace.show();
    REQUIRE(workspace.CustomizeBuiltIn() == true);
    REQUIRE(workspace.IsBuiltIn() == false);
    REQUIRE(QFileInfo::exists(QDir(workspace.GetDocument().GetPackageRoot()).filePath(ROOT_GRAPH)) == true);
    REQUIRE(workspace.CreateSubgraph(QStringLiteral("raytracing")) == true);
    REQUIRE(workspace.CreateConfiguration(QStringLiteral("RestirDi")) == true);
    RenderGraphDocument& document = workspace.GetDocument();
    const QString library = QStringLiteral("raytracing.lurender.json");
    const QString manifest = QStringLiteral("main.lushader.json");
    const QByteArray beforeLibrary = document.GetDraft(library);
    const QByteArray beforeManifest = document.GetDraft(manifest);
    const QByteArray beforeRaster = document.GetDraft(QStringLiteral("rasterize.lurender.json"));
    bool hasAppliedTemplate = false;
    QTimer::singleShot(0, &workspace, [&workspace, &hasAppliedTemplate]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* actionOrNull =
            menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphApplyConfigurationTemplate"));
        REQUIRE(actionOrNull != nullptr);
        REQUIRE(actionOrNull->isEnabled() == true);
        menuOrNull->close();
        QTimer::singleShot(0, &workspace, [&hasAppliedTemplate]() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialogOrNull != nullptr);
            QLineEdit* nameOrNull =
                dialogOrNull->findChild<QLineEdit*>(QStringLiteral("renderGraphTemplateConfiguration"));
            REQUIRE(nameOrNull != nullptr);
            CHECK(nameOrNull->text() == QStringLiteral("RestirDi"));
            QDialogButtonBox* buttonsOrNull = dialogOrNull->findChild<QDialogButtonBox*>();
            REQUIRE(buttonsOrNull != nullptr);
            const QString capture = qEnvironmentVariable("LUDUS_EDITOR_RESTIR_LIBRARY_CAPTURE");
            if (capture.isEmpty() == false)
            {
                REQUIRE(dialogOrNull->grab().save(capture + QStringLiteral("-dialog.png")) == true);
            }
            buttonsOrNull->button(QDialogButtonBox::Apply)->click();
            hasAppliedTemplate = true;
        });
        actionOrNull->trigger();
    });
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.centerOn(canvas.scene()->itemsBoundingRect().bottomRight() + QPointF(100, 100));
    QContextMenuEvent event(QContextMenuEvent::Mouse,
                            canvas.viewport()->rect().center(),
                            canvas.viewport()->mapToGlobal(canvas.viewport()->rect().center()));
    QApplication::sendEvent(canvas.viewport(), &event);
    REQUIRE(hasAppliedTemplate == true);
    CHECK(inspector->isVisible() == false);
    CHECK(canvas.GetConfiguration() == QStringLiteral("RestirDi"));
    CHECK(canvas.GetNodeCount() == 6);
    CHECK(document.GetDraft(library).contains("input.scene") == true);
    CHECK(document.GetDraft(library).contains("bytes_per_pixel") == true);
    CHECK(document.GetDraft(manifest).contains("RestirInitialClosest") == true);
    CHECK(document.GetDraft(QStringLiteral("restir_di/main_shaders/initial.slang")).isEmpty() == false);
    CHECK(document.GetDraft(QStringLiteral("rasterize.lurender.json")) == beforeRaster);
    CHECK(QFileInfo::exists(
              QDir(document.GetPackageRoot()).filePath(QStringLiteral("restir_di/main_shaders/initial.slang"))) ==
          false);
    const QByteArray afterLibrary = document.GetDraft(library);
    const QByteArray afterManifest = document.GetDraft(manifest);
    workspace.Undo();
    CHECK(document.GetDraft(library) == beforeLibrary);
    CHECK(document.GetDraft(manifest) == beforeManifest);
    CHECK(document.GetDraft(QStringLiteral("restir_di/main_shaders/initial.slang")).isEmpty() == true);
    workspace.Redo();
    CHECK(document.GetDraft(library) == afterLibrary);
    CHECK(document.GetDraft(manifest) == afterManifest);
    CHECK(workspace.ApplyRestirTemplate(QStringLiteral("RestirDi")) == false);
    CHECK(document.GetDraft(library) == afterLibrary);
    CHECK(document.GetDraft(manifest) == afterManifest);
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_RESTIR_LIBRARY_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(workspace.grab().save(capture + QStringLiteral("-canvas.png")) == true);
    }
    // Select the reusable configuration in the parent's existing Subgraph node.
    QJsonObject root;
    REQUIRE(document.GetGraph(ROOT_GRAPH, root) == true);
    QJsonArray configurations = root.value(QStringLiteral("configurations")).toArray();
    QJsonObject parent = configurations.first().toObject();
    QJsonArray nodes;
    for (const QJsonValueConstRef value : parent.value(QStringLiteral("nodes")).toArray())
    {
        QJsonObject node = value.toObject();
        if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph"))
        {
            if (node.value(QStringLiteral("id")).toString() != QStringLiteral("raster"))
            {
                continue;
            }
            node.insert(QStringLiteral("asset"), library);
            node.insert(QStringLiteral("configuration"), QStringLiteral("RestirDi"));
        }
        nodes.append(node);
    }
    QJsonArray edges;
    for (const QJsonValueConstRef value : parent.value(QStringLiteral("edges")).toArray())
    {
        if (value.toObject().value(QStringLiteral("to")).toString() != QStringLiteral("raster.depth"))
        {
            edges.append(value);
        }
    }
    parent.insert(QStringLiteral("nodes"), nodes);
    parent.insert(QStringLiteral("edges"), edges);
    configurations.replace(0, parent);
    root.insert(QStringLiteral("configurations"), configurations);
    REQUIRE(document.SetText(ROOT_GRAPH, EncodeCanonicalJson(root)) == RenderGraphDocumentStatus::Ok);
    const QJsonArray importedPrograms =
        QJsonDocument::fromJson(document.GetDraft(manifest)).object().value(QStringLiteral("programs")).toArray();
    for (const QJsonValueConstRef value : importedPrograms)
    {
        const QString source = value.toObject().value(QStringLiteral("source")).toString();
        INFO(source.toStdString());
        REQUIRE(document.GetDraft(source).isEmpty() == false);
    }
    QTemporaryDir unsavedSnapshot;
    REQUIRE(document.WriteSnapshot(unsavedSnapshot.path()) == RenderGraphDocumentStatus::Ok);
    for (const QJsonValueConstRef value : importedPrograms)
    {
        CHECK(QFileInfo::exists(
                  QDir(unsavedSnapshot.path()).filePath(value.toObject().value(QStringLiteral("source")).toString())) ==
              true);
    }
    const QString helper = qEnvironmentVariable("LUDUS_RESTIR_TEST_HELPER");
    const QString compiler = qEnvironmentVariable("LUDUS_RESTIR_TEST_COMPILER");
    if (helper.isEmpty() == false && compiler.isEmpty() == false)
    {
        RenderGraphBuildSettings settings;
        settings.ShouldOverrideTools = true;
        settings.Tools.Helper = helper;
        settings.Tools.Compiler = compiler;
        settings.Tools.SpirvCross = qEnvironmentVariable("LUDUS_RESTIR_TEST_TRANSLATOR");
        workspace.SetBuildSettings(settings);
        QSignalSpy built(&workspace, &RendererWorkspace::JobFinished);
        REQUIRE(workspace.Build() == true);
        REQUIRE(built.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
        INFO(workspace.GetLastResult().Message.toStdString());
        CHECK(workspace.GetLastResult().IsSuccessful == true);
        CHECK(document.GetSaved(manifest) == beforeManifest);
        CHECK(QFileInfo::exists(
                  QDir(document.GetPackageRoot()).filePath(QStringLiteral("restir_di/main_shaders/miss.slang"))) ==
              false);
    }
    REQUIRE(workspace.Save() == true);
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    REQUIRE(workspace.Validate() == true);
    REQUIRE(finished.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(workspace.GetLastResult().Message.toStdString());
    CHECK(workspace.GetLastResult().IsSuccessful == true);
    RenderGraphDocument reloaded;
    REQUIRE(reloaded.Load(document.GetPackageRoot(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reloaded.GetDraft(library) == afterLibrary);
    for (const QString& file : document.GetFiles())
    {
        INFO(file.toStdString());
        CHECK(reloaded.GetDraft(file) == document.GetDraft(file));
    }
    if (helper.isEmpty() == false && compiler.isEmpty() == false)
    {
        RendererWorkspace reopened({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
        reopened.SetProject(project.path(), 2);
        reopened.SetLivePreview(false);
        REQUIRE(reopened.IsBuiltIn() == false);
        RenderGraphBuildSettings settings;
        settings.ShouldOverrideTools = true;
        settings.Tools.Helper = helper;
        settings.Tools.Compiler = compiler;
        settings.Tools.SpirvCross = qEnvironmentVariable("LUDUS_RESTIR_TEST_TRANSLATOR");
        reopened.SetBuildSettings(settings);
        QSignalSpy built(&reopened, &RendererWorkspace::JobFinished);
        REQUIRE(reopened.Build() == true);
        REQUIRE(built.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
        INFO(reopened.GetLastResult().Message.toStdString());
        CHECK(reopened.GetLastResult().IsSuccessful == true);
    }
    const QString fixture = qEnvironmentVariable("LUDUS_RESTIR_LIBRARY_FIXTURE");
    if (fixture.isEmpty() == false)
    {
        REQUIRE(document.WriteSnapshot(fixture) == RenderGraphDocumentStatus::Ok);
    }
}

TEST_CASE("Root canvas inserts a ReSTIR subgraph transactionally and rejects conflicting budgets",
          "[editor][render-graph][restir-library]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray beforeRoot = document.GetDraft(ROOT_GRAPH);
    const QByteArray beforeManifest = document.GetDraft(QStringLiteral("main.lushader.json"));
    REQUIRE(workspace.ApplyRestirTemplate(QStringLiteral("Lighting")) == true);
    CHECK(document.GetDraft(ROOT_GRAPH).contains("Lighting.lurender.json") == true);
    CHECK(document.GetDraft(QStringLiteral("Lighting.lurender.json")).contains("RestirInitialClosest") == true);
    CHECK(workspace.GetCanvas().GetFile() == QStringLiteral("Lighting.lurender.json"));
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
    CHECK(document.GetDraft(QStringLiteral("main.lushader.json")) == beforeManifest);
    CHECK(document.GetDraft(QStringLiteral("Lighting.lurender.json")).isEmpty() == true);
    workspace.Redo();
    CHECK(document.GetDraft(QStringLiteral("Lighting.lurender.json")).isEmpty() == false);
    workspace.Undo();
    QJsonObject manifest = QJsonDocument::fromJson(beforeManifest).object();
    QJsonObject budgets = manifest.value(QStringLiteral("budgets")).toObject();
    budgets.insert(QStringLiteral("programs"), 1);
    manifest.insert(QStringLiteral("budgets"), budgets);
    REQUIRE(document.SetText(QStringLiteral("main.lushader.json"), EncodeCanonicalJson(manifest)) ==
            RenderGraphDocumentStatus::Ok);
    const QByteArray constrained = document.GetDraft(QStringLiteral("main.lushader.json"));
    QString error;
    CHECK(document.ApplyRestirTemplate(ROOT_GRAPH, QStringLiteral("Lighting"), error) ==
          RenderGraphDocumentStatus::Capacity);
    CHECK(error.contains(QStringLiteral("needs 9 shader programs")) == true);
    CHECK(error.contains(QStringLiteral("budgets.programs is 1")) == true);
    CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
    CHECK(document.GetDraft(QStringLiteral("main.lushader.json")) == constrained);
    CHECK(document.GetDraft(QStringLiteral("Lighting.lurender.json")).isEmpty() == true);
}

TEST_CASE("ReSTIR import upgrades default budgets beside an existing ray pipeline",
          "[editor][render-graph][restir-library]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    const QString configuration = QStringLiteral("Default");
    QJsonObject pipeline = MakePipelineNode(QStringLiteral("RayTracingPipeline"), QStringLiteral("trace"));
    REQUIRE(document.AddNode(ROOT_GRAPH, configuration, pipeline) == RenderGraphDocumentStatus::Ok);
    QJsonArray shaders;
    for (const QString& stage :
         {QStringLiteral("ray_generation"), QStringLiteral("miss"), QStringLiteral("closest_hit")})
    {
        shaders.append(QJsonObject{{QStringLiteral("id"), stage},
                                   {QStringLiteral("stage"), stage},
                                   {QStringLiteral("program"), QStringLiteral("Program")}});
    }
    pipeline.insert(QStringLiteral("shaders"), shaders);
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, configuration, pipeline) == RenderGraphDocumentStatus::Ok);
    const QString manifestFile = QStringLiteral("main.lushader.json");
    const QByteArray beforeRoot = document.GetDraft(ROOT_GRAPH);
    const QByteArray beforeManifest = document.GetDraft(manifestFile);
    const QJsonObject before = QJsonDocument::fromJson(beforeManifest).object();
    const QJsonArray previousPrograms = before.value(QStringLiteral("programs")).toArray();
    CHECK(previousPrograms.size() == 7);
    QString error;
    REQUIRE(document.ApplyRestirTemplate(ROOT_GRAPH, QStringLiteral("Lighting"), error) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(error.isEmpty() == true);
    const QByteArray afterManifest = document.GetDraft(manifestFile);
    const QJsonObject after = QJsonDocument::fromJson(afterManifest).object();
    CHECK(after.value(QStringLiteral("programs")).toArray().size() == 12);
    CHECK(after.value(QStringLiteral("budgets")).toObject().value(QStringLiteral("programs")).toInt() == 16);
    CHECK(after.value(QStringLiteral("budgets")).toObject().value(QStringLiteral("artifacts")).toInt() == 64);
    for (const QJsonValueConstRef program : previousPrograms)
    {
        CHECK(after.value(QStringLiteral("programs")).toArray().contains(program) == true);
    }
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
    CHECK(document.GetDraft(manifestFile) == beforeManifest);
    CHECK(document.GetDraft(QStringLiteral("Lighting.lurender.json")).isEmpty() == true);
    workspace.Redo();
    CHECK(document.GetDraft(manifestFile) == afterManifest);
}

TEST_CASE("ReSTIR import preserves custom admission budgets", "[editor][render-graph][restir-library]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    const QString manifestFile = QStringLiteral("main.lushader.json");
    QJsonObject manifest = QJsonDocument::fromJson(document.GetDraft(manifestFile)).object();
    const QJsonObject budgets{{QStringLiteral("configurations"), 4},
                              {QStringLiteral("programs"), 24},
                              {QStringLiteral("artifacts"), 96},
                              {QStringLiteral("pipelines"), 32}};
    manifest.insert(QStringLiteral("budgets"), budgets);
    REQUIRE(document.SetText(manifestFile, EncodeCanonicalJson(manifest)) == RenderGraphDocumentStatus::Ok);
    QString error;
    REQUIRE(document.ApplyRestirTemplate(ROOT_GRAPH, QStringLiteral("Lighting"), error) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(QJsonDocument::fromJson(document.GetDraft(manifestFile)).object().value(QStringLiteral("budgets")) ==
          budgets);
}

TEST_CASE("ReSTIR import identifies conflicting authored inputs without changing the draft",
          "[editor][render-graph][restir-library]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    const QString manifestFile = QStringLiteral("main.lushader.json");
    QString error;
    REQUIRE(document.ApplyRestirTemplate(ROOT_GRAPH, QStringLiteral("Lighting"), error) ==
            RenderGraphDocumentStatus::Ok);
    QJsonObject manifest = QJsonDocument::fromJson(document.GetDraft(manifestFile)).object();
    QString expectedIdentity;
    SECTION("program definition")
    {
        QJsonArray programs = manifest.value(QStringLiteral("programs")).toArray();
        for (qsizetype index = 0; index < programs.size(); ++index)
        {
            QJsonObject program = programs.at(index).toObject();
            if (program.value(QStringLiteral("id")).toString() == QStringLiteral("RestirGenerate"))
            {
                expectedIdentity = program.value(QStringLiteral("id")).toString();
                program.insert(QStringLiteral("function"), QStringLiteral("GenerateCustomRay"));
                programs.replace(index, program);
            }
        }
        manifest.insert(QStringLiteral("programs"), programs);
        REQUIRE(document.SetText(manifestFile, EncodeCanonicalJson(manifest)) == RenderGraphDocumentStatus::Ok);
    }
    SECTION("source contents")
    {
        expectedIdentity = QStringLiteral("restir_di/main_shaders/reservoir.slang");
        REQUIRE(document.SetText(expectedIdentity, document.GetDraft(expectedIdentity) + "\n// Authored change.\n") ==
                RenderGraphDocumentStatus::Ok);
    }
    SECTION("setting definition")
    {
        QJsonArray settings = manifest.value(QStringLiteral("settings")).toArray();
        REQUIRE(settings.isEmpty() == false);
        QJsonObject setting = settings.first().toObject();
        expectedIdentity = setting.value(QStringLiteral("id")).toString();
        setting.insert(QStringLiteral("maximum"), 2);
        settings.replace(0, setting);
        manifest.insert(QStringLiteral("settings"), settings);
        REQUIRE(document.SetText(manifestFile, EncodeCanonicalJson(manifest)) == RenderGraphDocumentStatus::Ok);
    }
    REQUIRE(expectedIdentity.isEmpty() == false);
    RenderGraphSnapshot before;
    const QStringList files = document.GetFiles();
    for (const QString& file : files)
    {
        before.Files.insert(file, document.GetDraft(file));
    }
    const uint64 revision = document.GetRevision();
    CHECK(document.ApplyRestirTemplate(ROOT_GRAPH, QStringLiteral("OtherLighting"), error) ==
          RenderGraphDocumentStatus::Conflict);
    CHECK(error.contains(expectedIdentity) == true);
    CHECK(error.contains(QStringLiteral("draft was preserved")) == true);
    CHECK(document.GetRevision() == revision);
    CHECK(document.GetFiles() == files);
    for (const QString& file : files)
    {
        CHECK(document.GetDraft(file) == before.Files.value(file));
    }
}

TEST_CASE("ReSTIR library import preserves legacy callbacks and extends a custom payload",
          "[editor][render-graph][restir-library]")
{
    QTemporaryDir package;
    const QString source = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/raytracing/");
    const QString manifestFile = QStringLiteral("main.lushader.json");
    REQUIRE(QDir().mkpath(QDir(package.path()).filePath(QStringLiteral("main_shaders"))) == true);
    for (const QString& name : {ROOT_GRAPH,
                                QStringLiteral("main_shaders/geometry.slang"),
                                QStringLiteral("main_shaders/color.slang"),
                                QStringLiteral("main_shaders/depth.slang"),
                                QStringLiteral("main_shaders/generate.slang"),
                                QStringLiteral("main_shaders/miss.slang"),
                                QStringLiteral("main_shaders/closest.slang")})
    {
        REQUIRE(QFile::copy(source + name, QDir(package.path()).filePath(name)) == true);
    }
    QFile sourceManifest(source + manifestFile + QStringLiteral(".in"));
    REQUIRE(sourceManifest.open(QIODevice::ReadOnly) == true);
    QJsonObject manifest =
        QJsonDocument::fromJson(sourceManifest.readAll().replace("@RG2_TEST_PROFILES@", "[\"metal\",\"wgsl\"]"))
            .object();
    QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
    for (qsizetype index = 0; index < techniques.size(); ++index)
    {
        QJsonObject technique = techniques.at(index).toObject();
        if (technique.value(QStringLiteral("id")).toString() == QStringLiteral("RendererTriangles.RayTracing"))
        {
            technique.insert(QStringLiteral("scene_interface"),
                             QJsonObject{{QStringLiteral("version"), 1},
                                         {QStringLiteral("surface_queries"), 1},
                                         {QStringLiteral("visibility_queries"), 1},
                                         {QStringLiteral("payload"), QStringLiteral("custom_payload.slang")}});
            techniques.replace(index, technique);
        }
    }
    manifest.insert(QStringLiteral("techniques"), techniques);
    const QByteArray customPayload = QByteArrayLiteral("struct ScenePayload { float3 Radiance; uint BounceCount; };\n");
    QFile payload(QDir(package.path()).filePath(QStringLiteral("custom_payload.slang")));
    REQUIRE(payload.open(QIODevice::WriteOnly) == true);
    REQUIRE(payload.write(customPayload) == customPayload.size());
    payload.close();
    QFile destinationManifest(QDir(package.path()).filePath(manifestFile));
    REQUIRE(destinationManifest.open(QIODevice::WriteOnly) == true);
    REQUIRE(destinationManifest.write(EncodeCanonicalJson(manifest)) > 0);
    destinationManifest.close();
    RenderGraphDocument document;
    REQUIRE(document.Load(package.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QByteArray oldClosest = document.GetDraft(QStringLiteral("main_shaders/closest.slang"));
    const QByteArray oldGenerate = document.GetDraft(QStringLiteral("main_shaders/generate.slang"));
    QString error;
    REQUIRE(document.ApplyRestirTemplate(ROOT_GRAPH, QStringLiteral("Lighting"), error) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(document.GetDraft(QStringLiteral("custom_payload.slang")) == customPayload);
    CHECK(document.GetDraft(QStringLiteral("main_shaders/closest.slang")) == oldClosest);
    CHECK(document.GetDraft(QStringLiteral("main_shaders/generate.slang")) == oldGenerate);
    const QByteArray mergedPayload = document.GetDraft(QStringLiteral("restir_di/main_shaders/payload.slang"));
    CHECK(mergedPayload.contains("BounceCount") == true);
    CHECK(mergedPayload.contains("RayLaunch Launch;") == true);
    // Both the old legacy pipeline and new ordered pipeline remain buildable root configurations.
    QJsonObject root;
    REQUIRE(document.GetGraph(ROOT_GRAPH, root) == true);
    QJsonArray configurations = root.value(QStringLiteral("configurations")).toArray();
    QJsonObject original = configurations.first().toObject();
    QJsonObject lighting = original;
    lighting.insert(QStringLiteral("id"), QStringLiteral("Lighting"));
    QJsonArray originalNodes;
    QJsonArray lightingNodes;
    for (const QJsonValueConstRef value : original.value(QStringLiteral("nodes")).toArray())
    {
        const QJsonObject node = value.toObject();
        if (node.value(QStringLiteral("id")).toString() == QStringLiteral("Lighting"))
        {
            continue;
        }
        originalNodes.append(node);
        lightingNodes.append(node.value(QStringLiteral("id")).toString() == QStringLiteral("trace")
                                 ? QJsonObject{{QStringLiteral("id"), QStringLiteral("trace")},
                                               {QStringLiteral("kind"), QStringLiteral("Subgraph")},
                                               {QStringLiteral("asset"), QStringLiteral("Lighting.lurender.json")},
                                               {QStringLiteral("configuration"), QStringLiteral("Lighting")}}
                                 : node);
    }
    original.insert(QStringLiteral("nodes"), originalNodes);
    lighting.insert(QStringLiteral("nodes"), lightingNodes);
    configurations.replace(0, original);
    configurations.append(lighting);
    root.insert(QStringLiteral("configurations"), configurations);
    REQUIRE(document.SetText(ROOT_GRAPH, EncodeCanonicalJson(root)) == RenderGraphDocumentStatus::Ok);
    const QString fixture = qEnvironmentVariable("LUDUS_RESTIR_LEGACY_FIXTURE");
    if (fixture.isEmpty() == false)
    {
        REQUIRE(document.WriteSnapshot(fixture) == RenderGraphDocumentStatus::Ok);
    }
}

TEST_CASE("Visibility configurations apply from the canvas and preserve authored sources",
          "[editor][render-graph][visibility-configuration]")
{
    QString identity;
    SECTION("visibility buffer and depth")
    {
        identity = QStringLiteral("visibility-buffer");
    }
    SECTION("visibility and secondary rays")
    {
        identity = QStringLiteral("visibility-secondary-rays");
    }
    SECTION("visibility and tiled direct lighting")
    {
        identity = QStringLiteral("tiled-vbuffer-nee");
    }
    const bool hasSecondaryRays = identity == QStringLiteral("visibility-secondary-rays");
    const bool hasTiledLighting = identity == QStringLiteral("tiled-vbuffer-nee");
    const bool hasRays = hasSecondaryRays == true || hasTiledLighting == true;
    QTemporaryDir project;
    QWidget properties;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    QWidget* inspector = workspace.TakeInspectorPanel(&properties);
    inspector->hide();
    properties.hide();
    workspace.show();
    const QJsonObject originalRoot = QJsonDocument::fromJson(workspace.GetDocument().GetDraft(ROOT_GRAPH)).object();
    REQUIRE(workspace.CreateSubgraph(QStringLiteral("VisibilityLibrary")) == true);
    REQUIRE(workspace.CreateConfiguration(QStringLiteral("Visibility")) == true);
    RenderGraphDocument& document = workspace.GetDocument();
    const QString library = QStringLiteral("VisibilityLibrary.lurender.json");
    const QString manifest = QStringLiteral("main.lushader.json");
    const QByteArray beforeLibrary = document.GetDraft(library);
    const QByteArray beforeManifest = document.GetDraft(manifest);
    const QByteArray beforeGeometry = document.GetDraft(QStringLiteral("main_shaders/geometry.slang"));
    const QStringList beforeFiles = document.GetFiles();
    QTimer::singleShot(0, &workspace, [&workspace, &identity]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* actionOrNull =
            menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphApplyConfigurationTemplate"));
        REQUIRE(actionOrNull != nullptr);
        menuOrNull->close();
        QTimer::singleShot(0, &workspace, [&identity]() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialogOrNull != nullptr);
            QComboBox* choiceOrNull =
                dialogOrNull->findChild<QComboBox*>(QStringLiteral("renderGraphConfigurationTemplate"));
            REQUIRE(choiceOrNull != nullptr);
            const int32 selected = choiceOrNull->findData(identity);
            REQUIRE(selected >= 0);
            choiceOrNull->setCurrentIndex(selected);
            QLineEdit* nameOrNull =
                dialogOrNull->findChild<QLineEdit*>(QStringLiteral("renderGraphTemplateConfiguration"));
            REQUIRE(nameOrNull != nullptr);
            CHECK(nameOrNull->text() == QStringLiteral("Visibility"));
            QDialogButtonBox* buttonsOrNull = dialogOrNull->findChild<QDialogButtonBox*>();
            REQUIRE(buttonsOrNull != nullptr);
            buttonsOrNull->button(QDialogButtonBox::Apply)->click();
            CHECK(dialogOrNull->isVisible() == false);
            dialogOrNull->reject();
        });
        actionOrNull->trigger();
    });
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.centerOn(canvas.scene()->itemsBoundingRect().bottomRight() + QPointF(100, 100));
    QContextMenuEvent context(QContextMenuEvent::Mouse,
                              canvas.viewport()->rect().center(),
                              canvas.viewport()->mapToGlobal(canvas.viewport()->rect().center()));
    QApplication::sendEvent(canvas.viewport(), &context);
    CHECK(inspector->isVisible() == false);
    CHECK(canvas.GetConfiguration() == QStringLiteral("Visibility"));
    REQUIRE(document.GetDraft(library).contains("VisibilityGeometry") == true);
    CHECK(document.GetDraft(library).contains("VisibilitySecondaryGenerate") == hasSecondaryRays);
    CHECK(document.GetDraft(library).contains("TiledNeeShade") == hasTiledLighting);
    // The tiles, prefix and shade callbacks arrive with the table header they include.
    CHECK(document.GetDraft(QStringLiteral("tiled_vbuffer_nee/main_shaders/tile_table.slang")).isEmpty() ==
          (hasTiledLighting == false));
    if (hasTiledLighting == true)
    {
        const QJsonArray techniques =
            QJsonDocument::fromJson(document.GetDraft(manifest)).object().value(QStringLiteral("techniques")).toArray();
        bool hasLightQuery = false;
        for (const QJsonValueConstRef technique : techniques)
        {
            const QJsonObject interface = technique.toObject().value(QStringLiteral("scene_interface")).toObject();
            hasLightQuery = hasLightQuery || (interface.value(QStringLiteral("visibility_buffer")).toBool() == true &&
                                              interface.value(QStringLiteral("visibility_queries")).toInt() >= 1);
        }
        CHECK(hasLightQuery == true);
    }
    CHECK(document.GetDraft(QStringLiteral("main_shaders/geometry.slang")) == beforeGeometry);
    CHECK(
        document.GetDraft(QStringLiteral("visibility_buffer/main_shaders/color.slang")).contains("EncodeVisibility") ==
        true);
    const QByteArray afterLibrary = document.GetDraft(library);
    const QByteArray afterManifest = document.GetDraft(manifest);
    workspace.Undo();
    CHECK(document.GetDraft(library) == beforeLibrary);
    CHECK(document.GetDraft(manifest) == beforeManifest);
    CHECK(document.GetFiles() == beforeFiles);
    workspace.Redo();
    CHECK(document.GetDraft(library) == afterLibrary);
    CHECK(document.GetDraft(manifest) == afterManifest);
    CHECK(workspace.ApplyConfigurationTemplate(QStringLiteral("Visibility"), identity) == false);
    CHECK(document.GetDraft(library) == afterLibrary);
    CHECK(document.GetDraft(manifest) == afterManifest);

    // Wire the selected reusable configuration through the parent's existing raster node.
    QJsonObject root;
    REQUIRE(document.GetGraph(ROOT_GRAPH, root) == true);
    QJsonArray configurations = root.value(QStringLiteral("configurations")).toArray();
    QJsonObject parent = configurations.first().toObject();
    QJsonArray nodes;
    for (const QJsonValueConstRef value : parent.value(QStringLiteral("nodes")).toArray())
    {
        QJsonObject node = value.toObject();
        if (node.value(QStringLiteral("kind")) == QStringLiteral("Subgraph"))
        {
            if (node.value(QStringLiteral("id")) != QStringLiteral("raster"))
            {
                continue;
            }
            node.insert(QStringLiteral("asset"), library);
            node.insert(QStringLiteral("configuration"), QStringLiteral("Visibility"));
        }
        nodes.append(node);
    }
    QJsonArray edges;
    for (const QJsonValueConstRef value : parent.value(QStringLiteral("edges")).toArray())
    {
        QJsonObject edge = value.toObject();
        if (hasRays == true && edge.value(QStringLiteral("to")) == QStringLiteral("raster.depth"))
        {
            continue;
        }
        if (hasRays == false)
        {
            if (edge.value(QStringLiteral("to")) == QStringLiteral("raster.color"))
                edge.insert(QStringLiteral("to"), QStringLiteral("raster.visibility"));
            if (edge.value(QStringLiteral("from")) == QStringLiteral("raster.color"))
                edge.insert(QStringLiteral("from"), QStringLiteral("raster.visibility"));
        }
        edges.append(edge);
    }
    parent.insert(QStringLiteral("nodes"), nodes);
    parent.insert(QStringLiteral("edges"), edges);
    configurations.replace(0, parent);
    QJsonObject originalConfiguration =
        originalRoot.value(QStringLiteral("configurations")).toArray().first().toObject();
    originalConfiguration.insert(QStringLiteral("id"), QStringLiteral("Original"));
    configurations.append(originalConfiguration);
    root.insert(QStringLiteral("configurations"), configurations);
    REQUIRE(document.SetText(ROOT_GRAPH, EncodeCanonicalJson(root)) == RenderGraphDocumentStatus::Ok);
    REQUIRE(workspace.Save() == true);
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    REQUIRE(workspace.Validate() == true);
    REQUIRE(finished.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(workspace.GetLastResult().Message.toStdString());
    CHECK(workspace.GetLastResult().IsSuccessful == true);
    RenderGraphDocument reloaded;
    REQUIRE(reloaded.Load(document.GetPackageRoot(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reloaded.GetDraft(library) == afterLibrary);
    const QString fixture = qEnvironmentVariable("LUDUS_VISIBILITY_CONFIGURATION_FIXTURE");
    if (fixture.isEmpty() == false)
    {
        const QString destination = QDir(fixture).filePath(identity);
        REQUIRE(QDir().mkpath(destination) == true);
        REQUIRE(document.WriteSnapshot(destination) == RenderGraphDocumentStatus::Ok);
    }
}

TEST_CASE("Root visibility insertion is atomic on library collisions and unsupported ray versions",
          "[editor][render-graph][visibility-configuration]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray beforeRoot = document.GetDraft(ROOT_GRAPH);
    const QByteArray beforeManifest = document.GetDraft(QStringLiteral("main.lushader.json"));
    REQUIRE(workspace.ApplyConfigurationTemplate(QStringLiteral("Visibility"), QStringLiteral("visibility-buffer")) ==
            true);
    CHECK(document.GetDraft(ROOT_GRAPH).contains("Visibility.lurender.json") == true);
    CHECK(document.GetDraft(QStringLiteral("Visibility.lurender.json")).contains("VisibilityGeometry") == true);
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
    CHECK(document.GetDraft(QStringLiteral("main.lushader.json")) == beforeManifest);
    workspace.Redo();
    const QByteArray insertedRoot = document.GetDraft(ROOT_GRAPH);
    QString error;
    CHECK(document.ApplyConfigurationTemplate(ROOT_GRAPH,
                                              QStringLiteral("Visibility"),
                                              QStringLiteral("visibility-buffer"),
                                              error) == RenderGraphDocumentStatus::Conflict);
    CHECK(document.GetDraft(ROOT_GRAPH) == insertedRoot);
    workspace.Undo();
    REQUIRE(workspace.ApplyRestirTemplate(QStringLiteral("Lighting")) == true);
    QJsonObject incompatible =
        QJsonDocument::fromJson(document.GetDraft(QStringLiteral("main.lushader.json"))).object();
    QJsonObject budgets = incompatible.value(QStringLiteral("budgets")).toObject();
    budgets.insert(QStringLiteral("programs"), 32);
    incompatible.insert(QStringLiteral("budgets"), budgets);
    QJsonArray techniques = incompatible.value(QStringLiteral("techniques")).toArray();
    for (qsizetype index = 0; index < techniques.size(); ++index)
    {
        QJsonObject technique = techniques.at(index).toObject();
        if (technique.value(QStringLiteral("id")) == QStringLiteral("RendererTriangles.RayTracing"))
        {
            QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
            interface.insert(QStringLiteral("version"), 2);
            technique.insert(QStringLiteral("scene_interface"), interface);
            techniques.replace(index, technique);
        }
    }
    incompatible.insert(QStringLiteral("techniques"), techniques);
    REQUIRE(document.SetText(QStringLiteral("main.lushader.json"), EncodeCanonicalJson(incompatible)) ==
            RenderGraphDocumentStatus::Ok);
    const QStringList files = document.GetFiles();
    const QByteArray restirManifest = document.GetDraft(QStringLiteral("main.lushader.json"));
    CHECK(document.ApplyConfigurationTemplate(ROOT_GRAPH,
                                              QStringLiteral("Reflections"),
                                              QStringLiteral("visibility-secondary-rays"),
                                              error) == RenderGraphDocumentStatus::Conflict);
    CHECK(error.contains(QStringLiteral("version")) == true);
    CHECK(document.GetDraft(QStringLiteral("main.lushader.json")) == restirManifest);
    CHECK(document.GetFiles() == files);
}

TEST_CASE("Visibility imports reuse compatible shader records and preserve conflicting edits",
          "[editor][render-graph][visibility-configuration]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    QString error;
    REQUIRE(document.ApplyConfigurationTemplate(ROOT_GRAPH,
                                                QStringLiteral("Visibility"),
                                                QStringLiteral("visibility-buffer"),
                                                error) == RenderGraphDocumentStatus::Ok);
    const QString shader = QStringLiteral("visibility_buffer/main_shaders/color.slang");
    const QByteArray original = document.GetDraft(shader);
    REQUIRE(document.ApplyConfigurationTemplate(ROOT_GRAPH,
                                                QStringLiteral("Reflections"),
                                                QStringLiteral("visibility-secondary-rays"),
                                                error) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetDraft(shader) == original);
    REQUIRE(document.ApplyConfigurationTemplate(ROOT_GRAPH,
                                                QStringLiteral("MoreReflections"),
                                                QStringLiteral("visibility-secondary-rays"),
                                                error) == RenderGraphDocumentStatus::Ok);
    SECTION("changed reserved source")
    {
        REQUIRE(document.SetText(shader, original + QByteArray("\n// Authored edit.\n")) ==
                RenderGraphDocumentStatus::Ok);
    }
    SECTION("incompatible shared payload")
    {
        REQUIRE(document.SetText(QStringLiteral("visibility_secondary_rays/main_shaders/payload.slang"),
                                 QByteArray("struct ScenePayload { float3 Changed; };\n")) ==
                RenderGraphDocumentStatus::Ok);
    }
    const QByteArray beforeRoot = document.GetDraft(ROOT_GRAPH);
    const QByteArray beforeManifest = document.GetDraft(QStringLiteral("main.lushader.json"));
    const QByteArray beforeShader = document.GetDraft(shader);
    const QStringList beforeFiles = document.GetFiles();
    CHECK(document.ApplyConfigurationTemplate(ROOT_GRAPH,
                                              QStringLiteral("Conflicting"),
                                              QStringLiteral("visibility-secondary-rays"),
                                              error) == RenderGraphDocumentStatus::Conflict);
    CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
    CHECK(document.GetDraft(QStringLiteral("main.lushader.json")) == beforeManifest);
    CHECK(document.GetDraft(shader) == beforeShader);
    CHECK(document.GetFiles() == beforeFiles);
}

TEST_CASE("Visibility import coexists with an authored ray configuration at default budgets",
          "[editor][render-graph][visibility-existing-package]")
{
    QTemporaryDir package;
    const QString existing = qEnvironmentVariable("LUDUS_VISIBILITY_EXISTING_PACKAGE");
    const QString source = existing.isEmpty() == true
                               ? QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/raytracing")
                               : existing;
    QDirIterator files(source, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext() == true)
    {
        const QString original = files.next();
        const QString relative = QDir(source).relativeFilePath(original);
        const QString destination = QDir(package.path()).filePath(relative);
        REQUIRE(QDir().mkpath(QFileInfo(destination).absolutePath()) == true);
        REQUIRE(QFile::copy(original, destination) == true);
    }
    const QString manifestFile = QStringLiteral("main.lushader.json");
    if (existing.isEmpty() == true)
    {
        QFile input(QDir(package.path()).filePath(manifestFile + QStringLiteral(".in")));
        REQUIRE(input.open(QIODevice::ReadOnly) == true);
        QJsonObject manifest =
            QJsonDocument::fromJson(input.readAll().replace("@RG2_TEST_PROFILES@", "[\"metal\",\"wgsl\"]")).object();
        QJsonArray programs = manifest.value(QStringLiteral("programs")).toArray();
        QJsonObject extra = programs.at(1).toObject();
        extra.insert(QStringLiteral("id"), QStringLiteral("CustomColor"));
        programs.append(extra);
        manifest.insert(QStringLiteral("programs"), programs);
        QFile output(QDir(package.path()).filePath(manifestFile));
        REQUIRE(output.open(QIODevice::WriteOnly) == true);
        REQUIRE(output.write(EncodeCanonicalJson(manifest)) > 0);
    }
    RenderGraphDocument document;
    REQUIRE(document.Load(package.path(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    RenderGraphSnapshot before;
    for (const QString& file : document.GetFiles())
    {
        before.Files.insert(file, document.GetDraft(file));
    }
    const QString library = document.GetFiles().contains(QStringLiteral("raytracing.lurender.json")) == true
                                ? QStringLiteral("raytracing.lurender.json")
                                : ROOT_GRAPH;
    QString error;
    const RenderGraphDocumentStatus status =
        document.ApplyConfigurationTemplate(library,
                                            QStringLiteral("Hybrid"),
                                            QStringLiteral("visibility-secondary-rays"),
                                            error);
    INFO(error.toStdString());
    REQUIRE(status == RenderGraphDocumentStatus::Ok);
    for (const QString& file : before.Files.keys())
    {
        if (file.endsWith(QStringLiteral(".slang")) == true)
        {
            CHECK(document.GetDraft(file) == before.Files.value(file));
        }
    }
    const QJsonObject manifest = QJsonDocument::fromJson(document.GetDraft(manifestFile)).object();
    CHECK(manifest.value(QStringLiteral("budgets")).toObject().value(QStringLiteral("programs")).toInt() == 16);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetFiles() == before.Files.keys());
    for (const QString& file : before.Files.keys())
    {
        CHECK(document.GetDraft(file) == before.Files.value(file));
    }
    REQUIRE(document.Redo() == true);
    // Keep the original root and select Hybrid as a second root configuration.
    QJsonObject root = QJsonDocument::fromJson(before.Files.value(ROOT_GRAPH)).object();
    QJsonArray configurations = root.value(QStringLiteral("configurations")).toArray();
    QJsonObject hybrid = configurations.first().toObject();
    hybrid.insert(QStringLiteral("id"), QStringLiteral("Hybrid"));
    QJsonArray nodes = hybrid.value(QStringLiteral("nodes")).toArray();
    for (qsizetype index = 0; index < nodes.size(); ++index)
    {
        QJsonObject node = nodes.at(index).toObject();
        if (node.value(QStringLiteral("kind")) == QStringLiteral("RayTracingPipeline"))
        {
            node = QJsonObject{{QStringLiteral("id"), node.value(QStringLiteral("id"))},
                               {QStringLiteral("kind"), QStringLiteral("Subgraph")},
                               {QStringLiteral("asset"), QStringLiteral("Hybrid.lurender.json")},
                               {QStringLiteral("configuration"), QStringLiteral("Hybrid")}};
        }
        else if (node.value(QStringLiteral("asset")) == library)
        {
            node.insert(QStringLiteral("configuration"), QStringLiteral("Hybrid"));
        }
        nodes.replace(index, node);
    }
    hybrid.insert(QStringLiteral("nodes"), nodes);
    configurations.append(hybrid);
    root.insert(QStringLiteral("configurations"), configurations);
    REQUIRE(document.SetText(ROOT_GRAPH, EncodeCanonicalJson(root)) == RenderGraphDocumentStatus::Ok);
    const QString fixture = qEnvironmentVariable("LUDUS_VISIBILITY_EXISTING_FIXTURE");
    if (fixture.isEmpty() == false)
    {
        REQUIRE(QDir().mkpath(fixture) == true);
        REQUIRE(document.WriteSnapshot(fixture) == RenderGraphDocumentStatus::Ok);
    }
}

TEST_CASE("Visibility custom program limits identify the required budget and preserve the draft",
          "[editor][render-graph][visibility-configuration]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    const QString manifestFile = QStringLiteral("main.lushader.json");
    QJsonObject manifest = QJsonDocument::fromJson(document.GetDraft(manifestFile)).object();
    QJsonObject budgets = manifest.value(QStringLiteral("budgets")).toObject();
    budgets.insert(QStringLiteral("programs"), 4);
    manifest.insert(QStringLiteral("budgets"), budgets);
    REQUIRE(document.SetText(manifestFile, EncodeCanonicalJson(manifest)) == RenderGraphDocumentStatus::Ok);
    const QByteArray before = document.GetDraft(manifestFile);
    const QStringList files = document.GetFiles();
    QString error;
    CHECK(document.ApplyConfigurationTemplate(ROOT_GRAPH,
                                              QStringLiteral("Hybrid"),
                                              QStringLiteral("visibility-secondary-rays"),
                                              error) == RenderGraphDocumentStatus::Conflict);
    CHECK(error.contains(QStringLiteral("budgets.programs")) == true);
    CHECK(error.contains(QStringLiteral("needs 9")) == true);
    CHECK(document.GetDraft(manifestFile) == before);
    CHECK(document.GetFiles() == files);
}

TEST_CASE("ReSTIR import rejects a missing declared shader or transitive include before publication",
          "[editor][render-graph][restir-library][restir-sources]")
{
    const QString missing = GENERATE(QStringLiteral("miss.slang"),
                                     QStringLiteral("reservoir.slang"),
                                     QStringLiteral("emitter_sampling.slang"),
                                     QStringLiteral("initial_emitter_sampling.slang"));
    QTemporaryDir project;
    QTemporaryDir templates;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    const QString canonical =
        QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/modules/graphics/renderer/render_graphs/restir_di/");
    REQUIRE(QDir().mkpath(QDir(templates.path()).filePath(QStringLiteral("main_shaders"))) == true);
    for (const QString& file : {QStringLiteral("main.lushader.json.in"), QStringLiteral("library.lurender.json")})
    {
        REQUIRE(QFile::copy(canonical + file, QDir(templates.path()).filePath(file)) == true);
    }
    for (const QString& file : QDir(canonical + QStringLiteral("main_shaders")).entryList(QDir::Files))
    {
        if (file != missing)
        {
            REQUIRE(QFile::copy(canonical + QStringLiteral("main_shaders/") + file,
                                QDir(templates.path()).filePath(QStringLiteral("main_shaders/") + file)) == true);
        }
    }
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray beforeManifest = document.GetDraft(QStringLiteral("main.lushader.json"));
    RenderGraphSnapshot candidate;
    for (const QString& file : document.GetFiles())
    {
        candidate.Files.insert(file, document.GetDraft(file));
    }
    const QString library = QStringLiteral("raytracing.lurender.json");
    candidate.Files.insert(library,
                           EncodeCanonicalJson(QJsonObject{{QStringLiteral("version"), 2},
                                                           {QStringLiteral("id"), QStringLiteral("RayLibrary")},
                                                           {QStringLiteral("default"), QStringLiteral("RestirDi")},
                                                           {QStringLiteral("configurations"), QJsonArray{}}}));
    QString error;
    CHECK(PrepareRestirConfiguration(candidate,
                                     ROOT_GRAPH,
                                     library,
                                     QStringLiteral("RestirDi"),
                                     error,
                                     templates.path()) == RenderGraphDocumentStatus::NotFound);
    CHECK(error.contains(missing) == true);
    CHECK(document.GetDraft(QStringLiteral("main.lushader.json")) == beforeManifest);
    CHECK(document.GetDraft(library).isEmpty() == true);
}

TEST_CASE("Nested template shader directories are prepared before saved graph references change",
          "[editor][render-graph][restir-library][nested-save]")
{
    const bool isLinkedDirectory = GENERATE(false, true);
    QTemporaryDir project;
    QTemporaryDir outside;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.CustomizeBuiltIn() == true);
    RenderGraphDocument& document = workspace.GetDocument();
    const QString package = document.GetPackageRoot();
    const QString manifest = QStringLiteral("main.lushader.json");
    QFile savedManifest(QDir(package).filePath(manifest));
    REQUIRE(savedManifest.open(QIODevice::ReadOnly) == true);
    const QByteArray beforeManifest = savedManifest.readAll();
    savedManifest.close();
    QString error;
    REQUIRE(document.ApplyRestirTemplate(ROOT_GRAPH, QStringLiteral("RestirDi"), error) ==
            RenderGraphDocumentStatus::Ok);
    const QString parent = QDir(package).filePath(QStringLiteral("restir_di"));
    if (isLinkedDirectory == true)
    {
        REQUIRE(QFile::link(outside.path(), parent) == true);
    }
    else
    {
        QFile blocker(parent);
        REQUIRE(blocker.open(QIODevice::WriteOnly | QIODevice::NewOnly) == true);
        blocker.close();
    }
    QStringList saved;
    const RenderGraphDocumentStatus expected =
        isLinkedDirectory == true ? RenderGraphDocumentStatus::Conflict : RenderGraphDocumentStatus::WriteFailed;
    CHECK(document.Save(saved) == expected);
    CHECK(saved.isEmpty() == true);
    CHECK(document.GetSaved(manifest) == beforeManifest);
    REQUIRE(savedManifest.open(QIODevice::ReadOnly) == true);
    CHECK(savedManifest.readAll() == beforeManifest);
    savedManifest.close();
    CHECK(document.IsDirty() == true);
    CHECK(QFileInfo::exists(QDir(package).filePath(QStringLiteral("RestirDi.lurender.json"))) == false);
    CHECK(QDir(outside.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty() == true);
    REQUIRE(QFile::remove(parent) == true);
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    CHECK(QFileInfo::exists(QDir(parent).filePath(QStringLiteral("main_shaders/miss.slang"))) == true);
    RenderGraphDocument reloaded;
    REQUIRE(reloaded.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    for (const QString& file : document.GetFiles())
    {
        INFO(file.toStdString());
        CHECK(reloaded.GetDraft(file) == document.GetDraft(file));
    }
}

TEST_CASE("Template program budget approval is bounded and transactional", "[editor][render-graph][template-budget]")
{
    const QString identity = GENERATE(QStringLiteral("restir-di"),
                                      QStringLiteral("visibility-buffer"),
                                      QStringLiteral("visibility-secondary-rays"));
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    const QString manifestFile = QStringLiteral("main.lushader.json");
    QJsonObject manifest = QJsonDocument::fromJson(document.GetDraft(manifestFile)).object();
    QJsonObject budgets = manifest.value(QStringLiteral("budgets")).toObject();
    budgets.insert(QStringLiteral("programs"), 4);
    manifest.insert(QStringLiteral("budgets"), budgets);
    REQUIRE(document.SetText(manifestFile, EncodeCanonicalJson(manifest)) == RenderGraphDocumentStatus::Ok);
    const QByteArray beforeManifest = document.GetDraft(manifestFile);
    const QByteArray beforeRoot = document.GetDraft(ROOT_GRAPH);
    const QStringList beforeFiles = document.GetFiles();
    QString error;
    RenderGraphTemplateBudgetIncrease budget;
    CHECK(document.ApplyConfigurationTemplate(ROOT_GRAPH, QStringLiteral("Lighting"), identity, error, {}, &budget) !=
          RenderGraphDocumentStatus::Ok);
    CHECK(budget.CurrentPrograms == 4);
    REQUIRE(budget.RequiredPrograms > budget.CurrentPrograms);
    const int32 requiredPrograms = budget.RequiredPrograms;
    budget.ApprovedPrograms = requiredPrograms - 1;
    CHECK(document.ApplyConfigurationTemplate(ROOT_GRAPH, QStringLiteral("Lighting"), identity, error, {}, &budget) !=
          RenderGraphDocumentStatus::Ok);
    CHECK(document.GetDraft(manifestFile) == beforeManifest);
    CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
    CHECK(document.GetFiles() == beforeFiles);
    budget.ApprovedPrograms = requiredPrograms;
    SECTION("Approved import and budget share undo and redo")
    {
        REQUIRE(
            document.ApplyConfigurationTemplate(ROOT_GRAPH, QStringLiteral("Lighting"), identity, error, {}, &budget) ==
            RenderGraphDocumentStatus::Ok);
        const QByteArray afterManifest = document.GetDraft(manifestFile);
        const QJsonObject after = QJsonDocument::fromJson(afterManifest).object();
        // Unchanged defaults retain their pre-existing template upgrade policy.
        const QJsonObject afterBudgets = after.value(QStringLiteral("budgets")).toObject();
        CHECK(afterBudgets.value(QStringLiteral("programs")).toInt() == requiredPrograms);
        CHECK(after.value(QStringLiteral("programs")).toArray().size() == requiredPrograms);
        CHECK(budget.RequiredPrograms == 0);
        CHECK(QFileInfo::exists(QDir(document.GetPackageRoot()).filePath(QStringLiteral("Lighting.lurender.json"))) ==
              false);
        REQUIRE(document.Undo() == true);
        CHECK(document.GetDraft(manifestFile) == beforeManifest);
        CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
        CHECK(document.GetFiles() == beforeFiles);
        REQUIRE(document.Redo() == true);
        CHECK(document.GetDraft(manifestFile) == afterManifest);
    }
    SECTION("Later conflicts discard the approved budget increase")
    {
        QJsonObject conflicting = manifest;
        if (identity != QStringLiteral("visibility-buffer"))
        {
            QJsonArray techniques = conflicting.value(QStringLiteral("techniques")).toArray();
            techniques.append(
                QJsonObject{{QStringLiteral("id"), QStringLiteral("RendererTriangles.RayTracing")},
                            {QStringLiteral("scene_interface"), QJsonObject{{QStringLiteral("version"), 2}}}});
            conflicting.insert(QStringLiteral("techniques"), techniques);
        }
        else
        {
            // A nonempty destination fails before committing any approved increase.
            budget.RequiredPrograms = requiredPrograms;
            CHECK(document.ApplyConfigurationTemplate(QStringLiteral("rasterize.lurender.json"),
                                                      QStringLiteral("ColorAndDepth"),
                                                      identity,
                                                      error,
                                                      {},
                                                      &budget) != RenderGraphDocumentStatus::Ok);
            CHECK(budget.RequiredPrograms == 0);
            CHECK(document.GetDraft(manifestFile) == beforeManifest);
            CHECK(document.GetFiles() == beforeFiles);
            return;
        }
        REQUIRE(document.SetText(manifestFile, EncodeCanonicalJson(conflicting)) == RenderGraphDocumentStatus::Ok);
        const QByteArray beforeConflict = document.GetDraft(manifestFile);
        CHECK(
            document.ApplyConfigurationTemplate(ROOT_GRAPH, QStringLiteral("Lighting"), identity, error, {}, &budget) !=
            RenderGraphDocumentStatus::Ok);
        CHECK(document.GetDraft(manifestFile) == beforeConflict);
        CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
        CHECK(document.GetFiles() == beforeFiles);
    }
}

TEST_CASE("Canvas template picker confirms a shader program budget increase", "[editor][render-graph][template-budget]")
{
    const bool shouldApprove = GENERATE(false, true);
    QTemporaryDir project;
    QWidget properties;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    QWidget* inspector = workspace.TakeInspectorPanel(&properties);
    inspector->hide();
    properties.hide();
    workspace.show();
    RenderGraphDocument& document = workspace.GetDocument();
    const QString manifestFile = QStringLiteral("main.lushader.json");
    QJsonObject manifest = QJsonDocument::fromJson(document.GetDraft(manifestFile)).object();
    QJsonObject budgets = manifest.value(QStringLiteral("budgets")).toObject();
    budgets.insert(QStringLiteral("programs"), 16);
    manifest.insert(QStringLiteral("budgets"), budgets);
    QJsonArray programs = manifest.value(QStringLiteral("programs")).toArray();
    while (programs.size() < 12)
    {
        QJsonObject program = programs.first().toObject();
        program.insert(QStringLiteral("id"), QStringLiteral("Custom%1").arg(programs.size()));
        programs.append(program);
    }
    manifest.insert(QStringLiteral("programs"), programs);
    REQUIRE(document.SetText(manifestFile, EncodeCanonicalJson(manifest)) == RenderGraphDocumentStatus::Ok);
    const QByteArray beforeManifest = document.GetDraft(manifestFile);
    const QByteArray beforeRoot = document.GetDraft(ROOT_GRAPH);
    const QStringList beforeFiles = document.GetFiles();
    QTimer::singleShot(0, &workspace, [&workspace, shouldApprove]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* actionOrNull =
            menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphApplyConfigurationTemplate"));
        REQUIRE(actionOrNull != nullptr);
        menuOrNull->close();
        QTimer::singleShot(0, &workspace, [&workspace, shouldApprove]() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialogOrNull != nullptr);
            QComboBox* choiceOrNull =
                dialogOrNull->findChild<QComboBox*>(QStringLiteral("renderGraphConfigurationTemplate"));
            REQUIRE(choiceOrNull != nullptr);
            choiceOrNull->setCurrentIndex(choiceOrNull->findData(QStringLiteral("visibility-secondary-rays")));
            QTimer::singleShot(0, &workspace, [shouldApprove]() {
                QMessageBox* confirmationOrNull = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                REQUIRE(confirmationOrNull != nullptr);
                CHECK(confirmationOrNull->objectName() == QStringLiteral("renderGraphTemplateBudgetConfirmation"));
                CHECK(confirmationOrNull->text().contains(QStringLiteral("from 16 to 17")) == true);
                CHECK(confirmationOrNull->defaultButton() == confirmationOrNull->button(QMessageBox::Cancel));
                if (shouldApprove == true)
                {
                    for (QAbstractButton* button : confirmationOrNull->buttons())
                    {
                        if (confirmationOrNull->buttonRole(button) == QMessageBox::AcceptRole)
                        {
                            button->click();
                            return;
                        }
                    }
                    FAIL("Increase and apply button is missing");
                }
                else
                {
                    confirmationOrNull->button(QMessageBox::Cancel)->click();
                }
            });
            QDialogButtonBox* buttonsOrNull = dialogOrNull->findChild<QDialogButtonBox*>();
            REQUIRE(buttonsOrNull != nullptr);
            buttonsOrNull->button(QDialogButtonBox::Apply)->click();
            CHECK(dialogOrNull->isVisible() == (shouldApprove == false));
            dialogOrNull->reject();
        });
        actionOrNull->trigger();
    });
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.centerOn(canvas.scene()->itemsBoundingRect().bottomRight() + QPointF(100, 100));
    QContextMenuEvent context(QContextMenuEvent::Mouse,
                              canvas.viewport()->rect().center(),
                              canvas.viewport()->mapToGlobal(canvas.viewport()->rect().center()));
    QApplication::sendEvent(canvas.viewport(), &context);
    CHECK(inspector->isVisible() == false);
    if (shouldApprove == true)
    {
        const QByteArray afterManifest = document.GetDraft(manifestFile);
        CHECK(QJsonDocument::fromJson(afterManifest)
                  .object()
                  .value(QStringLiteral("budgets"))
                  .toObject()
                  .value(QStringLiteral("programs"))
                  .toInt() == 17);
        workspace.Undo();
        CHECK(document.GetDraft(manifestFile) == beforeManifest);
        CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
        CHECK(document.GetFiles() == beforeFiles);
        workspace.Redo();
        CHECK(document.GetDraft(manifestFile) == afterManifest);
    }
    else
    {
        CHECK(document.GetDraft(manifestFile) == beforeManifest);
        CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
        CHECK(document.GetFiles() == beforeFiles);
    }
}

namespace
{
const QString COMPUTE_TEMPLATE = QStringLiteral("compute-ray-tracing");
const QString COMPUTE_MANIFEST = QStringLiteral("main.lushader.json");

QJsonObject FindRayTechnique(const QByteArray& manifest)
{
    for (const QJsonValueConstRef value :
         QJsonDocument::fromJson(manifest).object().value(QStringLiteral("techniques")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")) == QStringLiteral("RendererTriangles.RayTracing"))
        {
            return value.toObject();
        }
    }
    return {};
}

// Replace the root's raster Subgraph with the inserted one: same scene, view and color wiring.
void ReplaceRasterSubgraph(RenderGraphDocument& inoutDocument, const QString& subgraph)
{
    QJsonObject root;
    REQUIRE(inoutDocument.GetGraph(ROOT_GRAPH, root) == true);
    QJsonArray configurations = root.value(QStringLiteral("configurations")).toArray();
    QJsonObject parent = configurations.first().toObject();
    QJsonArray nodes;
    for (const QJsonValueConstRef value : parent.value(QStringLiteral("nodes")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")) != QStringLiteral("raster"))
        {
            nodes.append(value);
        }
    }
    QJsonArray edges;
    for (const QJsonValueConstRef value : parent.value(QStringLiteral("edges")).toArray())
    {
        QJsonObject edge = value.toObject();
        if (edge.value(QStringLiteral("to")) == QStringLiteral("raster.depth"))
        {
            continue;
        }
        for (const QString& end : {QStringLiteral("from"), QStringLiteral("to")})
        {
            edge.insert(end,
                        edge.value(end).toString().replace(QStringLiteral("raster."), subgraph + QLatin1Char('.')));
        }
        edges.append(edge);
    }
    parent.insert(QStringLiteral("nodes"), nodes);
    parent.insert(QStringLiteral("edges"), edges);
    configurations.replace(0, parent);
    root.insert(QStringLiteral("configurations"), configurations);
    REQUIRE(inoutDocument.SetText(ROOT_GRAPH, EncodeCanonicalJson(root)) == RenderGraphDocumentStatus::Ok);
}

// Choose a template in File -> New Render Graph and wait for its creation job.
void CreateGraphFromPicker(RendererWorkspace& inoutWorkspace, const QString& identity)
{
    QSignalSpy finished(&inoutWorkspace, &RendererWorkspace::JobFinished);
    QTimer::singleShot(0, &inoutWorkspace, [identity]() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        QComboBox* templateOrNull = dialogOrNull->findChild<QComboBox*>(QStringLiteral("newRenderGraphTemplate"));
        REQUIRE(templateOrNull != nullptr);
        const int32 selected = templateOrNull->findData(identity);
        REQUIRE(selected >= 0);
        templateOrNull->setCurrentIndex(selected);
        dialogOrNull->accept();
    });
    REQUIRE(inoutWorkspace.PromptNewGraph() == true);
    REQUIRE(finished.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(inoutWorkspace.GetLastResult().Message.toStdString());
}

void RequireValidation(RendererWorkspace& inoutWorkspace)
{
    QSignalSpy finished(&inoutWorkspace, &RendererWorkspace::JobFinished);
    REQUIRE(inoutWorkspace.Validate() == true);
    REQUIRE(finished.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(inoutWorkspace.GetLastResult().Message.toStdString());
    CHECK(inoutWorkspace.GetLastResult().IsSuccessful == true);
}
} // namespace

TEST_CASE("Simple triangle-loop configuration imports one kernel and preserves edits on collision",
          "[editor][render-graph][simple-compute-template]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.CustomizeBuiltIn() == true);
    RenderGraphDocument& document = workspace.GetDocument();
    const QString identity = QStringLiteral("simple-compute-ray-tracing");
    const QString source = QStringLiteral("simple_compute_ray_tracing/main_shaders/trace.slang");
    const QByteArray beforeRoot = document.GetDraft(ROOT_GRAPH);
    const QByteArray beforeManifest = document.GetDraft(COMPUTE_MANIFEST);
    const QStringList beforeFiles = document.GetFiles();
    REQUIRE(workspace.ApplyConfigurationTemplate(QStringLiteral("Simple"), identity) == true);
    const QByteArray library = document.GetDraft(QStringLiteral("Simple.lurender.json"));
    CHECK(library.contains("SimpleComputeRayTrace") == true);
    CHECK(library.contains("Buffer") == false);
    CHECK(document.GetDraft(source).contains("TryIntersectTriangle") == true);
    const QByteArray afterManifest = document.GetDraft(COMPUTE_MANIFEST);
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
    CHECK(document.GetDraft(COMPUTE_MANIFEST) == beforeManifest);
    CHECK(document.GetFiles() == beforeFiles);
    workspace.Redo();
    CHECK(document.GetDraft(COMPUTE_MANIFEST) == afterManifest);
    REQUIRE(document.SetText(source, document.GetDraft(source) + "\n// Authored edit\n") ==
            RenderGraphDocumentStatus::Ok);
    const QByteArray editedSource = document.GetDraft(source);
    const QStringList existingFiles = document.GetFiles();
    QString error;
    CHECK(document.ApplyConfigurationTemplate(ROOT_GRAPH, QStringLiteral("Another"), identity, error) ==
          RenderGraphDocumentStatus::Conflict);
    CHECK(error.contains(QStringLiteral("different contents")) == true);
    CHECK(document.GetDraft(source) == editedSource);
    CHECK(document.GetDraft(COMPUTE_MANIFEST) == afterManifest);
    CHECK(document.GetFiles() == existingFiles);
}

TEST_CASE("Compute ray-tracing template is created from the editor picker", "[editor][render-graph][compute-template]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    workspace.show();
    CreateGraphFromPicker(workspace, COMPUTE_TEMPLATE);
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray library = document.GetDraft(QStringLiteral("library.lurender.json"));
    CHECK(library.contains("ComputePipeline") == true);
    CHECK(library.contains("RayTracingPipeline") == false);
    CHECK(library.contains("bytes_per_pixel") == true);
    CHECK(document.GetDraft(ROOT_GRAPH).contains("Accumulate") == true);
    CHECK(document.GetDraft(QStringLiteral("main_shaders/hit_record.slang")).contains("HIT_RECORD_WORDS") == true);
    const QJsonObject technique = FindRayTechnique(document.GetDraft(COMPUTE_MANIFEST));
    // Node-owned scene compute passes need no default callbacks.
    CHECK(technique.contains(QStringLiteral("ray_generation")) == false);
    CHECK(technique.value(QStringLiteral("scene_interface")).toObject().value(QStringLiteral("version")).toInt() == 1);
    RequireValidation(workspace);
}

TEST_CASE("Compute ray-tracing configuration inserts on the root canvas and validates",
          "[editor][render-graph][compute-configuration]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.CustomizeBuiltIn() == true);
    RenderGraphDocument& document = workspace.GetDocument();
    const QString library = QStringLiteral("Lighting.lurender.json");
    const QByteArray beforeRoot = document.GetDraft(ROOT_GRAPH);
    const QByteArray beforeManifest = document.GetDraft(COMPUTE_MANIFEST);
    const QStringList beforeFiles = document.GetFiles();
    REQUIRE(workspace.ApplyConfigurationTemplate(QStringLiteral("Lighting"), COMPUTE_TEMPLATE) == true);
    CHECK(document.GetDraft(ROOT_GRAPH).contains("Lighting.lurender.json") == true);
    CHECK(document.GetDraft(library).contains("ComputeRayPrimary") == true);
    CHECK(document.GetDraft(library).contains("ComputeRayShade") == true);
    // Both kernels arrive with the hit record header they include.
    for (const QString& source : {QStringLiteral("compute_ray_tracing/main_shaders/primary.slang"),
                                  QStringLiteral("compute_ray_tracing/main_shaders/shade.slang"),
                                  QStringLiteral("compute_ray_tracing/main_shaders/hit_record.slang")})
    {
        INFO(source.toStdString());
        CHECK(document.GetDraft(source).isEmpty() == false);
    }
    const QJsonObject interface =
        FindRayTechnique(document.GetDraft(COMPUTE_MANIFEST)).value(QStringLiteral("scene_interface")).toObject();
    CHECK(interface.value(QStringLiteral("version")).toInt() == 1);
    CHECK(interface.value(QStringLiteral("surface_queries")).toInt() >= 1);
    CHECK(interface.value(QStringLiteral("visibility_queries")).toInt() >= 1);
    const QByteArray afterRoot = document.GetDraft(ROOT_GRAPH);
    const QByteArray afterManifest = document.GetDraft(COMPUTE_MANIFEST);
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
    CHECK(document.GetDraft(COMPUTE_MANIFEST) == beforeManifest);
    CHECK(document.GetFiles() == beforeFiles);
    workspace.Redo();
    CHECK(document.GetDraft(ROOT_GRAPH) == afterRoot);
    CHECK(document.GetDraft(COMPUTE_MANIFEST) == afterManifest);
    QString error;
    CHECK(document.ApplyConfigurationTemplate(ROOT_GRAPH, QStringLiteral("Lighting"), COMPUTE_TEMPLATE, error) ==
          RenderGraphDocumentStatus::Conflict);
    CHECK(document.GetDraft(ROOT_GRAPH) == afterRoot);

    ReplaceRasterSubgraph(document, QStringLiteral("Lighting"));
    REQUIRE(workspace.Save() == true);
    RequireValidation(workspace);
}

TEST_CASE("Veach and Guibas MIS template is created from the editor picker with three compared outputs",
          "[editor][render-graph][veach-guibas-template]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    workspace.show();
    CreateGraphFromPicker(workspace, QStringLiteral("veach-guibas-mis"));
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray library = document.GetDraft(QStringLiteral("library.lurender.json"));
    for (const char* program : {"ShadeMultipleImportance", "ShadeLightSampling", "ShadeBsdfSampling"})
    {
        INFO(program);
        CHECK(library.contains(program) == true);
    }
    const QByteArray root = document.GetDraft(ROOT_GRAPH);
    // MIS is exported first, so the viewport shows it by default; each strategy accumulates separately.
    CHECK(root.indexOf("\"output\"") < root.indexOf("\"lightSamplingOutput\""));
    CHECK(root.count("\"Accumulate\"") == 3);
    CHECK(root.count("\"Export\"") == 3);
    CHECK(document.GetDraft(QStringLiteral("main_shaders/multiple_importance.slang")).contains("ComputeMisWeight") ==
          true);
    CHECK(document.GetDraft(COMPUTE_MANIFEST).contains("MIS_HEURISTIC") == true);
    RequireValidation(workspace);
}

TEST_CASE("Veach and Guibas MIS configuration imports its shade passes and settings on the root canvas",
          "[editor][render-graph][veach-guibas-configuration]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.CustomizeBuiltIn() == true);
    RenderGraphDocument& document = workspace.GetDocument();
    const QString identity = QStringLiteral("veach-guibas-mis");
    const QByteArray beforeRoot = document.GetDraft(ROOT_GRAPH);
    const QByteArray beforeManifest = document.GetDraft(COMPUTE_MANIFEST);
    const QStringList beforeFiles = document.GetFiles();
    REQUIRE(workspace.ApplyConfigurationTemplate(QStringLiteral("Mis"), identity) == true);
    const QByteArray library = document.GetDraft(QStringLiteral("Mis.lurender.json"));
    for (const char* program : {"VeachMisPrimary",
                                "VeachMisShadeMultipleImportance",
                                "VeachMisShadeLightSampling",
                                "VeachMisShadeBsdfSampling"})
    {
        INFO(program);
        CHECK(library.contains(program) == true);
    }
    CHECK(library.contains("lightSampling") == true);
    CHECK(library.contains("bsdfSampling") == true);
    for (const QString& source : {QStringLiteral("veach_guibas_mis/main_shaders/primary.slang"),
                                  QStringLiteral("veach_guibas_mis/main_shaders/shade.slang"),
                                  QStringLiteral("veach_guibas_mis/main_shaders/hit_record.slang"),
                                  QStringLiteral("veach_guibas_mis/main_shaders/multiple_importance.slang")})
    {
        INFO(source.toStdString());
        CHECK(document.GetDraft(source).isEmpty() == false);
    }
    const QByteArray afterManifest = document.GetDraft(COMPUTE_MANIFEST);
    for (const char* setting : {"MIS_HEURISTIC", "MIS_POWER_EXPONENT", "MIS_CUTOFF_THRESHOLD"})
    {
        INFO(setting);
        CHECK(afterManifest.contains(setting) == true);
    }
    const QByteArray afterRoot = document.GetDraft(ROOT_GRAPH);
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
    CHECK(document.GetDraft(COMPUTE_MANIFEST) == beforeManifest);
    CHECK(document.GetFiles() == beforeFiles);
    workspace.Redo();
    CHECK(document.GetDraft(ROOT_GRAPH) == afterRoot);
    CHECK(document.GetDraft(COMPUTE_MANIFEST) == afterManifest);

    ReplaceRasterSubgraph(document, QStringLiteral("Mis"));
    REQUIRE(workspace.Save() == true);
    RequireValidation(workspace);
}

TEST_CASE("Line-sampling template is created from the editor picker with Properties hidden",
          "[editor][render-graph][line-sampling-template]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    QWidget properties;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    QWidget* inspector = workspace.TakeInspectorPanel(&properties);
    inspector->hide();
    properties.hide();
    workspace.show();
    CreateGraphFromPicker(workspace, QStringLiteral("line-sampling"));
    CHECK(inspector->isVisible() == false);
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray library = document.GetDraft(QStringLiteral("library.lurender.json"));
    for (const char* program : {"Primary", "ShadeLineSampling", "ShadeVisibleFraction"})
    {
        INFO(program);
        CHECK(library.contains(program) == true);
    }
    // Every pass selects the software triangle loop, so the package needs no ray hardware.
    CHECK(library.count("\"TriangleLoop\"") == 3);
    CHECK(library.contains("NativeQuery") == false);
    const QByteArray root = document.GetDraft(ROOT_GRAPH);
    CHECK(root.indexOf("\"output\"") < root.indexOf("\"visibleFractionOutput\""));
    CHECK(root.count("\"Accumulate\"") == 2);
    CHECK(root.count("\"Export\"") == 2);
    CHECK(document.GetDraft(QStringLiteral("main_shaders/line_sampling.slang")).contains("FindVisibleLineIntervals") ==
          true);
    const QJsonObject interface =
        FindRayTechnique(document.GetDraft(COMPUTE_MANIFEST)).value(QStringLiteral("scene_interface")).toObject();
    CHECK(interface.value(QStringLiteral("line_queries")).toInt() == 1);
    RequireValidation(workspace);
}

TEST_CASE("Line-sampling snippet applies from Quick Add on the canvas with Properties hidden",
          "[editor][render-graph][line-sampling-configuration]")
{
    QTemporaryDir project;
    QWidget properties;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.CustomizeBuiltIn() == true);
    QWidget* inspector = workspace.TakeInspectorPanel(&properties);
    inspector->hide();
    properties.hide();
    workspace.resize(TEMPLATE_WORKSPACE_WIDTH, TEMPLATE_WORKSPACE_HEIGHT);
    workspace.show();
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray beforeRoot = document.GetDraft(ROOT_GRAPH);
    const QByteArray beforeManifest = document.GetDraft(COMPUTE_MANIFEST);
    const QStringList beforeFiles = document.GetFiles();
    RenderGraphQuickAdd* paletteOrNull =
        workspace.findChild<RenderGraphQuickAdd*>(QStringLiteral("renderGraphQuickAdd"));
    REQUIRE(paletteOrNull != nullptr);
    paletteOrNull->SetRecent({});
    paletteOrNull->SetPinned({});
    QString offeredConfiguration;
    QTimer::singleShot(0, &workspace, [&offeredConfiguration]() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        QComboBox* choiceOrNull =
            dialogOrNull->findChild<QComboBox*>(QStringLiteral("renderGraphConfigurationTemplate"));
        REQUIRE(choiceOrNull != nullptr);
        CHECK(choiceOrNull->currentData().toString() == QStringLiteral("line-sampling"));
        QLineEdit* nameOrNull = dialogOrNull->findChild<QLineEdit*>(QStringLiteral("renderGraphTemplateConfiguration"));
        REQUIRE(nameOrNull != nullptr);
        offeredConfiguration = nameOrNull->text();
        QDialogButtonBox* buttonsOrNull = dialogOrNull->findChild<QDialogButtonBox*>();
        REQUIRE(buttonsOrNull != nullptr);
        buttonsOrNull->button(QDialogButtonBox::Apply)->click();
        CHECK(dialogOrNull->isVisible() == false);
        dialogOrNull->reject();
    });
    REQUIRE(workspace.ShowQuickAdd(QPointF(0.0, 0.0)) == true);
    QLineEdit* searchOrNull = paletteOrNull->findChild<QLineEdit*>(QStringLiteral("renderGraphQuickAddSearch"));
    REQUIRE(searchOrNull != nullptr);
    searchOrNull->setText(QStringLiteral("line sampling"));
    paletteOrNull->SetKind(RenderGraphQuickAddKind::Snippets);
    REQUIRE(paletteOrNull->GetHighlightedKey() == QStringLiteral("snippet:line-sampling"));
    REQUIRE(paletteOrNull->InsertHighlighted() == true);
    CHECK(inspector->isVisible() == false);
    CHECK(offeredConfiguration == QStringLiteral("LineSampling"));
    const QByteArray library = document.GetDraft(QStringLiteral("LineSampling.lurender.json"));
    for (const char* program :
         {"LineSamplingPrimary", "LineSamplingShadeLineSampling", "LineSamplingShadeVisibleFraction"})
    {
        INFO(program);
        CHECK(library.contains(program) == true);
    }
    CHECK(library.contains("visibleFraction") == true);
    for (const QString& source : {QStringLiteral("line_sampling/main_shaders/primary.slang"),
                                  QStringLiteral("line_sampling/main_shaders/shade.slang"),
                                  QStringLiteral("line_sampling/main_shaders/hit_record.slang"),
                                  QStringLiteral("line_sampling/main_shaders/line_sampling.slang")})
    {
        INFO(source.toStdString());
        CHECK(document.GetDraft(source).isEmpty() == false);
    }
    const QJsonObject interface =
        FindRayTechnique(document.GetDraft(COMPUTE_MANIFEST)).value(QStringLiteral("scene_interface")).toObject();
    CHECK(interface.value(QStringLiteral("version")).toInt() == 1);
    CHECK(interface.value(QStringLiteral("surface_queries")).toInt() >= 1);
    CHECK(interface.value(QStringLiteral("visibility_queries")).toInt() >= 1);
    CHECK(interface.value(QStringLiteral("line_queries")).toInt() == 1);
    const QByteArray afterRoot = document.GetDraft(ROOT_GRAPH);
    const QByteArray afterManifest = document.GetDraft(COMPUTE_MANIFEST);
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
    CHECK(document.GetDraft(COMPUTE_MANIFEST) == beforeManifest);
    CHECK(document.GetFiles() == beforeFiles);
    workspace.Redo();
    CHECK(document.GetDraft(ROOT_GRAPH) == afterRoot);
    CHECK(document.GetDraft(COMPUTE_MANIFEST) == afterManifest);

    ReplaceRasterSubgraph(document, QStringLiteral("LineSampling"));
    REQUIRE(workspace.Save() == true);
    RequireValidation(workspace);
}

TEST_CASE("Line-sampling import raises only the line budget it declares",
          "[editor][render-graph][line-sampling-configuration]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    QString error;
    // Other scene compute templates declare no line budget and leave the field absent.
    REQUIRE(document.ApplyConfigurationTemplate(ROOT_GRAPH, QStringLiteral("Compute"), COMPUTE_TEMPLATE, error) ==
            RenderGraphDocumentStatus::Ok);
    QJsonObject interface =
        FindRayTechnique(document.GetDraft(COMPUTE_MANIFEST)).value(QStringLiteral("scene_interface")).toObject();
    CHECK(interface.contains(QStringLiteral("line_queries")) == false);
    REQUIRE(document.ApplyConfigurationTemplate(ROOT_GRAPH,
                                                QStringLiteral("Lines"),
                                                QStringLiteral("line-sampling"),
                                                error) == RenderGraphDocumentStatus::Ok);
    interface =
        FindRayTechnique(document.GetDraft(COMPUTE_MANIFEST)).value(QStringLiteral("scene_interface")).toObject();
    CHECK(interface.value(QStringLiteral("line_queries")).toInt() == 1);
    // A larger authored line budget is retained by a later import.
    constexpr int32 AUTHORED_LINE_QUERIES = 4;
    QJsonObject manifest = QJsonDocument::fromJson(document.GetDraft(COMPUTE_MANIFEST)).object();
    QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
    for (qsizetype index = 0; index < techniques.size(); ++index)
    {
        QJsonObject technique = techniques.at(index).toObject();
        if (technique.value(QStringLiteral("id")) != QStringLiteral("RendererTriangles.RayTracing"))
        {
            continue;
        }
        interface.insert(QStringLiteral("line_queries"), AUTHORED_LINE_QUERIES);
        technique.insert(QStringLiteral("scene_interface"), interface);
        techniques.replace(index, technique);
    }
    manifest.insert(QStringLiteral("techniques"), techniques);
    REQUIRE(document.SetText(COMPUTE_MANIFEST, EncodeCanonicalJson(manifest)) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.ApplyConfigurationTemplate(ROOT_GRAPH,
                                                QStringLiteral("MoreLines"),
                                                QStringLiteral("line-sampling"),
                                                error) == RenderGraphDocumentStatus::Ok);
    CHECK(FindRayTechnique(document.GetDraft(COMPUTE_MANIFEST))
              .value(QStringLiteral("scene_interface"))
              .toObject()
              .value(QStringLiteral("line_queries"))
              .toInt() == AUTHORED_LINE_QUERIES);
}

TEST_CASE("RIS projected-solid-angle template is created from the editor picker with three compared outputs",
          "[editor][render-graph][ris-projected-solid-angle-template]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    workspace.show();
    CreateGraphFromPicker(workspace, QStringLiteral("ris-projected-solid-angle"));
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray library = document.GetDraft(QStringLiteral("library.lurender.json"));
    for (const char* program : {"Primary", "ShadeMultipleImportance", "ShadeLightSampling", "ShadeBsdfSampling"})
    {
        INFO(program);
        CHECK(library.contains(program) == true);
    }
    // Every pass walks the compute BVH, so the package needs no ray hardware.
    CHECK(library.count("\"ComputeBvh\"") == 4);
    CHECK(library.contains("NativeQuery") == false);
    const QByteArray root = document.GetDraft(ROOT_GRAPH);
    // The combined estimator is exported first, so the viewport shows it by default.
    CHECK(root.indexOf("\"output\"") < root.indexOf("\"lightSamplingOutput\""));
    CHECK(root.count("\"Accumulate\"") == 3);
    CHECK(root.count("\"Export\"") == 3);
    CHECK(document.GetDraft(QStringLiteral("main_shaders/projected_solid_angle_light.slang"))
              .contains("ComputeClampedOptimalMisWeights") == true);
    const QByteArray manifest = document.GetDraft(COMPUTE_MANIFEST);
    for (const char* setting : {"RIS_CANDIDATE_COUNT",
                                "RIS_NORMALIZATION_CANDIDATE_COUNT",
                                "RIS_TARGET",
                                "PROJECTED_SOLID_ANGLE_MIS_VISIBILITY"})
    {
        INFO(setting);
        CHECK(manifest.contains(setting) == true);
    }
    const QJsonObject interface = FindRayTechnique(manifest).value(QStringLiteral("scene_interface")).toObject();
    CHECK(interface.value(QStringLiteral("visibility_queries")).toInt() == 2);
    CHECK(interface.value(QStringLiteral("polygonal_light_sampling")).toBool() == true);
    RequireValidation(workspace);
}

TEST_CASE("RIS projected-solid-angle import enables the polygonal-light helpers on the shared ray technique",
          "[editor][render-graph][ris-projected-solid-angle-configuration]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    QString error;
    // Other scene compute templates leave the helper requirement absent.
    REQUIRE(document.ApplyConfigurationTemplate(ROOT_GRAPH, QStringLiteral("Compute"), COMPUTE_TEMPLATE, error) ==
            RenderGraphDocumentStatus::Ok);
    QJsonObject interface =
        FindRayTechnique(document.GetDraft(COMPUTE_MANIFEST)).value(QStringLiteral("scene_interface")).toObject();
    CHECK(interface.contains(QStringLiteral("polygonal_light_sampling")) == false);
    REQUIRE(document.ApplyConfigurationTemplate(ROOT_GRAPH,
                                                QStringLiteral("Ris"),
                                                QStringLiteral("ris-projected-solid-angle"),
                                                error) == RenderGraphDocumentStatus::Ok);
    const QByteArray library = document.GetDraft(QStringLiteral("Ris.lurender.json"));
    for (const char* program : {"RisProjectedSolidAnglePrimary",
                                "RisProjectedSolidAngleShadeMultipleImportance",
                                "RisProjectedSolidAngleShadeLightSampling",
                                "RisProjectedSolidAngleShadeBsdfSampling"})
    {
        INFO(program);
        CHECK(library.contains(program) == true);
    }
    for (const QString& source :
         {QStringLiteral("ris_projected_solid_angle/main_shaders/primary.slang"),
          QStringLiteral("ris_projected_solid_angle/main_shaders/shade.slang"),
          QStringLiteral("ris_projected_solid_angle/main_shaders/hit_record.slang"),
          QStringLiteral("ris_projected_solid_angle/main_shaders/resampled_light.slang"),
          QStringLiteral("ris_projected_solid_angle/main_shaders/projected_solid_angle_light.slang")})
    {
        INFO(source.toStdString());
        CHECK(document.GetDraft(source).isEmpty() == false);
    }
    interface =
        FindRayTechnique(document.GetDraft(COMPUTE_MANIFEST)).value(QStringLiteral("scene_interface")).toObject();
    CHECK(interface.value(QStringLiteral("visibility_queries")).toInt() >= 2);
    CHECK(interface.value(QStringLiteral("polygonal_light_sampling")).toBool() == true);
    // A later import of a template without the requirement keeps it.
    REQUIRE(document.ApplyConfigurationTemplate(ROOT_GRAPH,
                                                QStringLiteral("MoreCompute"),
                                                QStringLiteral("simple-compute-ray-tracing"),
                                                error) == RenderGraphDocumentStatus::Ok);
    CHECK(FindRayTechnique(document.GetDraft(COMPUTE_MANIFEST))
              .value(QStringLiteral("scene_interface"))
              .toObject()
              .value(QStringLiteral("polygonal_light_sampling"))
              .toBool() == true);
}

TEST_CASE("Polygonal-light-sampling template is created from the editor picker with four compared outputs",
          "[editor][render-graph][polygonal-light-sampling-template]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    workspace.show();
    CreateGraphFromPicker(workspace, QStringLiteral("polygonal-light-sampling"));
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray library = document.GetDraft(QStringLiteral("library.lurender.json"));
    for (const char* program :
         {"ShadeClampedOptimalMis", "ShadeBalanceHeuristic", "ShadeProjectedSolidAngle", "ShadeLtc"})
    {
        INFO(program);
        CHECK(library.contains(program) == true);
    }
    // Every pass walks the compute BVH, so the package needs no ray hardware.
    CHECK(library.count("\"ComputeBvh\"") == 4);
    CHECK(library.contains("NativeQuery") == false);
    const QByteArray root = document.GetDraft(ROOT_GRAPH);
    // Clamped optimal MIS is exported first, so the viewport shows it by default.
    CHECK(root.indexOf("\"output\"") < root.indexOf("\"balanceHeuristicOutput\""));
    CHECK(root.count("\"Accumulate\"") == 4);
    CHECK(root.count("\"Export\"") == 4);
    CHECK(document.GetDraft(QStringLiteral("main_shaders/polygonal_light.slang"))
              .contains("ComputePolygonalLightWeight") == true);
    const QByteArray manifest = document.GetDraft(COMPUTE_MANIFEST);
    CHECK(manifest.contains("PROJECTED_SOLID_ANGLE_MIS_VISIBILITY") == true);
    const QJsonObject interface = FindRayTechnique(manifest).value(QStringLiteral("scene_interface")).toObject();
    CHECK(interface.value(QStringLiteral("visibility_queries")).toInt() == 16);
    CHECK(interface.value(QStringLiteral("polygonal_light_sampling")).toBool() == true);
    RequireValidation(workspace);
}

TEST_CASE("Polygonal-light-sampling import prefixes its programs and raises the shared visibility budget",
          "[editor][render-graph][polygonal-light-sampling-configuration]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    QString error;
    REQUIRE(document.ApplyConfigurationTemplate(ROOT_GRAPH,
                                                QStringLiteral("Polygonal"),
                                                QStringLiteral("polygonal-light-sampling"),
                                                error) == RenderGraphDocumentStatus::Ok);
    const QByteArray library = document.GetDraft(QStringLiteral("Polygonal.lurender.json"));
    for (const char* program : {"PolygonalLightShadeClampedOptimalMis",
                                "PolygonalLightShadeBalanceHeuristic",
                                "PolygonalLightShadeProjectedSolidAngle",
                                "PolygonalLightShadeLtc"})
    {
        INFO(program);
        CHECK(library.contains(program) == true);
    }
    for (const QString& source : {QStringLiteral("polygonal_light_sampling/main_shaders/shade.slang"),
                                  QStringLiteral("polygonal_light_sampling/main_shaders/hit_record.slang"),
                                  QStringLiteral("polygonal_light_sampling/main_shaders/polygonal_light.slang")})
    {
        INFO(source.toStdString());
        CHECK(document.GetDraft(source).isEmpty() == false);
    }
    const QJsonObject interface =
        FindRayTechnique(document.GetDraft(COMPUTE_MANIFEST)).value(QStringLiteral("scene_interface")).toObject();
    CHECK(interface.value(QStringLiteral("visibility_queries")).toInt() == 16);
    CHECK(interface.value(QStringLiteral("polygonal_light_sampling")).toBool() == true);
}

namespace
{
/// The point-and-line MIS template's registry identity.
QString GetPointLineTemplate()
{
    return QStringLiteral("salesin-jarosz-lines");
}

/// A Graph setting's current value in a manifest draft.
float64 GetGraphSetting(const QByteArray& manifest, const QString& identity)
{
    for (const QJsonValueConstRef value :
         QJsonDocument::fromJson(manifest).object().value(QStringLiteral("settings")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == identity)
        {
            return value.toObject().value(QStringLiteral("value")).toDouble();
        }
    }
    FAIL("missing Graph setting " << identity.toStdString());
    return 0;
}

/// The imported template's PointLine-prefixed passes, settings and declared query budgets.
void CheckPointLineImport(const RenderGraphDocument& document, const QString& library)
{
    const QByteArray graph = document.GetDraft(library);
    for (const char* program : {"PointLinePrimary",
                                "PointLineShadeMultipleImportance",
                                "PointLineShadeLineSampling",
                                "PointLineShadePointSampling"})
    {
        INFO(program);
        CHECK(graph.contains(program) == true);
    }
    CHECK(graph.count("\"TriangleLoop\"") == 4);
    CHECK(graph.contains("NativeQuery") == false);
    CHECK(graph.contains("lineSampling") == true);
    CHECK(graph.contains("pointSampling") == true);
    for (const QString& source : {QStringLiteral("salesin_jarosz_lines/main_shaders/primary.slang"),
                                  QStringLiteral("salesin_jarosz_lines/main_shaders/shade.slang"),
                                  QStringLiteral("salesin_jarosz_lines/main_shaders/hit_record.slang"),
                                  QStringLiteral("salesin_jarosz_lines/main_shaders/line_sampling.slang"),
                                  QStringLiteral("salesin_jarosz_lines/main_shaders/multiple_importance.slang")})
    {
        INFO(source.toStdString());
        CHECK(document.GetDraft(source).isEmpty() == false);
    }
    const QByteArray manifest = document.GetDraft(COMPUTE_MANIFEST);
    CHECK(GetGraphSetting(manifest, QStringLiteral("POINT_LINE_MIS_HEURISTIC")) == 0);
    CHECK(GetGraphSetting(manifest, QStringLiteral("POINT_LINE_MIS_POWER_EXPONENT")) == 2);
    const QJsonObject interface = FindRayTechnique(manifest).value(QStringLiteral("scene_interface")).toObject();
    CHECK(interface.value(QStringLiteral("version")).toInt() == 1);
    CHECK(interface.value(QStringLiteral("surface_queries")).toInt() >= 1);
    CHECK(interface.value(QStringLiteral("visibility_queries")).toInt() >= 2);
    CHECK(interface.value(QStringLiteral("line_queries")).toInt() == 6);
}
} // namespace

TEST_CASE("Salesin and Jarosz template settings edit and save from the canvas with Properties hidden",
          "[editor][render-graph][point-line-template]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    QWidget properties;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    workspace.resize(TEMPLATE_WORKSPACE_WIDTH, TEMPLATE_WORKSPACE_HEIGHT);
    QWidget* inspector = workspace.TakeInspectorPanel(&properties);
    inspector->hide();
    properties.hide();
    workspace.show();
    CreateGraphFromPicker(workspace, GetPointLineTemplate());
    CHECK(inspector->isVisible() == false);
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray library = document.GetDraft(QStringLiteral("library.lurender.json"));
    for (const char* program : {"Primary", "ShadeMultipleImportance", "ShadeLineSampling", "ShadePointSampling"})
    {
        INFO(program);
        CHECK(library.contains(program) == true);
    }
    // Every pass selects the software triangle loop, so the package needs no ray hardware.
    CHECK(library.count("\"TriangleLoop\"") == 4);
    CHECK(library.contains("NativeQuery") == false);
    const QByteArray root = document.GetDraft(ROOT_GRAPH);
    // The combined image is exported first, so the viewport shows it by default.
    CHECK(root.indexOf("\"output\"") < root.indexOf("\"lineSamplingOutput\""));
    CHECK(root.indexOf("\"lineSamplingOutput\"") < root.indexOf("\"pointSamplingOutput\""));
    CHECK(root.count("\"Accumulate\"") == 3);
    CHECK(root.count("\"Export\"") == 3);
    CHECK(document.GetDraft(QStringLiteral("main_shaders/multiple_importance.slang")).contains("ComputeMisWeight") ==
          true);
    CHECK(document.GetDraft(QStringLiteral("main_shaders/line_sampling.slang")).contains("EvaluateLineDensity") ==
          true);
    const QJsonObject interface =
        FindRayTechnique(document.GetDraft(COMPUTE_MANIFEST)).value(QStringLiteral("scene_interface")).toObject();
    CHECK(interface.value(QStringLiteral("visibility_queries")).toInt() == 2);
    CHECK(interface.value(QStringLiteral("line_queries")).toInt() == 6);
    const QByteArray original = document.GetDraft(COMPUTE_MANIFEST);
    REQUIRE(GetGraphSetting(original, QStringLiteral("POINT_LINE_MIS_HEURISTIC")) == 0);

    // Background menu -> Edit Graph Settings...: choose the power heuristic and commit with Return.
    bool hasOpenedSettings = false;
    QTimer::singleShot(0, &workspace, [&workspace, &hasOpenedSettings]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* settingsOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphEditSettingsAction"));
        REQUIRE(settingsOrNull != nullptr);
        REQUIRE(settingsOrNull->isEnabled() == true);
        menuOrNull->close();
        QTimer::singleShot(0, &workspace, [&hasOpenedSettings]() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialogOrNull != nullptr);
            QSpinBox* heuristicOrNull =
                dialogOrNull->findChild<QSpinBox*>(QStringLiteral("renderGraphSetting_POINT_LINE_MIS_HEURISTIC"));
            REQUIRE(heuristicOrNull != nullptr);
            // Only balance (0) and power (1) are offered.
            CHECK(heuristicOrNull->maximum() == 1);
            heuristicOrNull->setFocus();
            heuristicOrNull->selectAll();
            QTest::keyClicks(heuristicOrNull, QStringLiteral("1"));
            QSignalSpy committed(heuristicOrNull, &QSpinBox::editingFinished);
            QTest::keyClick(heuristicOrNull, Qt::Key_Return);
            CHECK(committed.count() > 0);
            hasOpenedSettings = true;
            dialogOrNull->reject();
        });
        settingsOrNull->trigger();
    });
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const QPointF background = canvas.scene()->itemsBoundingRect().bottomRight() + QPointF(100, 100);
    canvas.centerOn(background);
    const QPoint position = canvas.mapFromScene(background);
    QContextMenuEvent context(QContextMenuEvent::Mouse, position, canvas.viewport()->mapToGlobal(position));
    QCoreApplication::sendEvent(canvas.viewport(), &context);
    REQUIRE(hasOpenedSettings == true);
    CHECK(inspector->isVisible() == false);
    CHECK(GetGraphSetting(document.GetDraft(COMPUTE_MANIFEST), QStringLiteral("POINT_LINE_MIS_HEURISTIC")) == 1);
    CHECK(document.GetSaved(COMPUTE_MANIFEST) == original);
    REQUIRE(workspace.CanUndo() == true);
    workspace.Undo();
    CHECK(GetGraphSetting(document.GetDraft(COMPUTE_MANIFEST), QStringLiteral("POINT_LINE_MIS_HEURISTIC")) == 0);
    workspace.Redo();
    CHECK(GetGraphSetting(document.GetDraft(COMPUTE_MANIFEST), QStringLiteral("POINT_LINE_MIS_HEURISTIC")) == 1);
    REQUIRE(workspace.Save() == true);
    RequireValidation(workspace);
    RenderGraphDocument reloaded;
    const QString path = QDir(project.path()).filePath(QStringLiteral("graphs/Main/main.lurender.json"));
    REQUIRE(reloaded.Load(QFileInfo(path).absolutePath(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(GetGraphSetting(reloaded.GetDraft(COMPUTE_MANIFEST), QStringLiteral("POINT_LINE_MIS_HEURISTIC")) == 1);
}

TEST_CASE("Salesin and Jarosz template settings edit and save through Properties",
          "[editor][render-graph][point-line-template]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    QWidget properties;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    QScrollArea* inspector = qobject_cast<QScrollArea*>(workspace.TakeInspectorPanel(&properties));
    REQUIRE(inspector != nullptr);
    QVBoxLayout* layout = new QVBoxLayout(&properties);
    layout->addWidget(inspector);
    inspector->show();
    workspace.show();
    CreateGraphFromPicker(workspace, GetPointLineTemplate());
    properties.resize(TEMPLATE_PROPERTIES_WIDTH, TEMPLATE_PROPERTIES_HEIGHT);
    properties.show();
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, QStringLiteral("Default"), QStringLiteral("output")) == true);
    QApplication::processEvents();
    QToolButton* sectionOrNull = inspector->findChild<QToolButton*>(QStringLiteral("renderGraphToggleSettings"));
    REQUIRE(sectionOrNull != nullptr);
    if (sectionOrNull->isChecked() == false)
    {
        sectionOrNull->click();
    }
    QApplication::processEvents();
    QSpinBox* heuristicOrNull =
        inspector->findChild<QSpinBox*>(QStringLiteral("renderGraphSetting_POINT_LINE_MIS_HEURISTIC"));
    QDoubleSpinBox* exponentOrNull =
        inspector->findChild<QDoubleSpinBox*>(QStringLiteral("renderGraphSetting_POINT_LINE_MIS_POWER_EXPONENT"));
    REQUIRE(heuristicOrNull != nullptr);
    REQUIRE(exponentOrNull != nullptr);
    CHECK(heuristicOrNull->isVisible() == true);
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray original = document.GetDraft(COMPUTE_MANIFEST);
    heuristicOrNull->setValue(1);
    Q_EMIT heuristicOrNull->editingFinished();
    constexpr float64 POWER_EXPONENT = 3;
    exponentOrNull->setValue(POWER_EXPONENT);
    Q_EMIT exponentOrNull->editingFinished();
    CHECK(GetGraphSetting(document.GetDraft(COMPUTE_MANIFEST), QStringLiteral("POINT_LINE_MIS_HEURISTIC")) == 1);
    CHECK(GetGraphSetting(document.GetDraft(COMPUTE_MANIFEST), QStringLiteral("POINT_LINE_MIS_POWER_EXPONENT")) ==
          POWER_EXPONENT);
    CHECK(document.GetSaved(COMPUTE_MANIFEST) == original);
    REQUIRE(workspace.CanUndo() == true);
    workspace.Undo();
    CHECK(GetGraphSetting(document.GetDraft(COMPUTE_MANIFEST), QStringLiteral("POINT_LINE_MIS_POWER_EXPONENT")) == 2);
    workspace.Redo();
    REQUIRE(workspace.Save() == true);
    CHECK(GetGraphSetting(document.GetSaved(COMPUTE_MANIFEST), QStringLiteral("POINT_LINE_MIS_HEURISTIC")) == 1);
    CHECK(GetGraphSetting(document.GetSaved(COMPUTE_MANIFEST), QStringLiteral("POINT_LINE_MIS_POWER_EXPONENT")) ==
          POWER_EXPONENT);
    RequireValidation(workspace);
}

TEST_CASE("Salesin and Jarosz snippet applies from Quick Add on the canvas with Properties hidden",
          "[editor][render-graph][point-line-configuration]")
{
    QTemporaryDir project;
    QWidget properties;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.CustomizeBuiltIn() == true);
    QWidget* inspector = workspace.TakeInspectorPanel(&properties);
    inspector->hide();
    properties.hide();
    workspace.resize(TEMPLATE_WORKSPACE_WIDTH, TEMPLATE_WORKSPACE_HEIGHT);
    workspace.show();
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray beforeRoot = document.GetDraft(ROOT_GRAPH);
    const QByteArray beforeManifest = document.GetDraft(COMPUTE_MANIFEST);
    const QStringList beforeFiles = document.GetFiles();
    RenderGraphQuickAdd* paletteOrNull =
        workspace.findChild<RenderGraphQuickAdd*>(QStringLiteral("renderGraphQuickAdd"));
    REQUIRE(paletteOrNull != nullptr);
    paletteOrNull->SetRecent({});
    paletteOrNull->SetPinned({});
    QString offeredConfiguration;
    QTimer::singleShot(0, &workspace, [&offeredConfiguration]() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        QComboBox* choiceOrNull =
            dialogOrNull->findChild<QComboBox*>(QStringLiteral("renderGraphConfigurationTemplate"));
        REQUIRE(choiceOrNull != nullptr);
        CHECK(choiceOrNull->currentData().toString() == GetPointLineTemplate());
        QLineEdit* nameOrNull = dialogOrNull->findChild<QLineEdit*>(QStringLiteral("renderGraphTemplateConfiguration"));
        REQUIRE(nameOrNull != nullptr);
        offeredConfiguration = nameOrNull->text();
        QDialogButtonBox* buttonsOrNull = dialogOrNull->findChild<QDialogButtonBox*>();
        REQUIRE(buttonsOrNull != nullptr);
        buttonsOrNull->button(QDialogButtonBox::Apply)->click();
        CHECK(dialogOrNull->isVisible() == false);
        dialogOrNull->reject();
    });
    REQUIRE(workspace.ShowQuickAdd(QPointF(0.0, 0.0)) == true);
    QLineEdit* searchOrNull = paletteOrNull->findChild<QLineEdit*>(QStringLiteral("renderGraphQuickAddSearch"));
    REQUIRE(searchOrNull != nullptr);
    searchOrNull->setText(QStringLiteral("salesin"));
    paletteOrNull->SetKind(RenderGraphQuickAddKind::Snippets);
    REQUIRE(paletteOrNull->GetHighlightedKey() == QStringLiteral("snippet:salesin-jarosz-lines"));
    REQUIRE(paletteOrNull->InsertHighlighted() == true);
    CHECK(inspector->isVisible() == false);
    CHECK(offeredConfiguration == QStringLiteral("PointLineMis"));
    CheckPointLineImport(document, QStringLiteral("PointLineMis.lurender.json"));
    const QByteArray afterRoot = document.GetDraft(ROOT_GRAPH);
    const QByteArray afterManifest = document.GetDraft(COMPUTE_MANIFEST);
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
    CHECK(document.GetDraft(COMPUTE_MANIFEST) == beforeManifest);
    CHECK(document.GetFiles() == beforeFiles);
    workspace.Redo();
    CHECK(document.GetDraft(ROOT_GRAPH) == afterRoot);
    CHECK(document.GetDraft(COMPUTE_MANIFEST) == afterManifest);

    ReplaceRasterSubgraph(document, QStringLiteral("PointLineMis"));
    REQUIRE(workspace.Save() == true);
    RequireValidation(workspace);
}

TEST_CASE("Salesin and Jarosz configuration applies from the canvas Configurations menu with Properties hidden",
          "[editor][render-graph][point-line-configuration]")
{
    QTemporaryDir project;
    QWidget properties;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.CustomizeBuiltIn() == true);
    QWidget* inspector = workspace.TakeInspectorPanel(&properties);
    inspector->hide();
    properties.hide();
    workspace.resize(TEMPLATE_WORKSPACE_WIDTH, TEMPLATE_WORKSPACE_HEIGHT);
    workspace.show();
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray beforeManifest = document.GetDraft(COMPUTE_MANIFEST);
    bool hasAppliedTemplate = false;
    QTimer::singleShot(0, &workspace, [&workspace, &hasAppliedTemplate]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* actionOrNull =
            menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphApplyConfigurationTemplate"));
        REQUIRE(actionOrNull != nullptr);
        REQUIRE(actionOrNull->isEnabled() == true);
        menuOrNull->close();
        QTimer::singleShot(0, &workspace, [&hasAppliedTemplate]() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialogOrNull != nullptr);
            QComboBox* choiceOrNull =
                dialogOrNull->findChild<QComboBox*>(QStringLiteral("renderGraphConfigurationTemplate"));
            REQUIRE(choiceOrNull != nullptr);
            const int32 selected = choiceOrNull->findData(GetPointLineTemplate());
            REQUIRE(selected >= 0);
            choiceOrNull->setCurrentIndex(selected);
            QLineEdit* nameOrNull =
                dialogOrNull->findChild<QLineEdit*>(QStringLiteral("renderGraphTemplateConfiguration"));
            REQUIRE(nameOrNull != nullptr);
            CHECK(nameOrNull->text() == QStringLiteral("PointLineMis"));
            QDialogButtonBox* buttonsOrNull = dialogOrNull->findChild<QDialogButtonBox*>();
            REQUIRE(buttonsOrNull != nullptr);
            buttonsOrNull->button(QDialogButtonBox::Apply)->click();
            hasAppliedTemplate = true;
        });
        actionOrNull->trigger();
    });
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.centerOn(canvas.scene()->itemsBoundingRect().bottomRight() + QPointF(100, 100));
    QContextMenuEvent event(QContextMenuEvent::Mouse,
                            canvas.viewport()->rect().center(),
                            canvas.viewport()->mapToGlobal(canvas.viewport()->rect().center()));
    QApplication::sendEvent(canvas.viewport(), &event);
    REQUIRE(hasAppliedTemplate == true);
    CHECK(inspector->isVisible() == false);
    CheckPointLineImport(document, QStringLiteral("PointLineMis.lurender.json"));
    workspace.Undo();
    CHECK(document.GetDraft(COMPUTE_MANIFEST) == beforeManifest);
    workspace.Redo();
    ReplaceRasterSubgraph(document, QStringLiteral("PointLineMis"));
    REQUIRE(workspace.Save() == true);
    RequireValidation(workspace);
}

TEST_CASE("Veach and Guibas and Salesin and Jarosz configurations apply to one package",
          "[editor][render-graph][point-line-configuration]")
{
    // Graph settings are package-wide. Both MIS templates once declared MIS_HEURISTIC with
    // different ranges, so the second import was rejected as a conflicting definition.
    QTemporaryDir project;
    RendererWorkspace workspace({FindTemplatePython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.CustomizeBuiltIn() == true);
    RenderGraphDocument& document = workspace.GetDocument();
    REQUIRE(workspace.ApplyConfigurationTemplate(QStringLiteral("Mis"), QStringLiteral("veach-guibas-mis")) == true);
    // Return to the root canvas, as the user does, so the second import makes its own library.
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, QStringLiteral("Default"), QStringLiteral("output")) == true);
    REQUIRE(workspace.ApplyConfigurationTemplate(QStringLiteral("PointLineMis"), GetPointLineTemplate()) == true);
    const QByteArray manifest = document.GetDraft(COMPUTE_MANIFEST);
    CHECK(GetGraphSetting(manifest, QStringLiteral("MIS_HEURISTIC")) == 1);
    CHECK(GetGraphSetting(manifest, QStringLiteral("POINT_LINE_MIS_HEURISTIC")) == 0);
    CHECK(GetGraphSetting(manifest, QStringLiteral("MIS_POWER_EXPONENT")) == 2);
    CHECK(GetGraphSetting(manifest, QStringLiteral("POINT_LINE_MIS_POWER_EXPONENT")) == 2);
    CHECK(document.GetDraft(QStringLiteral("Mis.lurender.json")).isEmpty() == false);
    CheckPointLineImport(document, QStringLiteral("PointLineMis.lurender.json"));
}

TEST_CASE("Compute ray-tracing import extends a shared ray technique and preserves conflicting edits",
          "[editor][render-graph][compute-configuration]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    QString error;
    REQUIRE(
        document.ApplyConfigurationTemplate(ROOT_GRAPH, QStringLiteral("Restir"), QStringLiteral("restir-di"), error) ==
        RenderGraphDocumentStatus::Ok);
    QJsonObject manifest = QJsonDocument::fromJson(document.GetDraft(COMPUTE_MANIFEST)).object();
    QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
    QJsonObject original;
    qsizetype rayIndex = -1;
    for (qsizetype index = 0; index < techniques.size(); ++index)
    {
        if (techniques.at(index).toObject().value(QStringLiteral("id")) ==
            QStringLiteral("RendererTriangles.RayTracing"))
        {
            rayIndex = index;
            original = techniques.at(index).toObject();
        }
    }
    REQUIRE(rayIndex >= 0);
    const auto setInterface = [&document, &manifest, &techniques, &original, rayIndex](const QJsonObject& interface) {
        QJsonObject technique = original;
        technique.insert(QStringLiteral("scene_interface"), interface);
        techniques.replace(rayIndex, technique);
        manifest.insert(QStringLiteral("techniques"), techniques);
        REQUIRE(document.SetText(COMPUTE_MANIFEST, EncodeCanonicalJson(manifest)) == RenderGraphDocumentStatus::Ok);
    };
    QJsonObject interface = original.value(QStringLiteral("scene_interface")).toObject();
    SECTION("budgets rise and callbacks remain")
    {
        constexpr int32 AUTHORED_SURFACE_QUERIES = 3;
        interface.insert(QStringLiteral("surface_queries"), AUTHORED_SURFACE_QUERIES);
        interface.insert(QStringLiteral("visibility_queries"), 0);
        setInterface(interface);
        REQUIRE(document.ApplyConfigurationTemplate(ROOT_GRAPH, QStringLiteral("Compute"), COMPUTE_TEMPLATE, error) ==
                RenderGraphDocumentStatus::Ok);
        const QJsonObject merged = FindRayTechnique(document.GetDraft(COMPUTE_MANIFEST));
        for (const QString& stage :
             {QStringLiteral("ray_generation"), QStringLiteral("miss"), QStringLiteral("closest_hit")})
        {
            CHECK(merged.value(stage) == original.value(stage));
        }
        const QJsonObject mergedInterface = merged.value(QStringLiteral("scene_interface")).toObject();
        CHECK(mergedInterface.value(QStringLiteral("payload")) == interface.value(QStringLiteral("payload")));
        CHECK(mergedInterface.value(QStringLiteral("surface_queries")).toInt() == AUTHORED_SURFACE_QUERIES);
        CHECK(mergedInterface.value(QStringLiteral("visibility_queries")).toInt() == 1);
        return;
    }
    SECTION("unsupported scene interface version")
    {
        interface.insert(QStringLiteral("version"), 2);
        setInterface(interface);
    }
    SECTION("authored hit record source")
    {
        REQUIRE(document.ApplyConfigurationTemplate(ROOT_GRAPH, QStringLiteral("First"), COMPUTE_TEMPLATE, error) ==
                RenderGraphDocumentStatus::Ok);
        const QString header = QStringLiteral("compute_ray_tracing/main_shaders/hit_record.slang");
        REQUIRE(document.SetText(header, document.GetDraft(header) + QByteArray("\n// Authored edit.\n")) ==
                RenderGraphDocumentStatus::Ok);
    }
    const QByteArray beforeRoot = document.GetDraft(ROOT_GRAPH);
    const QByteArray beforeManifest = document.GetDraft(COMPUTE_MANIFEST);
    const QStringList beforeFiles = document.GetFiles();
    CHECK(document.ApplyConfigurationTemplate(ROOT_GRAPH, QStringLiteral("Compute"), COMPUTE_TEMPLATE, error) ==
          RenderGraphDocumentStatus::Conflict);
    CHECK(error.contains(QStringLiteral("preserved")) == true);
    CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
    CHECK(document.GetDraft(COMPUTE_MANIFEST) == beforeManifest);
    CHECK(document.GetFiles() == beforeFiles);
}
