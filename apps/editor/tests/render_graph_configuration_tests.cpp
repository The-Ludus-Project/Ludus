#include "internal/render_graph_configuration_editor.h"
#include "internal/render_graph_pipeline.h"
#include "internal/renderer_workspace.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");
const QString DEFAULT_CONFIGURATION = QStringLiteral("Default");
QString MakePackage(const QTemporaryDir& project)
{
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/offscreen"));
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    REQUIRE(QDir().mkpath(package) == true);
    for (const QString& file : {ROOT_GRAPH, QStringLiteral("rasterize.lurender.json")})
    {
        REQUIRE(QFile::copy(source.filePath(file), QDir(package).filePath(file)) == true);
    }
    return package;
}
QJsonObject GetConfiguration(const RenderGraphDocument& document, const QString& file, const QString& identity)
{
    QJsonObject graph;
    REQUIRE(document.GetGraph(file, graph) == true);
    const QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    for (const QJsonValueConstRef value : configurations)
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == identity)
        {
            return value.toObject();
        }
    }
    return {};
}
QJsonArray GetInputConnections(const RenderGraphDocument& document, const QString& destination)
{
    QJsonArray result;
    const QJsonArray edges =
        GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION).value(QStringLiteral("edges")).toArray();
    for (const QJsonValueConstRef edge : edges)
    {
        if (edge.toObject().value(QStringLiteral("to")).toString() == destination)
        {
            result.append(edge);
        }
    }
    return result;
}
} // namespace

TEST_CASE("configuration interface UI creates connectable ports with undo and save",
          "[editor][render-graph][interface]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    const QString package = MakePackage(project);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, {}) == true);
    REQUIRE(workspace.CreateSubgraph(QStringLiteral("Custom")) == true);
    const QString library = QStringLiteral("Custom.lurender.json");
    static_cast<void>(workspace.SelectNode(library, {}, {}));
    REQUIRE(workspace.GetCanvas().GetFile() == library);
    REQUIRE(workspace.GetCanvas().GetConfiguration() == DEFAULT_CONFIGURATION);
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray original = document.GetDraft(library);
    const QByteArray parent = document.GetDraft(ROOT_GRAPH);
    QPushButton* edit = workspace.findChild<QPushButton*>(QStringLiteral("renderGraphEditInterface"));
    REQUIRE(edit != nullptr);
    REQUIRE(edit->isEnabled() == true);
    QTimer::singleShot(0, &workspace, [&workspace]() {
        RenderGraphConfigurationEditor* dialogOrNull =
            qobject_cast<RenderGraphConfigurationEditor*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        QTableWidget* inputs = dialogOrNull->findChild<QTableWidget*>(QStringLiteral("configurationInputs"));
        QTableWidget* outputs = dialogOrNull->findChild<QTableWidget*>(QStringLiteral("configurationOutputs"));
        dialogOrNull->findChild<QPushButton*>(QStringLiteral("configurationAddInput"))->click();
        inputs->item(0, 0)->setText(QStringLiteral("color"));
        dialogOrNull->findChild<QTabWidget*>()->setCurrentIndex(1);
        dialogOrNull->findChild<QPushButton*>(QStringLiteral("configurationAddOutput"))->click();
        outputs->item(0, 0)->setText(QStringLiteral("color"));
        CHECK(dialogOrNull->GetOutputs().at(0).toObject().value(QStringLiteral("source")).toString().isEmpty() == true);
        CHECK(workspace.GetDocument().IsDirty() == true); // Creation is already a draft; controls add no revision.
        dialogOrNull->findChild<QPushButton*>(QStringLiteral("configurationApply"))->click();
    });
    const uint64 revision = document.GetRevision();
    edit->click();
    CHECK(document.GetRevision() == revision + 1);
    CHECK(document.GetDraft(ROOT_GRAPH) == parent);
    const QByteArray authored = document.GetDraft(library);
    workspace.Undo();
    CHECK(document.GetDraft(library) == original);
    workspace.Redo();
    CHECK(document.GetDraft(library) == authored);

    RenderGraphCanvas& canvas = workspace.GetCanvas();
    REQUIRE(canvas.HasNode(QStringLiteral("input")) == true);
    REQUIRE(canvas.HasNode(QStringLiteral("#outputs")) == true);
    // Real mouse gestures connect the synthetic interface boxes through output.source.
    workspace.resize(1200, 800);
    workspace.show();
    QCoreApplication::processEvents();
    canvas.fitInView(canvas.sceneRect(), Qt::KeepAspectRatio);
    const QPoint from = canvas.mapFromScene(canvas.GetPortPosition(QStringLiteral("input.color"), true));
    const QPoint to = canvas.mapFromScene(canvas.GetPortPosition(QStringLiteral("#outputs.color"), false));
    QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(canvas.viewport(), to);
    QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, to);
    CHECK(
        GetConfiguration(document, library, DEFAULT_CONFIGURATION).value(QStringLiteral("edges")).toArray().isEmpty() ==
        true);
    CHECK(GetConfiguration(document, library, DEFAULT_CONFIGURATION)
              .value(QStringLiteral("outputs"))
              .toArray()
              .at(0)
              .toObject()
              .value(QStringLiteral("source"))
              .toString() == QStringLiteral("input.color"));
    const QByteArray connected = document.GetDraft(library);
    CHECK(canvas.RequestLink(QStringLiteral("input.color"), QStringLiteral("#outputs.color")) == false);
    CHECK(document.GetDraft(library) == connected);
    workspace.Undo();
    CHECK(document.GetDraft(library) == authored);
    workspace.Redo();
    CHECK(document.GetDraft(library) == connected);

    // Selecting the output link and deleting clears its source, preserving the port.
    const QPointF middle = (canvas.GetPortPosition(QStringLiteral("input.color"), true) +
                            canvas.GetPortPosition(QStringLiteral("#outputs.color"), false)) /
                           2;
    QTest::mouseClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, canvas.mapFromScene(middle));
    QTest::keyClick(&canvas, Qt::Key_Delete);
    CHECK(document.GetDraft(library) == authored);
    CHECK(canvas.HasNode(QStringLiteral("#outputs")) == true);
    workspace.Undo();
    CHECK(document.GetDraft(library) == connected);
    REQUIRE(workspace.Save() == true);
    RenderGraphDocument reloaded;
    REQUIRE(reloaded.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reloaded.GetDraft(library) == connected);

    // Applying unchanged controls closes without adding a history entry.
    const uint64 beforeUnchanged = document.GetRevision();
    QSignalSpy unchangedNotifications(&workspace, &RendererWorkspace::DocumentChanged);
    QTimer::singleShot(0, &workspace, []() {
        RenderGraphConfigurationEditor* dialogOrNull =
            qobject_cast<RenderGraphConfigurationEditor*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        dialogOrNull->findChild<QPushButton*>(QStringLiteral("configurationApply"))->click();
    });
    edit->click();
    CHECK(document.GetRevision() == beforeUnchanged);
    CHECK(unchangedNotifications.isEmpty() == true);
    // Invalid edits remain reviewable in the dialog; Cancel writes nothing.
    const uint64 beforeInvalid = document.GetRevision();
    QTimer::singleShot(0, &workspace, []() {
        RenderGraphConfigurationEditor* dialogOrNull =
            qobject_cast<RenderGraphConfigurationEditor*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        dialogOrNull->findChild<QTableWidget*>(QStringLiteral("configurationInputs"))
            ->item(0, 0)
            ->setText(QStringLiteral("123invalid"));
        dialogOrNull->findChild<QPushButton*>(QStringLiteral("configurationApply"))->click();
        CHECK(dialogOrNull->findChild<QLabel*>(QStringLiteral("configurationInterfaceError"))->text().isEmpty() ==
              false);
        dialogOrNull->reject();
    });
    edit->click();
    CHECK(document.GetRevision() == beforeInvalid);
    CHECK(document.GetDraft(library) == connected);
}

TEST_CASE("canvas replaces an occupied input atomically and preserves rejected connections",
          "[editor][render-graph][canvas-editing]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(MakePackage(project)).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    workspace.resize(1200, 800);
    workspace.show();
    QCoreApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphDocument& document = workspace.GetDocument();
    canvas.fitInView(canvas.sceneRect(), Qt::KeepAspectRatio);
    const QByteArray original = document.GetDraft(ROOT_GRAPH);
    const uint64 revision = document.GetRevision();
    const QPoint from = canvas.mapFromScene(canvas.GetPortPosition(QStringLiteral("color.texture"), true));
    const QPoint to = canvas.mapFromScene(canvas.GetPortPosition(QStringLiteral("output.texture"), false));
    QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(canvas.viewport(), to);
    QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, to);
    CHECK(document.GetRevision() == revision + 1);
    const QByteArray replacement = document.GetDraft(ROOT_GRAPH);
    CHECK(replacement != original);
    const QJsonArray connections = GetInputConnections(document, QStringLiteral("output.texture"));
    REQUIRE(connections.size() == 1);
    CHECK(connections.at(0).toObject().value(QStringLiteral("from")).toString() == QStringLiteral("color.texture"));
    CHECK(canvas.GetEdgeCount() == 6);
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
    workspace.Redo();
    CHECK(document.GetDraft(ROOT_GRAPH) == replacement);
    const uint64 beforeRejection = document.GetRevision();
    CHECK(canvas.RequestLink(QStringLiteral("depth.texture"), QStringLiteral("output.texture"), true) == false);
    CHECK(canvas.RequestLink(QStringLiteral("raster.color"), QStringLiteral("camera.target"), true) == false);
    CHECK(document.GetRevision() == beforeRejection);
    CHECK(document.GetDraft(ROOT_GRAPH) == replacement);
}

TEST_CASE("canvas wire and port menus disconnect and reconnect without the inspector",
          "[editor][render-graph][canvas-editing]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(MakePackage(project)).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    workspace.resize(1200, 800);
    workspace.show();
    QCoreApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphDocument& document = workspace.GetDocument();
    canvas.fitInView(canvas.sceneRect(), Qt::KeepAspectRatio);
    const QByteArray original = document.GetDraft(ROOT_GRAPH);
    const QPointF source = canvas.GetPortPosition(QStringLiteral("raster.color"), true);
    const QPointF destination = canvas.GetPortPosition(QStringLiteral("output.texture"), false);
    for (const QPointF position : {source * 0.5 + destination * 0.5, destination})
    {
        QTimer::singleShot(0, &canvas, []() {
            QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            REQUIRE(menuOrNull != nullptr);
            QAction* disconnect = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphDisconnectAction"));
            REQUIRE(disconnect != nullptr);
            menuOrNull->close();
            disconnect->trigger();
        });
        const QPoint point = canvas.mapFromScene(position);
        QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
        QApplication::sendEvent(canvas.viewport(), &context);
        CHECK(canvas.GetEdgeCount() == 5);
        CHECK(GetInputConnections(document, QStringLiteral("output.texture")).isEmpty() == true);
        workspace.Undo();
        CHECK(document.GetDraft(ROOT_GRAPH) == original);
    }
    QTimer::singleShot(0, &canvas, []() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QMenu* connections = menuOrNull->findChild<QMenu*>(QStringLiteral("renderGraphConnectPortMenu"));
        REQUIRE(connections != nullptr);
        QAction* choiceOrNull = nullptr;
        for (QAction* action : connections->actions())
        {
            CHECK(action->data().toString() != QStringLiteral("depth.texture"));
            if (action->data().toString() == QStringLiteral("color.texture"))
            {
                choiceOrNull = action;
            }
        }
        REQUIRE(choiceOrNull != nullptr);
        menuOrNull->close();
        choiceOrNull->trigger();
    });
    const QPoint point = canvas.mapFromScene(destination);
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QApplication::sendEvent(canvas.viewport(), &context);
    CHECK(document.GetDraft(ROOT_GRAPH) != original);
    const QJsonArray connections = GetInputConnections(document, QStringLiteral("output.texture"));
    REQUIRE(connections.size() == 1);
    CHECK(connections.at(0).toObject().value(QStringLiteral("from")).toString() == QStringLiteral("color.texture"));
    CHECK(canvas.GetEdgeCount() == 6);
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
}

TEST_CASE("canvas node menu edits authored fields with the inspector hidden", "[editor][render-graph][canvas-editing]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(MakePackage(project)).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    workspace.resize(1200, 800);
    workspace.show();
    QCoreApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const QByteArray original = workspace.GetDocument().GetDraft(ROOT_GRAPH);
    QTimer::singleShot(0, &canvas, [&canvas]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* edit = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphEditNodeAction"));
        REQUIRE(edit != nullptr);
        QTimer::singleShot(0, &canvas, []() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialogOrNull != nullptr);
            CHECK(dialogOrNull->objectName() == QStringLiteral("renderGraphNodeEditor"));
            QCheckBox* initialized =
                dialogOrNull->findChild<QCheckBox*>(QStringLiteral("renderGraphField_initialized"));
            REQUIRE(initialized != nullptr);
            initialized->click();
            QSpinBox* coordinate = dialogOrNull->findChild<QSpinBox*>(QStringLiteral("canvasLayoutX"));
            REQUIRE(coordinate != nullptr);
            coordinate->setValue(96);
            REQUIRE(QMetaObject::invokeMethod(coordinate, "editingFinished") == true);
            dialogOrNull->reject();
        });
        menuOrNull->close();
        edit->trigger();
    });
    const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("color")) + QPointF(20, 10));
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QApplication::sendEvent(canvas.viewport(), &context);
    CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) != original);
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) != original);
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) == original);
}

TEST_CASE("canvas configuration outputs replace and disconnect with their declarations preserved",
          "[editor][render-graph][canvas-editing]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(MakePackage(project)).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    const QJsonObject output{
        {QStringLiteral("id"), QStringLiteral("result")},
        {QStringLiteral("type"), QStringLiteral("ColorTexture")},
        {QStringLiteral("source"), QStringLiteral("raster.color")},
        {QStringLiteral("required"), false},
    };
    QString error;
    REQUIRE(document.ReplaceConfigurationInterface(ROOT_GRAPH, DEFAULT_CONFIGURATION, {}, {output}, error) ==
            RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.Rebuild();
    const QByteArray original = document.GetDraft(ROOT_GRAPH);
    const uint64 revision = document.GetRevision();
    REQUIRE(canvas.RequestLink(QStringLiteral("color.texture"), QStringLiteral("#outputs.result"), true) == true);
    CHECK(document.GetRevision() == revision + 1);
    CHECK(GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION)
              .value(QStringLiteral("outputs"))
              .toArray()
              .at(0)
              .toObject()
              .value(QStringLiteral("source"))
              .toString() == QStringLiteral("color.texture"));
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
    const uint64 beforeNoop = document.GetRevision();
    REQUIRE(canvas.RequestLink(QStringLiteral("color.texture"), QStringLiteral("camera.target"), true) == true);
    CHECK(document.GetRevision() == beforeNoop);
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
    CHECK(canvas.RequestLink(QStringLiteral("depth.texture"), QStringLiteral("#outputs.result"), true) == false);
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
    QTimer::singleShot(0, &canvas, []() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* disconnect = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphDisconnectAction"));
        REQUIRE(disconnect != nullptr);
        menuOrNull->close();
        disconnect->trigger();
    });
    const QPoint point = canvas.mapFromScene(canvas.GetPortPosition(QStringLiteral("#outputs.result"), false));
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QApplication::sendEvent(canvas.viewport(), &context);
    const QJsonArray outputs =
        GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION).value(QStringLiteral("outputs")).toArray();
    REQUIRE(outputs.size() == 1);
    CHECK(outputs.at(0).toObject().value(QStringLiteral("source")).toString().isEmpty() == true);
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
}

TEST_CASE("canvas pipeline editing pairs resources and reports connected port errors with Properties hidden",
          "[editor][render-graph][canvas-editing][pipeline]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(MakePackage(project)).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    REQUIRE(document.AddNode(ROOT_GRAPH,
                             DEFAULT_CONFIGURATION,
                             MakePipelineNode(QStringLiteral("ComputePipeline"), QStringLiteral("work"))) ==
            RenderGraphDocumentStatus::Ok);
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, QStringLiteral("work")) == true);
    workspace.resize(1200, 800);
    workspace.show();
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    properties->hide();
    QCoreApplication::processEvents();
    const QByteArray original = document.GetDraft(ROOT_GRAPH);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.Rebuild();
    QTimer::singleShot(0, &canvas, [&workspace, &canvas, &document, properties]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* edit = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphEditNodeAction"));
        REQUIRE(edit != nullptr);
        QTimer::singleShot(0, &canvas, [&workspace, &canvas, &document, properties]() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialogOrNull != nullptr);
            CHECK(properties->isVisible() == false);
            QPushButton* add = dialogOrNull->findChild<QPushButton*>(QStringLiteral("renderGraphAdd_outputs"));
            QTableWidget* resources = dialogOrNull->findChild<QTableWidget*>(QStringLiteral("outputs"));
            QPushButton* apply = dialogOrNull->findChild<QPushButton*>(QStringLiteral("renderGraphApplyPipeline"));
            REQUIRE(add != nullptr);
            REQUIRE(resources != nullptr);
            REQUIRE(apply != nullptr);
            add->click();
            resources->item(0, 0)->setText(QStringLiteral("color"));
            apply->click();
            CHECK(canvas.GetPortPosition(QStringLiteral("work.color"), false).y() ==
                  canvas.GetPortPosition(QStringLiteral("work.color"), true).y());
            REQUIRE(canvas.RequestLink(QStringLiteral("color.texture"), QStringLiteral("work.color")) == true);
            const uint64 revision = document.GetRevision();
            const QByteArray beforeRejectedEdit = document.GetDraft(ROOT_GRAPH);
            resources = dialogOrNull->findChild<QTableWidget*>(QStringLiteral("outputs"));
            apply = dialogOrNull->findChild<QPushButton*>(QStringLiteral("renderGraphApplyPipeline"));
            REQUIRE(resources != nullptr);
            REQUIRE(apply != nullptr);
            QComboBox* type = qobject_cast<QComboBox*>(resources->cellWidget(0, 1));
            REQUIRE(type != nullptr);
            type->setCurrentText(QStringLiteral("DepthTexture"));
            apply->click();
            CHECK(workspace.GetStatusText().contains(QStringLiteral("work.color")) == true);
            CHECK(workspace.GetStatusText().contains(QStringLiteral("Disconnect")) == true);
            CHECK(document.GetRevision() == revision);
            CHECK(document.GetDraft(ROOT_GRAPH) == beforeRejectedEdit);
            dialogOrNull->reject();
        });
        menuOrNull->close();
        edit->trigger();
    });
    const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("work")) + QPointF(20, 10));
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QApplication::sendEvent(canvas.viewport(), &context);
    workspace.Undo();
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
}
