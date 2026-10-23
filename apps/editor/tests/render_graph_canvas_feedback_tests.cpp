#include "internal/render_graph_canvas.h"
#include "internal/render_graph_canvas_feedback.h"
#include "internal/render_graph_pipeline_node.h"
#include "internal/render_graph_resources.h"
#include "internal/renderer_workspace.h"

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/render_preview.h"
#    include <QSignalSpy>
#endif

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QGraphicsEllipseItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");
const QString DEFAULT_CONFIGURATION = QStringLiteral("Default");
QString MakeFeedbackPackage(const QTemporaryDir& project)
{
    QJsonObject graph = QJsonDocument::fromJson(QByteArrayLiteral(R"JSON({
      "version":2,"id":"Main","default":"Default","configurations":[{
        "id":"Default","inputs":[],"outputs":[],"nodes":[
          {"id":"half","kind":"RenderSize","extent":{"reference":"View","scale":[1,2]}},
          {"id":"color","kind":"Target","type":"ColorTexture","format":"RGBA8Unorm","samples":1,"extent":"RenderSize","initialized":false,"imported":false,"presentation":false},
          {"id":"depth","kind":"Target","type":"DepthTexture","format":"Depth32Float","samples":1,"extent":[96,64],"initialized":false,"imported":false,"presentation":false},
          {"id":"scene","kind":"Scene"},
          {"id":"camera","kind":"Camera","active":false,"role":"Offscreen","target_type":"ColorTexture"},
          {"id":"dormant","kind":"Subgraph","asset":"child.lurender.json","configuration":"Default"},
          {"id":"output","kind":"Export","type":"ColorTexture"}],
        "edges":[{"from":"half.size","to":"color.size"},{"from":"color.texture","to":"output.texture"},
                 {"from":"color.texture","to":"camera.target"}]}]})JSON"))
                            .object();
    QJsonArray records = graph.value(QStringLiteral("configurations")).toArray();
    QJsonObject record = records.at(0).toObject();
    QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    nodes.append(MakePipelineNode(QStringLiteral("ComputePipeline"), QStringLiteral("unfinished")));
    record.insert(QStringLiteral("nodes"), nodes);
    records.replace(0, record);
    graph.insert(QStringLiteral("configurations"), records);
    const QByteArray child = QByteArrayLiteral(R"JSON({"version":2,"id":"Child","default":"Default","configurations":[{
      "id":"Default","inputs":[{"id":"color","type":"ColorTexture"}],
      "outputs":[{"id":"color","type":"ColorTexture","source":"input.color","required":false}],"nodes":[],"edges":[]}]})JSON");
    for (const QString& file : {ROOT_GRAPH, QStringLiteral("child.lurender.json")})
    {
        QFile output(QDir(project.path()).filePath(file));
        REQUIRE(output.open(QIODevice::WriteOnly) == true);
        const QByteArray bytes = file == ROOT_GRAPH ? EncodeCanonicalJson(graph) : child;
        REQUIRE(output.write(bytes) == bytes.size());
    }
    return project.path();
}
QJsonObject ReadGraph(const RenderGraphDocument& document, const QString& file = ROOT_GRAPH)
{
    QJsonObject graph;
    REQUIRE(document.GetGraph(file, graph) == true);
    return graph;
}
QJsonObject ReadRecord(const RenderGraphDocument& document)
{
    return ReadGraph(document).value(QStringLiteral("configurations")).toArray().at(0).toObject();
}
void WriteRecord(RenderGraphDocument& inoutDocument, const QJsonObject& record, const QString& file = ROOT_GRAPH)
{
    QJsonObject graph = ReadGraph(inoutDocument, file);
    graph.insert(QStringLiteral("configurations"), QJsonArray{record});
    REQUIRE(inoutDocument.SetText(file, EncodeCanonicalJson(graph)) == RenderGraphDocumentStatus::Ok);
}
QGraphicsRectItem* FindFrame(const RenderGraphCanvas& canvas, const QString& identity)
{
    for (QGraphicsItem* item : canvas.scene()->items())
    {
        QGraphicsRectItem* frameOrNull = qgraphicsitem_cast<QGraphicsRectItem*>(item);
        if (frameOrNull != nullptr && frameOrNull->data(0).toString() == identity)
        {
            return frameOrNull;
        }
    }
    return nullptr;
}
QGraphicsEllipseItem* FindPort(const RenderGraphCanvas& canvas, const QString& endpoint, bool isOutput)
{
    const QPointF position = canvas.GetPortPosition(endpoint, isOutput);
    for (QGraphicsItem* item : canvas.scene()->items(position))
    {
        QGraphicsEllipseItem* connectorOrNull = qgraphicsitem_cast<QGraphicsEllipseItem*>(item);
        if (connectorOrNull != nullptr)
        {
            return connectorOrNull;
        }
    }
    return nullptr;
}
QGraphicsTextItem* FindHint(const RenderGraphCanvas& canvas)
{
    for (QGraphicsItem* item : canvas.scene()->items())
    {
        if (item->data(0).toString() == QStringLiteral("#link-feedback"))
        {
            return qgraphicsitem_cast<QGraphicsTextItem*>(item);
        }
    }
    return nullptr;
}
void CheckCompilerSelection(RenderGraphDocument& inoutDocument, const QString& package)
{
    QStringList saved;
    REQUIRE(inoutDocument.Save(saved) == RenderGraphDocumentStatus::Ok);
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    REQUIRE(python.isEmpty() == false);
    QProcess process;
    process.setWorkingDirectory(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT));
    process.start(
        python,
        {QStringLiteral("-c"),
         QStringLiteral(
             "import sys,json;sys.path.insert(0,'scripts/python');from ludus_tools import render_graph as g;"
             "from pathlib import "
             "Path;p=Path(sys.argv[1]);l,p=g._library(p,p.parent);c=l.assets[p]['configurations'][0];"
             "print(json.dumps([n['id'] for n in l.execution_order(p,c,{v['id'] for v in c['outputs']})[0]]))"),
         QDir(package).filePath(ROOT_GRAPH)});
    REQUIRE(process.waitForFinished() == true);
    INFO(process.readAllStandardError().constData());
    REQUIRE(process.exitCode() == 0);
    const QJsonArray selected = QJsonDocument::fromJson(process.readAllStandardOutput()).array();
    QSet<QString> expected;
    for (const QJsonValueConstRef identity : selected)
    {
        expected.insert(identity.toString());
    }
    const QHash<QString, RenderGraphActivityFeedback> actual =
        InspectRenderGraphActivity(inoutDocument, ROOT_GRAPH, DEFAULT_CONFIGURATION);
    for (QHash<QString, RenderGraphActivityFeedback>::const_iterator node = actual.constBegin();
         node != actual.constEnd();
         ++node)
    {
        if (node.value().Activity == RenderGraphActivity::Disabled)
        {
            // The compiler forwards disabled nodes out of execution entirely.
            CHECK(expected.contains(node.key()) == false);
            continue;
        }
        CHECK(node.value().Activity ==
              (expected.contains(node.key()) == true ? RenderGraphActivity::Retained : RenderGraphActivity::Inactive));
    }
}
} // namespace

TEST_CASE("canvas draft activity matches compiler selection and preserves explicit nested effects",
          "[editor][render-graph][canvas-feedback]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeFeedbackPackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    SECTION("connected unused branches remain inactive")
    {
        REQUIRE(document.Connect(ROOT_GRAPH,
                                 DEFAULT_CONFIGURATION,
                                 QStringLiteral("color.texture"),
                                 QStringLiteral("dormant.color")) == RenderGraphDocumentStatus::Ok);
    }
    SECTION("active camera obligations survive unused view outputs")
    {
        QJsonObject record = ReadRecord(document);
        QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
        for (qsizetype index = 0; index < nodes.size(); ++index)
        {
            QJsonObject node = nodes.at(index).toObject();
            if (node.value(QStringLiteral("id")).toString() == QStringLiteral("camera"))
            {
                node.insert(QStringLiteral("active"), true);
                nodes.replace(index, node);
            }
        }
        record.insert(QStringLiteral("nodes"), nodes);
        WriteRecord(document, record);
    }
    SECTION("nested export effect survives unused interface outputs")
    {
        const QString child = QStringLiteral("child.lurender.json");
        QJsonObject record =
            ReadGraph(document, child).value(QStringLiteral("configurations")).toArray().at(0).toObject();
        record.insert(QStringLiteral("nodes"),
                      QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("sink")},
                                             {QStringLiteral("kind"), QStringLiteral("Export")},
                                             {QStringLiteral("type"), QStringLiteral("ColorTexture")}}});
        record.insert(QStringLiteral("edges"),
                      QJsonArray{QJsonObject{{QStringLiteral("from"), QStringLiteral("input.color")},
                                             {QStringLiteral("to"), QStringLiteral("sink.texture")}}});
        WriteRecord(document, record, child);
        REQUIRE(document.Connect(ROOT_GRAPH,
                                 DEFAULT_CONFIGURATION,
                                 QStringLiteral("color.texture"),
                                 QStringLiteral("dormant.color")) == RenderGraphDocumentStatus::Ok);
    }
    const uint64 revision = document.GetRevision();
    const QByteArray original = document.GetDraft(ROOT_GRAPH);
    const QHash<QString, RenderGraphActivityFeedback> feedback =
        InspectRenderGraphActivity(document, ROOT_GRAPH, DEFAULT_CONFIGURATION);
    CHECK(feedback.value(QStringLiteral("unfinished")).Activity == RenderGraphActivity::Inactive);
    CHECK(feedback.value(QStringLiteral("output")).Activity == RenderGraphActivity::Retained);
    CHECK(feedback.value(QStringLiteral("half")).Activity == RenderGraphActivity::Retained);
    CHECK(document.GetRevision() == revision);
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
    CheckCompilerSelection(document, project.path());
}

TEST_CASE("disabled nodes are labeled and keep their upstream route", "[editor][render-graph][canvas-feedback]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeFeedbackPackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject record = ReadRecord(document);
    QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    nodes.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("display")},
                             {QStringLiteral("kind"), QStringLiteral("ToneMap")},
                             {QStringLiteral("exposure"), 1},
                             {QStringLiteral("enabled"), false}});
    record.insert(QStringLiteral("nodes"), nodes);
    QJsonArray edges = record.value(QStringLiteral("edges")).toArray();
    for (qsizetype index = 0; index < edges.size(); ++index)
    {
        if (edges.at(index).toObject().value(QStringLiteral("to")).toString() == QStringLiteral("output.texture"))
        {
            edges.replace(index,
                          QJsonObject{{QStringLiteral("from"), QStringLiteral("display.color")},
                                      {QStringLiteral("to"), QStringLiteral("output.texture")}});
        }
    }
    edges.append(QJsonObject{{QStringLiteral("from"), QStringLiteral("color.texture")},
                             {QStringLiteral("to"), QStringLiteral("display.color")}});
    record.insert(QStringLiteral("edges"), edges);
    WriteRecord(document, record);
    const uint64 revision = document.GetRevision();
    const QHash<QString, RenderGraphActivityFeedback> feedback =
        InspectRenderGraphActivity(document, ROOT_GRAPH, DEFAULT_CONFIGURATION);
    CHECK(feedback.value(QStringLiteral("display")).Activity == RenderGraphActivity::Disabled);
    CHECK(feedback.value(QStringLiteral("display")).Label == QStringLiteral("Disabled · bypassed"));
    CHECK(feedback.value(QStringLiteral("color")).Activity == RenderGraphActivity::Retained);
    CHECK(feedback.value(QStringLiteral("output")).Activity == RenderGraphActivity::Retained);
    CHECK(document.GetRevision() == revision);
    CheckCompilerSelection(document, project.path());
}

TEST_CASE("broken draft structure and recursive imports never claim inactive nodes",
          "[editor][render-graph][canvas-feedback]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeFeedbackPackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    SECTION("dangling wire")
    {
        QJsonObject record = ReadRecord(document);
        QJsonArray edges = record.value(QStringLiteral("edges")).toArray();
        edges.append(QJsonObject{{QStringLiteral("from"), QStringLiteral("absent.texture")},
                                 {QStringLiteral("to"), QStringLiteral("camera.target")}});
        record.insert(QStringLiteral("edges"), edges);
        WriteRecord(document, record);
    }
    SECTION("import cycle")
    {
        const QString child = QStringLiteral("child.lurender.json");
        QJsonObject record =
            ReadGraph(document, child).value(QStringLiteral("configurations")).toArray().at(0).toObject();
        record.insert(QStringLiteral("nodes"),
                      QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("loop")},
                                             {QStringLiteral("kind"), QStringLiteral("Subgraph")},
                                             {QStringLiteral("asset"), child},
                                             {QStringLiteral("configuration"), DEFAULT_CONFIGURATION}}});
        WriteRecord(document, record, child);
    }
    SECTION("missing child JSON")
    {
        REQUIRE(document.SetText(QStringLiteral("child.lurender.json"), QByteArrayLiteral("{")) ==
                RenderGraphDocumentStatus::Ok);
    }
    SECTION("dependency cycle")
    {
        QJsonObject record = ReadRecord(document);
        QJsonArray edges = record.value(QStringLiteral("edges")).toArray();
        edges.append(QJsonObject{{QStringLiteral("from"), QStringLiteral("dormant.color")},
                                 {QStringLiteral("to"), QStringLiteral("dormant.color")}});
        record.insert(QStringLiteral("edges"), edges);
        WriteRecord(document, record);
    }
    const uint64 revision = document.GetRevision();
    for (const RenderGraphActivityFeedback& feedback :
         InspectRenderGraphActivity(document, ROOT_GRAPH, DEFAULT_CONFIGURATION))
    {
        CHECK(feedback.Activity == RenderGraphActivity::Unknown);
    }
    CHECK(document.GetRevision() == revision);
}

TEST_CASE("canvas texture metadata resolves fixed shared nested and view-relative sizes without nominal guesses",
          "[editor][render-graph][canvas-feedback]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeFeedbackPackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const auto describe = [&document](const QString& endpoint, const QSize& reference = QSize{}) {
        return DescribeRenderGraphResource(document, ROOT_GRAPH, DEFAULT_CONFIGURATION, endpoint, reference);
    };
    CHECK(describe(QStringLiteral("depth.texture")).SizeLabel == QStringLiteral("96 × 64 px"));
    CHECK(describe(QStringLiteral("color.texture")).SizeLabel.contains(QStringLiteral("awaiting viewport")) == true);
    CHECK(describe(QStringLiteral("color.texture"), {1919, 1079}).SizeLabel ==
          QStringLiteral("View 1/2 → 960 × 540 px"));
    CHECK(describe(QStringLiteral("color.texture"), {1921, 1081}).SizeLabel ==
          QStringLiteral("View 1/2 → 961 × 541 px"));
    CHECK(describe(QStringLiteral("half.size"), {1, 1}).SizeLabel == QStringLiteral("View 1/2 → 1 × 1 px"));
    CHECK(describe(QStringLiteral("half.size"), {100000, 100000}).SizeLabel.contains(QStringLiteral("exceeds limit")) ==
          true);
    REQUIRE(document.Connect(ROOT_GRAPH,
                             DEFAULT_CONFIGURATION,
                             QStringLiteral("color.texture"),
                             QStringLiteral("dormant.color")) == RenderGraphDocumentStatus::Ok);
    CHECK(describe(QStringLiteral("dormant.color"), {1919, 1079}).SizeLabel ==
          QStringLiteral("View 1/2 → 960 × 540 px"));
    CHECK(DescribeRenderGraphResource(document,
                                      QStringLiteral("child.lurender.json"),
                                      DEFAULT_CONFIGURATION,
                                      QStringLiteral("input.color"),
                                      {1919, 1079})
              .SizeLabel == QStringLiteral("Size unresolved"));
    QJsonObject metadata{{QStringLiteral("id"), QStringLiteral("metadata")},
                         {QStringLiteral("kind"), QStringLiteral("RenderSize")},
                         {QStringLiteral("extent"), QStringLiteral("Texture")}};
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, metadata) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.Connect(ROOT_GRAPH,
                             DEFAULT_CONFIGURATION,
                             QStringLiteral("depth.texture"),
                             QStringLiteral("metadata.texture")) == RenderGraphDocumentStatus::Ok);
    CHECK(describe(QStringLiteral("metadata.size")).SizeLabel == QStringLiteral("96 × 64 px"));
    CHECK(describe(QStringLiteral("missing.texture")).SizeLabel == QStringLiteral("Size unresolved"));
    QJsonObject pipeline = MakePipelineNode(QStringLiteral("GraphicsPipeline"), QStringLiteral("work"));
    pipeline.insert(QStringLiteral("inputs"),
                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("target")},
                                           {QStringLiteral("type"), QStringLiteral("ColorTexture")},
                                           {QStringLiteral("required"), true}}});
    pipeline.insert(QStringLiteral("outputs"),
                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("result")},
                                           {QStringLiteral("type"), QStringLiteral("ColorTexture")},
                                           {QStringLiteral("target"), QStringLiteral("target")}}});
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, pipeline) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.Connect(ROOT_GRAPH,
                             DEFAULT_CONFIGURATION,
                             QStringLiteral("color.texture"),
                             QStringLiteral("work.target")) == RenderGraphDocumentStatus::Ok);
    CHECK(describe(QStringLiteral("work.result"), {1919, 1079}).SizeLabel == QStringLiteral("View 1/2 → 960 × 540 px"));
    REQUIRE(document.ReplaceConnection(ROOT_GRAPH,
                                       DEFAULT_CONFIGURATION,
                                       QStringLiteral("metadata.size"),
                                       QStringLiteral("color.size")) == RenderGraphDocumentStatus::Ok);
    CHECK(describe(QStringLiteral("color.texture")).SizeLabel == QStringLiteral("96 × 64 px"));
    REQUIRE(document.ReplaceConnection(ROOT_GRAPH,
                                       DEFAULT_CONFIGURATION,
                                       QStringLiteral("color.texture"),
                                       QStringLiteral("metadata.texture")) == RenderGraphDocumentStatus::Ok);
    CHECK(describe(QStringLiteral("color.texture")).SizeLabel == QStringLiteral("Size unresolved"));
    const uint64 revision = document.GetRevision();
    static_cast<void>(describe(QStringLiteral("dormant.color"), {1919, 1079}));
    CHECK(document.GetRevision() == revision);
}

TEST_CASE("canvas annotations and hover compatibility work with Properties hidden and preserve history",
          "[editor][render-graph][canvas-feedback]")
{
    QTemporaryDir project;
    const QString package = MakeFeedbackPackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    properties->hide();
    workspace.resize(1450, 850);
    workspace.show();
    QCoreApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.fitInView(canvas.scene()->itemsBoundingRect().adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio);
    RenderGraphDocument& document = workspace.GetDocument();
    const uint64 revision = document.GetRevision();
    const QByteArray original = document.GetDraft(ROOT_GRAPH);
    QGraphicsRectItem* inactive = FindFrame(canvas, QStringLiteral("unfinished"));
    REQUIRE(inactive != nullptr);
    CHECK(inactive->opacity() < 1);
    CHECK(inactive->toolTip().contains(QStringLiteral("compiler skips this branch")) == true);
    // Inactive branches remain selectable and editable.
    canvas.SelectNode(QStringLiteral("unfinished"));
    canvas.SetReferenceSize({1919, 1079});
    CHECK(canvas.GetSelectedNodes() == QStringList{QStringLiteral("unfinished")});
    CHECK(FindFrame(canvas, QStringLiteral("color"))->toolTip().contains(QStringLiteral("960 × 540")) == true);
    CHECK(FindPort(canvas, QStringLiteral("color.texture"), true)->toolTip().contains(QStringLiteral("RGBA8Unorm")) ==
          true);
    canvas.SetReferenceSize({1921, 1081});
    CHECK(FindFrame(canvas, QStringLiteral("color"))->toolTip().contains(QStringLiteral("961 × 541")) == true);
    canvas.SetReferenceSize({});
    CHECK(FindFrame(canvas, QStringLiteral("color"))->toolTip().contains(QStringLiteral("awaiting viewport")) == true);
    const auto drag = [&canvas](const QString& from, const QString& to) {
        const QPoint start = canvas.mapFromScene(canvas.GetPortPosition(from, true));
        const QPoint end = canvas.mapFromScene(canvas.GetPortPosition(to, false));
        QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(canvas.viewport(), end);
    };
    drag(QStringLiteral("depth.texture"), QStringLiteral("output.texture"));
    REQUIRE(FindHint(canvas) != nullptr);
    CHECK(FindHint(canvas)->toPlainText().contains(QStringLiteral("TypeMismatch")) == true);
    CHECK(
        FindPort(canvas, QStringLiteral("output.texture"), false)->toolTip().contains(QStringLiteral("DepthTexture")) ==
        true);
    QTest::mouseRelease(canvas.viewport(),
                        Qt::LeftButton,
                        Qt::NoModifier,
                        canvas.mapFromScene(canvas.GetPortPosition(QStringLiteral("output.texture"), false)));
    CHECK(FindHint(canvas) == nullptr);
    CHECK(document.GetRevision() == revision);
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
    drag(QStringLiteral("color.texture"), QStringLiteral("dormant.color"));
    REQUIRE(FindHint(canvas) != nullptr);
    CHECK(FindHint(canvas)->toPlainText().contains(QStringLiteral("Compatible")) == true);
    QTest::keyClick(&canvas, Qt::Key_Escape);
    CHECK(FindHint(canvas) == nullptr);
    CHECK(document.GetRevision() == revision);
    drag(QStringLiteral("color.texture"), QStringLiteral("dormant.color"));
    QTest::mouseRelease(canvas.viewport(),
                        Qt::LeftButton,
                        Qt::NoModifier,
                        canvas.mapFromScene(canvas.GetPortPosition(QStringLiteral("dormant.color"), false)));
    CHECK(document.GetRevision() == revision + 1);
    workspace.Undo();
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
    CHECK(properties->isVisible() == false);
    const QString screenshot = qEnvironmentVariable("LUDUS_CANVAS_SCREENSHOT");
    if (screenshot.isEmpty() == false)
    {
        canvas.SetReferenceSize({1919, 1079});
        REQUIRE(canvas.grab().save(screenshot) == true);
    }
}

TEST_CASE("canvas hover explains attachment mismatches before a rejected drop",
          "[editor][render-graph][canvas-feedback]")
{
    QTemporaryDir project;
    const QString package = MakeFeedbackPackage(project);
    RenderGraphDocument setup;
    REQUIRE(setup.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QJsonObject raster{{QStringLiteral("id"), QStringLiteral("draw")},
                             {QStringLiteral("kind"), QStringLiteral("Raster")},
                             {QStringLiteral("mode"), QStringLiteral("ColorAndDepth")},
                             {QStringLiteral("material"), QStringLiteral("DefaultOpaque")},
                             {QStringLiteral("vertex_layout"), QStringLiteral("RendererStatic")},
                             {QStringLiteral("color_load"), QStringLiteral("Clear")},
                             {QStringLiteral("color_store"), QStringLiteral("Store")},
                             {QStringLiteral("depth_load"), QStringLiteral("Clear")},
                             {QStringLiteral("depth_store"), QStringLiteral("Store")}};
    REQUIRE(setup.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, raster) == RenderGraphDocumentStatus::Ok);
    REQUIRE(setup.Connect(ROOT_GRAPH,
                          DEFAULT_CONFIGURATION,
                          QStringLiteral("color.texture"),
                          QStringLiteral("draw.color")) == RenderGraphDocumentStatus::Ok);
    QStringList saved;
    REQUIRE(setup.Save(saved) == RenderGraphDocumentStatus::Ok);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    workspace.resize(1450, 850);
    workspace.show();
    workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"))->hide();
    QCoreApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.fitInView(canvas.scene()->itemsBoundingRect().adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio);
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray original = document.GetDraft(ROOT_GRAPH);
    const uint64 revision = document.GetRevision();
    const QPoint start = canvas.mapFromScene(canvas.GetPortPosition(QStringLiteral("depth.texture"), true));
    const QPoint end = canvas.mapFromScene(canvas.GetPortPosition(QStringLiteral("draw.depth"), false));
    QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(canvas.viewport(), end);
    REQUIRE(FindHint(canvas) != nullptr);
    CHECK(FindHint(canvas)->toPlainText().contains(QStringLiteral("AttachmentMismatch")) == true);
    CHECK(FindHint(canvas)->toPlainText().contains(QStringLiteral("matching raster attachment")) == true);
    const QRectF hint = FindHint(canvas)->sceneBoundingRect();
    CHECK(canvas.mapToScene(canvas.viewport()->rect()).boundingRect().contains(hint) == true);
    const QString screenshot = qEnvironmentVariable("LUDUS_CANVAS_SCREENSHOT");
    if (screenshot.isEmpty() == false)
    {
        REQUIRE(canvas.grab().save(screenshot + QStringLiteral(".rejection.png")) == true);
    }
    QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, end);
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
    CHECK(document.GetRevision() == revision);
    CHECK(FindHint(canvas) == nullptr);
    CHECK(FindPort(canvas, QStringLiteral("draw.depth"), false)
              ->toolTip()
              .contains(QStringLiteral("Unconnected input")) == true);
    // Added feedback rows do not overlap automatically placed nodes in the same column.
    CHECK(FindFrame(canvas, QStringLiteral("draw"))
              ->sceneBoundingRect()
              .intersects(FindFrame(canvas, QStringLiteral("output"))->sceneBoundingRect()) == false);
}

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
TEST_CASE("World drawable changes refresh canvas sizes before any GPU preview starts",
          "[editor][render-graph][canvas-feedback]")
{
    QTemporaryDir project;
    const QString package = MakeFeedbackPackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderPreview first;
    REQUIRE(first.HasDevice() == false);
    workspace.SetPreviewSource(&first);
    QSignalSpy dimensions(&first, &RenderPreview::DrawableSizeChanged);
    const uint64 revision = workspace.GetDocument().GetRevision();
    first.Resize(1919, 1079);
    CHECK(dimensions.count() == 1);
    CHECK(FindFrame(canvas, QStringLiteral("color"))->toolTip().contains(QStringLiteral("960 × 540")) == true);
    first.Resize(1919, 1079);
    CHECK(dimensions.count() == 1);
    first.Resize(1921, 1081);
    CHECK(FindFrame(canvas, QStringLiteral("color"))->toolTip().contains(QStringLiteral("961 × 541")) == true);
    first.Resize(0, 0);
    CHECK(FindFrame(canvas, QStringLiteral("color"))->toolTip().contains(QStringLiteral("awaiting viewport")) == true);
    {
        RenderPreview second;
        workspace.SetPreviewSource(&second);
        second.Resize(101, 51);
        CHECK(FindFrame(canvas, QStringLiteral("color"))->toolTip().contains(QStringLiteral("51 × 26")) == true);
        first.Resize(2000, 1000);
        CHECK(FindFrame(canvas, QStringLiteral("color"))->toolTip().contains(QStringLiteral("51 × 26")) == true);
    }
    CHECK(FindFrame(canvas, QStringLiteral("color"))->toolTip().contains(QStringLiteral("awaiting viewport")) == true);
    CHECK(workspace.GetDocument().GetRevision() == revision);
    CHECK(first.HasDevice() == false);
}
#endif
