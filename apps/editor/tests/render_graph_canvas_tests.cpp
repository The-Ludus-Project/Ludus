// RG3b node canvas: drag gestures, link pre-checks, removal and Subgraph navigation over the
// shared RenderGraphDocument operations. Offscreen widgets prove canvas/document wiring and
// undo; GPU output is covered separately by render_graph_preview_tests.cpp.
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_pipeline.h"
#include "internal/renderer_workspace.h"
#include "internal/theme_colour.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QGraphicsEllipseItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QNativeGestureEvent>
#include <QPlainTextEdit>
#include <QPointingDevice>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
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

/// Authored record of a node in the root graph's default configuration, or an empty object.
QJsonObject FindNode(const RenderGraphDocument& document, const QString& identity)
{
    for (const QJsonValueConstRef value :
         GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION).value(QStringLiteral("nodes")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == identity)
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

TEST_CASE("shared size policies edit through Properties and canvas with undo", "[editor][render-graph][render-size]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QComboBox* kinds = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    QPushButton* add = workspace.findChild<QPushButton*>(QStringLiteral("renderGraphAddNode"));
    REQUIRE(kinds != nullptr);
    REQUIRE(add != nullptr);
    kinds->setCurrentText(QStringLiteral("RenderSize"));
    add->click();
    QString identity = QStringLiteral("rendersize");
    bool shouldUseCanvas = false;
    SECTION("size in Properties") {}
    SECTION("target in Properties")
    {
        identity = QStringLiteral("color");
    }
    SECTION("size on canvas")
    {
        shouldUseCanvas = true;
    }
    SECTION("target on canvas")
    {
        identity = QStringLiteral("color");
        shouldUseCanvas = true;
    }
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, identity) == true);
    INFO(identity.toUtf8().constData());
    INFO(shouldUseCanvas);
    const bool isSizeNode = identity == QStringLiteral("rendersize");
    const QString source = isSizeNode == true ? QStringLiteral("Texture") : QStringLiteral("RenderSize");
    const QString port = identity + (isSizeNode == true ? QStringLiteral(".texture") : QStringLiteral(".size"));
    const auto exercise = [&workspace, &identity, &source, &port](QWidget& inoutFields) {
        QSpinBox* width = inoutFields.findChild<QSpinBox*>(QStringLiteral("renderGraphExtentWidth"));
        REQUIRE(width != nullptr);
        width->setValue(97);
        Q_EMIT width->editingFinished();
        QSpinBox* height = inoutFields.findChild<QSpinBox*>(QStringLiteral("renderGraphExtentHeight"));
        REQUIRE(height != nullptr);
        height->setValue(65);
        Q_EMIT height->editingFinished();
        const QJsonArray nodes = GetConfiguration(workspace.GetDocument(), ROOT_GRAPH, DEFAULT_CONFIGURATION)
                                     .value(QStringLiteral("nodes"))
                                     .toArray();
        for (const QJsonValueConstRef node : nodes)
        {
            if (node.toObject().value(QStringLiteral("id")).toString() == identity)
            {
                CHECK(node.toObject().value(QStringLiteral("extent")).toArray() == QJsonArray{97, 65});
            }
        }
        QComboBox* mode = inoutFields.findChild<QComboBox*>(QStringLiteral("renderGraphExtentMode"));
        REQUIRE(mode != nullptr);
        if (identity == QStringLiteral("rendersize"))
        {
            const uint64 beforeScale = workspace.GetDocument().GetRevision();
            mode->setCurrentText(QStringLiteral("Half view"));
            Q_EMIT mode->activated(mode->currentIndex());
            CHECK(workspace.GetDocument().GetRevision() == beforeScale + 1);
            CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("View") == true);
            CHECK(workspace.GetCanvas().GetPortPosition(port, false).isNull() == true);
            QComboBox* customMode = inoutFields.findChild<QComboBox*>(QStringLiteral("renderGraphExtentMode"));
            REQUIRE(customMode != nullptr);
            customMode->setCurrentText(QStringLiteral("Custom view scale"));
            Q_EMIT customMode->activated(customMode->currentIndex());
            QSpinBox* denominator = inoutFields.findChild<QSpinBox*>(QStringLiteral("renderGraphScaleDenominator"));
            REQUIRE(denominator != nullptr);
            CHECK(denominator->isEnabled() == true);
            denominator->setValue(3);
            Q_EMIT denominator->editingFinished();
            CHECK(workspace.GetDocument().GetRevision() == beforeScale + 2);
            workspace.Undo();
            workspace.Undo();
            mode = inoutFields.findChild<QComboBox*>(QStringLiteral("renderGraphExtentMode"));
            REQUIRE(mode != nullptr);
        }
        const uint64 revision = workspace.GetDocument().GetRevision();
        mode->setCurrentIndex(1);
        Q_EMIT mode->activated(1);
        CHECK(workspace.GetDocument().GetRevision() == revision + 1);
        CHECK(workspace.GetCanvas().GetPortPosition(port, false).isNull() == false);
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains(source.toUtf8()) == true);
        if (identity == QStringLiteral("rendersize"))
        {
            REQUIRE(workspace.GetCanvas().RequestLink(QStringLiteral("color.texture"), port) == true);
            const uint64 connectedRevision = workspace.GetDocument().GetRevision();
            QComboBox* connectedMode = inoutFields.findChild<QComboBox*>(QStringLiteral("renderGraphExtentMode"));
            REQUIRE(connectedMode != nullptr);
            connectedMode->setCurrentIndex(0);
            Q_EMIT connectedMode->activated(0);
            CHECK(workspace.GetDocument().GetRevision() == connectedRevision);
            QComboBox* preservedMode = inoutFields.findChild<QComboBox*>(QStringLiteral("renderGraphExtentMode"));
            REQUIRE(preservedMode != nullptr);
            CHECK(preservedMode->currentIndex() == 1);
            workspace.Undo();
        }
        workspace.Undo();
        CHECK(workspace.GetCanvas().GetPortPosition(port, false).isNull() == true);
        workspace.Redo();
        CHECK(workspace.GetCanvas().GetPortPosition(port, false).isNull() == false);
        REQUIRE(workspace.Save() == true);
    };
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    if (shouldUseCanvas == false)
    {
        exercise(*properties);
    }
    else
    {
        workspace.resize(1200, 800);
        workspace.show();
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
                exercise(*dialog);
                dialog->reject();
            });
            menu->close();
            edit->trigger();
        });
        const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(identity) + QPointF(20, 10));
        QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
        QApplication::sendEvent(canvas.viewport(), &context);
    }
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(ROOT_GRAPH) == workspace.GetDocument().GetDraft(ROOT_GRAPH));
}

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

TEST_CASE("Graph canvas colours come from the tokens and share the selection amber",
          "[editor][render-graph][canvas][design-tokens]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    const QColor amber = GetCanvasColour(design::ThemeColour::SelectionAmber);
    CHECK(canvas.backgroundBrush().color() == GetCanvasColour(design::ThemeColour::SurfaceViewport));
    CHECK(amber == GetViewportArgb(design::ThemeColour::SelectionAmber));
    const auto countItems = [&canvas](const QColor& pen, const QColor& brush) {
        int32 count = 0;
        for (const QGraphicsItem* item : canvas.scene()->items())
        {
            const QAbstractGraphicsShapeItem* shape = qgraphicsitem_cast<const QGraphicsRectItem*>(item);
            if (shape == nullptr)
            {
                continue;
            }
            const bool isPenMatch = pen.isValid() == false || shape->pen().color() == pen;
            const bool isBrushMatch = brush.isValid() == false || shape->brush().color() == brush;
            count += isPenMatch == true && isBrushMatch == true ? 1 : 0;
        }
        return count;
    };
    const QColor cardFill = GetCanvasColour(design::ThemeColour::SurfaceRaised);
    CHECK(countItems(QColor(), cardFill) >= canvas.GetNodeCount());
    CHECK(countItems(amber, QColor()) == 0);
    canvas.SelectNode(QStringLiteral("scene"));
    CHECK(countItems(amber, QColor()) >= 1);
}

TEST_CASE("Graph framing controls preserve authored state and frame only node bounds",
          "[editor][render-graph][canvas][framing]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    canvas.resize(900, 600);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    canvas.show();
    QCoreApplication::processEvents();
    QAction* fitGraph = canvas.findChild<QAction*>(QStringLiteral("renderGraphFitGraph"));
    QAction* fitSelection = canvas.findChild<QAction*>(QStringLiteral("renderGraphFitSelection"));
    QAction* resetZoom = canvas.findChild<QAction*>(QStringLiteral("renderGraphResetZoom"));
    QLabel* zoom = canvas.findChild<QLabel*>(QStringLiteral("renderGraphZoomLabel"));
    QToolBar* toolbar = canvas.findChild<QToolBar*>(QStringLiteral("renderGraphFramingToolbar"));
    REQUIRE(fitGraph != nullptr);
    REQUIRE(fitSelection != nullptr);
    REQUIRE(resetZoom != nullptr);
    REQUIRE(zoom != nullptr);
    REQUIRE(toolbar != nullptr);
    CHECK(toolbar->geometry().bottom() < canvas.viewport()->geometry().top());
    CHECK(fitGraph->isEnabled() == true);
    CHECK(fitSelection->isEnabled() == false);
    const QTransform beforeEmptyFit = canvas.transform();
    canvas.FitSelection();
    CHECK(canvas.transform() == beforeEmptyFit);
    canvas.SelectNodes({QStringLiteral("raster"), QStringLiteral("color")});
    const QStringList selected = canvas.GetSelectedNodes();
    const QByteArray draft = document.GetDraft(ROOT_GRAPH);
    const QPointF position = canvas.GetNodePosition(QStringLiteral("raster"));
    CHECK(fitSelection->isEnabled() == true);
    // Transient scene annotations must not change either framing operation.
    QGraphicsRectItem* hint = canvas.scene()->addRect(QRectF(-90000, -90000, 20, 20));
    fitGraph->trigger();
    const float64 graphScale = canvas.transform().m11();
    QRectF allBounds;
    QRectF selectedBounds;
    for (QGraphicsItem* item : canvas.scene()->items())
    {
        if (qgraphicsitem_cast<QGraphicsRectItem*>(item) != nullptr && item->data(0).toString().isEmpty() == false &&
            item->parentItem() == nullptr)
        {
            allBounds = allBounds.united(item->sceneBoundingRect());
            if (selected.contains(item->data(0).toString()) == true)
            {
                selectedBounds = selectedBounds.united(item->sceneBoundingRect());
            }
        }
    }
    CHECK(canvas.mapToScene(canvas.viewport()->rect()).boundingRect().contains(allBounds) == true);
    fitSelection->trigger();
    CHECK(canvas.transform().m11() > graphScale);
    CHECK(canvas.mapToScene(canvas.viewport()->rect()).boundingRect().contains(selectedBounds) == true);
    CHECK((canvas.mapToScene(canvas.viewport()->rect().center()) - selectedBounds.center()).manhattanLength() < 2);
    const QPointF centre = canvas.mapToScene(canvas.viewport()->rect().center());
    resetZoom->trigger();
    CHECK(canvas.transform().m11() == 1);
    CHECK(zoom->text() == QStringLiteral("100%"));
    CHECK((canvas.mapToScene(canvas.viewport()->rect().center()) - centre).manhattanLength() < 2);
    delete hint;
    canvas.resize(420, 300);
    QCoreApplication::processEvents();
    CHECK(toolbar->geometry().bottom() < canvas.viewport()->geometry().top());
    canvas.FitGraph();
    CHECK(canvas.mapToScene(canvas.viewport()->rect()).boundingRect().contains(allBounds) == true);
    CHECK(canvas.GetSelectedNodes() == selected);
    CHECK(canvas.GetNodePosition(QStringLiteral("raster")) == position);
    CHECK(document.GetDraft(ROOT_GRAPH) == draft);
    CHECK(document.GetRevision() == 0);
    CHECK(document.CanUndo() == false);
    canvas.ShowConfiguration({}, {});
    CHECK(fitGraph->isEnabled() == false);
    CHECK(fitSelection->isEnabled() == false);
    const QTransform empty = canvas.transform();
    canvas.FitGraph();
    canvas.FitSelection();
    CHECK(canvas.transform() == empty);
}

TEST_CASE("Graph framing shortcuts stay scoped to the canvas", "[editor][render-graph][canvas][framing]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QWidget window;
    QVBoxLayout* layout = new QVBoxLayout(&window);
    QLineEdit* text = new QLineEdit(&window);
    RenderGraphCanvas* canvas = new RenderGraphCanvas(document, &window);
    layout->addWidget(text);
    layout->addWidget(canvas);
    window.resize(900, 650);
    canvas->ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    window.show();
    window.activateWindow();
    QCoreApplication::processEvents();
    canvas->SelectNode(QStringLiteral("raster"));
    canvas->setFocus();
    QTest::keyClick(canvas, Qt::Key_F);
    const QTransform selection = canvas->transform();
    QTest::keyClick(canvas, Qt::Key_Home);
    CHECK(canvas->transform().m11() < selection.m11());
    QTest::keySequence(canvas, QKeySequence(QStringLiteral("Ctrl+0")));
    CHECK(canvas->transform().m11() == 1);
    const QTransform beforeText = canvas->transform();
    text->setFocus();
    QTest::keyClicks(text, QStringLiteral("f"));
    QTest::keyClick(text, Qt::Key_Home);
    QTest::keySequence(text, QKeySequence(QStringLiteral("Ctrl+0")));
    CHECK(text->text() == QStringLiteral("f"));
    CHECK(text->cursorPosition() == 0);
    CHECK(canvas->transform() == beforeText);
    CHECK(document.GetRevision() == 0);
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
    CHECK(canvas.findChild<QLabel*>(QStringLiteral("renderGraphZoomLabel"))->text() == QStringLiteral("125%"));
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

TEST_CASE("Graph wheel and Ctrl+scroll zoom smoothly without a pinch", "[editor][render-graph][canvas]")
{
    RenderGraphDocument document;
    RenderGraphCanvas canvas(document);
    const QPointF position(100, 100);
    QWheelEvent wheel(position, position, {}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvas.viewport(), &wheel);
    CHECK(wheel.isAccepted() == true);
    CHECK(canvas.transform().m11() > 1);
    const float64 before = canvas.transform().m11();
    QWheelEvent
        pixels(position, position, QPoint(0, 12), {}, Qt::NoButton, Qt::ControlModifier, Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(canvas.viewport(), &pixels);
    CHECK(canvas.transform().m11() > before);
    QWheelEvent
        reverse(position, position, QPoint(0, -12), {}, Qt::NoButton, Qt::ControlModifier, Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(canvas.viewport(), &reverse);
    CHECK(qAbs(canvas.transform().m11() - before) < 0.000001);
    CHECK(document.GetRevision() == 0);
}

TEST_CASE("Graph two-finger touchpad scroll pans without zooming or editing", "[editor][render-graph][canvas]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    canvas.resize(300, 200);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    canvas.show();
    QCoreApplication::processEvents();
    canvas.resetTransform();
    canvas.centerOn(canvas.sceneRect().center());
    canvas.SelectNode(QStringLiteral("raster"));
    const QStringList selected = canvas.GetSelectedNodes();
    const QByteArray original = document.GetDraft(ROOT_GRAPH);
    const QPointF position(120, 90);
    const QPointF before = canvas.viewportTransform().inverted().map(position);
    QWheelEvent begin(position, position, {}, {}, Qt::NoButton, Qt::NoModifier, Qt::ScrollBegin, false);
    QCoreApplication::sendEvent(canvas.viewport(), &begin);
    // Fingers moving right/down drag the content with them, as middle-drag does.
    QWheelEvent scroll(position,
                       position,
                       QPoint(30, 20),
                       QPoint(90, 60),
                       Qt::NoButton,
                       Qt::NoModifier,
                       Qt::ScrollUpdate,
                       false);
    QCoreApplication::sendEvent(canvas.viewport(), &scroll);
    CHECK(scroll.isAccepted() == true);
    CHECK(canvas.transform().m11() == 1);
    const QPointF after = canvas.viewportTransform().inverted().map(position);
    CHECK((before - after - QPointF(30, 20)).manhattanLength() < 2);
    QWheelEvent momentum(position,
                         position,
                         QPoint(-10, 0),
                         QPoint(-30, 0),
                         Qt::NoButton,
                         Qt::NoModifier,
                         Qt::ScrollMomentum,
                         false);
    QCoreApplication::sendEvent(canvas.viewport(), &momentum);
    const QPointF coasted = canvas.viewportTransform().inverted().map(position);
    CHECK((coasted - after - QPointF(10, 0)).manhattanLength() < 2);
    QWheelEvent end(position, position, {}, {}, Qt::NoButton, Qt::NoModifier, Qt::ScrollEnd, false);
    QCoreApplication::sendEvent(canvas.viewport(), &end);
    CHECK(canvas.transform().m11() == 1);
    CHECK(canvas.GetSelectedNodes() == selected);
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
    CHECK(document.GetRevision() == 0);
    CHECK(document.CanUndo() == false);
}

TEST_CASE("Graph two-finger double-tap frames the whole graph", "[editor][render-graph][canvas]")
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
    canvas.scale(6, 6);
    const QRectF graph = canvas.scene()->itemsBoundingRect();
    CHECK(canvas.mapToScene(canvas.viewport()->rect()).boundingRect().contains(graph) == false);
    const QPointF position(170, 220);
    const QPointingDevice* device = QPointingDevice::primaryPointingDevice();
    QNativeGestureEvent smartZoom(Qt::SmartZoomNativeGesture, device, 2, position, position, position, 1, {});
    QCoreApplication::sendEvent(canvas.viewport(), &smartZoom);
    CHECK(smartZoom.isAccepted() == true);
    CHECK(canvas.mapToScene(canvas.viewport()->rect()).boundingRect().contains(graph) == true);
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
    CHECK(canvas.ActivateNode(QStringLiteral("missing")) == false);
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
        shaders->setCurrentCell(0, 2);
        QLineEdit* programNameOrNull = editor->findChild<QLineEdit*>(QStringLiteral("renderGraphProgramName"));
        REQUIRE(programNameOrNull != nullptr);
        CHECK(programNameOrNull->isReadOnly() == true);
        CHECK(programNameOrNull->text() == shaders->item(0, 2)->text());
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
        CHECK(workspace.GetDocument().GetDraft(sources.last()).contains("SurfaceHit") == true);
        CHECK(workspace.GetDocument().GetFiles() == beforeStageChange);
        stage->setCurrentText(QStringLiteral("miss"));
        CHECK(workspace.GetSelectedShaderSources() == sources);
        CHECK(workspace.GetDocument().GetFiles() == beforeStageChange);
        CHECK(workspace.GetDocument().GetDraft(sources.last()).contains("SceneContext scene, Ray ray,") == true);
        CHECK(editor->findChild<QTableWidget*>(QStringLiteral("shaders")) == shaders);
        stage->setCurrentText(QStringLiteral("closest_hit"));
        workspace.Undo();
        CHECK(workspace.GetDocument().GetDraft(sources.last()).contains("SceneContext scene, Ray ray,") == true);
        workspace.Undo();
        CHECK(workspace.GetDocument().GetDraft(sources.last()).contains("SurfaceHit") == true);
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
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialogOrNull != nullptr)
        {
            const uint64 revision = workspace.GetDocument().GetRevision();
            QTableWidget* currentShadersOrNull = dialogOrNull->findChild<QTableWidget*>(QStringLiteral("shaders"));
            REQUIRE(currentShadersOrNull != nullptr);
            currentShadersOrNull->item(0, 2)->setText(QStringLiteral("Geometry"));
            CHECK(workspace.GetDocument().GetRevision() == revision);
            QLabel* errorOrNull = dialogOrNull->findChild<QLabel*>(QStringLiteral("renderGraphNodeError"));
            REQUIRE(errorOrNull != nullptr);
            CHECK(errorOrNull->isVisible() == true);
            CHECK(errorOrNull->text().contains(QStringLiteral("Geometry")) == true);
            CHECK(errorOrNull->text().contains(QStringLiteral("Set program to Program")) == true);
            currentShadersOrNull = dialogOrNull->findChild<QTableWidget*>(QStringLiteral("shaders"));
            REQUIRE(currentShadersOrNull != nullptr);
            currentShadersOrNull->item(0, 2)->setText(QStringLiteral("Program"));
            CHECK(workspace.GetDocument().GetRevision() == revision + 1);
            CHECK(errorOrNull->isVisible() == false);
            QLineEdit* currentNameOrNull =
                dialogOrNull->findChild<QLineEdit*>(QStringLiteral("renderGraphProgramName"));
            QPushButton* removeOrNull =
                dialogOrNull->findChild<QPushButton*>(QStringLiteral("renderGraphRemove_shaders"));
            REQUIRE(currentNameOrNull != nullptr);
            REQUIRE(removeOrNull != nullptr);
            while (currentShadersOrNull->rowCount() > 0)
            {
                const int32 count = currentShadersOrNull->rowCount();
                currentShadersOrNull->setCurrentCell(count - 1, 0);
                removeOrNull->click();
                REQUIRE(currentShadersOrNull->rowCount() == count - 1);
            }
            CHECK(currentNameOrNull->text().isEmpty() == true);
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

TEST_CASE("scene ray query budgets share Properties and canvas history with Properties hidden",
          "[editor][render-graph][canvas][query-budgets]")
{
    QTemporaryDir project;
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/scene_ray"));
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    for (const QString& file : {ROOT_GRAPH,
                                QStringLiteral("main_shaders/geometry.slang"),
                                QStringLiteral("main_shaders/color.slang"),
                                QStringLiteral("main_shaders/depth.slang"),
                                QStringLiteral("main_shaders/generate.slang"),
                                QStringLiteral("main_shaders/miss.slang"),
                                QStringLiteral("main_shaders/closest.slang"),
                                QStringLiteral("main_shaders/payload.slang")})
    {
        WriteFile(QDir(package).filePath(file), ReadFile(source.filePath(file)));
    }
    const QString manifestFile = QStringLiteral("main.lushader.json");
    QByteArray manifestBytes = ReadFile(source.filePath(manifestFile + QStringLiteral(".in")));
    manifestBytes.replace("@RG2_TEST_PROFILES@", "[\"metal\", \"wgsl\"]");
    QJsonObject manifest = QJsonDocument::fromJson(manifestBytes).object();
    QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
    QJsonObject technique = techniques.last().toObject();
    QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
    interface.insert(QStringLiteral("surface_queries"), 1);
    interface.insert(QStringLiteral("visibility_queries"), 1);
    technique.insert(QStringLiteral("scene_interface"), interface);
    techniques.replace(techniques.size() - 1, technique);
    manifest.insert(QStringLiteral("techniques"), techniques);
    WriteFile(QDir(package).filePath(manifestFile), EncodeCanonicalJson(manifest));
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, QStringLiteral("trace")) == true);
    QWidget propertiesHost;
    static_cast<void>(workspace.TakeInspectorPanel(&propertiesHost));
    workspace.resize(1200, 800);
    workspace.show();
    QCoreApplication::processEvents();
    const QByteArray originalManifest = workspace.GetDocument().GetDraft(manifestFile);
    const QByteArray originalGraph = workspace.GetDocument().GetDraft(ROOT_GRAPH);
    const uint64 revision = workspace.GetDocument().GetRevision();
    bool hasExercised = false;
    const auto exercise = [&](QWidget& inoutControls) {
        QSpinBox* surfaceOrNull = inoutControls.findChild<QSpinBox*>(QStringLiteral("renderGraphSurfaceQueries"));
        QSpinBox* visibilityOrNull = inoutControls.findChild<QSpinBox*>(QStringLiteral("renderGraphVisibilityQueries"));
        REQUIRE(surfaceOrNull != nullptr);
        REQUIRE(visibilityOrNull != nullptr);
        CHECK(surfaceOrNull->value() == 1);
        CHECK(visibilityOrNull->value() == 1);
        surfaceOrNull->setValue(2);
        CHECK(workspace.GetDocument().GetRevision() == revision);
        REQUIRE(QMetaObject::invokeMethod(surfaceOrNull, "editingFinished") == true);
        CHECK(workspace.GetDocument().GetRevision() == revision + 1);
        CHECK(inoutControls.findChild<QSpinBox*>(QStringLiteral("renderGraphSurfaceQueries")) == surfaceOrNull);
        REQUIRE(QMetaObject::invokeMethod(surfaceOrNull, "editingFinished") == true);
        CHECK(workspace.GetDocument().GetRevision() == revision + 1);
        visibilityOrNull->setValue(0);
        REQUIRE(QMetaObject::invokeMethod(visibilityOrNull, "editingFinished") == true);
        CHECK(workspace.GetDocument().GetRevision() == revision + 2);
        RenderGraphSceneQueryBudgets edited;
        REQUIRE(workspace.GetDocument().GetSceneQueryBudgets(edited) == true);
        CHECK(edited.SurfaceQueries == 2);
        CHECK(edited.VisibilityQueries == 0);
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) == originalGraph);
        CHECK(workspace.GetDocument().GetSaved(manifestFile) == originalManifest);
        const QByteArray editedManifest = workspace.GetDocument().GetDraft(manifestFile);
        workspace.Undo();
        workspace.Undo();
        CHECK(workspace.GetDocument().GetDraft(manifestFile) == originalManifest);
        workspace.Redo();
        workspace.Redo();
        CHECK(workspace.GetDocument().GetDraft(manifestFile) == editedManifest);
        REQUIRE(workspace.Save() == true);
        CHECK(ReadFile(QDir(package).filePath(manifestFile)) == editedManifest);
        RenderGraphDocument reloaded;
        REQUIRE(reloaded.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
        REQUIRE(reloaded.GetSceneQueryBudgets(edited) == true);
        CHECK(edited.SurfaceQueries == 2);
        CHECK(edited.VisibilityQueries == 0);
        hasExercised = true;
    };
    SECTION("Properties")
    {
        exercise(propertiesHost);
    }
    SECTION("canvas with Properties hidden")
    {
        propertiesHost.hide();
        RenderGraphCanvas& canvas = workspace.GetCanvas();
        canvas.Rebuild();
        QTimer::singleShot(0, &canvas, [&canvas, &exercise, &propertiesHost]() {
            QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            REQUIRE(menuOrNull != nullptr);
            QAction* editOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphEditNodeAction"));
            REQUIRE(editOrNull != nullptr);
            QTimer::singleShot(0, &canvas, [&exercise, &propertiesHost]() {
                QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                REQUIRE(dialogOrNull != nullptr);
                CHECK(propertiesHost.isVisible() == false);
                exercise(*dialogOrNull);
                dialogOrNull->reject();
            });
            menuOrNull->close();
            editOrNull->trigger();
        });
        const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("trace")) + QPointF(20, 10));
        QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
        QApplication::sendEvent(canvas.viewport(), &context);
    }
    CHECK(hasExercised == true);
}

TEST_CASE("legacy ray materials can be enabled from canvas with Properties hidden",
          "[editor][render-graph][canvas][scene-materials]")
{
    QTemporaryDir project;
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/raytracing"));
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    for (const QString& file : {ROOT_GRAPH,
                                QStringLiteral("main_shaders/geometry.slang"),
                                QStringLiteral("main_shaders/color.slang"),
                                QStringLiteral("main_shaders/depth.slang"),
                                QStringLiteral("main_shaders/generate.slang"),
                                QStringLiteral("main_shaders/miss.slang"),
                                QStringLiteral("main_shaders/closest.slang")})
    {
        WriteFile(QDir(package).filePath(file), ReadFile(source.filePath(file)));
    }
    QByteArray manifest = ReadFile(source.filePath(QStringLiteral("main.lushader.json.in")));
    manifest.replace("@RG2_TEST_PROFILES@", "[\"metal\", \"wgsl\"]");
    QJsonObject fullManifest = QJsonDocument::fromJson(manifest).object();
    const QJsonArray programs = fullManifest.value(QStringLiteral("programs")).toArray();
    QJsonObject budgets = fullManifest.value(QStringLiteral("budgets")).toObject();
    budgets.insert(QStringLiteral("programs"), programs.size());
    budgets.insert(QStringLiteral("artifacts"),
                   programs.size() * fullManifest.value(QStringLiteral("profiles")).toArray().size());
    fullManifest.insert(QStringLiteral("budgets"), budgets);
    manifest = EncodeCanonicalJson(fullManifest);
    const QString manifestFile = QStringLiteral("main.lushader.json");
    WriteFile(QDir(package).filePath(manifestFile), manifest);
    RendererWorkspace workspace({});
    workspace.resize(1400, 800);
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.show();
    REQUIRE(QTest::qWaitForWindowExposed(&workspace) == true);
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, QStringLiteral("trace")) == true);
    QWidget inspectorHost;
    static_cast<void>(workspace.TakeInspectorPanel(&inspectorHost));
    inspectorHost.hide();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.resetTransform();
    const QPointF position = canvas.GetNodePosition(QStringLiteral("trace"));
    canvas.centerOn(position);
    // Source text already belongs to the document while the presentation refresh is deferred.
    QComboBox* sourceFileOrNull = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphSourceFile"));
    QPlainTextEdit* sourceOrNull = workspace.findChild<QPlainTextEdit*>(QStringLiteral("renderGraphSource"));
    REQUIRE(sourceFileOrNull != nullptr);
    REQUIRE(sourceOrNull != nullptr);
    const QString editedFile = QStringLiteral("main_shaders/closest.slang");
    sourceFileOrNull->setCurrentText(editedFile);
    const QByteArray originalSource = workspace.GetDocument().GetDraft(editedFile);
    sourceOrNull->appendPlainText(QStringLiteral("// Preserve this accepted source edit."));
    const QByteArray editedSource = workspace.GetDocument().GetDraft(editedFile);
    REQUIRE(editedSource != originalSource);
    REQUIRE(editedSource.contains("// Preserve this accepted source edit.") == true);
    const QStringList shaderSources = workspace.GetSelectedShaderSources();
    const QStringList originalFiles = workspace.GetDocument().GetFiles();
    const uint64 revision = workspace.GetDocument().GetRevision();
    const QByteArray before = workspace.GetDocument().GetDraft(manifestFile);
    bool shouldExpand = false;
    SECTION("explicit material migration") {}
    SECTION("bundle expansion from the canvas")
    {
        shouldExpand = true;
    }
    QTimer::singleShot(0, &workspace, [&workspace, shouldExpand]() {
        QMenu* menuOrNull = workspace.findChild<QMenu*>(QStringLiteral("renderGraphNodeMenu"));
        REQUIRE(menuOrNull != nullptr);
        QAction* enable = menuOrNull->findChild<QAction*>(
            shouldExpand == true ? QStringLiteral("renderGraphExpandSceneBundleAction")
                                 : QStringLiteral("renderGraphEnableSceneMaterialsAction"));
        REQUIRE(enable != nullptr);
        REQUIRE(enable->isEnabled() == true);
        enable->trigger();
        menuOrNull->close();
    });
    const QPoint point = canvas.mapFromScene(position + QPointF(30, 10));
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QCoreApplication::sendEvent(canvas.viewport(), &context);
    if (shouldExpand == true)
    {
        CHECK(canvas.IsSceneBundleExpanded(QStringLiteral("trace")) == true);
        CHECK(canvas.GetPortPosition(QStringLiteral("trace.scene_geometry"), false).isNull() == false);
        CHECK(canvas.GetPortPosition(QStringLiteral("scene.materials"), true).isNull() == false);
        canvas.SetSceneBundleExpanded(QStringLiteral("trace"), false);
        CHECK(canvas.GetPortPosition(QStringLiteral("trace.scene_geometry"), false).isNull() == false);
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("trace.scene_lights") == true);
    }
    CHECK(workspace.GetDocument().GetRevision() == revision + 1);
    CHECK(QJsonDocument::fromJson(workspace.GetDocument().GetDraft(manifestFile))
              .object()
              .value(QStringLiteral("budgets"))
              .toObject() == budgets);
    CHECK(workspace.GetDocument().GetDraft(manifestFile).contains("scene_interface") == true);
    CHECK(workspace.GetSelectedShaderSources().size() == 3);
    CHECK(workspace.GetSelectedShaderSources() == shaderSources);
    CHECK(workspace.GetDocument().GetFiles() == originalFiles);
    for (const QString& file : workspace.GetSelectedShaderSources())
    {
        CHECK(workspace.GetDocument().GetDraft(file) ==
              (file == editedFile ? editedSource : ReadFile(source.filePath(file))));
    }
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(manifestFile) == before);
    CHECK(workspace.GetDocument().GetDraft(editedFile) == editedSource);
    workspace.Redo();
    CHECK(workspace.GetDocument().GetDraft(manifestFile).contains("scene_interface") == true);
    CHECK(workspace.GetDocument().GetDraft(editedFile).contains("// Preserve this accepted source edit.") == true);
}

TEST_CASE("diagnostic navigation rings the exact port without Properties or draft edits",
          "[editor][render-graph][canvas][diagnostics]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 7);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"))->hide();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphDocument& document = workspace.GetDocument();
    const uint64 revision = document.GetRevision();
    const QByteArray bytes = document.GetDraft(ROOT_GRAPH);
    const QJsonObject location{{QStringLiteral("source"), ROOT_GRAPH},
                               {QStringLiteral("configuration"), DEFAULT_CONFIGURATION},
                               {QStringLiteral("node"), QStringLiteral("raster")},
                               {QStringLiteral("port"), QStringLiteral("depth")},
                               {QStringLiteral("direction"), QStringLiteral("input")}};
    RenderGraphJobResult result;
    result.Request.ProjectEpoch = 7;
    result.Request.InputIdentity = QByteArray("earlier source");
    result.Code = QStringLiteral("RenderGraphUnboundInput");
    result.Message = QStringLiteral("Unconnected depth");
    result.Location = location;
    result.Diagnostics = QJsonArray{QJsonObject{{QStringLiteral("code"), result.Code},
                                                {QStringLiteral("message"), result.Message},
                                                {QStringLiteral("severity"), QStringLiteral("error")},
                                                {QStringLiteral("location"), location},
                                                {QStringLiteral("expected"), QStringLiteral("DepthTexture")},
                                                {QStringLiteral("actual"), QStringLiteral("unconnected")},
                                                {QStringLiteral("repair"), QStringLiteral("Connect a depth target.")}}};
    workspace.AcceptResult(result);
    REQUIRE(workspace.GetDiagnosticCount() == 1);
    REQUIRE(workspace.ActivateDiagnostic(0) == true);
    CHECK(canvas.GetDiagnosticLocation() == location);
    const auto hasRingAt = [&canvas](QPointF position) {
        for (QGraphicsItem* item : canvas.scene()->items())
        {
            const QGraphicsEllipseItem* ringOrNull = qgraphicsitem_cast<QGraphicsEllipseItem*>(item);
            if (ringOrNull != nullptr && ringOrNull->rect().width() > 20 &&
                ringOrNull->mapToScene(ringOrNull->rect().center()) == position)
            {
                return true;
            }
        }
        return false;
    };
    CHECK(hasRingAt(canvas.GetPortPosition(QStringLiteral("raster.depth"), false)) == true);
    workspace.AcceptResult(result);
    CHECK(canvas.GetDiagnosticLocation().isEmpty() == true);
    REQUIRE(workspace.ActivateDiagnostic(0) == true);
    CHECK(canvas.accessibleDescription().contains(QStringLiteral("DepthTexture")) == true);
    CHECK(canvas.accessibleDescription().contains(QStringLiteral("earlier source version")) == true);
    CHECK(workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"))->isHidden() == true);
    CHECK(document.GetRevision() == revision);
    CHECK(document.GetDraft(ROOT_GRAPH) == bytes);
    canvas.Rebuild();
    CHECK(canvas.GetDiagnosticLocation().isEmpty() == true);
    CHECK(canvas.accessibleDescription().isEmpty() == true);
    CHECK(canvas.ShowDiagnostic(QJsonObject{{QStringLiteral("node"), QStringLiteral("deleted")}}, {}) == false);
    canvas.ShowConfiguration(LIBRARY, QStringLiteral("ColorAndDepth"));
    REQUIRE(canvas.ShowDiagnostic(QJsonObject{{QStringLiteral("port"), QStringLiteral("color")},
                                              {QStringLiteral("direction"), QStringLiteral("output")}},
                                  QStringLiteral("Choose a matching output source.")) == true);
    CHECK(hasRingAt(canvas.GetPortPosition(QStringLiteral("#outputs.color"), false)) == true);
    CHECK(document.GetRevision() == revision);
}

TEST_CASE("field diagnostics open the shared canvas editor at the highlighted control",
          "[editor][render-graph][canvas][diagnostics]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 7);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"))->hide();
    QString field = QStringLiteral("depth_load");
    QString file = LIBRARY;
    QString configuration = QStringLiteral("ColorAndDepth");
    QString node = QStringLiteral("draw");
    SECTION("legacy attachment load") {}
    SECTION("ray launch field on a hidden tab")
    {
        field = QStringLiteral("invocation.dimensions");
        node = QStringLiteral("raytracingpipeline");
    }
    SECTION("shader assignments on a hidden tab")
    {
        field = QStringLiteral("shaders");
        node = QStringLiteral("raytracingpipeline");
    }
    if (node == QStringLiteral("raytracingpipeline"))
    {
        QComboBox* kinds = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
        REQUIRE(kinds != nullptr);
        kinds->setCurrentText(QStringLiteral("RayTracingPipeline"));
        workspace.findChild<QPushButton*>(QStringLiteral("renderGraphAddNode"))->click();
        file = ROOT_GRAPH;
        configuration = DEFAULT_CONFIGURATION;
    }
    const uint64 revision = workspace.GetDocument().GetRevision();
    RenderGraphJobResult result;
    result.Request.ProjectEpoch = 7;
    result.Code = QStringLiteral("RenderGraphInitialization");
    result.Message = QStringLiteral("Choose Clear");
    result.Location = QJsonObject{{QStringLiteral("source"), file},
                                  {QStringLiteral("configuration"), configuration},
                                  {QStringLiteral("node"), node},
                                  {QStringLiteral("field"), field}};
    workspace.AcceptResult(result);
    bool hasHighlightedField = false;
    QTimer::singleShot(0, &workspace, [&hasHighlightedField, field]() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        for (QWidget* control : dialogOrNull->findChildren<QWidget*>())
        {
            if (control->property("isRenderGraphDiagnosticTarget").toBool() == true)
            {
                hasHighlightedField = true;
                CHECK(control->isVisible() == true);
                CHECK((control->property("renderGraphField").toString() == field ||
                       control->objectName() == QStringLiteral("renderGraphField_") + field));
            }
        }
        dialogOrNull->reject();
    });
    REQUIRE(workspace.ActivateDiagnostic(0) == true);
    CHECK(hasHighlightedField == true);
    CHECK(workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"))->isHidden() == true);
    CHECK(workspace.GetDocument().GetRevision() == revision);
}

TEST_CASE("persistent buffer capacity edits share canvas and Properties history", "[editor][render-graph][ray-buffer]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QComboBox* kinds = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    QPushButton* add = workspace.findChild<QPushButton*>(QStringLiteral("renderGraphAddNode"));
    REQUIRE(kinds != nullptr);
    REQUIRE(add != nullptr);
    kinds->setCurrentText(QStringLiteral("Buffer"));
    add->click();
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, QStringLiteral("buffer")) == true);
    CHECK(workspace.GetCanvas().GetPortPosition(QStringLiteral("buffer.buffer"), true).isNull() == false);
    const auto exercise = [&workspace](QWidget& inoutFields) {
        QSpinBox* capacity = inoutFields.findChild<QSpinBox*>(QStringLiteral("renderGraphField_bytes"));
        REQUIRE(capacity != nullptr);
        const uint64 revision = workspace.GetDocument().GetRevision();
        capacity->setValue(98304);
        Q_EMIT capacity->editingFinished();
        CHECK(workspace.GetDocument().GetRevision() == revision + 1);
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("98304") == true);
        workspace.Undo();
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("98304") == false);
        workspace.Redo();
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("98304") == true);
        REQUIRE(workspace.Save() == true);
    };
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    SECTION("Properties")
    {
        exercise(*properties);
    }
    SECTION("canvas with Properties hidden")
    {
        workspace.resize(1200, 800);
        workspace.show();
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
                exercise(*dialog);
                dialog->reject();
            });
            menu->close();
            edit->trigger();
        });
        const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("buffer")) + QPointF(20, 10));
        QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
        QApplication::sendEvent(canvas.viewport(), &context);
    }
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(ROOT_GRAPH) == workspace.GetDocument().GetDraft(ROOT_GRAPH));
}

TEST_CASE("structured buffer fields edit on canvas with Properties hidden and undo", "[editor][render-graph][storage]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QComboBox* kindsOrNull = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    REQUIRE(kindsOrNull != nullptr);
    kindsOrNull->setCurrentText(QStringLiteral("StructuredBuffer"));
    workspace.findChild<QPushButton*>(QStringLiteral("renderGraphAddNode"))->click();
    const QString identity = QStringLiteral("structuredbuffer");
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, identity) == true);
    RenderGraphNodePorts ports;
    REQUIRE(GetRenderGraphNodePorts(workspace.GetDocument(),
                                    QJsonObject{{QStringLiteral("kind"), QStringLiteral("StructuredBuffer")}},
                                    ports) == true);
    REQUIRE(ports.Outputs.size() == 1);
    CHECK(ports.Outputs[0].Type == QStringLiteral("Buffer"));
    workspace.resize(1200, 800);
    workspace.show();
    QWidget* propertiesOrNull = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(propertiesOrNull != nullptr);
    propertiesOrNull->hide();
    QCoreApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.Rebuild();
    QTimer::singleShot(0, &canvas, [&canvas, propertiesOrNull]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* editOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphEditNodeAction"));
        REQUIRE(editOrNull != nullptr);
        QTimer::singleShot(0, &canvas, [propertiesOrNull]() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialogOrNull != nullptr);
            CHECK(propertiesOrNull->isHidden() == true);
            QSpinBox* strideOrNull = dialogOrNull->findChild<QSpinBox*>(QStringLiteral("renderGraphField_stride"));
            REQUIRE(strideOrNull != nullptr);
            strideOrNull->setValue(32);
            Q_EMIT strideOrNull->editingFinished();
            QLineEdit* countOrNull = dialogOrNull->findChild<QLineEdit*>(QStringLiteral("renderGraphField_count"));
            REQUIRE(countOrNull != nullptr);
            countOrNull->setText(QStringLiteral("1024"));
            Q_EMIT countOrNull->editingFinished();
            dialogOrNull->accept();
        });
        menuOrNull->close();
        editOrNull->trigger();
    });
    const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(identity) + QPointF(20, 10));
    QContextMenuEvent event(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QApplication::sendEvent(canvas.viewport(), &event);
    CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("1024") == true);
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("ColorTarget") == true);
    workspace.Redo();
    REQUIRE(workspace.Save() == true);
}

TEST_CASE("accumulation limits edit through canvas and Properties with undo and save",
          "[editor][render-graph][accumulation]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QComboBox* kinds = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    QPushButton* add = workspace.findChild<QPushButton*>(QStringLiteral("renderGraphAddNode"));
    REQUIRE(kinds != nullptr);
    REQUIRE(add != nullptr);
    kinds->setCurrentText(QStringLiteral("Accumulate"));
    add->click();
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, QStringLiteral("accumulate")) == true);
    CHECK(workspace.GetCanvas().GetPortPosition(QStringLiteral("accumulate.color"), true).isNull() == false);
    CHECK(workspace.GetCanvas().GetPortPosition(QStringLiteral("accumulate.color"), false).isNull() == false);
    const auto exercise = [&workspace](QWidget& inoutFields) {
        QSpinBox* sampleLimit = inoutFields.findChild<QSpinBox*>(QStringLiteral("renderGraphField_samples"));
        REQUIRE(sampleLimit != nullptr);
        const uint64 revision = workspace.GetDocument().GetRevision();
        sampleLimit->setValue(4095);
        Q_EMIT sampleLimit->editingFinished();
        CHECK(workspace.GetDocument().GetRevision() == revision + 1);
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("4095") == true);
        workspace.Undo();
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("4095") == false);
        workspace.Redo();
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("4095") == true);
        REQUIRE(workspace.Save() == true);
    };
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    SECTION("Properties")
    {
        exercise(*properties);
    }
    SECTION("canvas with Properties hidden")
    {
        workspace.resize(1200, 800);
        workspace.show();
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
                exercise(*dialog);
                dialog->reject();
            });
            menu->close();
            edit->trigger();
        });
        const QPoint point =
            canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("accumulate")) + QPointF(20, 10));
        QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
        QApplication::sendEvent(canvas.viewport(), &context);
    }
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(ROOT_GRAPH) == workspace.GetDocument().GetDraft(ROOT_GRAPH));
}

TEST_CASE("disabling a node keeps its settings and wires through canvas and Properties",
          "[editor][render-graph][disable-node]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QComboBox* kinds = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    QPushButton* add = workspace.findChild<QPushButton*>(QStringLiteral("renderGraphAddNode"));
    REQUIRE(kinds != nullptr);
    REQUIRE(add != nullptr);
    kinds->setCurrentText(QStringLiteral("ToneMap"));
    add->click();
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, QStringLiteral("tonemap")) == true);
    const auto findNode = [&workspace]() {
        QJsonObject graph;
        REQUIRE(workspace.GetDocument().GetGraph(ROOT_GRAPH, graph) == true);
        const QJsonArray nodes = graph.value(QStringLiteral("configurations"))
                                     .toArray()
                                     .at(0)
                                     .toObject()
                                     .value(QStringLiteral("nodes"))
                                     .toArray();
        for (const QJsonValueConstRef node : nodes)
        {
            if (node.toObject().value(QStringLiteral("id")).toString() == QStringLiteral("tonemap"))
            {
                return node.toObject();
            }
        }
        return QJsonObject{};
    };
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    QCheckBox* enabled = properties->findChild<QCheckBox*>(QStringLiteral("renderGraphField_enabled"));
    REQUIRE(enabled != nullptr);
    CHECK(enabled->isChecked() == true);
    CHECK(findNode().contains(QStringLiteral("enabled")) == false);
    const uint64 revision = workspace.GetDocument().GetRevision();
    enabled->setChecked(false);
    CHECK(workspace.GetDocument().GetRevision() == revision + 1);
    CHECK(findNode().value(QStringLiteral("enabled")) == QJsonValue(false));
    CHECK(findNode().value(QStringLiteral("exposure")).toDouble() == 1);
    CHECK(IsRenderGraphNodeEnabled(findNode()) == false);
    workspace.Undo();
    CHECK(findNode().contains(QStringLiteral("enabled")) == false);
    workspace.Redo();
    CHECK(IsRenderGraphNodeEnabled(findNode()) == false);

    // The canvas context menu reflects and clears the same authored field without Properties.
    workspace.resize(1200, 800);
    workspace.show();
    properties->hide();
    QCoreApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.Rebuild();
    bool wasChecked = false;
    QTimer::singleShot(0, &canvas, [&wasChecked]() {
        QMenu* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menu != nullptr);
        QAction* disable = menu->findChild<QAction*>(QStringLiteral("renderGraphDisableNodeAction"));
        REQUIRE(disable != nullptr);
        wasChecked = disable->isChecked();
        menu->close();
        disable->trigger();
    });
    const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("tonemap")) + QPointF(20, 10));
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QApplication::sendEvent(canvas.viewport(), &context);
    CHECK(wasChecked == true);
    CHECK(findNode().contains(QStringLiteral("enabled")) == false);
    CHECK(IsRenderGraphNodeEnabled(findNode()) == true);
    REQUIRE(workspace.Save() == true);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(ROOT_GRAPH) == workspace.GetDocument().GetDraft(ROOT_GRAPH));
}

TEST_CASE("disabling an Export node keeps its type and wire", "[editor][render-graph][disable-node]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, QStringLiteral("output")) == true);
    const QJsonObject original = FindNode(workspace.GetDocument(), QStringLiteral("output"));
    const QJsonArray edges = GetConfiguration(workspace.GetDocument(), ROOT_GRAPH, DEFAULT_CONFIGURATION)
                                 .value(QStringLiteral("edges"))
                                 .toArray();
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    QCheckBox* enabled = properties->findChild<QCheckBox*>(QStringLiteral("renderGraphField_enabled"));
    REQUIRE(enabled != nullptr);
    CHECK(enabled->isChecked() == true);
    CHECK(enabled->toolTip().contains(QStringLiteral("no root")) == true);
    enabled->setChecked(false);
    QJsonObject disabled = FindNode(workspace.GetDocument(), QStringLiteral("output"));
    CHECK(IsRenderGraphNodeEnabled(disabled) == false);
    CHECK(disabled.take(QStringLiteral("enabled")) == QJsonValue(false));
    CHECK(disabled == original);
    CHECK(GetConfiguration(workspace.GetDocument(), ROOT_GRAPH, DEFAULT_CONFIGURATION)
              .value(QStringLiteral("edges"))
              .toArray() == edges);
    workspace.Undo();
    CHECK(FindNode(workspace.GetDocument(), QStringLiteral("output")) == original);
}

TEST_CASE("tone mapping exposure edits through canvas and Properties with undo and save",
          "[editor][render-graph][tone-mapping]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QComboBox* kinds = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    QPushButton* add = workspace.findChild<QPushButton*>(QStringLiteral("renderGraphAddNode"));
    REQUIRE(kinds != nullptr);
    REQUIRE(add != nullptr);
    kinds->setCurrentText(QStringLiteral("ToneMap"));
    add->click();
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, QStringLiteral("tonemap")) == true);
    CHECK(workspace.GetCanvas().GetPortPosition(QStringLiteral("tonemap.color"), true).isNull() == false);
    CHECK(workspace.GetCanvas().GetPortPosition(QStringLiteral("tonemap.color"), false).isNull() == false);
    const auto exercise = [&workspace](QWidget& inoutFields) {
        QDoubleSpinBox* exposure = inoutFields.findChild<QDoubleSpinBox*>(QStringLiteral("renderGraphField_exposure"));
        REQUIRE(exposure != nullptr);
        const uint64 revision = workspace.GetDocument().GetRevision();
        exposure->setValue(2.25);
        Q_EMIT exposure->editingFinished();
        CHECK(workspace.GetDocument().GetRevision() == revision + 1);
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("2.25") == true);
        workspace.Undo();
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("2.25") == false);
        workspace.Redo();
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("2.25") == true);
        REQUIRE(workspace.Save() == true);
    };
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    SECTION("Properties")
    {
        exercise(*properties);
    }
    SECTION("canvas with Properties hidden")
    {
        workspace.resize(1200, 800);
        workspace.show();
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
                exercise(*dialog);
                dialog->reject();
            });
            menu->close();
            edit->trigger();
        });
        const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("tonemap")) + QPointF(20, 10));
        QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
        QApplication::sendEvent(canvas.viewport(), &context);
    }
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(ROOT_GRAPH) == workspace.GetDocument().GetDraft(ROOT_GRAPH));
}

TEST_CASE("OIDN processing quality edits through canvas and Properties with undo and save",
          "[editor][render-graph][denoising]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QComboBox* kinds = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    QPushButton* add = workspace.findChild<QPushButton*>(QStringLiteral("renderGraphAddNode"));
    REQUIRE(kinds != nullptr);
    REQUIRE(add != nullptr);
    kinds->setCurrentText(QStringLiteral("Denoise"));
    add->click();
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, QStringLiteral("denoise")) == true);
    CHECK(workspace.GetCanvas().GetPortPosition(QStringLiteral("denoise.color"), true).isNull() == false);
    CHECK(workspace.GetCanvas().GetPortPosition(QStringLiteral("denoise.color"), false).isNull() == false);
    const auto exercise = [&workspace](QWidget& inoutFields) {
        QComboBox* qualityOrNull = inoutFields.findChild<QComboBox*>(QStringLiteral("renderGraphField_quality"));
        REQUIRE(qualityOrNull != nullptr);
        CHECK(qualityOrNull->count() == 3);
        const uint64 revision = workspace.GetDocument().GetRevision();
        qualityOrNull->setCurrentText(QStringLiteral("High"));
        Q_EMIT qualityOrNull->activated(qualityOrNull->currentIndex());
        CHECK(workspace.GetDocument().GetRevision() == revision + 1);
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("High") == true);
        workspace.Undo();
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("High") == false);
        workspace.Redo();
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("High") == true);
        REQUIRE(workspace.Save() == true);
    };
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    SECTION("Properties")
    {
        exercise(*properties);
    }
    SECTION("canvas with Properties hidden")
    {
        workspace.resize(1200, 800);
        workspace.show();
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
                exercise(*dialog);
                dialog->reject();
            });
            menu->close();
            edit->trigger();
        });
        const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("denoise")) + QPointF(20, 10));
        QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
        QApplication::sendEvent(canvas.viewport(), &context);
    }
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(ROOT_GRAPH) == workspace.GetDocument().GetDraft(ROOT_GRAPH));
}

TEST_CASE("scene compute nodes are authored on the canvas with Properties hidden",
          "[editor][render-graph][pipeline][scene-compute]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    QComboBox* kindsOrNull = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    REQUIRE(kindsOrNull != nullptr);
    kindsOrNull->setCurrentText(QStringLiteral("ComputePipeline"));
    workspace.findChild<QPushButton*>(QStringLiteral("renderGraphAddNode"))->click();
    const QString identity = QStringLiteral("computepipeline");
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, identity) == true);
    const auto getNode = [&workspace, &identity]() {
        const QJsonObject graph = QJsonDocument::fromJson(workspace.GetDocument().GetDraft(ROOT_GRAPH)).object();
        for (const QJsonValueConstRef configuration : graph.value(QStringLiteral("configurations")).toArray())
        {
            for (const QJsonValueConstRef node : configuration.toObject().value(QStringLiteral("nodes")).toArray())
            {
                if (node.toObject().value(QStringLiteral("id")).toString() == identity)
                {
                    return node.toObject();
                }
            }
        }
        return QJsonObject{};
    };
    // New generic compute nodes keep the generic defaults.
    CHECK(getNode().value(QStringLiteral("state")).toObject() ==
          QJsonObject{{QStringLiteral("group_size"), QJsonArray{1, 1, 1}}});
    const QByteArray generic = workspace.GetDocument().GetDraft(ROOT_GRAPH);
    workspace.resize(1200, 800);
    workspace.show();
    QWidget* propertiesOrNull = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(propertiesOrNull != nullptr);
    propertiesOrNull->hide();
    QCoreApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.Rebuild();
    // The follow-up runs inside a modal dialog the action opens; actions without one ignore it.
    const auto openMenu = [&canvas, &identity](const QString& action, const auto& afterTrigger) {
        QTimer::singleShot(0, &canvas, [&canvas, action, afterTrigger]() {
            QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            REQUIRE(menuOrNull != nullptr);
            QAction* actionOrNull = menuOrNull->findChild<QAction*>(action);
            REQUIRE(actionOrNull != nullptr);
            REQUIRE(actionOrNull->isEnabled() == true);
            QTimer::singleShot(0, &canvas, afterTrigger);
            menuOrNull->close();
            actionOrNull->trigger();
        });
        const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(identity) + QPointF(20, 10));
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
        QApplication::sendEvent(canvas.viewport(), &event);
    };
    const uint64 revision = workspace.GetDocument().GetRevision();
    openMenu(QStringLiteral("renderGraphAddSceneInputsAction"), []() {});
    CHECK(workspace.GetDocument().GetRevision() == revision + 1);
    const QJsonObject converted = getNode();
    CHECK(converted.value(QStringLiteral("inputs")).toArray().at(0).toObject().value(QStringLiteral("type")) ==
          QStringLiteral("Scene"));
    CHECK(converted.value(QStringLiteral("state")).toObject().isEmpty() == true);
    CHECK(converted.value(QStringLiteral("invocation")).toObject().value(QStringLiteral("dimensions")) ==
          QStringLiteral("ColorTarget"));
    CHECK(canvas.GetPortPosition(identity + QStringLiteral(".scene"), false).isNull() == false);
    CHECK(canvas.GetPortPosition(identity + QStringLiteral(".view"), false).isNull() == false);
    openMenu(QStringLiteral("renderGraphEditNodeAction"), [&workspace, propertiesOrNull, revision]() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        CHECK(propertiesOrNull->isHidden() == true);
        QComboBox* modeOrNull = dialogOrNull->findChild<QComboBox*>(QStringLiteral("rayLaunchSizeMode"));
        REQUIRE(modeOrNull != nullptr);
        CHECK(modeOrNull->currentText() == QStringLiteral("Match color target"));
        modeOrNull->setCurrentIndex(1);
        CHECK(workspace.GetDocument().GetRevision() == revision + 2);
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("ColorTarget") == false);
        QGroupBox* advancedOrNull = dialogOrNull->findChild<QGroupBox*>(QStringLiteral("renderGraphComputeAdvanced"));
        REQUIRE(advancedOrNull != nullptr);
        CHECK(advancedOrNull->isChecked() == false);
        advancedOrNull->setChecked(true);
        CHECK(workspace.GetDocument().GetRevision() == revision + 3);
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH).contains("group_size") == true);
        dialogOrNull->accept();
    });
    const QJsonObject edited = getNode();
    CHECK(edited.value(QStringLiteral("invocation")).toObject().value(QStringLiteral("dimensions")).isArray() == true);
    CHECK(edited.value(QStringLiteral("state")).toObject() ==
          QJsonObject{{QStringLiteral("group_size"), QJsonArray{64, 1, 1}}});
    CHECK(workspace.GetDocument().GetRevision() == revision + 3);
    workspace.Undo();
    workspace.Undo();
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) == generic);
    workspace.Redo();
    workspace.Redo();
    workspace.Redo();
    CHECK(getNode() == edited);
    REQUIRE(workspace.Save() == true);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(ROOT_GRAPH) == workspace.GetDocument().GetDraft(ROOT_GRAPH));
}
