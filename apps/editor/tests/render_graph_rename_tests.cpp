#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_pipeline_node.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
const QString ROOT = QStringLiteral("main.lurender.json");
const QString LIBRARY = QStringLiteral("library.lurender.json");
const QString CONFIGURATION = QStringLiteral("Default");
const QString NODE = QStringLiteral("color");
const QString NAME = QStringLiteral("SceneColor");

QString MakeRenamePackage(const QTemporaryDir& project)
{
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    REQUIRE(QDir().mkpath(package) == true);
    const QByteArray bytes = R"({"version":2,"id":"Main","default":"Default","configurations":[
        {"id":"Default","inputs":[{"id":"color","type":"ColorTexture"}],
         "outputs":[{"id":"result","type":"ColorTexture","source":"color.texture"},
                    {"id":"untouched","type":"ColorTexture","source":"color_more.texture"},
                    {"id":"incomplete","type":"ColorTexture"}],"nodes":[
            {"id":"size","kind":"RenderSize","extent":[640,360]},
            {"id":"color","kind":"Target","type":"ColorTexture","format":"RGBA8Unorm",
             "extent":"RenderSize","samples":1,"imported":false,"initialized":false,"presentation":false},
            {"id":"color_more","kind":"Export","type":"ColorTexture"},
            {"id":"output","kind":"Export","type":"ColorTexture"}],
         "edges":[{"from":"size.size","to":"color.size"},
                  {"from":"color.texture","to":"output.texture"},
                  {"from":"input.color","to":"color_more.texture"}],
         "layout":{"positions":[{"node":"color","x":80,"y":120},{"node":"output","x":300,"y":0}]}}]})";
    QJsonObject graph = QJsonDocument::fromJson(bytes).object();
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    QJsonObject alternate = configurations.first().toObject();
    alternate.insert(QStringLiteral("id"), QStringLiteral("Alternate"));
    configurations.append(alternate);
    graph.insert(QStringLiteral("configurations"), configurations);
    const QByteArray canonical = EncodeCanonicalJson(graph);
    for (const QString& name : {ROOT, LIBRARY})
    {
        QFile file(QDir(package).filePath(name));
        REQUIRE(file.open(QIODevice::WriteOnly) == true);
        REQUIRE(file.write(canonical) == canonical.size());
    }
    return package;
}

QJsonObject ReadConfiguration(const RenderGraphDocument& document)
{
    QJsonObject graph;
    REQUIRE(document.GetGraph(ROOT, graph) == true);
    return graph.value(QStringLiteral("configurations")).toArray().first().toObject();
}
} // namespace

TEST_CASE("Node renames update every scoped reference in one undoable saved edit", "[editor][render-graph][rename]")
{
    QTemporaryDir project;
    const QString package = MakeRenamePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT) == RenderGraphDocumentStatus::Ok);
    const QByteArray before = document.GetDraft(ROOT);
    const QJsonObject original = ReadConfiguration(document);
    QString error;
    REQUIRE(document.RenameNode(ROOT, CONFIGURATION, NODE, NAME, error) == RenderGraphDocumentStatus::Ok);
    CHECK(error.isEmpty() == true);
    CHECK(document.GetRevision() == 1);
    const QJsonObject renamed = ReadConfiguration(document);
    CHECK(renamed.value(QStringLiteral("nodes")).toArray().at(1).toObject().value(QStringLiteral("id")) == NAME);
    const QJsonArray edges = renamed.value(QStringLiteral("edges")).toArray();
    CHECK(edges.at(0).toObject().value(QStringLiteral("to")) == QStringLiteral("SceneColor.size"));
    CHECK(edges.at(1).toObject().value(QStringLiteral("from")) == QStringLiteral("SceneColor.texture"));
    CHECK(edges.at(2) == original.value(QStringLiteral("edges")).toArray().at(2));
    const QJsonArray outputs = renamed.value(QStringLiteral("outputs")).toArray();
    CHECK(outputs.first().toObject().value(QStringLiteral("source")) == QStringLiteral("SceneColor.texture"));
    CHECK(outputs.at(1) == original.value(QStringLiteral("outputs")).toArray().at(1));
    CHECK(outputs.at(2) == original.value(QStringLiteral("outputs")).toArray().at(2));
    CHECK(renamed.value(QStringLiteral("inputs")) == original.value(QStringLiteral("inputs")));
    QJsonObject position = original.value(QStringLiteral("layout"))
                               .toObject()
                               .value(QStringLiteral("positions"))
                               .toArray()
                               .first()
                               .toObject();
    position.insert(QStringLiteral("node"), NAME);
    CHECK(renamed.value(QStringLiteral("layout")).toObject().value(QStringLiteral("positions")).toArray().first() ==
          position);
    const QJsonObject graph = QJsonDocument::fromJson(document.GetDraft(ROOT)).object();
    CHECK(graph.value(QStringLiteral("configurations")).toArray().at(1) ==
          QJsonDocument::fromJson(before).object().value(QStringLiteral("configurations")).toArray().at(1));
    CHECK(document.GetDraft(LIBRARY) == before);
    const QByteArray after = document.GetDraft(ROOT);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(ROOT) == before);
    CHECK(document.GetHistoryNodeSelection().Node == NODE);
    CHECK(document.CanUndo() == false);
    REQUIRE(document.Redo() == true);
    CHECK(document.GetDraft(ROOT) == after);
    CHECK(document.GetHistoryNodeSelection().Node == NAME);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(ROOT) == after);
}

TEST_CASE("Rejected and unchanged node names preserve bytes revision and redo", "[editor][render-graph][rename]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeRenamePackage(project), ROOT) == RenderGraphDocumentStatus::Ok);
    QString error;
    REQUIRE(document.RenameNode(ROOT, CONFIGURATION, NODE, NAME, error) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.Undo() == true);
    const QByteArray before = document.GetDraft(ROOT);
    const uint64 revision = document.GetRevision();
    for (const QString& name : {QString(),
                                QStringLiteral("bad name"),
                                QStringLiteral("input"),
                                QStringLiteral("1color"),
                                QStringLiteral("color.texture"),
                                QStringLiteral("cölör"),
                                QString(RenderGraphDocument::MAX_IDENTIFIER_LENGTH + 1, QLatin1Char('a')),
                                QStringLiteral("output")})
    {
        INFO(name.toStdString());
        CHECK(document.RenameNode(ROOT, CONFIGURATION, NODE, name, error) == RenderGraphDocumentStatus::InvalidEdit);
        CHECK(error.isEmpty() == false);
        CHECK(document.GetDraft(ROOT) == before);
        CHECK(document.GetRevision() == revision);
        CHECK(document.CanRedo() == true);
    }
    CHECK(document.RenameNode(ROOT, CONFIGURATION, QStringLiteral("missing"), NAME, error) ==
          RenderGraphDocumentStatus::InvalidEdit);
    CHECK(document.RenameNode(ROOT, QStringLiteral("missing"), NODE, NAME, error) ==
          RenderGraphDocumentStatus::InvalidEdit);
    CHECK(document.RenameNode(QStringLiteral("missing.json"), CONFIGURATION, NODE, NAME, error) ==
          RenderGraphDocumentStatus::Malformed);
    REQUIRE(document.RenameNode(ROOT, CONFIGURATION, NODE, NODE, error) == RenderGraphDocumentStatus::Ok);
    CHECK(error.isEmpty() == true);
    CHECK(document.GetDraft(ROOT) == before);
    CHECK(document.GetRevision() == revision);
    REQUIRE(document.Redo() == true);
    CHECK(ReadConfiguration(document)
              .value(QStringLiteral("nodes"))
              .toArray()
              .at(1)
              .toObject()
              .value(QStringLiteral("id")) == NAME);
}

TEST_CASE("Pipeline node renames preserve shader identities and optional layout", "[editor][render-graph][rename]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeRenamePackage(project), ROOT) == RenderGraphDocumentStatus::Ok);
    QJsonObject graph = QJsonDocument::fromJson(document.GetDraft(ROOT)).object();
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    QJsonObject configuration = configurations.first().toObject();
    QJsonArray nodes = configuration.value(QStringLiteral("nodes")).toArray();
    QJsonObject pipeline = MakePipelineNode(QStringLiteral("RayTracingPipeline"), NODE);
    pipeline.insert(QStringLiteral("shaders"),
                    QJsonArray{QJsonObject{{QStringLiteral("id"), NODE},
                                           {QStringLiteral("stage"), QStringLiteral("ray_generation")},
                                           {QStringLiteral("program"), NODE}}});
    nodes.replace(1, pipeline);
    configuration.insert(QStringLiteral("nodes"), nodes);
    configuration.remove(QStringLiteral("layout"));
    configurations.replace(0, configuration);
    graph.insert(QStringLiteral("configurations"), configurations);
    REQUIRE(document.SetText(ROOT, EncodeCanonicalJson(graph)) == RenderGraphDocumentStatus::Ok);
    const QByteArray shader = QByteArrayLiteral("// color remains an authored shader name\n");
    // Loading a source makes it part of the package independently of node identity.
    QFile source(QDir(document.GetPackageRoot()).filePath(QStringLiteral("color.slang")));
    REQUIRE(source.open(QIODevice::WriteOnly) == true);
    REQUIRE(source.write(shader) == shader.size());
    source.close();
    RenderGraphDocument loaded;
    REQUIRE(loaded.Load(document.GetPackageRoot(), ROOT) == RenderGraphDocumentStatus::Ok);
    REQUIRE(loaded.SetText(ROOT, document.GetDraft(ROOT)) == RenderGraphDocumentStatus::Ok);
    QString error;
    REQUIRE(loaded.RenameNode(ROOT, CONFIGURATION, NODE, NAME, error) == RenderGraphDocumentStatus::Ok);
    pipeline.insert(QStringLiteral("id"), NAME);
    CHECK(ReadConfiguration(loaded).value(QStringLiteral("nodes")).toArray().at(1) == pipeline);
    CHECK(ReadConfiguration(loaded).contains(QStringLiteral("layout")) == false);
    CHECK(loaded.GetDraft(QStringLiteral("color.slang")) == shader);
    CHECK(loaded.GetFiles().contains(QStringLiteral("SceneColor.slang")) == false);
}

TEST_CASE("Rename capacity failures preserve history and all draft bytes", "[editor][render-graph][rename]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeRenamePackage(project), ROOT) == RenderGraphDocumentStatus::Ok);
    QJsonObject graph = QJsonDocument::fromJson(document.GetDraft(ROOT)).object();
    graph.insert(QStringLiteral("padding"), QString());
    const qsizetype emptySize = EncodeCanonicalJson(graph).size();
    graph.insert(QStringLiteral("padding"), QString(RenderGraphDocument::MAX_FILE_BYTES - emptySize, QLatin1Char('p')));
    const QByteArray bytes = EncodeCanonicalJson(graph);
    REQUIRE(document.SetText(ROOT, bytes) == RenderGraphDocumentStatus::Ok);
    const uint64 revision = document.GetRevision();
    QString error;
    CHECK(document.RenameNode(ROOT, CONFIGURATION, NODE, NAME, error) == RenderGraphDocumentStatus::Capacity);
    CHECK(error.isEmpty() == false);
    CHECK(document.GetDraft(ROOT) == bytes);
    CHECK(document.GetRevision() == revision);
    REQUIRE(document.Undo() == true);
    CHECK(document.CanUndo() == false);
}

TEST_CASE("Canvas menu and edit dialog rename with Properties hidden", "[editor][render-graph][rename]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    const QString package = MakeRenamePackage(project);
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT)) == true);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.SelectNode(ROOT, CONFIGURATION, NODE) == true);
    workspace.resize(1200, 800);
    workspace.show();
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    properties->hide();
    QCoreApplication::processEvents();
    bool shouldUseRenameMenu = false;
    bool shouldCancel = false;
    SECTION("edit dialog") {}
    SECTION("rename menu")
    {
        shouldUseRenameMenu = true;
    }
    SECTION("cancel rename menu")
    {
        shouldUseRenameMenu = true;
        shouldCancel = true;
    }
    const QByteArray before = workspace.GetDocument().GetDraft(ROOT);
    const uint64 revision = workspace.GetDocument().GetRevision();
    bool hasOpenedDialog = false;
    bool hasRejectedDuplicate = false;
    bool hasUpdatedDialog = false;
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    QTimer::singleShot(0, &canvas, [&]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menuOrNull == nullptr)
        {
            return;
        }
        QAction* actionOrNull =
            menuOrNull->findChild<QAction*>(shouldUseRenameMenu == true ? QStringLiteral("renderGraphRenameNodeAction")
                                                                        : QStringLiteral("renderGraphEditNodeAction"));
        menuOrNull->close();
        if (actionOrNull == nullptr)
        {
            return;
        }
        QTimer::singleShot(0, &canvas, [&]() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialogOrNull == nullptr)
            {
                return;
            }
            QLineEdit* nameOrNull = dialogOrNull->findChild<QLineEdit*>(QStringLiteral("renderGraphNodeName"));
            QPushButton* renameOrNull = dialogOrNull->findChild<QPushButton*>(QStringLiteral("renderGraphRenameNode"));
            QLabel* errorOrNull = dialogOrNull->findChild<QLabel*>(QStringLiteral("renderGraphNodeNameError"));
            hasOpenedDialog = nameOrNull != nullptr && renameOrNull != nullptr && errorOrNull != nullptr &&
                              properties->isVisible() == false;
            if (hasOpenedDialog == false)
            {
                dialogOrNull->reject();
                return;
            }
            if (shouldCancel == true)
            {
                nameOrNull->setText(NAME);
                dialogOrNull->reject();
                return;
            }
            nameOrNull->setText(QStringLiteral("output"));
            renameOrNull->click();
            hasRejectedDuplicate = errorOrNull->text().isEmpty() == false &&
                                   workspace.GetDocument().GetDraft(ROOT) == before &&
                                   workspace.GetDocument().GetRevision() == revision;
            nameOrNull->setText(NAME);
            QTest::keyClick(nameOrNull, Qt::Key_Return);
            if (shouldUseRenameMenu == false)
            {
                QLineEdit* renamedOrNull = dialogOrNull->findChild<QLineEdit*>(QStringLiteral("renderGraphNodeName"));
                hasUpdatedDialog = renamedOrNull != nullptr && renamedOrNull->text() == NAME &&
                                   dialogOrNull->windowTitle() == QStringLiteral("Edit SceneColor");
                dialogOrNull->reject();
            }
        });
        actionOrNull->trigger();
    });
    const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(NODE) + QPointF(20, 10));
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QApplication::sendEvent(canvas.viewport(), &context);
    REQUIRE(hasOpenedDialog == true);
    if (shouldCancel == true)
    {
        CHECK(workspace.GetDocument().GetDraft(ROOT) == before);
        CHECK(workspace.GetDocument().GetRevision() == revision);
        return;
    }
    CHECK(hasRejectedDuplicate == true);
    if (shouldUseRenameMenu == false)
    {
        CHECK(hasUpdatedDialog == true);
    }
    CHECK(workspace.GetSelection() == QStringLiteral("main.lurender.json#Default/SceneColor"));
    CHECK(canvas.GetSelectedNodes() == QStringList{NAME});
    const QByteArray after = workspace.GetDocument().GetDraft(ROOT);
    CHECK(after != before);
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(ROOT) == before);
    CHECK(workspace.GetSelection() == QStringLiteral("main.lurender.json#Default/color"));
    workspace.Redo();
    CHECK(workspace.GetDocument().GetDraft(ROOT) == after);
    CHECK(workspace.GetSelection() == QStringLiteral("main.lurender.json#Default/SceneColor"));
}
