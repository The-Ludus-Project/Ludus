#include "internal/code_editor.h"
#include "internal/render_graph_configuration_editor.h"
#include "internal/render_graph_live_build.h"
#include "internal/render_graph_pipeline.h"
#include "internal/renderer_workspace.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QSysInfo>
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

TEST_CASE("multiline shader failures preserve the render graph editing area", "[editor][render-graph][layout]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    const QString package = MakePackage(project);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QTabWidget* views = workspace.findChild<QTabWidget*>(QStringLiteral("renderGraphViews"));
    QSplitter* splitter = workspace.findChild<QSplitter*>(QStringLiteral("renderGraphSplitter"));
    QLabel* status = workspace.findChild<QLabel*>(QStringLiteral("renderGraphStatus"));
    REQUIRE(views != nullptr);
    REQUIRE(splitter != nullptr);
    REQUIRE(status != nullptr);
    SECTION("canvas at a compact window size")
    {
        workspace.resize(1000, 600);
        views->setCurrentIndex(0);
    }
    SECTION("source at a regular window size")
    {
        workspace.resize(1280, 800);
        views->setCurrentIndex(1);
    }
    workspace.show();
    QCoreApplication::processEvents();
    const QSize windowSize = workspace.size();
    const int32 editingHeight = splitter->height();
    const int32 statusHeightAllowance = status->fontMetrics().lineSpacing() * 3;
    RenderGraphJobResult failure;
    failure.Request.ProjectEpoch = 1;
    failure.Request.Action = QStringLiteral("build");
    failure.Code = QStringLiteral("RenderGraphBuildShader");
    failure.Message = QStringLiteral("shader helper: Ludus shader build:\n");
    for (int32 line = 0; line < 80; ++line)
    {
        failure.Message += QStringLiteral("shader.slang(4): error 30015: undefined identifier '<primitives>'.\n"
                                          "    uint materialId = primitives[input.primitiveId].materialIndex;\n");
    }
    failure.Message += QStringLiteral("compilation ceased");
    workspace.AcceptResult(failure);
    QCoreApplication::processEvents();
    CHECK(workspace.size() == windowSize);
    CHECK(splitter->height() >= editingHeight - statusHeightAllowance);
    if (views->currentIndex() == 1)
    {
        QPlainTextEdit* source = workspace.findChild<QPlainTextEdit*>(QStringLiteral("renderGraphSource"));
        REQUIRE(source != nullptr);
        CHECK(source->viewport()->height() > statusHeightAllowance);
        source->selectAll();
        QTest::keyClicks(source, QStringLiteral("// fix shader here"));
        CHECK(source->toPlainText() == QStringLiteral("// fix shader here"));
    }
    CHECK(workspace.GetStatusText().contains(failure.Message) == true);
    CHECK(status->textFormat() == Qt::PlainText);
    CHECK(status->textInteractionFlags().testFlag(Qt::TextSelectableByMouse) == true);
    QScrollArea* statusPanel = workspace.findChild<QScrollArea*>(QStringLiteral("renderGraphStatusPanel"));
    REQUIRE(statusPanel != nullptr);
    CHECK(statusPanel->height() <= statusHeightAllowance);
    REQUIRE(statusPanel->verticalScrollBar()->maximum() > 0);
    statusPanel->verticalScrollBar()->setValue(statusPanel->verticalScrollBar()->maximum());
    CHECK(status->geometry().bottom() <= statusPanel->viewport()->rect().bottom());

    // A shorter result releases the scroll range and leaves editing usable after resizing.
    failure.Message = QStringLiteral("one shader error");
    workspace.AcceptResult(failure);
    workspace.resize(1100, 700);
    QCoreApplication::processEvents();
    CHECK(statusPanel->verticalScrollBar()->maximum() == 0);
    CHECK(splitter->height() >= editingHeight - statusHeightAllowance - 100);
}

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

TEST_CASE("shader editing shares history across refresh and source switches", "[editor][render-graph][text]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString package = MakePackage(project);
    const QString shader = QStringLiteral("editable.slang");
    const QByteArray original = QByteArrayLiteral("float4 Shade() { return float4(1); }\n");
    QFile file(QDir(package).filePath(shader));
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(original) == original.size());
    file.close();
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QComboBox* sourceOrNull = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphSourceFile"));
    CodeEditor* editorOrNull = workspace.findChild<CodeEditor*>(QStringLiteral("renderGraphSource"));
    REQUIRE(sourceOrNull != nullptr);
    REQUIRE(editorOrNull != nullptr);
    sourceOrNull->setCurrentText(shader);
    QSignalSpy draftChanges(&workspace, &RendererWorkspace::DocumentChanged);
    editorOrNull->moveCursor(QTextCursor::End);
    editorOrNull->insertPlainText(QStringLiteral("// drafted\n"));
    CHECK(draftChanges.count() == 0); // Typing must not re-register the package's file watches.
    const QByteArray edited = editorOrNull->toPlainText().toUtf8();
    CHECK(workspace.IsDirty() == true);
    CHECK(workspace.CanSave() == true);
    CHECK(workspace.GetDocument().GetDraft(shader) == edited);
    const QByteArray graph = workspace.GetDocument().GetDraft(ROOT_GRAPH);
    REQUIRE(workspace.GetDocument().SetText(ROOT_GRAPH, graph + '\n') == RenderGraphDocumentStatus::Ok);
    workspace.Undo(); // Refreshing other authored state must not erase focused text.
    CHECK(editorOrNull->toPlainText().toUtf8() == edited);
    sourceOrNull->setCurrentText(ROOT_GRAPH);
    CHECK(workspace.GetDocument().GetDraft(shader) == edited);
    sourceOrNull->setCurrentText(shader);
    CHECK(editorOrNull->toPlainText().toUtf8() == edited);
    QTabWidget* viewsOrNull = workspace.findChild<QTabWidget*>(QStringLiteral("renderGraphViews"));
    REQUIRE(viewsOrNull != nullptr);
    viewsOrNull->setCurrentIndex(0);
    workspace.Undo();
    CHECK(viewsOrNull->currentIndex() == 1);
    CHECK(workspace.GetDocument().GetDraft(shader) == original);
    CHECK(editorOrNull->toPlainText().toUtf8() == original);
    editorOrNull->moveCursor(QTextCursor::End);
    editorOrNull->insertPlainText(QStringLiteral("// saved without Apply\n"));
    const QByteArray saved = editorOrNull->toPlainText().toUtf8();
    REQUIRE(workspace.Save() == true);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    CHECK(file.readAll() == saved);
    CHECK(workspace.IsDirty() == false);
    QTest::keyClicks(editorOrNull, QStringLiteral("more"));
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(shader) == saved);
    CHECK(workspace.IsDirty() == false);
}

TEST_CASE("source capacity rejection restores accepted text without blocking navigation",
          "[editor][render-graph][text]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QComboBox* sourceOrNull = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphSourceFile"));
    CodeEditor* editorOrNull = workspace.findChild<CodeEditor*>(QStringLiteral("renderGraphSource"));
    REQUIRE(sourceOrNull != nullptr);
    REQUIRE(editorOrNull != nullptr);
    sourceOrNull->setCurrentText(ROOT_GRAPH);
    const QByteArray original = workspace.GetDocument().GetDraft(ROOT_GRAPH);
    editorOrNull->selectAll();
    const QString oversized(RenderGraphDocument::MAX_FILE_BYTES + 1, QLatin1Char('x'));
    editorOrNull->insertPlainText(oversized);
    CHECK(editorOrNull->toPlainText().toUtf8() == original);
    CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) == original);
    CHECK(workspace.IsDirty() == false);
    CHECK(workspace.GetDocument().CanUndo() == false);
    sourceOrNull->setCurrentText(QStringLiteral("rasterize.lurender.json"));
    CHECK(sourceOrNull->currentText() == QStringLiteral("rasterize.lurender.json"));
}

TEST_CASE("text admission rejects split scalars and stale revisions without history", "[editor][render-graph][text]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QByteArray original = QString::fromUtf8("😀text").toUtf8();
    REQUIRE(document.SetText(ROOT_GRAPH, original) == RenderGraphDocumentStatus::Ok);
    const uint64 revision = document.GetRevision();
    const CodeTextEdit split{1, 1, {" ", 1}};
    CHECK(document.ApplyTextEdit(ROOT_GRAPH, revision, split, {}, {}, false) == CodeEditStatus::InvalidText);
    const CodeTextEdit valid{0, 4, {"x", 1}};
    CHECK(document.ApplyTextEdit(ROOT_GRAPH, revision - 1, valid, {}, {}, false) == CodeEditStatus::Conflict);
    CHECK(document.ApplyTextEdit(ROOT_GRAPH, revision, valid, {1, 1}, {}, false) == CodeEditStatus::InvalidText);
    CHECK(document.GetRevision() == revision);
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
    REQUIRE(document.Undo() == true);
    CHECK(document.CanUndo() == false);
}

TEST_CASE("bound source edit records an offscreen latency baseline", "[.benchmark][editor][render-graph][text]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    const QString shader = QStringLiteral("large.slang");
    QByteArray original;
    const QByteArray line = QByteArrayLiteral("float4 Shade(float4 value) { return value; }\n");
    while (original.size() + line.size() <= RenderGraphDocument::MAX_FILE_BYTES - 1)
    {
        original += line;
    }
    QFile file(QDir(package).filePath(shader));
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(original) == original.size());
    file.close();
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QComboBox* sourcesOrNull = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphSourceFile"));
    CodeEditor* editorOrNull = workspace.findChild<CodeEditor*>(QStringLiteral("renderGraphSource"));
    REQUIRE(sourcesOrNull != nullptr);
    REQUIRE(editorOrNull != nullptr);
    sourcesOrNull->setCurrentText(shader);
    QTabWidget* viewsOrNull = workspace.findChild<QTabWidget*>(QStringLiteral("renderGraphViews"));
    REQUIRE(viewsOrNull != nullptr);
    viewsOrNull->setCurrentIndex(1);
    workspace.resize(1200, 800);
    workspace.show();
    editorOrNull->GoToLine(editorOrNull->blockCount() / 2);
    editorOrNull->moveCursor(QTextCursor::EndOfBlock);
    QApplication::processEvents();
    constexpr int32 SAMPLE_COUNT = 20;
    QVector<int64> samples;
    for (int32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        QElapsedTimer timer;
        timer.start();
        QTest::keyClicks(editorOrNull, QStringLiteral("x"));
        QApplication::processEvents();
        samples.append(timer.nsecsElapsed());
        workspace.Undo();
        QApplication::processEvents();
    }
    // Small test sample, no production sorting/container facility is needed.
    for (int32 index = 1; index < samples.size(); ++index)
    {
        for (int32 previous = index; previous > 0 && samples.at(previous) < samples.at(previous - 1); --previous)
        {
            samples.swapItemsAt(previous, previous - 1);
        }
    }
    INFO("Offscreen input/model/event processing; excludes native compositor latency. System="
         << QSysInfo::prettyProductName().toStdString() << "; Qt=" << qVersion());
    INFO("Bytes=" << original.size() << "; p50 ns=" << samples.at(SAMPLE_COUNT / 2) << "; p95 ns=" << samples.last());
    CHECK(workspace.GetDocument().GetDraft(shader) == original);
}

TEST_CASE("Subgraph replacements repair direct connections and output sources in one edit",
          "[editor][render-graph][subgraph-cleanup]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString library = QStringLiteral("rasterize.lurender.json");
    QJsonObject root;
    REQUIRE(document.GetGraph(ROOT_GRAPH, root) == true);
    QJsonArray configurations = root.value(QStringLiteral("configurations")).toArray();
    QJsonObject parent = configurations.first().toObject();
    parent.insert(QStringLiteral("outputs"),
                  QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("colorAlias")},
                                         {QStringLiteral("type"), QStringLiteral("ColorTexture")},
                                         {QStringLiteral("required"), false},
                                         {QStringLiteral("source"), QStringLiteral("raster.color")}},
                             QJsonObject{{QStringLiteral("id"), QStringLiteral("depthAlias")},
                                         {QStringLiteral("type"), QStringLiteral("DepthTexture")},
                                         {QStringLiteral("required"), false},
                                         {QStringLiteral("source"), QStringLiteral("raster.depth")}}});
    configurations.replace(0, parent);
    root.insert(QStringLiteral("configurations"), configurations);
    REQUIRE(document.SetText(ROOT_GRAPH, EncodeCanonicalJson(root)) == RenderGraphDocumentStatus::Ok);
    const QByteArray before = document.GetDraft(ROOT_GRAPH);
    const QByteArray beforeLibrary = document.GetDraft(library);
    QJsonObject replacement;
    for (const QJsonValueConstRef value : parent.value(QStringLiteral("nodes")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")) == QStringLiteral("raster"))
        {
            replacement = value.toObject();
        }
    }
    REQUIRE(replacement.isEmpty() == false);
    bool shouldKeepDepth = true;
    QString error;
    SECTION("Switching configurations removes absent input and output ports")
    {
        replacement.insert(QStringLiteral("configuration"), QStringLiteral("DepthOnly"));
        REQUIRE(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, replacement, &error) ==
                RenderGraphDocumentStatus::Ok);
    }
    SECTION("Switching assets also removes incompatible port types")
    {
        REQUIRE(document.CreateSubgraph(ROOT_GRAPH,
                                        DEFAULT_CONFIGURATION,
                                        QStringLiteral("other"),
                                        QStringLiteral("Other")) == RenderGraphDocumentStatus::Ok);
        const QString other = QStringLiteral("Other.lurender.json");
        QJsonObject child = GetConfiguration(document, library, QStringLiteral("DepthOnly"));
        child.insert(QStringLiteral("id"), QStringLiteral("Retyped"));
        QJsonArray inputs = child.value(QStringLiteral("inputs")).toArray();
        QJsonArray edges = child.value(QStringLiteral("edges")).toArray();
        for (qsizetype index = 0; index < inputs.size(); ++index)
        {
            QJsonObject input = inputs.at(index).toObject();
            if (input.value(QStringLiteral("id")) == QStringLiteral("depth"))
            {
                input.insert(QStringLiteral("id"), QStringLiteral("color"));
                inputs.replace(index, input);
            }
        }
        for (qsizetype index = 0; index < edges.size(); ++index)
        {
            QJsonObject edge = edges.at(index).toObject();
            if (edge.value(QStringLiteral("from")) == QStringLiteral("input.depth"))
            {
                edge.insert(QStringLiteral("from"), QStringLiteral("input.color"));
                edges.replace(index, edge);
            }
        }
        QJsonArray outputs = child.value(QStringLiteral("outputs")).toArray();
        QJsonObject output = outputs.first().toObject();
        output.insert(QStringLiteral("id"), QStringLiteral("color"));
        outputs.replace(0, output);
        child.insert(QStringLiteral("inputs"), inputs);
        child.insert(QStringLiteral("edges"), edges);
        child.insert(QStringLiteral("outputs"), outputs);
        QJsonObject otherGraph;
        REQUIRE(document.GetGraph(other, otherGraph) == true);
        otherGraph.insert(QStringLiteral("configurations"), QJsonArray{child});
        otherGraph.insert(QStringLiteral("default"), QStringLiteral("Retyped"));
        REQUIRE(document.SetText(other, EncodeCanonicalJson(otherGraph)) == RenderGraphDocumentStatus::Ok);
        replacement.insert(QStringLiteral("asset"), other);
        replacement.insert(QStringLiteral("configuration"), QStringLiteral("Retyped"));
        const QByteArray beforeSwitch = document.GetDraft(ROOT_GRAPH);
        REQUIRE(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, replacement, &error) ==
                RenderGraphDocumentStatus::Ok);
        REQUIRE(document.Undo() == true);
        CHECK(document.GetDraft(ROOT_GRAPH) == beforeSwitch);
        REQUIRE(document.Redo() == true);
        shouldKeepDepth = false;
    }
    SECTION("Creating a new empty library cleans the rebound node")
    {
        REQUIRE(document.CreateSubgraph(ROOT_GRAPH,
                                        DEFAULT_CONFIGURATION,
                                        QStringLiteral("raster"),
                                        QStringLiteral("Empty")) == RenderGraphDocumentStatus::Ok);
        shouldKeepDepth = false;
    }
    CHECK(error.isEmpty() == true);
    CHECK(GetInputConnections(document, QStringLiteral("raster.color")).isEmpty() == true);
    CHECK(GetInputConnections(document, QStringLiteral("raster.depth")).isEmpty() == (shouldKeepDepth == false));
    CHECK(GetInputConnections(document, QStringLiteral("raster.scene")).isEmpty() ==
          (replacement.value(QStringLiteral("configuration")) == QStringLiteral("ColorAndDepth")));
    CHECK(GetInputConnections(document, QStringLiteral("camera.target")).size() == 1);
    parent = GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION);
    for (const QJsonValueConstRef edge : parent.value(QStringLiteral("edges")).toArray())
    {
        CHECK(edge.toObject().value(QStringLiteral("from")) != QStringLiteral("raster.color"));
    }
    const QJsonArray aliases = parent.value(QStringLiteral("outputs")).toArray();
    REQUIRE(aliases.size() == 2);
    CHECK(aliases.first().toObject().value(QStringLiteral("source")).toString().isEmpty() == true);
    CHECK(aliases.last().toObject().value(QStringLiteral("source")).toString() ==
          (shouldKeepDepth == true ? QStringLiteral("raster.depth") : QString()));
    CHECK(document.GetDraft(library) == beforeLibrary);
    const QByteArray after = document.GetDraft(ROOT_GRAPH);
    REQUIRE(document.Undo() == true);
    // The asset-switch section had an unrelated Subgraph added before the replacement.
    if (replacement.value(QStringLiteral("asset")) == library)
    {
        CHECK(document.GetDraft(ROOT_GRAPH) == before);
    }
    REQUIRE(document.Redo() == true);
    CHECK(document.GetDraft(ROOT_GRAPH) == after);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(ROOT_GRAPH) == after);
}

TEST_CASE("Unavailable Subgraph replacements preserve sources and history", "[editor][render-graph][subgraph-cleanup]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject replacement;
    for (const QJsonValueConstRef node :
         GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION).value(QStringLiteral("nodes")).toArray())
    {
        if (node.toObject().value(QStringLiteral("id")) == QStringLiteral("raster"))
        {
            replacement = node.toObject();
        }
    }
    const QByteArray before = document.GetDraft(ROOT_GRAPH);
    const uint64 revision = document.GetRevision();
    replacement.insert(QStringLiteral("configuration"), QStringLiteral("Missing"));
    QString error;
    CHECK(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, replacement, &error) ==
          RenderGraphDocumentStatus::InvalidEdit);
    CHECK(error.contains(QStringLiteral("Missing")) == true);
    CHECK(document.GetDraft(ROOT_GRAPH) == before);
    CHECK(document.GetRevision() == revision);
}

TEST_CASE("Subgraph dropdowns clean missing connections through Properties and the canvas",
          "[editor][render-graph][subgraph-cleanup]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    const bool shouldSwitchAsset = GENERATE(false, true);
    const QString library = QStringLiteral("rasterize.lurender.json");
    const QString selection = QStringLiteral("InternalDepth");
    QJsonObject child = GetConfiguration(document, library, QStringLiteral("ColorAndDepth"));
    child.insert(QStringLiteral("id"), selection);
    QJsonArray inputs;
    for (const QJsonValueConstRef input : child.value(QStringLiteral("inputs")).toArray())
    {
        if (input.toObject().value(QStringLiteral("id")) != QStringLiteral("depth"))
        {
            inputs.append(input);
        }
    }
    QJsonArray nodes = child.value(QStringLiteral("nodes")).toArray();
    for (const QJsonValueConstRef node :
         GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION).value(QStringLiteral("nodes")).toArray())
    {
        if (node.toObject().value(QStringLiteral("id")) == QStringLiteral("depth"))
        {
            QJsonObject internal = node.toObject();
            internal.insert(QStringLiteral("id"), QStringLiteral("internalDepth"));
            nodes.append(internal);
        }
    }
    QJsonArray edges;
    for (const QJsonValueConstRef value : child.value(QStringLiteral("edges")).toArray())
    {
        QJsonObject edge = value.toObject();
        if (edge.value(QStringLiteral("from")) == QStringLiteral("input.depth"))
        {
            edge.insert(QStringLiteral("from"), QStringLiteral("internalDepth.texture"));
        }
        edges.append(edge);
    }
    child.insert(QStringLiteral("inputs"), inputs);
    child.insert(QStringLiteral("nodes"), nodes);
    child.insert(QStringLiteral("edges"), edges);
    QString target = library;
    if (shouldSwitchAsset == true)
    {
        REQUIRE(document.CreateSubgraph(ROOT_GRAPH,
                                        DEFAULT_CONFIGURATION,
                                        QStringLiteral("other"),
                                        QStringLiteral("Other")) == RenderGraphDocumentStatus::Ok);
        target = QStringLiteral("Other.lurender.json");
    }
    QJsonObject childGraph;
    REQUIRE(document.GetGraph(target, childGraph) == true);
    QJsonArray configurations = childGraph.value(QStringLiteral("configurations")).toArray();
    configurations.append(child);
    childGraph.insert(QStringLiteral("configurations"), configurations);
    childGraph.insert(QStringLiteral("default"), selection);
    REQUIRE(document.SetText(target, EncodeCanonicalJson(childGraph)) == RenderGraphDocumentStatus::Ok);
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, QStringLiteral("raster")) == true);
    const QByteArray before = document.GetDraft(ROOT_GRAPH);
    const QString field = shouldSwitchAsset == true ? QStringLiteral("asset") : QStringLiteral("configuration");
    const QString choice = shouldSwitchAsset == true ? target : selection;
    const auto exercise = [&workspace, &document, &before, &field, &choice](QWidget& inoutFields) {
        QComboBox* choiceOrNull = inoutFields.findChild<QComboBox*>(QStringLiteral("renderGraphField_") + field);
        REQUIRE(choiceOrNull != nullptr);
        const uint64 revision = document.GetRevision();
        choiceOrNull->setCurrentText(choice);
        Q_EMIT choiceOrNull->activated(choiceOrNull->currentIndex());
        CHECK(document.GetRevision() == revision + 1);
        CHECK(GetInputConnections(document, QStringLiteral("raster.color")).size() == 1);
        CHECK(GetInputConnections(document, QStringLiteral("raster.depth")).isEmpty() == true);
        CHECK(GetInputConnections(document, QStringLiteral("output.texture")).size() == 1);
        const QByteArray after = document.GetDraft(ROOT_GRAPH);
        workspace.Undo();
        CHECK(document.GetDraft(ROOT_GRAPH) == before);
        workspace.Redo();
        CHECK(document.GetDraft(ROOT_GRAPH) == after);
        REQUIRE(workspace.Save() == true);
    };
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    SECTION("Properties")
    {
        exercise(*properties);
    }
    SECTION("Canvas with Properties hidden")
    {
        workspace.resize(1200, 800);
        workspace.show();
        properties->hide();
        QCoreApplication::processEvents();
        RenderGraphCanvas& canvas = workspace.GetCanvas();
        canvas.Rebuild();
        QTimer::singleShot(0, &canvas, [&canvas, &exercise, properties]() {
            QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            REQUIRE(menuOrNull != nullptr);
            QAction* editOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphEditNodeAction"));
            REQUIRE(editOrNull != nullptr);
            QTimer::singleShot(0, &canvas, [&exercise, properties]() {
                QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                REQUIRE(dialogOrNull != nullptr);
                CHECK(properties->isVisible() == false);
                exercise(*dialogOrNull);
                dialogOrNull->reject();
            });
            menuOrNull->close();
            editOrNull->trigger();
        });
        const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("raster")) + QPointF(20, 10));
        QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
        QApplication::sendEvent(canvas.viewport(), &context);
    }
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(ROOT_GRAPH) == document.GetDraft(ROOT_GRAPH));
}

TEST_CASE("Source focus retains the editor and restores tree and API preferences",
          "[editor][render-graph][source-focus]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString package = MakePackage(project);
    QFile shader(QDir(package).filePath(QStringLiteral("focus.slang")));
    REQUIRE(shader.open(QIODevice::WriteOnly) == true);
    shader.write("float4 Color();\n");
    shader.close();
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QTabWidget* views = workspace.findChild<QTabWidget*>(QStringLiteral("renderGraphViews"));
    QComboBox* files = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphSourceFile"));
    CodeEditor* source = workspace.findChild<CodeEditor*>(QStringLiteral("renderGraphSource"));
    QPushButton* api = workspace.findChild<QPushButton*>(QStringLiteral("renderGraphShaderApiToggle"));
    QPushButton* focus = workspace.findChild<QPushButton*>(QStringLiteral("renderGraphFocusSource"));
    QSplitter* split = workspace.findChild<QSplitter*>(QStringLiteral("renderGraphSourceSplitter"));
    REQUIRE(views != nullptr);
    REQUIRE(files != nullptr);
    REQUIRE(source != nullptr);
    REQUIRE(api != nullptr);
    REQUIRE(focus != nullptr);
    REQUIRE(split != nullptr);
    files->setCurrentText(QStringLiteral("focus.slang"));
    views->setCurrentIndex(1);
    workspace.resize(1400, 850);
    workspace.show();
    api->setChecked(true);
    QCoreApplication::processEvents();
    QWidget* reference = split->widget(1);
    REQUIRE(reference->isVisible() == true);
    source->GoToLine(1, 4);
    const int32 position = source->textCursor().position();
    const int32 editorWidth = source->width();
    const QString captures = qEnvironmentVariable("LUDUS_EDITOR_LAYOUT_CAPTURE");
    if (captures.isEmpty() == false)
    {
        REQUIRE(workspace.grab().save(captures + QStringLiteral("-source-workspace.png")) == true);
    }
    QObject::connect(&workspace, &RendererWorkspace::FocusViewRequested, &workspace, [&workspace, focus]() {
        workspace.SetChromeVisible(focus->text() != QStringLiteral("Focus Source"));
        workspace.FocusActiveView();
    });
    focus->click();
    QCoreApplication::processEvents();
    CHECK(views->currentIndex() == 1);
    CHECK(source->isVisible() == true);
    CHECK(reference->isVisible() == false);
    CHECK(source->width() > editorWidth);
    CHECK(source->textCursor().position() == position);
    CHECK(api->isChecked() == true);
    if (captures.isEmpty() == false)
    {
        REQUIRE(workspace.grab().save(captures + QStringLiteral("-source-workspace-focused.png")) == true);
    }
    focus->click();
    QCoreApplication::processEvents();
    CHECK(reference->isVisible() == true);
    CHECK(views->currentIndex() == 1);
    CHECK(source->textCursor().position() == position);
    api->setChecked(false);
    files->setCurrentText(ROOT_GRAPH);
    files->setCurrentText(QStringLiteral("focus.slang"));
    CHECK(reference->isHidden() == true);
}

TEST_CASE("Source status separates dirty files live preview and failed revisions",
          "[editor][render-graph][source-status]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    QFile shader(QDir(package).filePath(QStringLiteral("status.slang")));
    REQUIRE(shader.open(QIODevice::WriteOnly) == true);
    shader.write("float4 Color();\n");
    shader.close();
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QLabel* status = workspace.findChild<QLabel*>(QStringLiteral("renderGraphSourceStatus"));
    REQUIRE(status != nullptr);
    CHECK(status->text().contains(QStringLiteral("Live Preview off")) == true);
    RenderGraphJobResult failure;
    failure.Request.ProjectEpoch = 1;
    failure.Request.Action = QStringLiteral("build");
    QComboBox* configuration = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphConfiguration"));
    REQUIRE(configuration != nullptr);
    failure.Request.InputIdentity =
        ComputeRenderGraphBuildIdentity(workspace.GetDocument(), configuration->currentText());
    failure.Code = QStringLiteral("ShaderFailure");
    failure.Message = QStringLiteral("Fix shader");
    workspace.AcceptResult(failure);
    INFO(status->text().toStdString());
    CHECK(status->text().contains(QStringLiteral("Build / validation failed")) == true);
    CHECK(status->text().contains(QStringLiteral("Earlier build")) == false);
    RenderGraphJobResult obsolete = failure;
    obsolete.Request.ProjectEpoch = 0;
    obsolete.Message = QStringLiteral("Obsolete result");
    workspace.AcceptResult(obsolete);
    CHECK(status->text().contains(QStringLiteral("Obsolete")) == false);
    CodeEditor* source = workspace.findChild<CodeEditor*>(QStringLiteral("renderGraphSource"));
    QComboBox* files = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphSourceFile"));
    REQUIRE(source != nullptr);
    REQUIRE(files != nullptr);
    files->setCurrentText(QStringLiteral("status.slang"));
    source->moveCursor(QTextCursor::End);
    source->insertPlainText(QStringLiteral(" "));
    CHECK(status->text().contains(QStringLiteral("Unsaved")) == true);
    CHECK(status->text().contains(QStringLiteral("Preview retained")) == true);
    CHECK(QTest::qWaitFor([status]() { return status->text().contains(QStringLiteral("Earlier build")); }) == true);
}
