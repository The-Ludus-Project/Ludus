#include "internal/renderer_workspace.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
const QString ROOT = QStringLiteral("main.lurender.json");
const QString LIBRARY = QStringLiteral("library.lurender.json");
const QString DEFAULT = QStringLiteral("Default");
const QStringList GROUP{QStringLiteral("texture"), QStringLiteral("output")};

QString MakeSelectionPackage(const QTemporaryDir& project)
{
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    REQUIRE(QDir().mkpath(package) == true);
    const QByteArray bytes = R"({"version":1,"id":"Main","default":"Default","configurations":[
        {"id":"Default","inputs":[],"outputs":[],"nodes":[
            {"id":"texture","kind":"Target","type":"ColorTexture","format":"RGBA8Unorm",
             "extent":[640,360],"samples":1,"imported":false,"initialized":false,"presentation":false},
            {"id":"output","kind":"Export","type":"ColorTexture"},
            {"id":"camera","kind":"Camera","active":true,"role":"Offscreen","target_type":"ColorTexture"}],
         "edges":[{"from":"texture.texture","to":"output.texture"},
                  {"from":"texture.texture","to":"camera.target"}],
         "layout":{"positions":[{"node":"texture","x":0,"y":0},{"node":"output","x":300,"y":0},
                                    {"node":"camera","x":0,"y":250}]}}]})";
    const QByteArray canonical = EncodeCanonicalJson(QJsonDocument::fromJson(bytes).object());
    for (const QString& name : {ROOT, LIBRARY})
    {
        QFile file(QDir(package).filePath(name));
        REQUIRE(file.open(QIODevice::WriteOnly) == true);
        REQUIRE(file.write(canonical) == canonical.size());
    }
    return package;
}

QJsonObject ReadConfiguration(const RenderGraphDocument& document, const QString& file, const QString& identity)
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

QVector<RenderGraphNodePosition> GetPositions()
{
    return {{QStringLiteral("texture"), 0, 0}, {QStringLiteral("output"), 300, 0}};
}

QTreeWidgetItem* FindNode(QTreeWidget& inoutTree, const QString& file, const QString& node)
{
    for (QTreeWidgetItemIterator item(&inoutTree); *item != nullptr; ++item)
    {
        if ((*item)->data(0, Qt::UserRole).toString() == file && (*item)->data(0, Qt::UserRole + 2).toString() == node)
        {
            return *item;
        }
    }
    return nullptr;
}

void OpenSelectionPackage(RendererWorkspace& inoutWorkspace, const QTemporaryDir& project)
{
    const QString package = MakeSelectionPackage(project);
    inoutWorkspace.SetProject(project.path(), 1);
    REQUIRE(inoutWorkspace.OpenGraph(QDir(package).filePath(ROOT)) == true);
    inoutWorkspace.SetLivePreview(false);
    inoutWorkspace.resize(1200, 800);
    inoutWorkspace.show();
    QCoreApplication::processEvents();
    REQUIRE(inoutWorkspace.SelectNode(ROOT, DEFAULT, {}) == true);
}
} // namespace

TEST_CASE("Node transfers remap internal edges and layout in one history step", "[editor][render-graph][clipboard]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeSelectionPackage(project), ROOT) == RenderGraphDocumentStatus::Ok);
    QJsonObject payload;
    QString error;
    REQUIRE(document.CopyNodes(ROOT, DEFAULT, GROUP, GetPositions(), payload, error) == RenderGraphDocumentStatus::Ok);
    CHECK(payload.value(QStringLiteral("edges")).toArray().size() == 1);
    const QByteArray before = document.GetDraft(ROOT);
    QStringList created;
    REQUIRE(document.PasteNodes(ROOT, DEFAULT, payload, 40, 80, created, error) == RenderGraphDocumentStatus::Ok);
    CHECK(created == QStringList{QStringLiteral("texture_2"), QStringLiteral("output_2")});
    const QJsonObject configuration = ReadConfiguration(document, ROOT, DEFAULT);
    CHECK(configuration.value(QStringLiteral("nodes")).toArray().size() == 5);
    const QJsonObject edge = configuration.value(QStringLiteral("edges")).toArray().last().toObject();
    CHECK(edge.value(QStringLiteral("from")).toString() == QStringLiteral("texture_2.texture"));
    CHECK(edge.value(QStringLiteral("to")).toString() == QStringLiteral("output_2.texture"));
    const QJsonArray layout =
        configuration.value(QStringLiteral("layout")).toObject().value(QStringLiteral("positions")).toArray();
    CHECK(layout.last().toObject().value(QStringLiteral("x")).toInt() == 340);
    CHECK(layout.last().toObject().value(QStringLiteral("y")).toInt() == 80);
    CHECK(document.GetRevision() == 1);
    const QByteArray pasted = document.GetDraft(ROOT);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(ROOT) == before);
    REQUIRE(document.Redo() == true);
    CHECK(document.GetDraft(ROOT) == pasted);
    REQUIRE(document.RemoveNodes(ROOT, DEFAULT, created) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetDraft(ROOT) == before);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(ROOT) == pasted);
}

TEST_CASE("Configurations and node groups can be created pasted and saved across graph files",
          "[editor][render-graph][clipboard]")
{
    QTemporaryDir project;
    const QString package = MakeSelectionPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT) == RenderGraphDocumentStatus::Ok);
    QJsonObject payload;
    QString error;
    REQUIRE(document.CopyNodes(ROOT, DEFAULT, GROUP, GetPositions(), payload, error) == RenderGraphDocumentStatus::Ok);
    const QByteArray source = document.GetDraft(ROOT);
    const QByteArray original = document.GetDraft(LIBRARY);
    REQUIRE(document.AddConfiguration(LIBRARY, QStringLiteral("Alternate")) == RenderGraphDocumentStatus::Ok);
    const QByteArray empty = document.GetDraft(LIBRARY);
    CHECK(document.AddConfiguration(LIBRARY, QStringLiteral("Alternate")) == RenderGraphDocumentStatus::InvalidEdit);
    CHECK(document.AddConfiguration(LIBRARY, QStringLiteral("bad name")) == RenderGraphDocumentStatus::InvalidEdit);
    CHECK(document.GetDraft(LIBRARY) == empty);
    QStringList created;
    REQUIRE(document.PasteNodes(LIBRARY, QStringLiteral("Alternate"), payload, 0, 0, created, error) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(created == QStringList{QStringLiteral("texture"), QStringLiteral("output")});
    CHECK(document.GetDraft(ROOT) == source);
    CHECK(ReadConfiguration(document, LIBRARY, QStringLiteral("Alternate"))
              .value(QStringLiteral("nodes"))
              .toArray()
              .size() == 2);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(LIBRARY) == document.GetDraft(LIBRARY));
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(LIBRARY) == empty);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(LIBRARY) == original);
    REQUIRE(document.Redo() == true);
    CHECK(document.GetDraft(LIBRARY) == empty);
}

TEST_CASE("Invalid clipboard edits preserve draft revision and output parameters", "[editor][render-graph][clipboard]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeSelectionPackage(project), ROOT) == RenderGraphDocumentStatus::Ok);
    QJsonObject payload;
    QString error;
    REQUIRE(document.CopyNodes(ROOT, DEFAULT, GROUP, GetPositions(), payload, error) == RenderGraphDocumentStatus::Ok);
    const QByteArray before = document.GetDraft(ROOT);
    QStringList created{QStringLiteral("preserved")};
    SECTION("Unknown node in a removal group")
    {
        CHECK(document.RemoveNodes(ROOT, DEFAULT, {QStringLiteral("texture"), QStringLiteral("missing")}) ==
              RenderGraphDocumentStatus::InvalidEdit);
    }
    SECTION("Missing layout record")
    {
        payload.insert(QStringLiteral("positions"), QJsonArray{});
        CHECK(document.PasteNodes(ROOT, DEFAULT, payload, 0, 0, created, error) ==
              RenderGraphDocumentStatus::InvalidEdit);
    }
    SECTION("Foreign endpoint")
    {
        payload.insert(QStringLiteral("edges"),
                       QJsonArray{QJsonObject{{QStringLiteral("from"), QStringLiteral("camera.view")},
                                              {QStringLiteral("to"), QStringLiteral("output.texture")}}});
        CHECK(document.PasteNodes(ROOT, DEFAULT, payload, 0, 0, created, error) ==
              RenderGraphDocumentStatus::InvalidEdit);
    }
    SECTION("Coordinate overflow")
    {
        CHECK(document.PasteNodes(ROOT, DEFAULT, payload, RenderGraphDocument::MAX_COORDINATE, 0, created, error) ==
              RenderGraphDocumentStatus::InvalidEdit);
    }
    CHECK(created == QStringList{QStringLiteral("preserved")});
    CHECK(document.GetDraft(ROOT) == before);
    CHECK(document.GetRevision() == 0);
    CHECK(document.CanUndo() == false);
}

TEST_CASE("Canvas box selection and panning are separate gestures", "[editor][render-graph][clipboard][selection]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenSelectionPackage(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.resetTransform();
    canvas.centerOn(300, 150);
    const QPoint first = canvas.mapFromScene(QPointF(-20, -20));
    const QPoint last = canvas.mapFromScene(QPointF(510, 100));
    const QTransform transform = canvas.viewportTransform();
    QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(canvas.viewport(), last);
    QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, last);
    CHECK(canvas.GetSelectedNodes() == QStringList{QStringLiteral("output"), QStringLiteral("texture")});
    CHECK(canvas.viewportTransform() == transform);
    QTreeWidget* tree = workspace.findChild<QTreeWidget*>(QStringLiteral("renderGraphTree"));
    REQUIRE(tree != nullptr);
    CHECK(tree->selectedItems().size() == 2);
    const QStringList selected = canvas.GetSelectedNodes();
    QTest::mousePress(canvas.viewport(), Qt::MiddleButton, Qt::NoModifier, first);
    QTest::mouseMove(canvas.viewport(), first + QPoint(40, 30));
    QTest::mouseRelease(canvas.viewport(), Qt::MiddleButton, Qt::NoModifier, first + QPoint(40, 30));
    CHECK(canvas.viewportTransform() != transform);
    const QTransform afterMiddle = canvas.viewportTransform();
    QTest::keyPress(&canvas, Qt::Key_Space);
    QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(canvas.viewport(), first + QPoint(20, 20));
    QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, first + QPoint(20, 20));
    QTest::keyRelease(&canvas, Qt::Key_Space);
    CHECK(canvas.viewportTransform() != afterMiddle);
    CHECK(canvas.GetSelectedNodes() == selected);
    CHECK(workspace.GetDocument().GetRevision() == 0);
}

TEST_CASE("Source tree group right click preserves selection and copies the complete group",
          "[editor][render-graph][clipboard][selection]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenSelectionPackage(workspace, project);
    QTreeWidget* tree = workspace.findChild<QTreeWidget*>(QStringLiteral("renderGraphTree"));
    REQUIRE(tree != nullptr);
    QTreeWidgetItem* texture = FindNode(*tree, ROOT, QStringLiteral("texture"));
    QTreeWidgetItem* output = FindNode(*tree, ROOT, QStringLiteral("output"));
    REQUIRE(texture != nullptr);
    REQUIRE(output != nullptr);
    tree->setCurrentItem(texture);
    output->setSelected(true);
    REQUIRE(tree->selectedItems().size() == 2);
    QTimer::singleShot(0, &workspace, [&workspace, tree]() {
        QMenu* menuOrNull = workspace.findChild<QMenu*>(QStringLiteral("renderGraphNodeMenu"));
        REQUIRE(menuOrNull != nullptr);
        CHECK(tree->selectedItems().size() == 2);
        QAction* copy = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphCopyNodes"));
        REQUIRE(copy != nullptr);
        CHECK(copy->isEnabled() == true);
        copy->trigger();
        menuOrNull->close();
    });
    const QPoint point = tree->visualItemRect(output).center();
    // Include the native press as well as the menu event, so selection behavior is exercised.
    QTest::mousePress(tree->viewport(), Qt::RightButton, Qt::NoModifier, point);
    QTest::mouseRelease(tree->viewport(), Qt::RightButton, Qt::NoModifier, point);
    Q_EMIT tree->customContextMenuRequested(point);
    REQUIRE(workspace.CreateConfiguration(QStringLiteral("Pasted")) == true);
    REQUIRE(workspace.PasteNodes(QPointF(100, 100)) == true);
    CHECK(workspace.GetCanvas().GetSelectedNodes().size() == 2);
    const QJsonObject configuration = ReadConfiguration(workspace.GetDocument(), ROOT, QStringLiteral("Pasted"));
    CHECK(configuration.value(QStringLiteral("nodes")).toArray().size() == 2);
    CHECK(configuration.value(QStringLiteral("edges")).toArray().size() == 1);
    workspace.Undo();
    CHECK(workspace.GetCanvas().GetNodeCount() == 0);
    workspace.Undo();
    CHECK(workspace.GetCanvas().GetConfiguration() == DEFAULT);
}

TEST_CASE("Canvas keyboard copy paste and group deletion share document history",
          "[editor][render-graph][clipboard][selection]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenSelectionPackage(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    QTest::keySequence(&canvas, QKeySequence(QKeySequence::SelectAll));
    REQUIRE(canvas.GetSelectedNodes().size() == 3);
    const QPoint camera = canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("camera")) + QPointF(30, 10));
    QTest::mouseClick(canvas.viewport(), Qt::LeftButton, Qt::ControlModifier, camera);
    CHECK(canvas.GetSelectedNodes().size() == 2);
    CHECK(workspace.GetSelection().endsWith(QStringLiteral("/camera")) == false);
    QTest::keySequence(&canvas, QKeySequence(QKeySequence::SelectAll));
    QTest::keySequence(&canvas, QKeySequence(QKeySequence::Copy));
    QTest::keySequence(&canvas, QKeySequence(QKeySequence::Paste));
    CHECK(canvas.GetNodeCount() == 6);
    CHECK(canvas.GetSelectedNodes().size() == 3);
    CHECK(workspace.GetDocument().GetRevision() == 1);
    QTest::keyClick(&canvas, Qt::Key_Delete);
    CHECK(canvas.GetNodeCount() == 3);
    CHECK(workspace.GetDocument().GetRevision() == 2);
    workspace.Undo();
    CHECK(canvas.GetNodeCount() == 6);
    workspace.Undo();
    CHECK(canvas.GetNodeCount() == 3);
}

TEST_CASE("Pasting into another package preserves shared shader identity or rejects a mismatch",
          "[editor][render-graph][clipboard]")
{
    QTemporaryDir sourceProject;
    QTemporaryDir destinationProject;
    const QString sourcePackage = MakeSelectionPackage(sourceProject);
    const QString destinationPackage = MakeSelectionPackage(destinationProject);
    for (const QString& package : {sourcePackage, destinationPackage})
    {
        QFile manifest(QDir(package).filePath(QStringLiteral("main.lushader.json")));
        REQUIRE(manifest.open(QIODevice::WriteOnly) == true);
        REQUIRE(manifest.write(
                    "{\"programs\":[{\"id\":\"Compute\",\"stage\":\"compute\",\"source\":\"shader.slang\"}]}") > 0);
        QFile shader(QDir(package).filePath(QStringLiteral("shader.slang")));
        REQUIRE(shader.open(QIODevice::WriteOnly) == true);
        REQUIRE(shader.write("void ShaderMain() {}\n") > 0);
    }
    RenderGraphDocument source;
    RenderGraphDocument destination;
    REQUIRE(source.Load(sourcePackage, ROOT) == RenderGraphDocumentStatus::Ok);
    REQUIRE(destination.Load(destinationPackage, ROOT) == RenderGraphDocumentStatus::Ok);
    const QJsonObject shader{{QStringLiteral("id"), QStringLiteral("main")},
                             {QStringLiteral("stage"), QStringLiteral("compute")},
                             {QStringLiteral("program"), QStringLiteral("Compute")}};
    const QJsonObject pipeline{{QStringLiteral("id"), QStringLiteral("compute")},
                               {QStringLiteral("kind"), QStringLiteral("ComputePipeline")},
                               {QStringLiteral("shaders"), QJsonArray{shader}}};
    REQUIRE(source.AddNode(ROOT, DEFAULT, pipeline) == RenderGraphDocumentStatus::Ok);
    QJsonObject payload;
    QString error;
    REQUIRE(source.CopyNodes(ROOT,
                             DEFAULT,
                             {QStringLiteral("compute")},
                             {{QStringLiteral("compute"), 0, 0}},
                             payload,
                             error) == RenderGraphDocumentStatus::Ok);
    const QByteArray before = destination.GetDraft(ROOT);
    QStringList created;
    REQUIRE(destination.PasteNodes(ROOT, DEFAULT, payload, 0, 0, created, error) == RenderGraphDocumentStatus::Ok);
    CHECK(destination.GetDraft(QStringLiteral("shader.slang")) == source.GetDraft(QStringLiteral("shader.slang")));
    CHECK(ReadConfiguration(destination, ROOT, DEFAULT).value(QStringLiteral("nodes")).toArray().last().toObject() ==
          pipeline);
    REQUIRE(destination.Undo() == true);
    CHECK(destination.GetDraft(ROOT) == before);
    REQUIRE(destination.SetText(QStringLiteral("shader.slang"), QByteArray("void Different() {}\n")) ==
            RenderGraphDocumentStatus::Ok);
    const uint64 revision = destination.GetRevision();
    CHECK(destination.PasteNodes(ROOT, DEFAULT, payload, 0, 0, created, error) ==
          RenderGraphDocumentStatus::InvalidEdit);
    CHECK(error.contains(QStringLiteral("shader.slang")) == true);
    CHECK(destination.GetDraft(ROOT) == before);
    CHECK(destination.GetRevision() == revision);
}

TEST_CASE("Configuration and paste capacities fail without partial changes", "[editor][render-graph][clipboard]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeSelectionPackage(project), ROOT) == RenderGraphDocumentStatus::Ok);
    SECTION("Configuration capacity")
    {
        for (int32 i = 1; i < RenderGraphDocument::MAX_CONFIGURATIONS; ++i)
        {
            REQUIRE(document.AddConfiguration(ROOT, QStringLiteral("Configuration") + QString::number(i)) ==
                    RenderGraphDocumentStatus::Ok);
        }
        const QByteArray before = document.GetDraft(ROOT);
        const uint64 revision = document.GetRevision();
        CHECK(document.AddConfiguration(ROOT, QStringLiteral("Overflow")) == RenderGraphDocumentStatus::Capacity);
        CHECK(document.GetDraft(ROOT) == before);
        CHECK(document.GetRevision() == revision);
    }
    SECTION("Node capacity")
    {
        QJsonObject payload;
        QString error;
        REQUIRE(document.CopyNodes(ROOT, DEFAULT, GROUP, GetPositions(), payload, error) ==
                RenderGraphDocumentStatus::Ok);
        QJsonObject graph;
        REQUIRE(document.GetGraph(ROOT, graph) == true);
        QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
        QJsonObject configuration = configurations.first().toObject();
        QJsonArray nodes = configuration.value(QStringLiteral("nodes")).toArray();
        while (nodes.size() < RenderGraphDocument::MAX_NODES - 1)
        {
            nodes.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("scene") + QString::number(nodes.size())},
                                     {QStringLiteral("kind"), QStringLiteral("Scene")}});
        }
        configuration.insert(QStringLiteral("nodes"), nodes);
        configurations.replace(0, configuration);
        graph.insert(QStringLiteral("configurations"), configurations);
        REQUIRE(document.SetText(ROOT, EncodeCanonicalJson(graph)) == RenderGraphDocumentStatus::Ok);
        const QByteArray before = document.GetDraft(ROOT);
        const uint64 revision = document.GetRevision();
        QStringList created;
        CHECK(document.PasteNodes(ROOT, DEFAULT, payload, 0, 0, created, error) == RenderGraphDocumentStatus::Capacity);
        CHECK(document.GetDraft(ROOT) == before);
        CHECK(document.GetRevision() == revision);
    }
}

TEST_CASE("Canvas group move and context menu preserve every selected node",
          "[editor][render-graph][clipboard][selection]")
{
    QTemporaryDir project;
    QWidget inspectorHost;
    RendererWorkspace workspace({});
    OpenSelectionPackage(workspace, project);
    static_cast<void>(workspace.TakeInspectorPanel(&inspectorHost));
    inspectorHost.hide();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    QTest::keySequence(&canvas, QKeySequence(QKeySequence::SelectAll));
    const QStringList selected = canvas.GetSelectedNodes();
    canvas.resetTransform();
    canvas.centerOn(300, 150);
    const QPointF texture = canvas.GetNodePosition(QStringLiteral("texture"));
    const QPointF output = canvas.GetNodePosition(QStringLiteral("output"));
    const QPoint press = canvas.mapFromScene(texture + QPointF(30, 10));
    QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, press);
    QTest::mouseMove(canvas.viewport(), press + QPoint(40, 20));
    QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, press + QPoint(40, 20));
    CHECK(canvas.GetSelectedNodes() == selected);
    CHECK(canvas.GetNodePosition(QStringLiteral("texture")) == texture + QPointF(40, 20));
    CHECK(canvas.GetNodePosition(QStringLiteral("output")) == output + QPointF(40, 20));
    CHECK(workspace.GetDocument().GetRevision() == 1);
    QTimer::singleShot(0, &workspace, [&workspace, &canvas, selected]() {
        QMenu* menuOrNull = workspace.findChild<QMenu*>(QStringLiteral("renderGraphNodeMenu"));
        REQUIRE(menuOrNull != nullptr);
        CHECK(canvas.GetSelectedNodes() == selected);
        QAction* copy = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphCopyNodes"));
        REQUIRE(copy != nullptr);
        copy->trigger();
        menuOrNull->close();
    });
    const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("output")) + QPointF(30, 10));
    QTest::mousePress(canvas.viewport(), Qt::RightButton, Qt::NoModifier, point);
    QTest::mouseRelease(canvas.viewport(), Qt::RightButton, Qt::NoModifier, point);
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QCoreApplication::sendEvent(canvas.viewport(), &context);
    REQUIRE(workspace.PasteNodes(QPointF(100, 100)) == true);
    CHECK(canvas.GetSelectedNodes().size() == selected.size());
    CHECK(canvas.GetNodeCount() == 6);
}

TEST_CASE("Graph file context menu adds a configuration through the name dialog",
          "[editor][render-graph][clipboard][selection]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenSelectionPackage(workspace, project);
    QTreeWidget* tree = workspace.findChild<QTreeWidget*>(QStringLiteral("renderGraphTree"));
    REQUIRE(tree != nullptr);
    QTreeWidgetItem* fileOrNull = nullptr;
    for (int32 i = 0; i < tree->topLevelItemCount(); ++i)
    {
        if (tree->topLevelItem(i)->data(0, Qt::UserRole).toString() == LIBRARY)
        {
            fileOrNull = tree->topLevelItem(i);
        }
    }
    REQUIRE(fileOrNull != nullptr);
    tree->setCurrentItem(fileOrNull);
    const QByteArray before = workspace.GetDocument().GetDraft(LIBRARY);
    QTimer::singleShot(0, &workspace, [&workspace]() {
        QMenu* menuOrNull = workspace.findChild<QMenu*>(QStringLiteral("renderGraphNodeMenu"));
        REQUIRE(menuOrNull != nullptr);
        QAction* create = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphAddConfiguration"));
        REQUIRE(create != nullptr);
        QTimer::singleShot(0, &workspace, [&workspace]() {
            QInputDialog* dialogOrNull = workspace.findChild<QInputDialog*>();
            REQUIRE(dialogOrNull != nullptr);
            dialogOrNull->setTextValue(QStringLiteral("Debug"));
            dialogOrNull->accept();
        });
        create->trigger();
        menuOrNull->close();
    });
    Q_EMIT tree->customContextMenuRequested(tree->visualItemRect(fileOrNull).center());
    CHECK(workspace.GetCanvas().GetFile() == LIBRARY);
    CHECK(workspace.GetCanvas().GetConfiguration() == QStringLiteral("Debug"));
    CHECK(workspace.GetCanvas().GetNodeCount() == 0);
    CHECK(workspace.GetDocument().GetRevision() == 1);
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(LIBRARY) == before);
}

TEST_CASE("Configuration deletion preserves complete history and publishes only on Save",
          "[editor][render-graph][configuration-delete]")
{
    QTemporaryDir project;
    const QString package = MakeSelectionPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT) == RenderGraphDocumentStatus::Ok);
    const QString alternate = QStringLiteral("Alternate");
    REQUIRE(document.AddConfiguration(ROOT, alternate) == RenderGraphDocumentStatus::Ok);
    const QByteArray original = document.GetDraft(ROOT);
    const QByteArray library = document.GetDraft(LIBRARY);
    const QByteArray saved = document.GetSaved(ROOT);
    const uint64 revision = document.GetRevision();
    QString error;
    SECTION("remove the populated default and choose a surviving default")
    {
        REQUIRE(document.RemoveConfiguration(ROOT, DEFAULT, error) == RenderGraphDocumentStatus::Ok);
        CHECK(document.GetHistoryNodeSelection().Configuration == alternate);
        QJsonObject graph;
        REQUIRE(document.GetGraph(ROOT, graph) == true);
        CHECK(graph.value(QStringLiteral("default")).toString() == alternate);
        CHECK(ReadConfiguration(document, ROOT, DEFAULT).isEmpty() == true);
    }
    SECTION("remove a nondefault without changing the default")
    {
        REQUIRE(document.RemoveConfiguration(ROOT, alternate, error) == RenderGraphDocumentStatus::Ok);
        QJsonObject graph;
        REQUIRE(document.GetGraph(ROOT, graph) == true);
        CHECK(graph.value(QStringLiteral("default")).toString() == DEFAULT);
        CHECK(ReadConfiguration(document, ROOT, alternate).isEmpty() == true);
    }
    CHECK(error.isEmpty() == true);
    CHECK(document.GetRevision() == revision + 1);
    CHECK(document.GetDraft(LIBRARY) == library);
    CHECK(document.GetSaved(ROOT) == saved);
    const QByteArray deleted = document.GetDraft(ROOT);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(ROOT) == original);
    REQUIRE(document.Redo() == true);
    CHECK(document.GetDraft(ROOT) == deleted);
    QStringList savedFiles;
    REQUIRE(document.Save(savedFiles) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(ROOT) == deleted);
    CHECK(reopened.GetDraft(LIBRARY) == library);
    REQUIRE(document.Undo() == true);
    REQUIRE(document.Save(savedFiles) == RenderGraphDocumentStatus::Ok);
    REQUIRE(reopened.Load(package, ROOT) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(ROOT) == original);
}

TEST_CASE("Configuration deletion rejects missing last and referenced configurations atomically",
          "[editor][render-graph][configuration-delete]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeSelectionPackage(project), ROOT) == RenderGraphDocumentStatus::Ok);
    const QString alternate = QStringLiteral("Alternate");
    QString file = ROOT;
    QString configuration = DEFAULT;
    RenderGraphDocumentStatus expected = RenderGraphDocumentStatus::InvalidEdit;
    QString repair;
    SECTION("last configuration")
    {
        repair = QStringLiteral("Add another");
    }
    SECTION("missing configuration")
    {
        configuration = QStringLiteral("Missing");
        expected = RenderGraphDocumentStatus::NotFound;
    }
    SECTION("missing file")
    {
        file = QStringLiteral("missing.lurender.json");
        expected = RenderGraphDocumentStatus::NotFound;
    }
    SECTION("same-file reference")
    {
        REQUIRE(document.AddConfiguration(ROOT, alternate) == RenderGraphDocumentStatus::Ok);
        REQUIRE(document.AddNode(ROOT,
                                 alternate,
                                 {{QStringLiteral("id"), QStringLiteral("caller")},
                                  {QStringLiteral("kind"), QStringLiteral("Subgraph")},
                                  {QStringLiteral("asset"), ROOT},
                                  {QStringLiteral("configuration"), DEFAULT}}) == RenderGraphDocumentStatus::Ok);
        repair = ROOT + QLatin1Char('#') + alternate + QStringLiteral("/caller");
    }
    SECTION("other-file reference")
    {
        REQUIRE(document.AddConfiguration(ROOT, alternate) == RenderGraphDocumentStatus::Ok);
        REQUIRE(document.AddNode(LIBRARY,
                                 DEFAULT,
                                 {{QStringLiteral("id"), QStringLiteral("caller")},
                                  {QStringLiteral("kind"), QStringLiteral("Subgraph")},
                                  {QStringLiteral("asset"), ROOT},
                                  {QStringLiteral("configuration"), DEFAULT}}) == RenderGraphDocumentStatus::Ok);
        repair = LIBRARY + QLatin1Char('#') + DEFAULT + QStringLiteral("/caller");
    }
    const QByteArray original = document.GetDraft(ROOT);
    const QByteArray library = document.GetDraft(LIBRARY);
    const uint64 revision = document.GetRevision();
    const bool canUndo = document.CanUndo();
    QString error;
    CHECK(document.RemoveConfiguration(file, configuration, error) == expected);
    CHECK(error.isEmpty() == false);
    CHECK(error.contains(repair) == true);
    CHECK(document.GetDraft(ROOT) == original);
    CHECK(document.GetDraft(LIBRARY) == library);
    CHECK(document.GetRevision() == revision);
    CHECK(document.CanUndo() == canUndo);
}

TEST_CASE("Tree and canvas menus delete configurations with Properties hidden and restore selection",
          "[editor][render-graph][configuration-delete][selection]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenSelectionPackage(workspace, project);
    const QString alternate = QStringLiteral("Alternate");
    REQUIRE(workspace.SelectNode(LIBRARY, DEFAULT, {}) == true);
    REQUIRE(workspace.CreateConfiguration(alternate) == true);
    REQUIRE(workspace.SelectNode(LIBRARY, DEFAULT, {}) == true);
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    properties->hide();
    const QByteArray original = workspace.GetDocument().GetDraft(LIBRARY);
    const QByteArray root = workspace.GetDocument().GetDraft(ROOT);
    const uint64 revision = workspace.GetDocument().GetRevision();
    const auto deleteFromMenu = [&workspace]() {
        QMenu* menuOrNull = workspace.findChild<QMenu*>(QStringLiteral("renderGraphNodeMenu"));
        REQUIRE(menuOrNull != nullptr);
        QAction* remove = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphDeleteConfiguration"));
        REQUIRE(remove != nullptr);
        REQUIRE(remove->isEnabled() == true);
        menuOrNull->close();
        remove->trigger();
    };
    SECTION("source-tree menu")
    {
        QTreeWidget* tree = workspace.findChild<QTreeWidget*>(QStringLiteral("renderGraphTree"));
        REQUIRE(tree != nullptr);
        REQUIRE(tree->currentItem() != nullptr);
        QTimer::singleShot(0, &workspace, deleteFromMenu);
        Q_EMIT tree->customContextMenuRequested(tree->visualItemRect(tree->currentItem()).center());
    }
    SECTION("canvas menu")
    {
        RenderGraphCanvas& canvas = workspace.GetCanvas();
        QTimer::singleShot(0, &workspace, deleteFromMenu);
        const QPoint point = canvas.viewport()->rect().bottomRight() - QPoint(20, 20);
        QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
        QCoreApplication::sendEvent(canvas.viewport(), &context);
    }
    SECTION("tree Delete shortcut")
    {
        QTreeWidget* tree = workspace.findChild<QTreeWidget*>(QStringLiteral("renderGraphTree"));
        REQUIRE(tree != nullptr);
        tree->setFocus();
        QCoreApplication::processEvents();
        QTest::keyClick(tree, Qt::Key_Delete);
    }
    CHECK(properties->isVisible() == false);
    CHECK(workspace.GetDocument().GetRevision() == revision + 1);
    CHECK(workspace.GetDocument().GetDraft(ROOT) == root);
    CHECK(workspace.GetCanvas().GetFile() == LIBRARY);
    CHECK(workspace.GetCanvas().GetConfiguration() == alternate);
    CHECK(workspace.GetCanvas().GetNodeCount() == 0);
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(LIBRARY) == original);
    CHECK(workspace.GetCanvas().GetConfiguration() == DEFAULT);
    CHECK(workspace.GetCanvas().GetNodeCount() == 3);
    CHECK(workspace.GetCanvas().GetSelectedNodes().isEmpty() == true);
    workspace.Redo();
    CHECK(workspace.GetCanvas().GetConfiguration() == alternate);
    REQUIRE(workspace.Save() == true);
    CHECK(workspace.GetDocument().IsDirty() == false);
    const uint64 beforeRejected = workspace.GetDocument().GetRevision();
    CHECK(workspace.RemoveSelectedConfiguration() == false);
    CHECK(workspace.GetStatusText().contains(QStringLiteral("last configuration")) == true);
    CHECK(workspace.GetDocument().GetRevision() == beforeRejected);
}
