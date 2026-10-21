// RG3b node canvas: drag gestures, link pre-checks, removal and Subgraph navigation over the
// shared RenderGraphDocument operations. Offscreen widgets prove canvas/document wiring and
// undo; GPU output is covered separately by render_graph_preview_tests.cpp.
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_pipeline.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QMenu>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWheelEvent>

#include <limits>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
constexpr int32 JOB_TIMEOUT_MILLISECONDS = 60000;
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");
const QString LIBRARY = QStringLiteral("rasterize.lurender.json");
const QString DEFAULT_CONFIGURATION = QStringLiteral("Default");

QByteArray ReadFile(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

void WriteFile(const QString& path, const QByteArray& bytes)
{
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()) == true);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
    REQUIRE(file.write(bytes) == bytes.size());
}

/// Copy the checked-in RG2 offscreen package (no authored layout) into project/graphs.
QString MakePackage(const QTemporaryDir& project)
{
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/offscreen"));
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    for (const QString& file : {ROOT_GRAPH,
                                LIBRARY,
                                QStringLiteral("main_shaders/color.slang"),
                                QStringLiteral("main_shaders/depth.slang"),
                                QStringLiteral("main_shaders/geometry.slang")})
    {
        WriteFile(QDir(package).filePath(file), ReadFile(source.filePath(file)));
    }
    QByteArray manifest = ReadFile(source.filePath(QStringLiteral("main.lushader.json.in")));
    manifest.replace("@RG2_TEST_PROFILES@", "[\"metal\", \"wgsl\"]");
    WriteFile(QDir(package).filePath(QStringLiteral("main.lushader.json")), manifest);
    return package;
}

QString FindPython()
{
    const QString managed = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/host-tools/venv/bin/python");
    return QFileInfo(managed).isExecutable() == true ? managed
                                                     : QStandardPaths::findExecutable(QStringLiteral("python3"));
}

// File and configuration are distinct named identities.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QJsonObject GetConfiguration(const RenderGraphDocument& document, const QString& file, const QString& configuration)
{
    QJsonObject graph;
    REQUIRE(document.GetGraph(file, graph) == true);
    const QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    for (const QJsonValueConstRef value : configurations)
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == configuration)
        {
            return value.toObject();
        }
    }
    return {};
}

/// Authored layout position of a node, or false when it has none.
bool GetLayout(const RenderGraphDocument& document, const QString& node, QPoint& outPosition)
{
    const QJsonArray positions = GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION)
                                     .value(QStringLiteral("layout"))
                                     .toObject()
                                     .value(QStringLiteral("positions"))
                                     .toArray();
    for (const QJsonValueConstRef position : positions)
    {
        if (position.toObject().value(QStringLiteral("node")).toString() == node)
        {
            outPosition = {position.toObject().value(QStringLiteral("x")).toInt(),
                           position.toObject().value(QStringLiteral("y")).toInt()};
            return true;
        }
    }
    return false;
}

bool HasEdge(const RenderGraphDocument& document, const QString& from, const QString& to)
{
    const QJsonArray edges =
        GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION).value(QStringLiteral("edges")).toArray();
    for (const QJsonValueConstRef edge : edges)
    {
        if (edge.toObject().value(QStringLiteral("from")).toString() == from &&
            edge.toObject().value(QStringLiteral("to")).toString() == to)
        {
            return true;
        }
    }
    return false;
}

QPoint ToViewport(const RenderGraphCanvas& canvas, QPointF scenePosition)
{
    return canvas.mapFromScene(scenePosition);
}
} // namespace

TEST_CASE("RG3b canvas shows a configuration by domain with typed ports", "[editor][render-graph][canvas]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    CHECK(canvas.GetNodeCount() == 6);
    CHECK(canvas.GetEdgeCount() == 6);
    CHECK(canvas.GetNodeDomain(QStringLiteral("scene")) == RenderGraphDomain::SceneView);
    CHECK(canvas.GetNodeDomain(QStringLiteral("camera")) == RenderGraphDomain::SceneView);
    CHECK(canvas.GetNodeDomain(QStringLiteral("color")) == RenderGraphDomain::Resource);
    CHECK(canvas.GetNodeDomain(QStringLiteral("output")) == RenderGraphDomain::Resource);
    CHECK(canvas.GetNodeDomain(QStringLiteral("raster")) == RenderGraphDomain::Subgraph);
    // Subgraph ports come from the referenced configuration's interface.
    CHECK(canvas.GetPortPosition(QStringLiteral("raster.color"), true).isNull() == false);
    CHECK(canvas.GetPortPosition(QStringLiteral("raster.view"), false).isNull() == false);
    CHECK(canvas.GetPortPosition(QStringLiteral("raster.view"), true).isNull() == true);
    // Unplaced nodes are arranged by dependency for display only: no draft edit happened.
    CHECK(canvas.GetNodePosition(QStringLiteral("raster")).x() > canvas.GetNodePosition(QStringLiteral("scene")).x());
    CHECK(canvas.GetNodePosition(QStringLiteral("output")).x() > canvas.GetNodePosition(QStringLiteral("raster")).x());
    CHECK(document.GetRevision() == 0);
    CHECK(document.IsDirty() == false);

    // The library configuration shows its interface as input and output pseudo-nodes.
    canvas.ShowConfiguration(LIBRARY, QStringLiteral("ColorAndDepth"));
    CHECK(canvas.GetNodeCount() == 1);
    CHECK(canvas.HasNode(QStringLiteral("input")) == true);
    CHECK(canvas.GetNodeDomain(QStringLiteral("input")) == RenderGraphDomain::Interface);
    CHECK(canvas.GetPortPosition(QStringLiteral("input.scene"), true).isNull() == false);
}

TEST_CASE("Graph pinch zoom keeps its focal point and never edits the draft", "[editor][render-graph][canvas]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    canvas.resize(900, 600);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    canvas.show();
    QCoreApplication::processEvents();
    canvas.resetTransform();
    canvas.centerOn(600, 150);
    canvas.SelectNode(QStringLiteral("raster"));
    const QStringList selected = canvas.GetSelectedNodes();
    const QByteArray original = document.GetDraft(ROOT_GRAPH);
    const QPointF position(170, 220);
    const QPointF before = canvas.viewportTransform().inverted().map(position);
    const QPointF global = canvas.viewport()->mapToGlobal(position.toPoint());
    const QPointingDevice* device = QPointingDevice::primaryPointingDevice();
    QNativeGestureEvent begin(Qt::BeginNativeGesture, device, 2, position, position, global, 0, {});
    QCoreApplication::sendEvent(canvas.viewport(), &begin);
    QNativeGestureEvent outward(Qt::ZoomNativeGesture, device, 2, position, position, global, 0.25, {});
    QCoreApplication::sendEvent(canvas.viewport(), &outward);
    CHECK(outward.isAccepted() == true);
    CHECK(canvas.transform().m11() == 1.25);
    const QPointF after = canvas.viewportTransform().inverted().map(position);
    CHECK((after - before).manhattanLength() < 2);
    QNativeGestureEvent inward(Qt::ZoomNativeGesture, device, 2, position, position, global, -0.2, {});
    QCoreApplication::sendEvent(canvas.viewport(), &inward);
    CHECK(canvas.transform().m11() == 1);
    QNativeGestureEvent end(Qt::EndNativeGesture, device, 2, position, position, global, 0, {});
    QCoreApplication::sendEvent(canvas.viewport(), &end);
    CHECK(canvas.GetSelectedNodes() == selected);
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
    CHECK(document.GetRevision() == 0);
    CHECK(document.CanUndo() == false);
}

TEST_CASE("Graph wheel and pixel scrolling zoom smoothly without a pinch", "[editor][render-graph][canvas]")
{
    RenderGraphDocument document;
    RenderGraphCanvas canvas(document);
    const QPointF position(100, 100);
    QWheelEvent wheel(position, position, {}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvas.viewport(), &wheel);
    CHECK(wheel.isAccepted() == true);
    CHECK(canvas.transform().m11() > 1);
    const float64 before = canvas.transform().m11();
    QWheelEvent pixels(position, position, QPoint(0, 12), {}, Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(canvas.viewport(), &pixels);
    CHECK(canvas.transform().m11() > before);
    QWheelEvent reverse(position, position, QPoint(0, -12), {}, Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(canvas.viewport(), &reverse);
    CHECK(qAbs(canvas.transform().m11() - before) < 0.000001);
    CHECK(document.GetRevision() == 0);
}

TEST_CASE("Graph zoom is bounded and rejects invalid magnification", "[editor][render-graph][canvas]")
{
    RenderGraphDocument document;
    RenderGraphCanvas canvas(document);
    const QPointF position(100, 100);
    const QPointingDevice* device = QPointingDevice::primaryPointingDevice();
    for (int32 index = 0; index < 100; ++index)
    {
        QNativeGestureEvent zoom(Qt::ZoomNativeGesture, device, 2, position, position, position, 1, {});
        QCoreApplication::sendEvent(canvas.viewport(), &zoom);
    }
    const float64 maximum = canvas.transform().m11();
    CHECK(maximum > 1);
    CHECK(maximum < 100);
    for (const float64 value :
         {-1.0, -2.0, std::numeric_limits<float64>::quiet_NaN(), std::numeric_limits<float64>::infinity()})
    {
        QNativeGestureEvent invalid(Qt::ZoomNativeGesture, device, 2, position, position, position, value, {});
        QCoreApplication::sendEvent(canvas.viewport(), &invalid);
        CHECK(canvas.transform().m11() == maximum);
    }
    QNativeGestureEvent rotate(Qt::RotateNativeGesture, device, 2, position, position, position, 30, {});
    QCoreApplication::sendEvent(canvas.viewport(), &rotate);
    CHECK(canvas.transform().m11() == maximum);
    CHECK(canvas.transform().m12() == 0);
    for (int32 index = 0; index < 100; ++index)
    {
        QNativeGestureEvent zoom(Qt::ZoomNativeGesture, device, 2, position, position, position, -0.5, {});
        QCoreApplication::sendEvent(canvas.viewport(), &zoom);
    }
    CHECK(canvas.transform().m11() > 0);
    CHECK(canvas.transform().m11() < 1);
    CHECK(canvas.transform().isInvertible() == true);
    CHECK(document.GetRevision() == 0);
}

TEST_CASE("RG3b canvas drag gestures are one undo step each", "[editor][render-graph][canvas]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    REQUIRE(canvas.GetFile() == ROOT_GRAPH);
    REQUIRE(canvas.GetConfiguration() == DEFAULT_CONFIGURATION);
    RenderGraphDocument& document = workspace.GetDocument();
    const QPointF scene = canvas.GetNodePosition(QStringLiteral("scene"));
    const QPointF camera = canvas.GetNodePosition(QStringLiteral("camera"));
    const uint64 start = document.GetRevision();

    // Many intermediate positions in one gesture produce exactly one committed edit.
    QSignalSpy moves(&canvas, &RenderGraphCanvas::MoveRequested);
    canvas.BeginDrag({QStringLiteral("scene"), QStringLiteral("camera")});
    for (int32 step = 1; step <= 10; ++step)
    {
        canvas.UpdateDrag(QPointF(4.0 * step, 2.5 * step));
        CHECK(document.GetRevision() == start);
    }
    canvas.EndDrag();
    REQUIRE(moves.count() == 1);
    CHECK(document.GetRevision() == start + 1);
    QPoint position;
    REQUIRE(GetLayout(document, QStringLiteral("scene"), position) == true);
    CHECK(position == (scene + QPointF(40, 25)).toPoint());
    REQUIRE(GetLayout(document, QStringLiteral("camera"), position) == true);
    CHECK(position == (camera + QPointF(40, 25)).toPoint());
    CHECK(canvas.GetNodePosition(QStringLiteral("scene")) == scene + QPointF(40, 25));

    // A second gesture of the same node is its own undo step.
    canvas.BeginDrag({QStringLiteral("scene")});
    canvas.UpdateDrag(QPointF(-100, 0));
    canvas.EndDrag();
    CHECK(document.GetRevision() == start + 2);
    workspace.Undo();
    REQUIRE(GetLayout(document, QStringLiteral("scene"), position) == true);
    CHECK(position == (scene + QPointF(40, 25)).toPoint());
    CHECK(canvas.GetNodePosition(QStringLiteral("scene")) == scene + QPointF(40, 25));

    // One undo removes the whole first gesture, restoring display placement for both nodes.
    workspace.Undo();
    CHECK(GetLayout(document, QStringLiteral("scene"), position) == false);
    CHECK(GetLayout(document, QStringLiteral("camera"), position) == false);
    CHECK(canvas.GetNodePosition(QStringLiteral("camera")) == camera);
    CHECK(document.IsDirty() == false);
    workspace.Redo();
    REQUIRE(GetLayout(document, QStringLiteral("camera"), position) == true);
    CHECK(position == (camera + QPointF(40, 25)).toPoint());

    // A gesture that returns to its origin commits nothing.
    const uint64 beforeNoop = document.GetRevision();
    canvas.BeginDrag({QStringLiteral("output")});
    canvas.UpdateDrag(QPointF(30, 30));
    canvas.UpdateDrag(QPointF(0, 0));
    canvas.EndDrag();
    CHECK(document.GetRevision() == beforeNoop);
}

TEST_CASE("RG3b canvas mouse drag and keyboard removal use document operations", "[editor][render-graph][canvas]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.resize(1400, 800);
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.show();
    REQUIRE(QTest::qWaitForWindowExposed(&workspace) == true);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphDocument& document = workspace.GetDocument();
    canvas.resetTransform();
    const QPointF origin = canvas.GetNodePosition(QStringLiteral("output"));
    canvas.centerOn(origin);
    const QPoint press = ToViewport(canvas, origin + QPointF(20, 10));
    const uint64 start = document.GetRevision();
    QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, press);
    CHECK(workspace.GetSelection() == ROOT_GRAPH + QStringLiteral("#Default/output"));
    for (int32 step = 1; step <= 5; ++step)
    {
        QTest::mouseMove(canvas.viewport(), press + QPoint(6 * step, 4 * step));
    }
    QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, press + QPoint(30, 20));
    CHECK(document.GetRevision() == start + 1);
    QPoint position;
    REQUIRE(GetLayout(document, QStringLiteral("output"), position) == true);
    CHECK(position == (origin + QPointF(30, 20)).toPoint());

    // Delete removes the selected node and its connections as one undoable edit.
    QTest::keyClick(&canvas, Qt::Key_Delete);
    CHECK(canvas.HasNode(QStringLiteral("output")) == false);
    CHECK(HasEdge(document, QStringLiteral("raster.color"), QStringLiteral("output.texture")) == false);
    workspace.Undo();
    CHECK(canvas.HasNode(QStringLiteral("output")) == true);
    CHECK(HasEdge(document, QStringLiteral("raster.color"), QStringLiteral("output.texture")) == true);
}

TEST_CASE("RG3b canvas rejects illegal links with the validator's reasons", "[editor][render-graph][canvas]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({FindPython(), {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    workspace.SetProject(project.path(), 3);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphDocument& document = workspace.GetDocument();
    QSignalSpy rejected(&canvas, &RenderGraphCanvas::LinkRejected);
    // Endpoints and the expected code are distinct named strings.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const auto expectRejected = [&](const QString& from, const QString& to, const QString& code) {
        const uint64 revision = document.GetRevision();
        CHECK(canvas.RequestLink(from, to) == false);
        REQUIRE(rejected.isEmpty() == false);
        CHECK(rejected.takeLast().at(0).toString().startsWith(code + QLatin1Char(':')) == true);
        CHECK(document.GetRevision() == revision);
        CHECK(workspace.GetStatusText().contains(code) == true);
    };
    expectRejected(QStringLiteral("raster.color"), QStringLiteral("camera.target"), QStringLiteral("MultipleWriters"));
    expectRejected(QStringLiteral("output.texture"), QStringLiteral("raster.scene"), QStringLiteral("Port"));
    expectRejected(QStringLiteral("scene.scene"), QStringLiteral("raster.color"), QStringLiteral("MultipleWriters"));
    expectRejected(QStringLiteral("scene.missing"), QStringLiteral("raster.scene"), QStringLiteral("Port"));
    expectRejected(QStringLiteral("scene.scene"), QStringLiteral("camera.view"), QStringLiteral("Port"));

    // Free inputs: type mismatch, then a cycle through the camera that feeds the raster view.
    REQUIRE(document.Disconnect(ROOT_GRAPH,
                                DEFAULT_CONFIGURATION,
                                QStringLiteral("raster.color"),
                                QStringLiteral("output.texture")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.Disconnect(ROOT_GRAPH,
                                DEFAULT_CONFIGURATION,
                                QStringLiteral("color.texture"),
                                QStringLiteral("camera.target")) == RenderGraphDocumentStatus::Ok);
    canvas.Rebuild();
    expectRejected(QStringLiteral("depth.texture"), QStringLiteral("output.texture"), QStringLiteral("TypeMismatch"));
    expectRejected(QStringLiteral("raster.color"), QStringLiteral("camera.target"), QStringLiteral("Cycle"));

    // A legal link is one undoable Connect through the shared document.
    const uint64 revision = document.GetRevision();
    CHECK(canvas.RequestLink(QStringLiteral("raster.color"), QStringLiteral("output.texture")) == true);
    CHECK(document.GetRevision() == revision + 1);
    CHECK(HasEdge(document, QStringLiteral("raster.color"), QStringLiteral("output.texture")) == true);
    CHECK(canvas.GetEdgeCount() == 5);
    workspace.Undo();
    CHECK(HasEdge(document, QStringLiteral("raster.color"), QStringLiteral("output.texture")) == false);

    // The shared CLI validator reports the same code for an edge the canvas rejects.
    const QString python = FindPython();
    if (python.isEmpty() == true)
    {
        SKIP("python3 is required to run the shared render-graph backend");
    }
    REQUIRE(document.Connect(ROOT_GRAPH,
                             DEFAULT_CONFIGURATION,
                             QStringLiteral("depth.texture"),
                             QStringLiteral("output.texture")) == RenderGraphDocumentStatus::Ok);
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    REQUIRE(workspace.Validate() == true);
    REQUIRE(finished.wait(JOB_TIMEOUT_MILLISECONDS) == true);
    CHECK(workspace.GetLastResult().IsSuccessful == false);
    CHECK(workspace.GetLastResult().Code == QStringLiteral("RenderGraphTypeMismatch"));
}

TEST_CASE("RG3b double-clicking a Subgraph navigates to its source", "[editor][render-graph][canvas]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.resize(1400, 800);
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.show();
    REQUIRE(QTest::qWaitForWindowExposed(&workspace) == true);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    QSignalSpy activated(&canvas, &RenderGraphCanvas::SubgraphActivated);
    CHECK(canvas.ActivateNode(QStringLiteral("scene")) == false);
    canvas.resetTransform();
    const QPointF raster = canvas.GetNodePosition(QStringLiteral("raster"));
    canvas.centerOn(raster);
    QTest::mouseDClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, ToViewport(canvas, raster + QPointF(30, 8)));
    REQUIRE(activated.count() == 1);
    CHECK(activated.at(0).at(0).toString() == LIBRARY);
    CHECK(canvas.GetFile() == LIBRARY);
    CHECK(canvas.GetConfiguration() == QStringLiteral("ColorAndDepth"));
    CHECK(canvas.HasNode(QStringLiteral("draw")) == true);
    CHECK(workspace.GetSelection() == LIBRARY + QStringLiteral("#ColorAndDepth"));
    CHECK(workspace.GetSourceFile() == LIBRARY);
    // Navigation is not an edit.
    CHECK(workspace.GetDocument().GetRevision() == 0);
}

TEST_CASE("Canvas supports native Select All toggle additive and Escape selection",
          "[editor][render-graph][canvas][selection]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    canvas.resize(1100, 700);
    canvas.show();
    canvas.activateWindow();
    QCoreApplication::processEvents();
    canvas.setFocus();
    QTest::keySequence(&canvas, QKeySequence(QKeySequence::SelectAll));
    CHECK(canvas.GetSelectedNodes().size() == canvas.GetNodeCount());
    QTest::keyClick(&canvas, Qt::Key_Escape);
    CHECK(canvas.GetSelectedNodes().isEmpty() == true);
    const QPoint scene = ToViewport(canvas, canvas.GetNodePosition(QStringLiteral("scene")) + QPointF(40, 12));
    const QPoint camera = ToViewport(canvas, canvas.GetNodePosition(QStringLiteral("camera")) + QPointF(40, 12));
    QTest::mouseClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, scene);
    QTest::mouseClick(canvas.viewport(), Qt::LeftButton, Qt::ControlModifier, camera);
    CHECK(canvas.GetSelectedNodes().size() == 2);
    QTest::mouseClick(canvas.viewport(), Qt::LeftButton, Qt::ControlModifier, scene);
    CHECK(canvas.GetSelectedNodes() == QStringList{QStringLiteral("camera")});
    QTest::mouseClick(canvas.viewport(), Qt::LeftButton, Qt::ShiftModifier, scene);
    CHECK(canvas.GetSelectedNodes().size() == 2);
    CHECK(document.GetRevision() == 0);
}

TEST_CASE("pipeline inspector commits typed ports directly to the undoable draft", "[editor][render-graph][pipeline]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    QComboBox* kinds = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    REQUIRE(kinds != nullptr);
    CHECK(kinds->findText(QStringLiteral("Raster")) < 0);
    kinds->setCurrentText(QStringLiteral("ComputePipeline"));
    QPushButton* add = workspace.findChild<QPushButton*>(QStringLiteral("renderGraphAddNode"));
    REQUIRE(add != nullptr);
    add->click();
    CHECK(workspace.GetSelection().endsWith(QStringLiteral("/computepipeline")) == true);
    RenderGraphPipelineEditor* editor = workspace.findChild<RenderGraphPipelineEditor*>();
    REQUIRE(editor != nullptr);
    QPushButton* addInput = editor->findChild<QPushButton*>(QStringLiteral("renderGraphAdd_inputs"));
    REQUIRE(addInput != nullptr);
    const uint64 revision = workspace.GetDocument().GetRevision();
    addInput->click();
    CHECK(workspace.GetDocument().GetRevision() == revision + 1);
    QTableWidget* inputs = editor->findChild<QTableWidget*>(QStringLiteral("inputs"));
    REQUIRE(inputs != nullptr);
    REQUIRE(inputs->rowCount() == 1);
    inputs->item(0, 0)->setText(QStringLiteral("history"));
    CHECK(editor->findChild<QPushButton*>(QStringLiteral("renderGraphApplyPipeline")) == nullptr);
    CHECK(workspace.GetDocument().GetRevision() == revision + 2);
    CHECK(workspace.GetCanvas().GetPortPosition(QStringLiteral("computepipeline.history"), false).isNull() == false);
    workspace.Undo();
    CHECK(workspace.GetCanvas().GetPortPosition(QStringLiteral("computepipeline.history"), false).isNull() == true);
}

TEST_CASE("shader rows create registered sources through Properties and canvas controls",
          "[editor][render-graph][pipeline][shader-creation]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QComboBox* kinds = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    REQUIRE(kinds != nullptr);
    kinds->setCurrentText(QStringLiteral("RayTracingPipeline"));
    QPushButton* add = workspace.findChild<QPushButton*>(QStringLiteral("renderGraphAddNode"));
    REQUIRE(add != nullptr);
    add->click();
    const auto exercise = [&workspace, &package](RenderGraphPipelineEditor* editor) {
        REQUIRE(editor != nullptr);
        QGroupBox* advanced = editor->findChild<QGroupBox*>(QStringLiteral("renderGraphRayAdvanced"));
        QSpinBox* attributes = editor->findChild<QSpinBox*>(QStringLiteral("renderGraphRay_attribute_bytes"));
        QPushButton* automatic = editor->findChild<QPushButton*>(QStringLiteral("renderGraphRayAutomatic"));
        REQUIRE(advanced != nullptr);
        REQUIRE(attributes != nullptr);
        REQUIRE(automatic != nullptr);
        CHECK(advanced->isChecked() == false);
        const uint64 stateRevision = workspace.GetDocument().GetRevision();
        advanced->setChecked(true);
        CHECK(workspace.GetDocument().GetRevision() == stateRevision);
        constexpr int32 TRIANGLE_ATTRIBUTE_BYTES = 8;
        attributes->setValue(TRIANGLE_ATTRIBUTE_BYTES);
        CHECK(workspace.GetDocument().GetRevision() == stateRevision + 1);
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("attribute_bytes") == true);
        automatic->click();
        CHECK(workspace.GetDocument().GetRevision() == stateRevision + 2);
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("attribute_bytes") == false);
        CHECK(advanced->isChecked() == false);
        QComboBox* launchMode = editor->findChild<QComboBox*>(QStringLiteral("rayLaunchSizeMode"));
        REQUIRE(launchMode != nullptr);
        CHECK(launchMode->currentText() == QStringLiteral("Match color target"));
        const uint64 sizeRevision = workspace.GetDocument().GetRevision();
        launchMode->setCurrentIndex(1);
        CHECK(workspace.GetDocument().GetRevision() == sizeRevision + 1);
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("ColorTarget") == false);
        launchMode->setCurrentIndex(0);
        CHECK(workspace.GetDocument().GetRevision() == sizeRevision + 2);
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("ColorTarget") == true);
        QPushButton* addShader = editor->findChild<QPushButton*>(QStringLiteral("renderGraphAdd_shaders"));
        REQUIRE(addShader != nullptr);
        QTableWidget* shaders = editor->findChild<QTableWidget*>(QStringLiteral("shaders"));
        REQUIRE(shaders != nullptr);
        addShader->click();
        REQUIRE(workspace.GetSelectedShaderSources().size() == 1);
        CHECK(shaders->item(0, 2)->text() != QStringLiteral("Program"));
        shaders->item(0, 0)->setText(QStringLiteral("raygen"));
        const QStringList firstSources = workspace.GetSelectedShaderSources();
        addShader->click();
        shaders->item(1, 0)->setText(QStringLiteral("closestHit"));
        QComboBox* stage = qobject_cast<QComboBox*>(shaders->cellWidget(1, 1));
        REQUIRE(stage != nullptr);
        const QStringList beforeStageChange = workspace.GetDocument().GetFiles();
        stage->setCurrentText(QStringLiteral("closest_hit"));
        const QStringList sources = workspace.GetSelectedShaderSources();
        REQUIRE(sources.size() == 2);
        CHECK(sources.first() == firstSources.first());
        QComboBox* selector = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphSourceFile"));
        REQUIRE(selector != nullptr);
        for (const QString& source : sources)
        {
            CHECK(selector->findText(source) >= 0);
            CHECK(workspace.GetDocument().GetDraft(source).contains("ShaderMain") == true);
        }
        CHECK(workspace.GetDocument().GetDraft(sources.last()).contains("RayHit") == true);
        CHECK(workspace.GetDocument().GetFiles() == beforeStageChange);
        stage->setCurrentText(QStringLiteral("miss"));
        CHECK(workspace.GetSelectedShaderSources() == sources);
        CHECK(workspace.GetDocument().GetFiles() == beforeStageChange);
        CHECK(workspace.GetDocument().GetDraft(sources.last()).contains("ShaderMain(Ray input)") == true);
        CHECK(editor->findChild<QTableWidget*>(QStringLiteral("shaders")) == shaders);
        stage->setCurrentText(QStringLiteral("closest_hit"));
        workspace.Undo();
        CHECK(workspace.GetDocument().GetDraft(sources.last()).contains("ShaderMain(Ray input)") == true);
        workspace.Undo();
        CHECK(workspace.GetDocument().GetDraft(sources.last()).contains("RayHit") == true);
        workspace.Undo();
        CHECK(workspace.GetDocument().GetFiles() == beforeStageChange);
        CHECK(selector->findText(sources.last()) >= 0);
        CHECK(workspace.GetDocument().GetDraft(sources.last()).contains("RayLaunch") == true);
        workspace.Redo();
        workspace.Redo();
        workspace.Redo();
        CHECK(workspace.GetSelectedShaderSources() == sources);
        CHECK(workspace.GetDocument().GetFiles() == beforeStageChange);
        // Undo/Redo rebuilds the field controls; use the currently displayed editor.
        QWidget* ownerOrNull = QApplication::activeModalWidget();
        if (ownerOrNull == nullptr)
        {
            ownerOrNull = &workspace;
        }
        RenderGraphPipelineEditor* currentEditorOrNull = nullptr;
        for (RenderGraphPipelineEditor* candidate : ownerOrNull->findChildren<RenderGraphPipelineEditor*>())
        {
            if (candidate->isHidden() == false)
            {
                currentEditorOrNull = candidate;
            }
        }
        REQUIRE(currentEditorOrNull != nullptr);
        QPushButton* completeShader =
            currentEditorOrNull->findChild<QPushButton*>(QStringLiteral("renderGraphAdd_shaders"));
        QTableWidget* completeShaders = currentEditorOrNull->findChild<QTableWidget*>(QStringLiteral("shaders"));
        REQUIRE(completeShader != nullptr);
        REQUIRE(completeShaders != nullptr);
        completeShader->click();
        constexpr int32 MISS_ROW = 2;
        completeShaders->item(MISS_ROW, 0)->setText(QStringLiteral("miss"));
        QComboBox* missStage = qobject_cast<QComboBox*>(completeShaders->cellWidget(MISS_ROW, 1));
        REQUIRE(missStage != nullptr);
        const QByteArray beforeTechnique = workspace.GetDocument().GetDraft(QStringLiteral("main.lushader.json"));
        missStage->setCurrentText(QStringLiteral("miss"));
        CHECK(workspace.GetSelectedShaderSources().size() == 3);
        CHECK(workspace.GetDocument()
                  .GetDraft(QStringLiteral("main.lushader.json"))
                  .contains("RendererTriangles.RayTracing") == true);
        workspace.Undo();
        CHECK(workspace.GetDocument().GetDraft(QStringLiteral("main.lushader.json")) == beforeTechnique);
        workspace.Redo();
        CHECK(workspace.GetDocument()
                  .GetDraft(QStringLiteral("main.lushader.json"))
                  .contains("RendererTriangles.RayTracing") == true);
        REQUIRE(workspace.Save() == true);
        for (const QString& source : sources)
        {
            CHECK(QFileInfo::exists(QDir(package).filePath(source)) == true);
        }
    };
    SECTION("Properties")
    {
        exercise(workspace.findChild<RenderGraphPipelineEditor*>());
    }
    SECTION("canvas with Properties hidden")
    {
        workspace.resize(1200, 800);
        workspace.show();
        QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
        REQUIRE(properties != nullptr);
        properties->hide();
        QCoreApplication::processEvents();
        RenderGraphCanvas& canvas = workspace.GetCanvas();
        canvas.Rebuild();
        QTimer::singleShot(0, &canvas, [&canvas, &exercise, properties]() {
            QMenu* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            REQUIRE(menu != nullptr);
            QAction* edit = menu->findChild<QAction*>(QStringLiteral("renderGraphEditNodeAction"));
            REQUIRE(edit != nullptr);
            QTimer::singleShot(0, &canvas, [&exercise, properties]() {
                QDialog* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                REQUIRE(dialog != nullptr);
                CHECK(properties->isVisible() == false);
                exercise(dialog->findChild<RenderGraphPipelineEditor*>());
                dialog->reject();
            });
            menu->close();
            edit->trigger();
        });
        const QPoint point =
            canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("raytracingpipeline")) + QPointF(20, 10));
        QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
        QApplication::sendEvent(canvas.viewport(), &context);
    }
}
