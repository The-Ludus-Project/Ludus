// Pipeline authoring regressions run without a Renderer device, including sanitizer builds.
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_configuration_editor.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_pipeline.h"
#include "internal/render_graph_pipeline_node.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");
const QString LIBRARY = QStringLiteral("rasterize.lurender.json");
const QString DEFAULT_CONFIGURATION = QStringLiteral("Default");
QString MakePackage(const QTemporaryDir& project)
{
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/offscreen"));
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    for (const QString& file : {ROOT_GRAPH, LIBRARY, QStringLiteral("main.lushader.json.in")})
    {
        QFile input(source.filePath(file));
        REQUIRE(input.open(QIODevice::ReadOnly) == true);
        QByteArray bytes = input.readAll();
        bytes.replace("@RG2_TEST_PROFILES@", "[\"metal\", \"wgsl\"]");
        REQUIRE(QDir().mkpath(package) == true);
        QFile output(QDir(package).filePath(file.endsWith(QStringLiteral(".in")) ? file.chopped(3) : file));
        REQUIRE(output.open(QIODevice::WriteOnly) == true);
        REQUIRE(output.write(bytes) == bytes.size());
    }
    return package;
}
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
} // namespace

TEST_CASE("pipeline nodes edit custom interfaces with connection-safe undo and save",
          "[editor][render-graph][pipeline]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    for (const QString& kind :
         {QStringLiteral("GraphicsPipeline"), QStringLiteral("ComputePipeline"), QStringLiteral("RayTracingPipeline")})
    {
        const QJsonObject node = MakePipelineNode(kind, QStringLiteral("work"));
        REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
        QJsonObject graph;
        REQUIRE(document.GetGraph(ROOT_GRAPH, graph) == true);
        CHECK(graph.value(QStringLiteral("version")).toInt() == 2);
        RenderGraphNodePorts ports;
        REQUIRE(GetRenderGraphNodePorts(document, node, ports) == true);
        CHECK(ports.Inputs.isEmpty() == true);
        CHECK(ports.Outputs.isEmpty() == true);
        REQUIRE(document.Undo() == true);
    }
    QJsonObject node = MakePipelineNode(QStringLiteral("ComputePipeline"), QStringLiteral("work"));
    node.insert(QStringLiteral("inputs"),
                QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("image")},
                                       {QStringLiteral("type"), QStringLiteral("ColorTexture")},
                                       {QStringLiteral("required"), true}}});
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.Connect(ROOT_GRAPH,
                             DEFAULT_CONFIGURATION,
                             QStringLiteral("color.texture"),
                             QStringLiteral("work.image")) == RenderGraphDocumentStatus::Ok);
    QJsonObject removed = node;
    removed.insert(QStringLiteral("inputs"), QJsonArray{});
    const uint64 revision = document.GetRevision();
    QString replacementError;
    CHECK(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, removed, &replacementError) ==
          RenderGraphDocumentStatus::InvalidEdit);
    CHECK(replacementError.contains(QStringLiteral("work.image")) == true);
    CHECK(replacementError.contains(QStringLiteral("Disconnect")) == true);
    CHECK(document.GetRevision() == revision);
    REQUIRE(document.Disconnect(ROOT_GRAPH,
                                DEFAULT_CONFIGURATION,
                                QStringLiteral("color.texture"),
                                QStringLiteral("work.image")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, removed) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.Undo() == true);
    RenderGraphCanvas canvas(document);
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    CHECK(canvas.GetPortPosition(QStringLiteral("work.image"), false).isNull() == false);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reloaded;
    REQUIRE(reloaded.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reloaded.GetDraft(ROOT_GRAPH) == document.GetDraft(ROOT_GRAPH));
}

TEST_CASE("legacy raster conversion retains shader state and resource ports", "[editor][render-graph][pipeline]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QJsonObject configuration = GetConfiguration(document, LIBRARY, QStringLiteral("ColorAndDepth"));
    const QJsonObject original = configuration.value(QStringLiteral("nodes")).toArray().at(0).toObject();
    const QJsonObject manifest =
        QJsonDocument::fromJson(document.GetDraft(QStringLiteral("main.lushader.json"))).object();
    QJsonObject node;
    REQUIRE(MigrateRasterNode(original, manifest, node) == true);
    CHECK(node.value(QStringLiteral("id")) == original.value(QStringLiteral("id")));
    REQUIRE(document.ReplaceNode(LIBRARY, QStringLiteral("ColorAndDepth"), node) == RenderGraphDocumentStatus::Ok);
    RenderGraphNodePorts before;
    RenderGraphNodePorts after;
    REQUIRE(GetRenderGraphNodePorts(document, original, before) == true);
    REQUIRE(GetRenderGraphNodePorts(document, node, after) == true);
    CHECK(before.Inputs.size() == after.Inputs.size());
    CHECK(before.Outputs.size() == after.Outputs.size());
    CHECK(node.value(QStringLiteral("shaders")).toArray().size() == 2);
    REQUIRE(document.Undo() == true);
    CHECK(GetConfiguration(document, LIBRARY, QStringLiteral("ColorAndDepth")) == configuration);

    QJsonObject ambiguous = manifest;
    QJsonArray techniques = ambiguous.value(QStringLiteral("techniques")).toArray();
    QJsonObject technique = techniques.at(0).toObject();
    QJsonArray recipes = technique.value(QStringLiteral("recipes")).toArray();
    recipes.append(recipes.at(0));
    technique.insert(QStringLiteral("recipes"), recipes);
    techniques.replace(0, technique);
    ambiguous.insert(QStringLiteral("techniques"), techniques);
    const QJsonObject previous = node;
    CHECK(MigrateRasterNode(original, ambiguous, node) == false);
    CHECK(node == previous);
}

TEST_CASE("configuration interface edits protect internal and containing graph links",
          "[editor][render-graph][interface]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString configuration = QStringLiteral("ColorAndDepth");
    const QJsonObject record = GetConfiguration(document, LIBRARY, configuration);
    const QJsonArray inputs = record.value(QStringLiteral("inputs")).toArray();
    const QJsonArray outputs = record.value(QStringLiteral("outputs")).toArray();
    const QByteArray original = document.GetDraft(LIBRARY);
    const uint64 revision = document.GetRevision();
    QString error;
    QJsonArray changedInputs = inputs;
    changedInputs.removeAt(0);
    CHECK(document.ReplaceConfigurationInterface(LIBRARY, configuration, changedInputs, outputs, error) ==
          RenderGraphDocumentStatus::InvalidEdit);
    CHECK(error.isEmpty() == false);
    QJsonArray changedOutputs = outputs;
    changedOutputs.removeAt(changedOutputs.size() - 1); // color is consumed by the root Export.
    CHECK(document.ReplaceConfigurationInterface(LIBRARY, configuration, inputs, changedOutputs, error) ==
          RenderGraphDocumentStatus::InvalidEdit);
    CHECK(error.contains(QStringLiteral("Disconnect")) == true);
    changedOutputs = outputs;
    QJsonObject output = changedOutputs.at(0).toObject();
    output.insert(QStringLiteral("source"), QStringLiteral("draw.color")); // depth cannot expose color.
    changedOutputs.replace(0, output);
    CHECK(document.ReplaceConfigurationInterface(LIBRARY, configuration, inputs, changedOutputs, error) ==
          RenderGraphDocumentStatus::InvalidEdit);
    changedInputs = inputs;
    changedInputs.append(inputs.at(0));
    CHECK(document.ReplaceConfigurationInterface(LIBRARY, configuration, changedInputs, outputs, error) ==
          RenderGraphDocumentStatus::InvalidEdit);
    CHECK(document.GetRevision() == revision);
    CHECK(document.GetDraft(LIBRARY) == original);

    // Disconnecting and reconnecting a configuration output changes its source, never edges.
    const QString endpoint = QStringLiteral("#outputs.color");
    REQUIRE(document.Disconnect(LIBRARY, configuration, QStringLiteral("draw.color"), endpoint) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(CheckRenderGraphLink(document, LIBRARY, configuration, QStringLiteral("draw.depth"), endpoint).Code ==
          QStringLiteral("TypeMismatch"));
    CHECK(CheckRenderGraphLink(document, LIBRARY, configuration, QStringLiteral("draw.color"), endpoint).IsLegal() ==
          true);
    REQUIRE(document.Connect(LIBRARY, configuration, QStringLiteral("draw.color"), endpoint) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(document.GetDraft(LIBRARY) == original);
    REQUIRE(document.RemoveNode(LIBRARY, configuration, QStringLiteral("draw")) == RenderGraphDocumentStatus::Ok);
    const QJsonArray disconnected =
        GetConfiguration(document, LIBRARY, configuration).value(QStringLiteral("outputs")).toArray();
    CHECK(disconnected.size() == outputs.size());
    for (const QJsonValueConstRef port : disconnected)
    {
        CHECK(port.toObject().value(QStringLiteral("source")).toString().isEmpty() == true);
    }
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(LIBRARY) == original);
}

TEST_CASE("configuration interfaces enforce graph version types and bounded unique names",
          "[editor][render-graph][interface]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.CreateSubgraph(ROOT_GRAPH,
                                    DEFAULT_CONFIGURATION,
                                    QStringLiteral("Custom"),
                                    QStringLiteral("Custom")) == RenderGraphDocumentStatus::Ok);
    const QString library = QStringLiteral("Custom.lurender.json");
    const QJsonObject input{{QStringLiteral("id"), QStringLiteral("resource")},
                            {QStringLiteral("type"), QStringLiteral("Buffer")}};
    const QJsonObject output{{QStringLiteral("id"), QStringLiteral("resource")},
                             {QStringLiteral("type"), QStringLiteral("Buffer")},
                             {QStringLiteral("source"), QStringLiteral("input.resource")},
                             {QStringLiteral("required"), false}};
    QString error;
    CHECK(document.ReplaceConfigurationInterface(library, DEFAULT_CONFIGURATION, {input}, {output}, error) ==
          RenderGraphDocumentStatus::InvalidEdit);
    // Adding a v2 pipeline upgrades the graph and permits its extended interface types.
    REQUIRE(document.AddNode(library,
                             DEFAULT_CONFIGURATION,
                             MakePipelineNode(QStringLiteral("RayTracingPipeline"), QStringLiteral("trace"))) ==
            RenderGraphDocumentStatus::Ok);
    REQUIRE(document.ReplaceConfigurationInterface(library, DEFAULT_CONFIGURATION, {input}, {output}, error) ==
            RenderGraphDocumentStatus::Ok);
    RenderGraphConfigurationEditor editor(document,
                                          library,
                                          GetConfiguration(document, library, DEFAULT_CONFIGURATION));
    CHECK(editor.GetInputs() == QJsonArray{input});
    CHECK(editor.GetOutputs() == QJsonArray{output});
    QTableWidget* inputTable = editor.findChild<QTableWidget*>(QStringLiteral("configurationInputs"));
    REQUIRE(inputTable != nullptr);
    inputTable->item(0, 0)->setText(QStringLiteral("renamed"));
    CHECK(editor.GetOutputs() == QJsonArray{output}); // Filtering must retain the source until explicitly changed.
    inputTable->item(0, 0)->setText(QStringLiteral("resource"));
    const QByteArray previous = document.GetDraft(library);
    const uint64 revision = document.GetRevision();
    QJsonArray tooMany;
    for (int32 i = 0; i <= MAX_CONFIGURATION_PORTS; ++i)
    {
        QJsonObject port = input;
        port.insert(QStringLiteral("id"), QStringLiteral("port") + QString::number(i));
        tooMany.append(port);
    }
    CHECK(document.ReplaceConfigurationInterface(library, DEFAULT_CONFIGURATION, tooMany, {}, error) ==
          RenderGraphDocumentStatus::InvalidEdit);
    QJsonObject invalid = input;
    invalid.insert(QStringLiteral("id"), QStringLiteral("name\n"));
    CHECK(document.ReplaceConfigurationInterface(library, DEFAULT_CONFIGURATION, {invalid}, {}, error) ==
          RenderGraphDocumentStatus::InvalidEdit);
    CHECK(document.GetRevision() == revision);
    CHECK(document.GetDraft(library) == previous);
}

TEST_CASE("writable resources create paired ports and automatic target associations",
          "[editor][render-graph][pipeline]")
{
    for (const QString& kind :
         {QStringLiteral("GraphicsPipeline"), QStringLiteral("ComputePipeline"), QStringLiteral("RayTracingPipeline")})
    {
        RenderGraphPipelineEditor editor(MakePipelineNode(kind, QStringLiteral("work")));
        QJsonObject applied;
        QObject::connect(&editor, &RenderGraphPipelineEditor::Applied, &editor, [&applied](const QJsonObject& node) {
            applied = node;
        });
        QPushButton* add = editor.findChild<QPushButton*>(QStringLiteral("renderGraphAdd_outputs"));
        QPushButton* apply = editor.findChild<QPushButton*>(QStringLiteral("renderGraphApplyPipeline"));
        QTableWidget* resources = editor.findChild<QTableWidget*>(QStringLiteral("outputs"));
        REQUIRE(add != nullptr);
        REQUIRE(apply != nullptr);
        REQUIRE(resources != nullptr);
        CHECK(resources->property("fields").toStringList().contains(QStringLiteral("target")) == false);
        add->click();
        resources->item(0, 0)->setText(QStringLiteral("color"));
        apply->click();
        REQUIRE(applied.isEmpty() == false);
        const QJsonObject input = applied.value(QStringLiteral("inputs")).toArray().at(0).toObject();
        const QJsonObject output = applied.value(QStringLiteral("outputs")).toArray().at(0).toObject();
        CHECK(input.value(QStringLiteral("id")).toString() == QStringLiteral("color"));
        CHECK(output.value(QStringLiteral("target")) == input.value(QStringLiteral("id")));
        CHECK(output.value(QStringLiteral("type")) == input.value(QStringLiteral("type")));
        QTemporaryDir project;
        RenderGraphDocument document;
        REQUIRE(document.Load(MakePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
        REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, MakePipelineNode(kind, QStringLiteral("work"))) ==
                RenderGraphDocumentStatus::Ok);
        REQUIRE(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, applied) == RenderGraphDocumentStatus::Ok);
        REQUIRE(document.Undo() == true);
        REQUIRE(document.Redo() == true);
        QStringList saved;
        REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
        RenderGraphDocument reloaded;
        REQUIRE(reloaded.Load(QDir(project.path()).filePath(QStringLiteral("graphs")), ROOT_GRAPH) ==
                RenderGraphDocumentStatus::Ok);
        CHECK(reloaded.GetDraft(ROOT_GRAPH) == document.GetDraft(ROOT_GRAPH));
        QComboBox* type = qobject_cast<QComboBox*>(resources->cellWidget(0, 1));
        REQUIRE(type != nullptr);
        type->setCurrentText(QStringLiteral("DepthTexture"));
        apply->click();
        CHECK(applied.value(QStringLiteral("outputs")).toArray().at(0).toObject().value(QStringLiteral("format")) ==
              QStringLiteral("Depth32Float"));
        CHECK(applied.value(QStringLiteral("inputs")).toArray().at(0).toObject().value(QStringLiteral("type")) ==
              QStringLiteral("DepthTexture"));
        QPushButton* remove = editor.findChild<QPushButton*>(QStringLiteral("renderGraphRemove_outputs"));
        REQUIRE(remove != nullptr);
        remove->click();
        apply->click();
        CHECK(applied.value(QStringLiteral("inputs")).toArray().isEmpty() == true);
        CHECK(applied.value(QStringLiteral("outputs")).toArray().isEmpty() == true);
    }
}

TEST_CASE("paired resources preserve legacy endpoint names and align on the canvas", "[editor][render-graph][pipeline]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString configuration = QStringLiteral("ColorAndDepth");
    const QJsonObject original =
        GetConfiguration(document, LIBRARY, configuration).value(QStringLiteral("nodes")).toArray().at(0).toObject();
    QJsonObject node;
    const QJsonObject manifest =
        QJsonDocument::fromJson(document.GetDraft(QStringLiteral("main.lushader.json"))).object();
    REQUIRE(MigrateRasterNode(original, manifest, node) == true);
    QJsonArray outputs = node.value(QStringLiteral("outputs")).toArray();
    QJsonObject output = outputs.at(0).toObject();
    output.insert(QStringLiteral("id"), QStringLiteral("depthResult"));
    outputs.replace(0, output);
    node.insert(QStringLiteral("outputs"), outputs);
    RenderGraphPipelineEditor editor(node);
    QJsonObject applied;
    QObject::connect(&editor, &RenderGraphPipelineEditor::Applied, &editor, [&applied](const QJsonObject& candidate) {
        applied = candidate;
    });
    QPushButton* apply = editor.findChild<QPushButton*>(QStringLiteral("renderGraphApplyPipeline"));
    REQUIRE(apply != nullptr);
    apply->click();
    CHECK(applied == node);
    QTableWidget* resources = editor.findChild<QTableWidget*>(QStringLiteral("outputs"));
    REQUIRE(resources != nullptr);
    CHECK(resources->item(0, 0)->toolTip().contains(QStringLiteral("Input port: depth")) == true);
    REQUIRE(document.RemoveNode(LIBRARY, configuration, QStringLiteral("draw")) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.AddNode(LIBRARY, configuration, applied) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    canvas.ShowConfiguration(LIBRARY, configuration);
    CHECK(canvas.GetPortPosition(QStringLiteral("draw.depth"), false).y() ==
          canvas.GetPortPosition(QStringLiteral("draw.depthResult"), true).y());
    CHECK(canvas.GetPortPosition(QStringLiteral("draw.color"), false).y() ==
          canvas.GetPortPosition(QStringLiteral("draw.color"), true).y());
}

TEST_CASE("paired resource validation identifies duplicate ports and preserves drafts",
          "[editor][render-graph][pipeline]")
{
    RenderGraphPipelineEditor editor(MakePipelineNode(QStringLiteral("ComputePipeline"), QStringLiteral("work")));
    QJsonObject applied;
    QObject::connect(&editor, &RenderGraphPipelineEditor::Applied, &editor, [&applied](const QJsonObject& node) {
        applied = node;
    });
    QPushButton* add = editor.findChild<QPushButton*>(QStringLiteral("renderGraphAdd_outputs"));
    QPushButton* apply = editor.findChild<QPushButton*>(QStringLiteral("renderGraphApplyPipeline"));
    QTableWidget* resources = editor.findChild<QTableWidget*>(QStringLiteral("outputs"));
    QLabel* error = editor.findChild<QLabel*>(QStringLiteral("renderGraphPipelineError"));
    REQUIRE(add != nullptr);
    REQUIRE(apply != nullptr);
    REQUIRE(resources != nullptr);
    REQUIRE(error != nullptr);
    add->click();
    add->click();
    resources->item(1, 0)->setText(QStringLiteral("resource"));
    apply->click();
    CHECK(applied.isEmpty() == true);
    CHECK(error->text().contains(QStringLiteral("resource")) == true);
    CHECK(error->text().contains(QStringLiteral("unique name")) == true);
    resources->item(1, 0)->setText(QStringLiteral("invalid name"));
    apply->click();
    CHECK(applied.isEmpty() == true);
    CHECK(error->text().contains(QStringLiteral("starting with a letter")) == true);
    resources->item(1, 0)->setText(QString());
    apply->click();
    CHECK(applied.isEmpty() == true);
    resources->item(1, 0)->setText(QStringLiteral("other"));
    apply->click();
    CHECK(applied.isEmpty() == false);
    CHECK(error->text().isEmpty() == true);
}

TEST_CASE("same-name writable ports repair a stale target only when applied", "[editor][render-graph][pipeline]")
{
    QJsonObject node = MakePipelineNode(QStringLiteral("RayTracingPipeline"), QStringLiteral("trace"));
    node.insert(QStringLiteral("inputs"),
                QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("color")},
                                       {QStringLiteral("type"), QStringLiteral("ColorTexture")},
                                       {QStringLiteral("required"), true}}});
    node.insert(QStringLiteral("outputs"),
                QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("color")},
                                       {QStringLiteral("type"), QStringLiteral("ColorTexture")},
                                       {QStringLiteral("target"), QStringLiteral("input")},
                                       {QStringLiteral("format"), QStringLiteral("RGBA8Unorm")},
                                       {QStringLiteral("load"), QStringLiteral("Clear")},
                                       {QStringLiteral("store"), QStringLiteral("Store")}}});
    const QJsonObject original = node;
    RenderGraphPipelineEditor editor(node);
    QJsonObject applied;
    QObject::connect(&editor, &RenderGraphPipelineEditor::Applied, &editor, [&applied](const QJsonObject& candidate) {
        applied = candidate;
    });
    CHECK(node == original);
    CHECK(applied.isEmpty() == true);
    QPushButton* apply = editor.findChild<QPushButton*>(QStringLiteral("renderGraphApplyPipeline"));
    REQUIRE(apply != nullptr);
    apply->click();
    CHECK(applied.value(QStringLiteral("inputs")) == node.value(QStringLiteral("inputs")));
    CHECK(applied.value(QStringLiteral("outputs")).toArray().at(0).toObject().value(QStringLiteral("target")) ==
          QStringLiteral("color"));
}
