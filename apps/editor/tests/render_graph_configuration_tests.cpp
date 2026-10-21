#include "internal/render_graph_configuration_editor.h"
#include "internal/render_graph_live_build.h"
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
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
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

TEST_CASE("canvas interface editing previews repairs with Properties hidden", "[editor][render-graph][interface]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    const QString library = QStringLiteral("rasterize.lurender.json");
    const QString configuration = QStringLiteral("ColorAndDepth");
    REQUIRE(workspace.SelectNode(library, configuration, QStringLiteral("draw")) == true);
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    properties->hide();
    workspace.resize(1200, 800);
    workspace.show();
    QCoreApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.fitInView(canvas.sceneRect(), Qt::KeepAspectRatio);
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray child = document.GetDraft(library);
    const QByteArray parent = document.GetDraft(ROOT_GRAPH);
    const uint64 revision = document.GetRevision();
    bool shouldApply = false;
    const auto editDialog = [&]() {
        RenderGraphConfigurationEditor* dialogOrNull =
            qobject_cast<RenderGraphConfigurationEditor*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        CHECK(properties->isVisible() == false);
        QTableWidget* inputs = dialogOrNull->findChild<QTableWidget*>(QStringLiteral("configurationInputs"));
        QTableWidget* outputs = dialogOrNull->findChild<QTableWidget*>(QStringLiteral("configurationOutputs"));
        // Rename a connected input and output. Original identities survive repeated keystrokes.
        inputs->item(3, 0)->setText(QStringLiteral("intermediate"));
        inputs->item(3, 0)->setText(QStringLiteral("target"));
        outputs->item(1, 0)->setText(QStringLiteral("result"));
        CHECK(dialogOrNull->GetInputRenames().value(QStringLiteral("color")) == QStringLiteral("target"));
        CHECK(dialogOrNull->GetOutputRenames().value(QStringLiteral("color")) == QStringLiteral("result"));
        const QString repairs =
            dialogOrNull->findChild<QLabel*>(QStringLiteral("configurationInterfaceRepairs"))->text();
        CHECK(repairs.contains(QStringLiteral("input.color")) == true);
        CHECK(repairs.contains(QStringLiteral("raster.color")) == true);
        CHECK(repairs.contains(QStringLiteral("raster.result")) == true);
        CHECK(repairs.contains(QStringLiteral("one undo step")) == true);
        CHECK(document.GetRevision() == revision);
        CHECK(document.GetDraft(library) == child);
        CHECK(document.GetDraft(ROOT_GRAPH) == parent);
        if (shouldApply == true)
        {
            dialogOrNull->findChild<QPushButton*>(QStringLiteral("configurationApply"))->click();
        }
        else
        {
            dialogOrNull->reject();
        }
    };
    const auto openFromCanvas = [&]() {
        QTimer::singleShot(0, &canvas, editDialog);
        const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("input")) + QPointF(20, 10));
        QTest::mouseDClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, point);
    };
    openFromCanvas();
    CHECK(document.GetRevision() == revision);
    CHECK(document.GetDraft(library) == child);
    CHECK(document.GetDraft(ROOT_GRAPH) == parent);
    shouldApply = true;
    openFromCanvas();
    CHECK(document.GetRevision() == revision + 1);
    const QByteArray repairedChild = document.GetDraft(library);
    const QByteArray repairedParent = document.GetDraft(ROOT_GRAPH);
    CHECK(repairedChild.contains("input.target") == true);
    CHECK(repairedParent.contains("raster.result") == true);
    workspace.Undo();
    CHECK(document.GetDraft(library) == child);
    CHECK(document.GetDraft(ROOT_GRAPH) == parent);
    workspace.Redo();
    CHECK(document.GetDraft(library) == repairedChild);
    CHECK(document.GetDraft(ROOT_GRAPH) == repairedParent);
    REQUIRE(workspace.Save() == true);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(library) == repairedChild);
    CHECK(reopened.GetDraft(ROOT_GRAPH) == repairedParent);
}

TEST_CASE("pipeline controls edit drafts without Apply and Save finishes the active cell",
          "[editor][render-graph][save-preview]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QComboBox* kinds = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    kinds->setCurrentText(QStringLiteral("ComputePipeline"));
    workspace.findChild<QPushButton*>(QStringLiteral("renderGraphAddNode"))->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    RenderGraphPipelineEditor* editor = workspace.findChild<RenderGraphPipelineEditor*>();
    REQUIRE(editor != nullptr);
    CHECK(editor->findChild<QPushButton*>(QStringLiteral("renderGraphApplyPipeline")) == nullptr);
    QSignalSpy edits(editor, &RenderGraphPipelineEditor::Edited);
    editor->findChild<QPushButton*>(QStringLiteral("renderGraphAdd_inputs"))->click();
    REQUIRE(edits.count() == 1);
    QTableWidget* inputs = editor->findChild<QTableWidget*>(QStringLiteral("inputs"));
    REQUIRE(inputs != nullptr);
    inputs->item(0, 0)->setText(QStringLiteral("history"));
    QComboBox* type = qobject_cast<QComboBox*>(inputs->cellWidget(0, 1));
    REQUIRE(type != nullptr);
    type->setCurrentText(QStringLiteral("Float32"));
    inputs->item(0, 2)->setCheckState(Qt::Unchecked);
    REQUIRE(edits.count() == 4);
    CHECK(workspace.GetCanvas().GetPortPosition(QStringLiteral("computepipeline.history"), false).isNull() == false);
    workspace.Undo();
    workspace.Redo();
    REQUIRE(workspace.Save() == true);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    editor = workspace.findChild<RenderGraphPipelineEditor*>();
    REQUIRE(editor != nullptr);
    inputs = editor->findChild<QTableWidget*>(QStringLiteral("inputs"));
    REQUIRE(inputs != nullptr);
    workspace.TakeInspectorPanel(&workspace)->show();
    workspace.show();
    workspace.activateWindow();
    editor->findChild<QTabWidget*>()->setCurrentIndex(0);
    inputs->setCurrentCell(0, 0);
    inputs->editItem(inputs->item(0, 0));
    QLineEdit* field = inputs->findChild<QLineEdit*>();
    REQUIRE(field != nullptr);
    field->setFocus();
    QCoreApplication::processEvents();
    field->selectAll();
    QTest::keyClicks(field, QStringLiteral("latest"));
    CHECK(inputs->item(0, 0)->text() == QStringLiteral("history"));
    CHECK(workspace.GetDocument().IsDirty() == false);
    CHECK(workspace.CanSave() == true);
    REQUIRE(workspace.Save() == true);
    CHECK(workspace.GetDocument().IsDirty() == false);
    CHECK(workspace.GetCanvas().GetPortPosition(QStringLiteral("computepipeline.latest"), false).isNull() == false);
    CHECK(workspace.GetDocument().GetSaved(ROOT_GRAPH).contains("latest") == true);
    workspace.Undo();
    CHECK(workspace.GetCanvas().GetPortPosition(QStringLiteral("computepipeline.history"), false).isNull() == false);
}

TEST_CASE("saved preview builds coalesce and never capture later unsaved edits", "[editor][render-graph][save-preview]")
{
    constexpr int32 TIMEOUT_MILLISECONDS = 10000;
    constexpr int32 SETTLE_MILLISECONDS = 600;
    QTemporaryDir project;
    QTemporaryDir tooling;
    const QString package = MakePackage(project);
    const QString shader = QStringLiteral("color.slang");
    const auto write = [](const QString& path, const QByteArray& bytes) {
        REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()) == true);
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly) == true);
        REQUIRE(file.write(bytes) == bytes.size());
    };
    write(QDir(package).filePath(shader), QByteArrayLiteral("original"));
    write(QDir(package).filePath(QStringLiteral("main.lushader.json")), QByteArrayLiteral("{}"));
    const QString pythonPackage = QDir(tooling.path()).filePath(QStringLiteral("scripts/python/ludus_tools"));
    write(QDir(pythonPackage).filePath(QStringLiteral("render_graph.py")), {});
    write(QDir(pythonPackage).filePath(QStringLiteral("__main__.py")), QByteArrayLiteral(R"PY(
import json
from pathlib import Path
import sys
import time
root = Path(sys.argv[sys.argv.index('--root') + 1])
captured = (root / 'color.slang').read_text()
release = Path(__file__).resolve().parents[3] / 'release'
while not release.exists():
    time.sleep(0.01)
if captured == 'broken':
    print(json.dumps({'schema': 1, 'error': {'code': 'CompileFailed', 'message': 'bad shader'}}))
    sys.exit(1)
print(json.dumps({'schema': 1, 'captured': captured}))
)PY"));
    RendererWorkspace workspace({QStandardPaths::findExecutable(QStringLiteral("python3")), {}, tooling.path()});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetBuildTools(QStringLiteral("helper"), QStringLiteral("compiler"));
    workspace.SetLivePreview(true);
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    const auto waitFor = [&](const auto& isReady) {
        QElapsedTimer clock;
        clock.start();
        while (isReady() == false && clock.elapsed() < TIMEOUT_MILLISECONDS)
        {
            QTest::qWait(1);
        }
        return isReady();
    };
    REQUIRE(waitFor([&]() { return workspace.IsJobActive(); }) == true);
    RenderGraphDocument& document = workspace.GetDocument();
    REQUIRE(document.SetText(shader, QByteArrayLiteral("first save")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(workspace.Save() == true);
    REQUIRE(document.SetText(shader, QByteArrayLiteral("latest save")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(workspace.Save() == true);
    const uint64 savedRevision = document.GetRevision();
    REQUIRE(document.SetText(shader, QByteArrayLiteral("unsaved")) == RenderGraphDocumentStatus::Ok);
    QTest::qWait(SETTLE_MILLISECONDS);
    CHECK(finished.count() == 0);
    write(QDir(tooling.path()).filePath(QStringLiteral("release")), {});
    REQUIRE(waitFor([&]() { return finished.count() == 2 && workspace.IsJobActive() == false; }) == true);
    CHECK(workspace.GetLastResult().Payload.value(QStringLiteral("captured")).toString() ==
          QStringLiteral("latest save"));
    CHECK(workspace.GetLastResult().Request.Source == RenderGraphSource::Saved);
    CHECK(workspace.GetLastResult().Request.Revision == savedRevision);
    CHECK(document.GetDraft(shader) == QByteArrayLiteral("unsaved"));
    CHECK(document.IsDirty() == true);
    QTest::qWait(SETTLE_MILLISECONDS);
    CHECK(finished.count() == 2);
    REQUIRE(workspace.BuildAndPreview() == true);
    REQUIRE(waitFor([&]() { return finished.count() == 3; }) == true);
    CHECK(workspace.GetLastResult().Payload.value(QStringLiteral("captured")).toString() == QStringLiteral("unsaved"));
    CHECK(workspace.GetLastResult().Request.Source == RenderGraphSource::Draft);
    REQUIRE(document.Undo() == true);
    REQUIRE(document.IsDirty() == false);
    REQUIRE(document.MoveNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, QStringLiteral("scene"), 10, 20) ==
            RenderGraphDocumentStatus::Ok);
    REQUIRE(workspace.Save() == true);
    QTest::qWait(SETTLE_MILLISECONDS);
    CHECK(finished.count() == 3);
    workspace.SetLivePreview(false);
    REQUIRE(document.SetText(shader, QByteArrayLiteral("broken")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(workspace.Save() == true);
    QTest::qWait(SETTLE_MILLISECONDS);
    CHECK(finished.count() == 3);
    REQUIRE(QFile::remove(QDir(tooling.path()).filePath(QStringLiteral("release"))) == true);
    workspace.SetLivePreview(true);
    REQUIRE(waitFor([&]() { return workspace.IsJobActive(); }) == true);
    REQUIRE(document.SetText(shader, QByteArrayLiteral("later unsaved edit")) == RenderGraphDocumentStatus::Ok);
    write(QDir(tooling.path()).filePath(QStringLiteral("release")), {});
    REQUIRE(waitFor([&]() { return finished.count() == 4; }) == true);
    CHECK(workspace.GetLastResult().IsSuccessful == false);
    CHECK(workspace.GetRenderProblem().contains(QStringLiteral("bad shader")) == true);
    CHECK(workspace.GetStatusText().contains(QStringLiteral("Preview unchanged")) == true);
    QTest::qWait(SETTLE_MILLISECONDS);
    CHECK(finished.count() == 4);
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

TEST_CASE("canvas pipeline edits keep active controls and Save commits the active cell",
          "[editor][render-graph][canvas-editing][save-preview]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(MakePackage(project)).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    workspace.resize(1200, 800);
    workspace.show();
    QComboBox* kinds = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    REQUIRE(kinds != nullptr);
    kinds->setCurrentText(QStringLiteral("ComputePipeline"));
    workspace.findChild<QPushButton*>(QStringLiteral("renderGraphAddNode"))->click();
    REQUIRE(workspace.Save() == true);
    QCoreApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    QTimer::singleShot(0, &canvas, [&canvas, &workspace]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* edit = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphEditNodeAction"));
        REQUIRE(edit != nullptr);
        QTimer::singleShot(0, &canvas, [&workspace]() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialogOrNull != nullptr);
            RenderGraphPipelineEditor* editor = dialogOrNull->findChild<RenderGraphPipelineEditor*>();
            REQUIRE(editor != nullptr);
            CHECK(editor->findChild<QPushButton*>(QStringLiteral("renderGraphApplyPipeline")) == nullptr);
            editor->findChild<QPushButton*>(QStringLiteral("renderGraphAdd_inputs"))->click();
            CHECK(dialogOrNull->findChild<RenderGraphPipelineEditor*>() == editor);
            QTableWidget* inputs = editor->findChild<QTableWidget*>(QStringLiteral("inputs"));
            REQUIRE(inputs != nullptr);
            inputs->item(0, 0)->setText(QStringLiteral("history"));
            QComboBox* type = qobject_cast<QComboBox*>(inputs->cellWidget(0, 1));
            REQUIRE(type != nullptr);
            type->setCurrentText(QStringLiteral("Float32"));
            CHECK(dialogOrNull->findChild<RenderGraphPipelineEditor*>() == editor);
            CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("history") == true);
            CHECK(workspace.GetDocument().GetSaved(ROOT_GRAPH).contains("history") == false);
            inputs->editItem(inputs->item(0, 0));
            QLineEdit* field = inputs->findChild<QLineEdit*>();
            REQUIRE(field != nullptr);
            field->setFocus();
            QCoreApplication::processEvents();
            REQUIRE(field->hasFocus() == true);
            field->selectAll();
            QTest::keyClicks(field, QStringLiteral("latest"));
            REQUIRE(workspace.Save() == true);
            CHECK(workspace.GetDocument().GetSaved(ROOT_GRAPH).contains("latest") == true);
            CHECK(workspace.GetDocument().IsDirty() == false);
            dialogOrNull->reject();
        });
        menuOrNull->close();
        edit->trigger();
    });
    const QPoint point =
        canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("computepipeline")) + QPointF(20, 10));
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QApplication::sendEvent(canvas.viewport(), &context);
    CHECK(workspace.GetDocument().IsDirty() == false);
    CHECK(canvas.GetPortPosition(QStringLiteral("computepipeline.latest"), false).isNull() == false);
    workspace.Undo();
    CHECK(canvas.GetPortPosition(QStringLiteral("computepipeline.history"), false).isNull() == false);
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
            REQUIRE(add != nullptr);
            REQUIRE(resources != nullptr);
            add->click();
            resources->item(0, 0)->setText(QStringLiteral("color"));
            CHECK(canvas.GetPortPosition(QStringLiteral("work.color"), false).y() ==
                  canvas.GetPortPosition(QStringLiteral("work.color"), true).y());
            REQUIRE(canvas.RequestLink(QStringLiteral("color.texture"), QStringLiteral("work.color")) == true);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            const uint64 revision = document.GetRevision();
            const QByteArray beforeRejectedEdit = document.GetDraft(ROOT_GRAPH);
            resources = dialogOrNull->findChild<QTableWidget*>(QStringLiteral("outputs"));
            REQUIRE(resources != nullptr);
            QComboBox* type = qobject_cast<QComboBox*>(resources->cellWidget(0, 1));
            REQUIRE(type != nullptr);
            type->setCurrentText(QStringLiteral("DepthTexture"));
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
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
}
