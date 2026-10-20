#include "internal/render_graph_configuration_editor.h"
#include "internal/renderer_workspace.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
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
