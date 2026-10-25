#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_options.h"
#include "internal/render_graph_pipeline_node.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");
const QString LIBRARY = QStringLiteral("rasterize.lurender.json");
const QString DEFAULT = QStringLiteral("Default");
const QString COLOR_AND_DEPTH = QStringLiteral("ColorAndDepth");
const QString WORK = QStringLiteral("work");
const QString CULL = QStringLiteral("Cull");
constexpr int32 DETAILS_WIDTH = 1200;
constexpr int32 DETAILS_HEIGHT = 800;
constexpr int32 STATE_PART = 3;
constexpr int32 OPTIONS_PART = 5;
QJsonObject MakeOption(const QString& target)
{
    return {
        {QStringLiteral("id"), CULL},
        {QStringLiteral("label"), QStringLiteral("Face culling")},
        {QStringLiteral("type"), QStringLiteral("Enum")},
        {QStringLiteral("default"), QStringLiteral("None")},
        {QStringLiteral("values"), QJsonArray{QStringLiteral("None"), QStringLiteral("Back"), QStringLiteral("Front")}},
        {QStringLiteral("target"), target}};
}
QString MakePackage(const QTemporaryDir& project)
{
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/offscreen"));
    for (const QString& file : {ROOT_GRAPH,
                                LIBRARY,
                                QStringLiteral("main.lushader.json.in"),
                                QStringLiteral("main_shaders/geometry.slang"),
                                QStringLiteral("main_shaders/color.slang"),
                                QStringLiteral("main_shaders/depth.slang")})
    {
        QFile input(source.filePath(file));
        REQUIRE(input.open(QIODevice::ReadOnly) == true);
        QByteArray bytes = input.readAll();
        bytes.replace("@RG2_TEST_PROFILES@", "[\"metal\"]");
        const QString destination =
            QDir(package).filePath(file.endsWith(QStringLiteral(".in")) == true ? file.chopped(3) : file);
        REQUIRE(QDir().mkpath(QFileInfo(destination).absolutePath()) == true);
        QFile output(destination);
        REQUIRE(output.open(QIODevice::WriteOnly) == true);
        REQUIRE(output.write(bytes) == bytes.size());
    }
    return package;
}
QJsonObject
GetNode(const RenderGraphDocument& document, const QString& file, const QString& configuration, const QString& identity)
{
    QJsonObject graph;
    REQUIRE(document.GetGraph(file, graph) == true);
    for (const QJsonValueConstRef entry : graph.value(QStringLiteral("configurations")).toArray())
    {
        if (entry.toObject().value(QStringLiteral("id")) == configuration)
        {
            for (const QJsonValueConstRef node : entry.toObject().value(QStringLiteral("nodes")).toArray())
            {
                if (node.toObject().value(QStringLiteral("id")) == identity)
                {
                    return node.toObject();
                }
            }
        }
    }
    return {};
}
void AddOptions(RenderGraphDocument& inoutDocument, const QString& file, const QString& configuration)
{
    REQUIRE(inoutDocument.AddNode(file, configuration, MakePipelineNode(QStringLiteral("GraphicsPipeline"), WORK)) ==
            RenderGraphDocumentStatus::Ok);
    QJsonObject node = GetNode(inoutDocument, file, configuration, WORK);
    node.insert(QStringLiteral("options"), QJsonArray{MakeOption(QStringLiteral("state.cull"))});
    node.insert(QStringLiteral("shaders"),
                QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("vertex")},
                                       {QStringLiteral("stage"), QStringLiteral("vertex")},
                                       {QStringLiteral("program"), QStringLiteral("Geometry")}}});
    QString error;
    REQUIRE(inoutDocument.ReplaceNode(file, configuration, node, &error) == RenderGraphDocumentStatus::Ok);
}
} // namespace
TEST_CASE("pipeline details expose clickable parts and per-use choices with Properties hidden",
          "[editor][render-graph][options]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    QWidget inspectorHost;
    RendererWorkspace workspace({});
    workspace.resize(DETAILS_WIDTH, DETAILS_HEIGHT);
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    AddOptions(workspace.GetDocument(), ROOT_GRAPH, DEFAULT);
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT, WORK) == true);
    workspace.TakeInspectorPanel(&inspectorHost)->hide();
    workspace.show();
    const QByteArray before = workspace.GetDocument().GetDraft(ROOT_GRAPH);
    const QByteArray saved = workspace.GetDocument().GetSaved(ROOT_GRAPH);
    QCoreApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const QPointF position = canvas.GetNodePosition(WORK);
    canvas.centerOn(position);
    bool hasParts = false;
    bool hasState = false;
    bool hasShader = false;
    bool hasOptions = false;
    QTimer::singleShot(0, &workspace, [&]() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialogOrNull == nullptr)
        {
            return;
        }
        QTabWidget* tabsOrNull = dialogOrNull->findChild<QTabWidget*>(QStringLiteral("renderGraphPipelineParts"));
        QToolButton* stateOrNull = dialogOrNull->findChild<QToolButton*>(QStringLiteral("renderGraphPart_3"));
        QToolButton* optionsOrNull = dialogOrNull->findChild<QToolButton*>(QStringLiteral("renderGraphPart_5"));
        hasParts = tabsOrNull != nullptr && stateOrNull != nullptr && optionsOrNull != nullptr;
        if (hasParts == true)
        {
            QToolButton* shaderOrNull =
                dialogOrNull->findChild<QToolButton*>(QStringLiteral("renderGraphShaderPart_vertex"));
            if (shaderOrNull != nullptr)
            {
                shaderOrNull->click();
                QTableWidget* shadersOrNull = dialogOrNull->findChild<QTableWidget*>(QStringLiteral("shaders"));
                hasShader =
                    shadersOrNull != nullptr && shadersOrNull->currentRow() == 0 && tabsOrNull->currentIndex() == 2;
            }
            stateOrNull->click();
            hasState = tabsOrNull->currentIndex() == STATE_PART;
            const QString capture = qEnvironmentVariable("LUDUS_NODE_DETAILS_CAPTURE");
            if (capture.isEmpty() == false)
            {
                static_cast<void>(dialogOrNull->grab().save(capture));
            }
            optionsOrNull->click();
            hasOptions = tabsOrNull->currentIndex() == OPTIONS_PART;
            QComboBox* choiceOrNull = dialogOrNull->findChild<QComboBox*>(QStringLiteral("renderGraphOption_Cull"));
            if (choiceOrNull != nullptr)
            {
                choiceOrNull->setCurrentIndex(2);
                Q_EMIT choiceOrNull->activated(2);
            }
        }
        dialogOrNull->accept();
    });
    QTest::mouseDClick(canvas.viewport(),
                       Qt::LeftButton,
                       Qt::NoModifier,
                       canvas.mapFromScene(position + QPointF(30, 8)));
    CHECK(hasParts == true);
    CHECK(hasState == true);
    CHECK(hasShader == true);
    CHECK(hasOptions == true);
    CHECK(GetNode(workspace.GetDocument(), ROOT_GRAPH, DEFAULT, WORK)
              .value(QStringLiteral("overrides"))
              .toObject()
              .value(CULL) == QStringLiteral("Back"));
    CHECK(workspace.GetDocument().GetSaved(ROOT_GRAPH) == saved);
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) == before);
    workspace.Redo();
    REQUIRE(workspace.Save() == true);
    CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) == workspace.GetDocument().GetSaved(ROOT_GRAPH));
}
TEST_CASE("exposed option declarations protect existing overrides and rename bindings atomically",
          "[editor][render-graph][options]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    AddOptions(document, LIBRARY, COLOR_AND_DEPTH);
    QString error;
    REQUIRE(document.SetConfigurationOptions(LIBRARY,
                                             COLOR_AND_DEPTH,
                                             QJsonArray{MakeOption(QStringLiteral("work.Cull"))},
                                             error) == RenderGraphDocumentStatus::Ok);
    QJsonObject reference = GetNode(document, ROOT_GRAPH, DEFAULT, QStringLiteral("raster"));
    reference.insert(QStringLiteral("overrides"), QJsonObject{{CULL, QStringLiteral("Front")}});
    REQUIRE(document.ReplaceNode(ROOT_GRAPH, DEFAULT, reference, &error) == RenderGraphDocumentStatus::Ok);
    const QByteArray before = document.GetDraft(LIBRARY);
    const uint64 revision = document.GetRevision();
    CHECK(document.SetConfigurationOptions(LIBRARY, COLOR_AND_DEPTH, {}, error) ==
          RenderGraphDocumentStatus::InvalidEdit);
    CHECK(document.GetDraft(LIBRARY) == before);
    CHECK(document.GetRevision() == revision);
    CHECK(error.isEmpty() == false);
    REQUIRE(document.RenameNode(LIBRARY, COLOR_AND_DEPTH, WORK, QStringLiteral("renamed"), error) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(document.GetDraft(LIBRARY).contains("renamed.Cull") == true);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(LIBRARY) == before);
    reference.insert(QStringLiteral("overrides"), QJsonObject{{CULL, QStringLiteral("Hidden")}});
    CHECK(document.ReplaceNode(ROOT_GRAPH, DEFAULT, reference, &error) == RenderGraphDocumentStatus::InvalidEdit);
}
TEST_CASE("finite option forms preserve typed values and reset by removing overrides",
          "[editor][render-graph][options]")
{
    RenderGraphOptionsEditor editor(QJsonArray{MakeOption(QStringLiteral("state.cull"))},
                                    QJsonObject{{CULL, QStringLiteral("Back")}},
                                    {},
                                    false);
    QSignalSpy edits(&editor, &RenderGraphOptionsEditor::OverridesEdited);
    QComboBox* choiceOrNull = editor.findChild<QComboBox*>(QStringLiteral("renderGraphOption_Cull"));
    REQUIRE(choiceOrNull != nullptr);
    choiceOrNull->setCurrentIndex(0);
    Q_EMIT choiceOrNull->activated(0);
    REQUIRE(edits.size() == 1);
    CHECK(edits.first().first().toJsonObject().isEmpty() == true);
    QString error;
    CHECK(ValidateRenderGraphOptions(QJsonArray{MakeOption(QStringLiteral("state.cull"))},
                                     {{CULL, QStringLiteral("Hidden")}},
                                     error) == false);
}

TEST_CASE("subgraph details change only the caller and shader alternatives survive cleanup",
          "[editor][render-graph][options]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    QWidget inspectorHost;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    AddOptions(workspace.GetDocument(), LIBRARY, COLOR_AND_DEPTH);
    QString error;
    REQUIRE(workspace.GetDocument().SetConfigurationOptions(LIBRARY,
                                                            COLOR_AND_DEPTH,
                                                            QJsonArray{MakeOption(QStringLiteral("work.Cull"))},
                                                            error) == RenderGraphDocumentStatus::Ok);
    const QByteArray library = workspace.GetDocument().GetDraft(LIBRARY);
    const QByteArray root = workspace.GetDocument().GetDraft(ROOT_GRAPH);
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT, QStringLiteral("raster")) == true);
    workspace.TakeInspectorPanel(&inspectorHost)->hide();
    bool hasChoice = false;
    QTimer::singleShot(0, &workspace, [&]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menuOrNull == nullptr)
        {
            return;
        }
        QAction* detailsOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphEditNodeAction"));
        menuOrNull->close();
        if (detailsOrNull == nullptr)
        {
            return;
        }
        QTimer::singleShot(0, &workspace, [&]() {
            QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialogOrNull == nullptr)
            {
                return;
            }
            QComboBox* choiceOrNull = dialogOrNull->findChild<QComboBox*>(QStringLiteral("renderGraphOption_Cull"));
            hasChoice = choiceOrNull != nullptr;
            if (choiceOrNull != nullptr)
            {
                choiceOrNull->setCurrentText(QStringLiteral("Front"));
                Q_EMIT choiceOrNull->activated(choiceOrNull->currentIndex());
            }
            dialogOrNull->accept();
        });
        detailsOrNull->trigger();
    });
    Q_EMIT workspace.GetCanvas().ContextMenuRequested(QStringLiteral("raster"), {});
    CHECK(hasChoice == true);
    CHECK(workspace.GetDocument().GetDraft(LIBRARY) == library);
    CHECK(GetNode(workspace.GetDocument(), ROOT_GRAPH, DEFAULT, QStringLiteral("raster"))
              .value(QStringLiteral("overrides"))
              .toObject()
              .value(CULL) == QStringLiteral("Front"));
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) == root);

    // An alternative need not be the current assignment or a technique root to survive cleanup.
    RenderGraphDocument& document = workspace.GetDocument();
    const QString manifestFile = QStringLiteral("main.lushader.json");
    QJsonObject manifest = QJsonDocument::fromJson(document.GetDraft(manifestFile)).object();
    QJsonArray programs = manifest.value(QStringLiteral("programs")).toArray();
    QJsonObject alternative = programs.first().toObject();
    alternative.insert(QStringLiteral("id"), QStringLiteral("Alternative"));
    programs.append(alternative);
    manifest.insert(QStringLiteral("programs"), programs);
    REQUIRE(document.SetText(manifestFile, EncodeCanonicalJson(manifest)) == RenderGraphDocumentStatus::Ok);
    QJsonObject pipeline = GetNode(document, LIBRARY, COLOR_AND_DEPTH, WORK);
    pipeline.insert(QStringLiteral("shaders"),
                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("vertex")},
                                           {QStringLiteral("stage"), QStringLiteral("vertex")},
                                           {QStringLiteral("program"), QStringLiteral("Geometry")}}});
    QJsonArray declarations = pipeline.value(QStringLiteral("options")).toArray();
    QJsonObject option = MakeOption(QStringLiteral("shaders.vertex.program"));
    option.insert(QStringLiteral("id"), QStringLiteral("Program"));
    option.insert(QStringLiteral("values"), QJsonArray{QStringLiteral("Geometry"), QStringLiteral("Alternative")});
    option.insert(QStringLiteral("default"), QStringLiteral("Geometry"));
    declarations.append(option);
    pipeline.insert(QStringLiteral("options"), declarations);
    REQUIRE(document.ReplaceNode(LIBRARY, COLOR_AND_DEPTH, pipeline, &error) == RenderGraphDocumentStatus::Ok);
    QStringList removedPrograms;
    QStringList removedSources;
    REQUIRE(document.RemoveUnusedShaders(removedPrograms, removedSources, error) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetDraft(manifestFile).contains("Alternative") == true);
}
