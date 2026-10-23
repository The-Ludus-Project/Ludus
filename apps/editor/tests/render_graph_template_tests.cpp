// Built-in creation and canvas settings use the real shared backend and document history.
#include "internal/controller.h"
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
constexpr int32 TEMPLATE_JOB_TIMEOUT_MILLISECONDS = 60000;
constexpr int32 TEMPLATE_WORKSPACE_WIDTH = 1280;
constexpr int32 TEMPLATE_WORKSPACE_HEIGHT = 800;
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
    const bool hasSecondaryRays = identity == QStringLiteral("visibility-secondary-rays");
    CHECK(manifest.contains("visibility_buffer") == hasSecondaryRays);
    const QByteArray graph = workspace.GetDocument().GetDraft(ROOT_GRAPH);
    CHECK(graph.contains("GraphicsPipeline") == true);
    CHECK(graph.contains("RayTracingPipeline") == hasSecondaryRays);
    CHECK(graph.contains("visibilityOutput") == true);
    CHECK(graph.contains("depthOutput") == true);
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
    REQUIRE(workspace.Save() == true);
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    REQUIRE(workspace.Validate() == true);
    REQUIRE(finished.wait(TEMPLATE_JOB_TIMEOUT_MILLISECONDS) == true);
    INFO(workspace.GetLastResult().Message.toStdString());
    CHECK(workspace.GetLastResult().IsSuccessful == true);
    RenderGraphDocument reloaded;
    REQUIRE(reloaded.Load(document.GetPackageRoot(), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reloaded.GetDraft(library) == afterLibrary);
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
          RenderGraphDocumentStatus::Conflict);
    CHECK(error.contains(QStringLiteral("budgets")) == true);
    CHECK(document.GetDraft(ROOT_GRAPH) == beforeRoot);
    CHECK(document.GetDraft(QStringLiteral("main.lushader.json")) == constrained);
    CHECK(document.GetDraft(QStringLiteral("Lighting.lurender.json")).isEmpty() == true);
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
