#include "widget_fixture.h"

#include "internal/render_graph_canvas.h"
#include "internal/render_graph_canvas_feedback.h"
#include "internal/render_graph_pipeline_node.h"
#include "internal/render_graph_resources.h"
#include "internal/renderer_workspace.h"
#include "internal/theme_colour.h"

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/render_preview.h"
#    include <QSignalSpy>
#endif

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsTextItem>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QStringView>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <utility>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");
const QString DEFAULT_CONFIGURATION = QStringLiteral("Default");
/// Viewport pixels between probes when looking for empty canvas.
constexpr int32 EMPTY_PROBE_STEP = 8;
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
QGraphicsSimpleTextItem* FindNodeText(const RenderGraphCanvas& canvas, const QString& identity, QStringView prefix)
{
    for (QGraphicsItem* item : canvas.scene()->items())
    {
        QGraphicsSimpleTextItem* textOrNull = qgraphicsitem_cast<QGraphicsSimpleTextItem*>(item);
        if (textOrNull == nullptr || textOrNull->text().startsWith(prefix) == false)
        {
            continue;
        }
        const QGraphicsItem* owner = item;
        while (owner->parentItem() != nullptr)
        {
            owner = owner->parentItem();
        }
        if (owner->data(0).toString() == identity)
        {
            return textOrNull;
        }
    }
    return nullptr;
}

void CheckCompilerSelection(RenderGraphDocument& inoutDocument, const QString& package)
{
    QStringList saved;
    REQUIRE(inoutDocument.Save(saved) == RenderGraphDocumentStatus::Ok);
    const QString python = QString::fromUtf8(LUDUS_TEST_PYTHON);
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

TEST_CASE("semantic zoom simplifies labels without changing ports, selection or draft feedback",
          "[editor][render-graph][canvas-feedback][semantic-zoom]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeFeedbackPackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    canvas.resize(1000, 650);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    canvas.SetReferenceSize(QSize(1920, 1080));
    canvas.show();
    QCoreApplication::processEvents();
    canvas.ResetZoom();
    canvas.SelectNode(QStringLiteral("color"));
    const QStringList selected = canvas.GetSelectedNodes();
    const QByteArray draft = document.GetDraft(ROOT_GRAPH);
    const QPointF portPosition = canvas.GetPortPosition(QStringLiteral("half.size"), true);
    QGraphicsRectItem* frame = FindFrame(canvas, QStringLiteral("half"));
    REQUIRE(frame != nullptr);
    const QRectF frameBounds = frame->sceneBoundingRect();
    QGraphicsSimpleTextItem* title = FindNodeText(canvas, QStringLiteral("half"), QStringLiteral("half :"));
    QGraphicsSimpleTextItem* activity = FindNodeText(canvas, QStringLiteral("half"), QStringLiteral("On draft route"));
    QGraphicsSimpleTextItem* size = FindNodeText(canvas, QStringLiteral("half"), QStringLiteral("View 1/2"));
    QGraphicsSimpleTextItem* port = FindNodeText(canvas, QStringLiteral("half"), QStringLiteral("size"));
    REQUIRE(title != nullptr);
    REQUIRE(activity != nullptr);
    REQUIRE(size != nullptr);
    REQUIRE(port != nullptr);
    const QString fullTitle = title->text();
    const QString explanation = activity->toolTip();
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_GRAPH_FRAMING_CAPTURE");
    const auto captureView = [&canvas, &capture](const QString& name) {
        if (capture.isEmpty() == false)
        {
            REQUIRE(QDir().mkpath(capture) == true);
            REQUIRE(canvas.grab().save(QDir(capture).filePath(name + QStringLiteral(".png"))) == true);
        }
    };
    captureView(QStringLiteral("detail"));
    for (float64 scale : {0.6, 0.4, 0.1, 1.0})
    {
        canvas.setTransform(QTransform::fromScale(scale, scale));
        canvas.centerOn(canvas.sceneRect().center());
        canvas.viewport()->repaint();
        QCoreApplication::processEvents();
        INFO(scale);
        CHECK(title->isVisible() == (scale >= 0.4));
        CHECK(port->isVisible() == (scale >= 0.6));
        CHECK(size->isVisible() == (scale == 1));
        // Below the working view a card shows its title only: no ports, no captions.
        CHECK(activity->isVisible() == (scale >= 0.6));
        if (title->isVisible() == true)
        {
            CHECK(title->text() == (scale == 1 ? fullTitle : QStringLiteral("half")));
        }
        if (activity->isVisible() == true)
        {
            CHECK(activity->text() == (scale == 1 ? QStringLiteral("On draft route") : QStringLiteral("Draft route")));
        }
        CHECK(activity->toolTip() == explanation);
        CHECK(title->toolTip() == QStringLiteral("half : RenderSize"));
        CHECK(frame->toolTip().contains(QStringLiteral("960 × 540")) == true);
        CHECK(frame->sceneBoundingRect() == frameBounds);
        CHECK(canvas.GetPortPosition(QStringLiteral("half.size"), true) == portPosition);
        CHECK(FindPort(canvas, QStringLiteral("half.size"), true)->isVisible() == true);
        CHECK(canvas.GetSelectedNodes() == selected);
        if (scale == 0.4)
        {
            CHECK(title->boundingRect().height() * title->scale() * scale >= 11);
            // An inactive card is shown dimmed, not captioned.
            CHECK(FindNodeText(canvas, QStringLiteral("depth"), QStringLiteral("Inactive")) == nullptr);
            QGraphicsRectItem* inactiveFrame = FindFrame(canvas, QStringLiteral("depth"));
            REQUIRE(inactiveFrame != nullptr);
            CHECK(inactiveFrame->opacity() < 1);
            canvas.SetReferenceSize(QSize(1280, 720));
            CHECK(size->isVisible() == false);
            canvas.SetReferenceSize(QSize(1920, 1080));
            captureView(QStringLiteral("overview"));
        }
        if (scale == 0.6)
        {
            captureView(QStringLiteral("working"));
        }
    }
    CHECK(document.GetDraft(ROOT_GRAPH) == draft);
    CHECK(document.GetRevision() == 0);
    CHECK(document.CanUndo() == false);
}

TEST_CASE("semantic overview preserves unknown and disabled activity classifications",
          "[editor][render-graph][canvas-feedback][semantic-zoom]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeFeedbackPackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QString expected;
    SECTION("disabled output remains disabled")
    {
        QJsonObject record = ReadRecord(document);
        QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
        for (qsizetype index = 0; index < nodes.size(); ++index)
        {
            QJsonObject node = nodes.at(index).toObject();
            if (node.value(QStringLiteral("id")).toString() == QStringLiteral("output"))
            {
                node.insert(QStringLiteral("enabled"), false);
                nodes.replace(index, node);
            }
        }
        record.insert(QStringLiteral("nodes"), nodes);
        WriteRecord(document, record);
        expected = QStringLiteral("Disabled");
    }
    SECTION("broken structure stays unknown")
    {
        QJsonObject record = ReadRecord(document);
        QJsonArray edges = record.value(QStringLiteral("edges")).toArray();
        edges.append(QJsonObject{{QStringLiteral("from"), QStringLiteral("missing.texture")},
                                 {QStringLiteral("to"), QStringLiteral("output.texture")}});
        record.insert(QStringLiteral("edges"), edges);
        WriteRecord(document, record);
        expected = QStringLiteral("Activity unknown");
    }
    const uint64 revision = document.GetRevision();
    const QByteArray draft = document.GetDraft(ROOT_GRAPH);
    RenderGraphCanvas canvas(document);
    canvas.resize(900, 600);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    canvas.show();
    QCoreApplication::processEvents();
    canvas.setTransform(QTransform::fromScale(0.6, 0.6));
    canvas.viewport()->repaint();
    QCoreApplication::processEvents();
    QGraphicsSimpleTextItem* label = FindNodeText(canvas, QStringLiteral("output"), expected);
    REQUIRE(label != nullptr);
    CHECK(label->isVisible() == true);
    CHECK(label->toolTip().isEmpty() == false);
    CHECK(document.GetRevision() == revision);
    CHECK(document.GetDraft(ROOT_GRAPH) == draft);
}

TEST_CASE("semantic zoom keeps diagnostics visible on inactive nodes at every scale",
          "[editor][render-graph][canvas-feedback][semantic-zoom]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeFeedbackPackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    canvas.resize(900, 600);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    canvas.show();
    QCoreApplication::processEvents();
    const QJsonObject location{{QStringLiteral("node"), QStringLiteral("depth")},
                               {QStringLiteral("port"), QStringLiteral("texture")},
                               {QStringLiteral("direction"), QStringLiteral("output")}};
    const QString description = QStringLiteral("Texture contract needs attention");
    canvas.ResetZoom();
    REQUIRE(canvas.ShowDiagnostic(location, description) == true);
    QGraphicsSimpleTextItem* label = FindNodeText(canvas, QStringLiteral("depth"), QStringLiteral("Error:"));
    REQUIRE(label != nullptr);
    QGraphicsRectItem* marker = qgraphicsitem_cast<QGraphicsRectItem*>(label->parentItem());
    REQUIRE(marker != nullptr);
    for (float64 scale : {1.0, 0.4, 0.01})
    {
        canvas.setTransform(QTransform::fromScale(scale, scale));
        canvas.centerOn(canvas.sceneRect().center());
        canvas.viewport()->repaint();
        QCoreApplication::processEvents();
        CHECK(label->isVisible() == true);
        CHECK(label->effectiveOpacity() == 1);
        CHECK(marker->pen().isCosmetic() == true);
        CHECK(label->boundingRect().height() * label->scale() * scale >= 11);
        CHECK(label->text().isEmpty() == false);
        CHECK(label->toolTip() == description);
        CHECK(canvas.GetDiagnosticLocation() == location);
        CHECK(canvas.accessibleDescription() == description);
    }
    CHECK(label->text() == QStringLiteral("!"));
    canvas.ResetZoom();
    CHECK(label->text().startsWith(QStringLiteral("Error:")) == true);
    canvas.Rebuild();
    CHECK(canvas.GetDiagnosticLocation().isEmpty() == true);
    CHECK(document.GetRevision() == 0);
}

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
    SECTION("disabled nested export leaves no effect")
    {
        const QString child = QStringLiteral("child.lurender.json");
        QJsonObject record =
            ReadGraph(document, child).value(QStringLiteral("configurations")).toArray().at(0).toObject();
        record.insert(QStringLiteral("nodes"),
                      QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("sink")},
                                             {QStringLiteral("kind"), QStringLiteral("Export")},
                                             {QStringLiteral("type"), QStringLiteral("ColorTexture")},
                                             {QStringLiteral("enabled"), false}}});
        record.insert(QStringLiteral("edges"),
                      QJsonArray{QJsonObject{{QStringLiteral("from"), QStringLiteral("input.color")},
                                             {QStringLiteral("to"), QStringLiteral("sink.texture")}}});
        WriteRecord(document, record, child);
        REQUIRE(document.Connect(ROOT_GRAPH,
                                 DEFAULT_CONFIGURATION,
                                 QStringLiteral("color.texture"),
                                 QStringLiteral("dormant.color")) == RenderGraphDocumentStatus::Ok);
        CHECK(InspectRenderGraphActivity(document, ROOT_GRAPH, DEFAULT_CONFIGURATION)
                  .value(QStringLiteral("dormant"))
                  .Activity == RenderGraphActivity::Inactive);
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

TEST_CASE("disabled exports are labeled and release their upstream route", "[editor][render-graph][canvas-feedback]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeFeedbackPackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject record = ReadRecord(document);
    QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    for (qsizetype index = 0; index < nodes.size(); ++index)
    {
        QJsonObject node = nodes.at(index).toObject();
        if (node.value(QStringLiteral("id")).toString() == QStringLiteral("output"))
        {
            node.insert(QStringLiteral("enabled"), false);
            nodes.replace(index, node);
        }
    }
    // An unconnected disabled Export is accepted, as in a graph that keeps an unused depth Export.
    nodes.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("depthOutput")},
                             {QStringLiteral("kind"), QStringLiteral("Export")},
                             {QStringLiteral("type"), QStringLiteral("DepthTexture")},
                             {QStringLiteral("enabled"), false}});
    record.insert(QStringLiteral("nodes"), nodes);
    WriteRecord(document, record);
    const QHash<QString, RenderGraphActivityFeedback> feedback =
        InspectRenderGraphActivity(document, ROOT_GRAPH, DEFAULT_CONFIGURATION);
    for (const QString& identity : {QStringLiteral("output"), QStringLiteral("depthOutput")})
    {
        CHECK(feedback.value(identity).Activity == RenderGraphActivity::Disabled);
        CHECK(feedback.value(identity).Label == QStringLiteral("Disabled · not exported"));
    }
    CHECK(feedback.value(QStringLiteral("color")).Activity == RenderGraphActivity::Inactive);
    CHECK(feedback.value(QStringLiteral("half")).Activity == RenderGraphActivity::Inactive);
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

TEST_CASE("hovering a card focuses its wires and fades the others", "[editor][render-graph][canvas-feedback][overview]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeFeedbackPackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    canvas.resize(1000, 650);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    canvas.show();
    QCoreApplication::processEvents();
    canvas.ResetZoom();
    const auto findWire = [&canvas](const QString& description) -> QGraphicsPathItem* {
        for (QGraphicsItem* item : canvas.scene()->items())
        {
            QGraphicsPathItem* wireOrNull = qgraphicsitem_cast<QGraphicsPathItem*>(item);
            if (wireOrNull != nullptr && wireOrNull->toolTip().startsWith(description) == true)
            {
                return wireOrNull;
            }
        }
        return nullptr;
    };
    QGraphicsPathItem* sizeWire = findWire(QStringLiteral("half.size → color.size"));
    QGraphicsPathItem* outputWire = findWire(QStringLiteral("color.texture → output.texture"));
    REQUIRE(sizeWire != nullptr);
    REQUIRE(outputWire != nullptr);
    QGraphicsRectItem* half = FindFrame(canvas, QStringLiteral("half"));
    REQUIRE(half != nullptr);
    const QPoint hover = canvas.mapFromScene(half->sceneBoundingRect().center());
    SendTestMouseMove(*canvas.viewport(), hover);
    QCoreApplication::processEvents();
    CHECK(sizeWire->opacity() == 1);
    CHECK(outputWire->opacity() < 0.5);
    // Leave the card for empty canvas, wherever the framing put the cards.
    QPoint empty(2, 2);
    for (int32 y = 2; y < canvas.viewport()->height() && canvas.items(empty).isEmpty() == false; y += EMPTY_PROBE_STEP)
    {
        empty = QPoint(2, y);
    }
    REQUIRE(canvas.items(empty).isEmpty() == true);
    SendTestMouseMove(*canvas.viewport(), empty);
    QCoreApplication::processEvents();
    CHECK(outputWire->opacity() == 1);
}

TEST_CASE("display placement never lays an unplaced card over a placed one",
          "[editor][render-graph][canvas-feedback][layout]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeFeedbackPackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject record = ReadRecord(document);
    // The author placed one card exactly where the column layout would put the first unplaced one.
    record.insert(QStringLiteral("layout"),
                  QJsonObject{{QStringLiteral("positions"),
                               QJsonArray{QJsonObject{{QStringLiteral("node"), QStringLiteral("color")},
                                                      {QStringLiteral("x"), 0},
                                                      {QStringLiteral("y"), 0}}}}});
    WriteRecord(document, record);
    RenderGraphCanvas canvas(document);
    canvas.resize(1000, 650);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    canvas.show();
    QCoreApplication::processEvents();
    QVector<QRectF> cards;
    for (const QString& identity : {QStringLiteral("half"),
                                    QStringLiteral("color"),
                                    QStringLiteral("depth"),
                                    QStringLiteral("scene"),
                                    QStringLiteral("camera"),
                                    QStringLiteral("dormant"),
                                    QStringLiteral("output"),
                                    QStringLiteral("unfinished")})
    {
        QGraphicsRectItem* frame = FindFrame(canvas, identity);
        REQUIRE(frame != nullptr);
        cards.append(frame->sceneBoundingRect());
    }
    for (qsizetype first = 0; first < cards.size(); ++first)
    {
        for (qsizetype second = first + 1; second < cards.size(); ++second)
        {
            INFO(first << " " << second);
            CHECK(cards.at(first).intersects(cards.at(second)) == false);
        }
    }
}

TEST_CASE("saved cards that overlap are nudged apart on screen without rewriting the saved layout",
          "[editor][render-graph][canvas-feedback][layout]")
{
    constexpr int32 STACK_STEP = 10;
    constexpr int32 TIDY_STEP = 400;
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeFeedbackPackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject record = ReadRecord(document);
    // Three cards the author left stacked, two tidy ones, and nothing else placed.
    QJsonArray positions;
    const QStringList stacked{QStringLiteral("half"), QStringLiteral("color"), QStringLiteral("depth")};
    for (qsizetype index = 0; index < stacked.size(); ++index)
    {
        positions.append(QJsonObject{{QStringLiteral("node"), stacked.at(index)},
                                     {QStringLiteral("x"), 0},
                                     {QStringLiteral("y"), static_cast<int32>(index) * STACK_STEP}});
    }
    positions.append(QJsonObject{{QStringLiteral("node"), QStringLiteral("scene")},
                                 {QStringLiteral("x"), 900},
                                 {QStringLiteral("y"), 0}});
    positions.append(QJsonObject{{QStringLiteral("node"), QStringLiteral("camera")},
                                 {QStringLiteral("x"), 900},
                                 {QStringLiteral("y"), TIDY_STEP}});
    record.insert(QStringLiteral("layout"), QJsonObject{{QStringLiteral("positions"), positions}});
    WriteRecord(document, record);
    const QByteArray savedText = document.GetDraft(ROOT_GRAPH);
    const uint64 savedRevision = document.GetRevision();
    RenderGraphCanvas canvas(document);
    canvas.resize(1000, 650);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    canvas.show();
    QCoreApplication::processEvents();
    QVector<QRectF> cards;
    for (const QString& identity : stacked)
    {
        QGraphicsRectItem* frame = FindFrame(canvas, identity);
        REQUIRE(frame != nullptr);
        cards.append(frame->sceneBoundingRect());
    }
    // Nobody overlaps, the top-to-bottom order and the column are kept, and only whole units were added.
    for (qsizetype index = 1; index < cards.size(); ++index)
    {
        CHECK(cards.at(index - 1).intersects(cards.at(index)) == false);
        CHECK(cards.at(index).top() > cards.at(index - 1).top());
        CHECK(cards.at(index).left() == cards.at(0).left());
    }
    CHECK(canvas.GetNodePosition(QStringLiteral("half")) == QPointF(0, 0));
    CHECK(canvas.GetNodePosition(QStringLiteral("color")).y() >= STACK_STEP);
    // Cards that already clear each other stay exactly where they were saved.
    CHECK(canvas.GetNodePosition(QStringLiteral("scene")) == QPointF(900, 0));
    CHECK(canvas.GetNodePosition(QStringLiteral("camera")) == QPointF(900, TIDY_STEP));
    // The file was never edited, and the author is told what happened and can tidy.
    CHECK(document.GetRevision() == savedRevision);
    CHECK(document.GetDraft(ROOT_GRAPH) == savedText);
    REQUIRE(canvas.IsTidySuggested() == true);
    const QLabel* text = canvas.findChild<QLabel*>(QStringLiteral("renderGraphTidyNoticeText"));
    REQUIRE(text != nullptr);
    CHECK(text->text().contains(QStringLiteral("2 overlapping cards are shown moved apart")) == true);
    CHECK(text->text().contains(QStringLiteral("unchanged")) == true);
    // A rebuild (every edit makes one) leaves the nudged cards where they were instead of sliding them again.
    const QPointF nudged = canvas.GetNodePosition(QStringLiteral("depth"));
    canvas.Rebuild();
    CHECK(canvas.GetNodePosition(QStringLiteral("depth")) == nudged);
    // A card the author drops somewhere stays exactly there, even on top of another.
    const QPointF colourPosition = canvas.GetNodePosition(QStringLiteral("color"));
    QJsonObject moved = ReadRecord(document);
    QJsonArray movedPositions =
        moved.value(QStringLiteral("layout")).toObject().value(QStringLiteral("positions")).toArray();
    for (qsizetype index = 0; index < movedPositions.size(); ++index)
    {
        QJsonObject position = movedPositions.at(index).toObject();
        if (position.value(QStringLiteral("node")).toString() == QStringLiteral("depth"))
        {
            position.insert(QStringLiteral("x"), static_cast<int32>(colourPosition.x()));
            position.insert(QStringLiteral("y"), static_cast<int32>(colourPosition.y()));
            movedPositions.replace(index, position);
        }
    }
    moved.insert(QStringLiteral("layout"), QJsonObject{{QStringLiteral("positions"), movedPositions}});
    WriteRecord(document, moved);
    canvas.Rebuild();
    CHECK(canvas.GetNodePosition(QStringLiteral("depth")) == colourPosition);
    CHECK(canvas.GetNodePosition(QStringLiteral("color")) == colourPosition);
}

TEST_CASE("a per-pixel buffer is a resource and keeps the draft's activity known",
          "[editor][render-graph][canvas-feedback]")
{
    // ComputeRayTracing stores its hits in a Buffer node; that must not make every card read "Activity unknown".
    CHECK(GetRenderGraphDomain(QStringLiteral("Buffer")) == RenderGraphDomain::Resource);
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeFeedbackPackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject record = ReadRecord(document);
    QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    nodes.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("hits")},
                             {QStringLiteral("kind"), QStringLiteral("Buffer")},
                             {QStringLiteral("bytes_per_pixel"), 32},
                             {QStringLiteral("extent"), QStringLiteral("RenderSize")}});
    record.insert(QStringLiteral("nodes"), nodes);
    QJsonArray edges = record.value(QStringLiteral("edges")).toArray();
    edges.append(QJsonObject{{QStringLiteral("from"), QStringLiteral("half.size")},
                             {QStringLiteral("to"), QStringLiteral("hits.size")}});
    record.insert(QStringLiteral("edges"), edges);
    WriteRecord(document, record);
    const QHash<QString, RenderGraphActivityFeedback> activity =
        InspectRenderGraphActivity(document, ROOT_GRAPH, DEFAULT_CONFIGURATION);
    REQUIRE(activity.contains(QStringLiteral("hits")) == true);
    for (QHash<QString, RenderGraphActivityFeedback>::const_iterator entry = activity.cbegin();
         entry != activity.cend();
         ++entry)
    {
        INFO(entry.key().toStdString());
        CHECK(entry.value().Activity != RenderGraphActivity::Unknown);
    }
}

namespace
{
/// A root graph of Subgraph cards over one library: ray tracing beside compute, compute only, a subgraph nested in
/// a subgraph, one with no passes and one that refers to itself, plus a raster pass on the root itself.
QString MakePassQueuePackage(const QTemporaryDir& project)
{
    const auto subgraph = [](const QString& identity, const QString& configuration) {
        return QJsonObject{{QStringLiteral("id"), identity},
                           {QStringLiteral("kind"), QStringLiteral("Subgraph")},
                           {QStringLiteral("asset"), QStringLiteral("passes.lurender.json")},
                           {QStringLiteral("configuration"), configuration}};
    };
    const auto configuration = [](const QString& identity, const QJsonArray& nodes) {
        return QJsonObject{{QStringLiteral("id"), identity},
                           {QStringLiteral("inputs"), QJsonArray{}},
                           {QStringLiteral("outputs"), QJsonArray{}},
                           {QStringLiteral("nodes"), nodes},
                           {QStringLiteral("edges"), QJsonArray{}}};
    };
    const QJsonObject root{
        {QStringLiteral("version"), 2},
        {QStringLiteral("id"), QStringLiteral("Main")},
        {QStringLiteral("default"), DEFAULT_CONFIGURATION},
        {QStringLiteral("configurations"),
         QJsonArray{configuration(
             DEFAULT_CONFIGURATION,
             QJsonArray{subgraph(QStringLiteral("SliceSamplingLuminaireReference"), QStringLiteral("Rays")),
                        subgraph(QStringLiteral("compute"), QStringLiteral("Compute")),
                        subgraph(QStringLiteral("nested"), QStringLiteral("Nested")),
                        subgraph(QStringLiteral("empty"), QStringLiteral("Empty")),
                        subgraph(QStringLiteral("cycle"), QStringLiteral("Cycle")),
                        MakePipelineNode(QStringLiteral("GraphicsPipeline"), QStringLiteral("draw"))})}}};
    const QJsonObject library{
        {QStringLiteral("version"), 2},
        {QStringLiteral("id"), QStringLiteral("Passes")},
        {QStringLiteral("default"), QStringLiteral("Rays")},
        {QStringLiteral("configurations"),
         QJsonArray{
             configuration(QStringLiteral("Rays"),
                           QJsonArray{MakePipelineNode(QStringLiteral("ComputePipeline"), QStringLiteral("prepare")),
                                      MakePipelineNode(QStringLiteral("RayTracingPipeline"), QStringLiteral("trace"))}),
             configuration(QStringLiteral("Compute"),
                           QJsonArray{MakePipelineNode(QStringLiteral("ComputePipeline"), QStringLiteral("resolve"))}),
             configuration(QStringLiteral("Nested"),
                           QJsonArray{subgraph(QStringLiteral("inner"), QStringLiteral("Compute"))}),
             configuration(QStringLiteral("Empty"), QJsonArray{}),
             configuration(QStringLiteral("Cycle"),
                           QJsonArray{subgraph(QStringLiteral("again"), QStringLiteral("Cycle"))}),
         }}};
    const std::pair<QString, QJsonObject> files[] = {{ROOT_GRAPH, root},
                                                     {QStringLiteral("passes.lurender.json"), library}};
    for (const std::pair<QString, QJsonObject>& file : files)
    {
        QFile output(QDir(project.path()).filePath(file.first));
        REQUIRE(output.open(QIODevice::WriteOnly) == true);
        const QByteArray bytes = EncodeCanonicalJson(file.second);
        REQUIRE(output.write(bytes) == bytes.size());
    }
    return project.path();
}

/// The colour a card is drawn in: its header strip's colour, which the minimap shares. (A card that cannot run on
/// this device greys its header on the canvas, so the minimap's copy is the one to compare.)
QColor GetHeaderColour(const RenderGraphCanvas& canvas, const QString& identity)
{
    QGraphicsRectItem* frameOrNull = FindFrame(canvas, identity);
    REQUIRE(frameOrNull != nullptr);
    for (const RenderGraphMinimapCard& card : canvas.GetMinimapCards())
    {
        if (card.Bounds == frameOrNull->sceneBoundingRect())
        {
            return card.Colour;
        }
    }
    return {};
}

/// The title label inside a card's header.
QGraphicsSimpleTextItem* FindTitle(const RenderGraphCanvas& canvas, const QString& identity)
{
    QGraphicsRectItem* frameOrNull = FindFrame(canvas, identity);
    REQUIRE(frameOrNull != nullptr);
    for (QGraphicsItem* child : frameOrNull->childItems())
    {
        for (QGraphicsItem* grandchild : child->childItems())
        {
            if (QGraphicsSimpleTextItem* titleOrNull = qgraphicsitem_cast<QGraphicsSimpleTextItem*>(grandchild);
                titleOrNull != nullptr)
            {
                return titleOrNull;
            }
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE("pass cards and subgraph cards are coloured by the heaviest work they run, with a legend",
          "[editor][render-graph][canvas-feedback][overview][design-tokens]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePassQueuePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    canvas.resize(1000, 650);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    canvas.show();
    QCoreApplication::processEvents();
    const QColor rays = GetCanvasColour(design::ThemeColour::GraphRayTracing);
    const QColor compute = GetCanvasColour(design::ThemeColour::GraphCompute);
    const QColor raster = GetCanvasColour(design::ThemeColour::GraphRaster);
    CHECK(GetHeaderColour(canvas, QStringLiteral("SliceSamplingLuminaireReference")) == rays);
    CHECK(GetHeaderColour(canvas, QStringLiteral("compute")) == compute);
    CHECK(GetHeaderColour(canvas, QStringLiteral("nested")) == compute);
    CHECK(GetHeaderColour(canvas, QStringLiteral("draw")) == raster);
    // Without passes (or only a cycle back to itself) a subgraph keeps the plain subgraph colour.
    const QColor plain = GetHeaderColour(canvas, QStringLiteral("empty"));
    CHECK(plain != rays);
    CHECK(plain != compute);
    CHECK(plain != raster);
    CHECK(GetHeaderColour(canvas, QStringLiteral("cycle")) == plain);
    CHECK(rays != compute);
    CHECK(compute != raster);
    CHECK(FindFrame(canvas, QStringLiteral("compute"))->toolTip().contains(QStringLiteral("compute passes")) == true);
    // The minimap draws each card in its header colour.
    QSet<QRgb> mapColours;
    for (const RenderGraphMinimapCard& card : canvas.GetMinimapCards())
    {
        mapColours.insert(card.Colour.rgb());
    }
    CHECK(mapColours.contains(rays.rgb()) == true);
    CHECK(mapColours.contains(compute.rgb()) == true);
    // The legend in the canvas toolbar names every colour.
    QToolButton* legend = canvas.findChild<QToolButton*>(QStringLiteral("renderGraphColourLegend"));
    REQUIRE(legend != nullptr);
    CHECK(legend->toolTip().isEmpty() == false);
    REQUIRE(legend->menu() != nullptr);
    QStringList meanings;
    for (const QLabel* entry : legend->menu()->findChildren<QLabel*>(QStringLiteral("renderGraphColourLegendEntry")))
    {
        meanings.append(entry->text());
    }
    CHECK(meanings.contains(QStringLiteral("Ray tracing pass")) == true);
    CHECK(meanings.contains(QStringLiteral("Compute pass")) == true);
    CHECK(meanings.contains(QStringLiteral("Raster pass")) == true);
    CHECK(document.GetRevision() == 0);
}

TEST_CASE("overview titles stay on one line, shortened at their end, and never leave a blank card",
          "[editor][render-graph][canvas-feedback][overview][semantic-zoom]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePassQueuePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    canvas.resize(1000, 650);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    canvas.show();
    QCoreApplication::processEvents();
    const QString longName = QStringLiteral("SliceSamplingLuminaireReference");
    QGraphicsSimpleTextItem* title = FindTitle(canvas, longName);
    REQUIRE(title != nullptr);
    // The overview zooms a MacBook Air fits a large graph at; every card still names itself.
    for (float64 scale : {0.25, 0.31, 0.41, 0.62, 1.0})
    {
        INFO(scale);
        canvas.setTransform(QTransform::fromScale(scale, scale));
        canvas.viewport()->repaint();
        QCoreApplication::processEvents();
        for (const QString& identity : {longName, QStringLiteral("compute"), QStringLiteral("draw")})
        {
            QGraphicsSimpleTextItem* cardTitle = FindTitle(canvas, identity);
            REQUIRE(cardTitle != nullptr);
            CHECK(cardTitle->isVisible() == true);
            CHECK(cardTitle->text().contains(QLatin1Char('\n')) == false);
            CHECK(cardTitle->text().isEmpty() == false);
        }
        // Shortened at its end: the visible part is the start of the name, with the full title in the tooltip.
        const QString shown = title->text();
        const QString kept = shown.endsWith(QChar(0x2026)) == true ? shown.chopped(1) : shown;
        CHECK(kept.isEmpty() == false);
        CHECK(longName.startsWith(kept) == true);
        CHECK(title->toolTip().startsWith(longName) == true);
        // It fits its card.
        CHECK(title->boundingRect().width() * title->scale() <= FindFrame(canvas, longName)->rect().width());
    }
    canvas.setTransform(QTransform::fromScale(0.31, 0.31));
    canvas.viewport()->repaint();
    QCoreApplication::processEvents();
    CHECK(title->text().endsWith(QChar(0x2026)) == true);
    CHECK(title->text().size() > 3);
}
