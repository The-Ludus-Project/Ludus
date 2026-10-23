#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_resources.h"
#include "internal/render_graph_upscale_node.h"
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
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
const QString ROOT = QStringLiteral("main.lurender.json");
const QString CONFIGURATION = QStringLiteral("Default");

QString MakeUpscaleTestPackage(const QTemporaryDir& project)
{
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/offscreen"));
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    REQUIRE(QDir().mkpath(package) == true);
    for (const QString& file : {ROOT, QStringLiteral("rasterize.lurender.json")})
    {
        REQUIRE(QFile::copy(source.filePath(file), QDir(package).filePath(file)) == true);
    }
    return package;
}

QJsonObject GetUpscaleTestNode(const RenderGraphDocument& document, const QString& identity)
{
    QJsonObject graph;
    REQUIRE(document.GetGraph(ROOT, graph) == true);
    const QJsonArray nodes = graph.value(QStringLiteral("configurations"))
                                 .toArray()
                                 .at(0)
                                 .toObject()
                                 .value(QStringLiteral("nodes"))
                                 .toArray();
    for (const QJsonValueConstRef node : nodes)
    {
        if (node.toObject().value(QStringLiteral("id")).toString() == identity)
        {
            return node.toObject();
        }
    }
    return {};
}
} // namespace

TEST_CASE("upscaling presets use the document transaction and persist without shaders", "[editor][upscale]")
{
    QTemporaryDir project;
    const QString package = MakeUpscaleTestPackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT)) == true);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray original = document.GetDraft(ROOT);
    QComboBox* kinds = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    REQUIRE(kinds != nullptr);
    for (const QString& algorithm : GetUpscaleAlgorithms())
    {
        CAPTURE(algorithm.toStdString());
        const int32 index = kinds->findData(algorithm);
        REQUIRE(index >= 0);
        kinds->setCurrentIndex(index);
        workspace.findChild<QPushButton*>(QStringLiteral("renderGraphAddNode"))->click();
        const QJsonObject node = GetUpscaleTestNode(document, algorithm);
        CHECK(node == MakeUpscaleNode(algorithm, algorithm));
        CHECK(QJsonDocument::fromJson(document.GetDraft(ROOT)).object().value(QStringLiteral("version")).toInt() == 2);
        RenderGraphNodePorts ports;
        REQUIRE(GetRenderGraphNodePorts(document, node, ports) == true);
        const bool isSpatial = algorithm == QStringLiteral("bilinear") || algorithm == QStringLiteral("fsr-1") ||
                               algorithm == QStringLiteral("metalfx-spatial");
        constexpr int32 TEMPORAL_INPUT_COUNT = 8;
        CHECK(ports.Inputs.size() == (isSpatial == true ? 2 : TEMPORAL_INPUT_COUNT));
        CHECK(ports.Outputs.size() == 1);
        CHECK(workspace.GetCanvas().GetPortPosition(algorithm + QStringLiteral(".destination"), false).isNull() ==
              false);
        QLabel* availability = workspace.findChild<QLabel*>(QStringLiteral("renderGraphUpscaleAvailability"));
        REQUIRE(availability != nullptr);
        CHECK(availability->text().contains(QStringLiteral("backend is unavailable")) ==
              (algorithm != QStringLiteral("bilinear") && algorithm != QStringLiteral("metalfx-spatial")));
        workspace.Undo();
        CHECK(document.GetDraft(ROOT) == original);
        workspace.Redo();
        CHECK(GetUpscaleTestNode(document, algorithm) == node);
        REQUIRE(workspace.Save() == true);
        RenderGraphDocument reopened;
        REQUIRE(reopened.Load(package, ROOT) == RenderGraphDocumentStatus::Ok);
        CHECK(GetUpscaleTestNode(reopened, algorithm) == node);
        // Restore the original draft for the next preset without changing package shader files.
        workspace.Undo();
    }
}

TEST_CASE("upscaler creation and algorithm editing work with Properties hidden", "[editor][upscale][canvas]")
{
    QTemporaryDir project;
    const QString package = MakeUpscaleTestPackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT)) == true);
    workspace.SetLivePreview(false);
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    constexpr int32 WINDOW_WIDTH = 1200;
    constexpr int32 WINDOW_HEIGHT = 800;
    workspace.resize(WINDOW_WIDTH, WINDOW_HEIGHT);
    workspace.show();
    properties->hide();
    QApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    QTimer::singleShot(0, &canvas, []() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QMenu* createOrNull = menuOrNull->findChild<QMenu*>(QStringLiteral("renderGraphCreateNodeMenu"));
        REQUIRE(createOrNull != nullptr);
        for (QAction* action : createOrNull->actions())
        {
            if (action->text().contains(QStringLiteral("Bilinear")) == true)
            {
                menuOrNull->close();
                action->trigger();
                return;
            }
        }
        FAIL("Bilinear preset missing from the canvas menu");
    });
    const QPoint emptyPoint = canvas.viewport()->rect().bottomRight();
    QContextMenuEvent createContext(QContextMenuEvent::Mouse, emptyPoint, canvas.viewport()->mapToGlobal(emptyPoint));
    QApplication::sendEvent(canvas.viewport(), &createContext);
    REQUIRE(canvas.HasNode(QStringLiteral("bilinear")) == true);
    QTimer::singleShot(0, &canvas, [&canvas, properties]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* editOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphEditNodeAction"));
        REQUIRE(editOrNull != nullptr);
        QTimer::singleShot(0, &canvas, [properties]() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialogOrNull != nullptr);
            CHECK(properties->isVisible() == false);
            QComboBox* algorithmOrNull =
                dialogOrNull->findChild<QComboBox*>(QStringLiteral("renderGraphField_algorithm"));
            REQUIRE(algorithmOrNull != nullptr);
            algorithmOrNull->setCurrentText(QStringLiteral("xess-sr"));
            algorithmOrNull->activated(algorithmOrNull->currentIndex());
            dialogOrNull->reject();
        });
        menuOrNull->close();
        editOrNull->trigger();
    });
    constexpr int32 HEADER_X = 20;
    constexpr int32 HEADER_Y = 10;
    const QPoint point =
        canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("bilinear")) + QPointF(HEADER_X, HEADER_Y));
    QContextMenuEvent editContext(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QApplication::sendEvent(canvas.viewport(), &editContext);
    CHECK(GetUpscaleTestNode(workspace.GetDocument(), QStringLiteral("bilinear")).value(QStringLiteral("algorithm")) ==
          QStringLiteral("xess-sr"));
    CHECK(canvas.GetPortPosition(QStringLiteral("bilinear.motion"), false).isNull() == false);
    workspace.Undo();
    CHECK(canvas.GetPortPosition(QStringLiteral("bilinear.motion"), false).isNull() == true);
    workspace.Redo();
    REQUIRE(workspace.Save() == true);
}

TEST_CASE("changing a temporal upscaler preserves connected ports on rejection", "[editor][upscale]")
{
    QTemporaryDir project;
    const QString package = MakeUpscaleTestPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT) == RenderGraphDocumentStatus::Ok);
    REQUIRE(
        document.AddNode(ROOT, CONFIGURATION, MakeUpscaleNode(QStringLiteral("dlss-sr"), QStringLiteral("upscale"))) ==
        RenderGraphDocumentStatus::Ok);
    REQUIRE(document.Connect(ROOT, CONFIGURATION, QStringLiteral("depth.texture"), QStringLiteral("upscale.depth")) ==
            RenderGraphDocumentStatus::Ok);
    const QByteArray before = document.GetDraft(ROOT);
    QString error;
    CHECK(document.ReplaceNode(ROOT,
                               CONFIGURATION,
                               MakeUpscaleNode(QStringLiteral("fsr-1"), QStringLiteral("upscale")),
                               &error) == RenderGraphDocumentStatus::InvalidEdit);
    CHECK(document.GetDraft(ROOT) == before);
    CHECK(error.contains(QStringLiteral("Disconnect")) == true);
}

TEST_CASE("pasted upscalers upgrade old graphs and infer their output from destination storage", "[editor][upscale]")
{
    QTemporaryDir project;
    const QString package = MakeUpscaleTestPackage(project);
    RenderGraphDocument source;
    RenderGraphDocument destination;
    REQUIRE(source.Load(package, ROOT) == RenderGraphDocumentStatus::Ok);
    REQUIRE(destination.Load(package, ROOT) == RenderGraphDocumentStatus::Ok);
    const QByteArray original = destination.GetDraft(ROOT);
    REQUIRE(QJsonDocument::fromJson(original).object().value(QStringLiteral("version")).toInt() == 1);
    REQUIRE(source.AddNode(ROOT, CONFIGURATION, MakeUpscaleNode(QStringLiteral("fsr-1"), QStringLiteral("upscale"))) ==
            RenderGraphDocumentStatus::Ok);
    QJsonObject payload;
    QString error;
    REQUIRE(source.CopyNodes(ROOT,
                             CONFIGURATION,
                             {QStringLiteral("upscale")},
                             {{QStringLiteral("upscale"), 0, 0}},
                             payload,
                             error) == RenderGraphDocumentStatus::Ok);
    QStringList pasted;
    REQUIRE(destination.PasteNodes(ROOT, CONFIGURATION, payload, 0, 0, pasted, error) == RenderGraphDocumentStatus::Ok);
    REQUIRE(pasted == QStringList{QStringLiteral("upscale")});
    CHECK(QJsonDocument::fromJson(destination.GetDraft(ROOT)).object().value(QStringLiteral("version")).toInt() == 2);
    REQUIRE(destination.Connect(ROOT,
                                CONFIGURATION,
                                QStringLiteral("color.texture"),
                                QStringLiteral("upscale.destination")) == RenderGraphDocumentStatus::Ok);
    const RenderGraphResourceFeedback target =
        DescribeRenderGraphResource(destination, ROOT, CONFIGURATION, QStringLiteral("color.texture"), {});
    const RenderGraphResourceFeedback output =
        DescribeRenderGraphResource(destination, ROOT, CONFIGURATION, QStringLiteral("upscale.color"), {});
    CHECK(target.SizeLabel.isEmpty() == false);
    CHECK(output.SizeLabel == target.SizeLabel);
    CHECK(destination.Undo() == true);
    CHECK(destination.Undo() == true);
    CHECK(destination.GetDraft(ROOT) == original);
}

TEST_CASE("upscaling presets own output and render providers in one undo transaction", "[editor][upscale][sizes]")
{
    QTemporaryDir project;
    const QString package = MakeUpscaleTestPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT) == RenderGraphDocumentStatus::Ok);
    const QByteArray before = document.GetDraft(ROOT);
    for (const QString& algorithm : GetUpscaleAlgorithms())
    {
        CAPTURE(algorithm.toStdString());
        REQUIRE(document.AddUpscalePreset(ROOT, CONFIGURATION, algorithm, QStringLiteral("upscale")) ==
                RenderGraphDocumentStatus::Ok);
        CHECK(GetUpscaleTestNode(document, QStringLiteral("upscale-output-size")).value(QStringLiteral("kind")) ==
              QStringLiteral("OutputSize"));
        CHECK(GetUpscaleTestNode(document, QStringLiteral("upscale-render-size"))
                  .value(QStringLiteral("scale"))
                  .toArray() == QJsonArray{1, 2});
        const RenderGraphResourceFeedback color = DescribeRenderGraphResource(document,
                                                                              ROOT,
                                                                              CONFIGURATION,
                                                                              QStringLiteral("upscale-color.texture"),
                                                                              QSize(1921, 1081));
        const RenderGraphResourceFeedback destination =
            DescribeRenderGraphResource(document,
                                        ROOT,
                                        CONFIGURATION,
                                        QStringLiteral("upscale-destination.texture"),
                                        QSize(1921, 1081));
        CHECK(color.SizeLabel.contains(QStringLiteral("961 × 541")) == true);
        CHECK(destination.SizeLabel.contains(QStringLiteral("1921 × 1081")) == true);
        if (algorithm != QStringLiteral("bilinear") && algorithm != QStringLiteral("fsr-1") &&
            algorithm != QStringLiteral("metalfx-spatial"))
        {
            const RenderGraphResourceFeedback depth =
                DescribeRenderGraphResource(document,
                                            ROOT,
                                            CONFIGURATION,
                                            QStringLiteral("upscale-depth.texture"),
                                            QSize(1921, 1081));
            const RenderGraphResourceFeedback motion =
                DescribeRenderGraphResource(document,
                                            ROOT,
                                            CONFIGURATION,
                                            QStringLiteral("upscale-motion.texture"),
                                            QSize(1921, 1081));
            CHECK(depth.SizeLabel == color.SizeLabel);
            CHECK(motion.SizeLabel == color.SizeLabel);
        }
        REQUIRE(document.Undo() == true);
        CHECK(document.GetDraft(ROOT) == before);
    }
    REQUIRE(document.AddUpscalePreset(ROOT, CONFIGURATION, QStringLiteral("fsr-1"), QStringLiteral("upscale")) ==
            RenderGraphDocumentStatus::Ok);
    const QByteArray configured = document.GetDraft(ROOT);
    CHECK(document.AddUpscalePreset(ROOT, CONFIGURATION, QStringLiteral("fsr-1"), QStringLiteral("upscale")) !=
          RenderGraphDocumentStatus::Ok);
    CHECK(document.GetDraft(ROOT) == configured);
}

TEST_CASE("output resolution and render scale settings save from Properties or the canvas",
          "[editor][upscale][sizes][canvas]")
{
    bool shouldUseCanvas = false;
    SECTION("Properties") {}
    SECTION("canvas with Properties hidden")
    {
        shouldUseCanvas = true;
    }
    QTemporaryDir project;
    const QString package = MakeUpscaleTestPackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT)) == true);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.GetDocument().AddUpscalePreset(ROOT,
                                                     CONFIGURATION,
                                                     QStringLiteral("fsr-1"),
                                                     QStringLiteral("upscale")) == RenderGraphDocumentStatus::Ok);
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    constexpr int32 WINDOW_WIDTH = 1200;
    constexpr int32 WINDOW_HEIGHT = 800;
    workspace.resize(WINDOW_WIDTH, WINDOW_HEIGHT);
    workspace.show();
    properties->setVisible(shouldUseCanvas == false);
    for (const QString& identity : {QStringLiteral("upscale-output-size"), QStringLiteral("upscale-render-size")})
    {
        REQUIRE(workspace.SelectNode(ROOT, CONFIGURATION, identity) == true);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const auto exercise = [&identity](QWidget& inoutFields) {
            if (identity.endsWith(QStringLiteral("output-size")) == true)
            {
                QComboBox* modeOrNull = inoutFields.findChild<QComboBox*>(QStringLiteral("renderGraphExtentMode"));
                REQUIRE(modeOrNull != nullptr);
                modeOrNull->setCurrentIndex(0);
                modeOrNull->activated(0);
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                QSpinBox* widthOrNull = inoutFields.findChild<QSpinBox*>(QStringLiteral("renderGraphExtentWidth"));
                REQUIRE(widthOrNull != nullptr);
                constexpr int32 OUTPUT_WIDTH = 1919;
                widthOrNull->setValue(OUTPUT_WIDTH);
                widthOrNull->editingFinished();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                QSpinBox* heightOrNull = inoutFields.findChild<QSpinBox*>(QStringLiteral("renderGraphExtentHeight"));
                REQUIRE(heightOrNull != nullptr);
                constexpr int32 OUTPUT_HEIGHT = 1079;
                heightOrNull->setValue(OUTPUT_HEIGHT);
                heightOrNull->editingFinished();
            }
            else
            {
                QSpinBox* denominatorOrNull =
                    inoutFields.findChild<QSpinBox*>(QStringLiteral("renderGraphScaleDenominator"));
                REQUIRE(denominatorOrNull != nullptr);
                CHECK(denominatorOrNull->isHidden() == false);
                CHECK(denominatorOrNull->isEnabled() == true);
                constexpr int32 RENDER_DENOMINATOR = 3;
                denominatorOrNull->setValue(RENDER_DENOMINATOR);
                denominatorOrNull->editingFinished();
            }
        };
        if (shouldUseCanvas == false)
        {
            exercise(*properties);
        }
        else
        {
            QApplication::processEvents();
            RenderGraphCanvas& canvas = workspace.GetCanvas();
            canvas.Rebuild();
            canvas.RevealNode(identity);
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
            constexpr int32 HEADER_X = 20;
            constexpr int32 HEADER_Y = 10;
            const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(identity) + QPointF(HEADER_X, HEADER_Y));
            QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
            QApplication::sendEvent(canvas.viewport(), &context);
        }
    }
    const RenderGraphResourceFeedback color = DescribeRenderGraphResource(workspace.GetDocument(),
                                                                          ROOT,
                                                                          CONFIGURATION,
                                                                          QStringLiteral("upscale-color.texture"),
                                                                          {});
    const RenderGraphResourceFeedback destination =
        DescribeRenderGraphResource(workspace.GetDocument(),
                                    ROOT,
                                    CONFIGURATION,
                                    QStringLiteral("upscale-destination.texture"),
                                    {});
    CHECK(color.SizeLabel.contains(QStringLiteral("640 × 360")) == true);
    CHECK(destination.SizeLabel.contains(QStringLiteral("1919 × 1079")) == true);
    REQUIRE(workspace.Save() == true);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(ROOT) == workspace.GetDocument().GetDraft(ROOT));
}

TEST_CASE("sized preset admission is atomic at graph capacity", "[editor][upscale][sizes]")
{
    QJsonArray nodes;
    for (int32 index = 0; index < RenderGraphDocument::MAX_NODES; ++index)
    {
        nodes.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("node%1").arg(index)},
                                 {QStringLiteral("kind"), QStringLiteral("Scene")}});
    }
    QJsonObject configuration{{QStringLiteral("nodes"), nodes}, {QStringLiteral("edges"), QJsonArray{}}};
    const QJsonObject before = configuration;
    CHECK(AppendUpscalePreset(configuration, QStringLiteral("fsr-1"), QStringLiteral("upscale")) == false);
    CHECK(configuration == before);
}

TEST_CASE("MetalFX color interpretation edits work with Properties hidden", "[editor][upscale][canvas][metalfx]")
{
    QTemporaryDir project;
    const QString package = MakeUpscaleTestPackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT)) == true);
    workspace.SetLivePreview(false);
    QWidget* properties = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(properties != nullptr);
    constexpr int32 WINDOW_WIDTH = 1200;
    constexpr int32 WINDOW_HEIGHT = 800;
    workspace.resize(WINDOW_WIDTH, WINDOW_HEIGHT);
    workspace.show();
    properties->hide();
    QApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    QTimer::singleShot(0, &canvas, []() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QMenu* createOrNull = menuOrNull->findChild<QMenu*>(QStringLiteral("renderGraphCreateNodeMenu"));
        REQUIRE(createOrNull != nullptr);
        for (QAction* action : createOrNull->actions())
        {
            if (action->text().contains(QStringLiteral("MetalFX Spatial")) == true)
            {
                menuOrNull->close();
                action->trigger();
                return;
            }
        }
        FAIL("MetalFX Spatial preset missing from the canvas menu");
    });
    const QPoint emptyPoint = canvas.viewport()->rect().bottomRight();
    QContextMenuEvent createContext(QContextMenuEvent::Mouse, emptyPoint, canvas.viewport()->mapToGlobal(emptyPoint));
    QApplication::sendEvent(canvas.viewport(), &createContext);
    REQUIRE(canvas.HasNode(QStringLiteral("metalfx-spatial")) == true);
    QTimer::singleShot(0, &canvas, [&canvas, properties]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* editOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphEditNodeAction"));
        REQUIRE(editOrNull != nullptr);
        QTimer::singleShot(0, &canvas, [properties]() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialogOrNull != nullptr);
            CHECK(properties->isVisible() == false);
            QComboBox* colorProcessingOrNull =
                dialogOrNull->findChild<QComboBox*>(QStringLiteral("renderGraphField_color_processing"));
            REQUIRE(colorProcessingOrNull != nullptr);
            colorProcessingOrNull->setCurrentText(QStringLiteral("Perceptual"));
            colorProcessingOrNull->activated(colorProcessingOrNull->currentIndex());
            dialogOrNull->reject();
        });
        menuOrNull->close();
        editOrNull->trigger();
    });
    constexpr int32 HEADER_X = 20;
    constexpr int32 HEADER_Y = 10;
    const QPoint point =
        canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("metalfx-spatial")) + QPointF(HEADER_X, HEADER_Y));
    QContextMenuEvent editContext(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QApplication::sendEvent(canvas.viewport(), &editContext);
    CHECK(GetUpscaleTestNode(workspace.GetDocument(), QStringLiteral("metalfx-spatial"))
              .value(QStringLiteral("color_processing")) == QStringLiteral("Perceptual"));
    CHECK(GetUpscaleTestNode(workspace.GetDocument(), QStringLiteral("metalfx-spatial"))
              .value(QStringLiteral("algorithm")) == QStringLiteral("metalfx-spatial"));
    workspace.Undo();
    CHECK(GetUpscaleTestNode(workspace.GetDocument(), QStringLiteral("metalfx-spatial"))
              .value(QStringLiteral("color_processing")) == QStringLiteral("Linear"));
    workspace.Redo();
    REQUIRE(workspace.Save() == true);
    QJsonObject changed = GetUpscaleTestNode(workspace.GetDocument(), QStringLiteral("metalfx-spatial"));
    changed.insert(QStringLiteral("algorithm"), QStringLiteral("bilinear"));
    REQUIRE(workspace.GetDocument().ReplaceNode(ROOT, CONFIGURATION, changed) == RenderGraphDocumentStatus::Ok);
    CHECK(GetUpscaleTestNode(workspace.GetDocument(), QStringLiteral("metalfx-spatial"))
              .contains(QStringLiteral("color_processing")) == false);
    workspace.Undo();
    CHECK(GetUpscaleTestNode(workspace.GetDocument(), QStringLiteral("metalfx-spatial"))
              .value(QStringLiteral("color_processing")) == QStringLiteral("Perceptual"));
    REQUIRE(workspace.Save() == true);
}
