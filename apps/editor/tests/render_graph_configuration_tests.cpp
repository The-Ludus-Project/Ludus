#include "internal/render_graph_configuration_editor.h"
#include "internal/render_graph_live_build.h"
#include "internal/render_graph_pipeline.h"
#include "internal/renderer_workspace.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
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
    workspace.SetLivePreview(true);
    REQUIRE(waitFor([&]() { return finished.count() == 4; }) == true);
    CHECK(workspace.GetLastResult().IsSuccessful == false);
    CHECK(workspace.GetStatusText().contains(QStringLiteral("Preview unchanged")) == true);
    QTest::qWait(SETTLE_MILLISECONDS);
    CHECK(finished.count() == 4);
}
