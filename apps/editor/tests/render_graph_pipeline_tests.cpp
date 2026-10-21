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
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
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
        bytes.replace("@RG2_TEST_PROFILES@", "[\"metal\", \"wgsl\"]");
        REQUIRE(QDir().mkpath(QFileInfo(QDir(package).filePath(file)).absolutePath()) == true);
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

TEST_CASE("pipeline shader creation commits source manifest and references together",
          "[editor][render-graph][pipeline][shader-creation]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject node = MakePipelineNode(QStringLiteral("RayTracingPipeline"), QStringLiteral("rays"));
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    const QByteArray originalGraph = document.GetDraft(ROOT_GRAPH);
    const QString manifestFile = QStringLiteral("main.lushader.json");
    const QByteArray originalManifest = document.GetDraft(manifestFile);
    const QStringList originalFiles = document.GetFiles();
    const QJsonArray shaders{QJsonObject{{QStringLiteral("id"), QStringLiteral("raygen")},
                                         {QStringLiteral("stage"), QStringLiteral("ray_generation")},
                                         {QStringLiteral("program"), QStringLiteral("Program")}},
                             QJsonObject{{QStringLiteral("id"), QStringLiteral("closestHit")},
                                         {QStringLiteral("stage"), QStringLiteral("closest_hit")},
                                         {QStringLiteral("program"), QStringLiteral("Program")}}};
    node.insert(QStringLiteral("shaders"), shaders);
    const uint64 revision = document.GetRevision();
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetRevision() == revision + 1);
    const QByteArray createdGraph = document.GetDraft(ROOT_GRAPH);
    const QByteArray createdManifest = document.GetDraft(manifestFile);
    const QStringList createdFiles = document.GetFiles();
    const QJsonObject configuration = GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION);
    const QJsonArray assigned = configuration.value(QStringLiteral("nodes"))
                                    .toArray()
                                    .last()
                                    .toObject()
                                    .value(QStringLiteral("shaders"))
                                    .toArray();
    const QJsonArray programs =
        QJsonDocument::fromJson(createdManifest).object().value(QStringLiteral("programs")).toArray();
    REQUIRE(assigned.size() == 2);
    REQUIRE(programs.size() == 5);
    for (qsizetype i = 0; i < assigned.size(); ++i)
    {
        const QJsonObject shader = assigned.at(i).toObject();
        const QJsonObject program = programs.at(programs.size() - assigned.size() + i).toObject();
        CHECK(shader.value(QStringLiteral("program")) == program.value(QStringLiteral("id")));
        CHECK(shader.value(QStringLiteral("stage")) == program.value(QStringLiteral("stage")));
        const QString source = program.value(QStringLiteral("source")).toString();
        CHECK(document.GetDraft(source).contains("ShaderMain") == true);
        CHECK(QFileInfo::exists(QDir(package).filePath(source)) == false);
    }
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(ROOT_GRAPH) == originalGraph);
    CHECK(document.GetDraft(manifestFile) == originalManifest);
    CHECK(document.GetFiles() == originalFiles);
    REQUIRE(document.Redo() == true);
    CHECK(document.GetDraft(ROOT_GRAPH) == createdGraph);
    CHECK(document.GetDraft(manifestFile) == createdManifest);
    CHECK(document.GetFiles() == createdFiles);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reloaded;
    REQUIRE(reloaded.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    for (const QString& file : createdFiles)
    {
        CHECK(reloaded.GetDraft(file) == document.GetDraft(file));
    }
    // Undo after publication removes both new files on the next explicit Save.
    REQUIRE(document.Undo() == true);
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    for (const QString& file : createdFiles)
    {
        if (originalFiles.contains(file) == false)
        {
            CHECK(QFileInfo::exists(QDir(package).filePath(file)) == false);
        }
    }
}

TEST_CASE("pipeline shader creation rejects conflicts without partial drafts",
          "[editor][render-graph][pipeline][shader-creation]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject node = MakePipelineNode(QStringLiteral("ComputePipeline"), QStringLiteral("work"));
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    QJsonArray shaders{QJsonObject{{QStringLiteral("id"), QStringLiteral("kernel")},
                                   {QStringLiteral("stage"), QStringLiteral("compute")},
                                   {QStringLiteral("program"), QStringLiteral("Kernel")}}};
    RenderGraphDocumentStatus expected = RenderGraphDocumentStatus::InvalidEdit;
    SECTION("case insensitive filename conflict")
    {
        QFile file(QDir(package).filePath(QStringLiteral("shader_kernel.slang")));
        REQUIRE(file.open(QIODevice::WriteOnly) == true);
        REQUIRE(file.write("// user source\n") > 0);
        file.close();
        // Keep a file in the draft to exercise portable case-insensitive conflict detection.
        RenderGraphDocument loaded;
        REQUIRE(loaded.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
        document = loaded;
        REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
        expected = RenderGraphDocumentStatus::Conflict;
    }
    SECTION("manifest program budget")
    {
        const QString manifestFile = QStringLiteral("main.lushader.json");
        QJsonObject manifest = QJsonDocument::fromJson(document.GetDraft(manifestFile)).object();
        QJsonObject budgets = manifest.value(QStringLiteral("budgets")).toObject();
        budgets.insert(QStringLiteral("programs"), manifest.value(QStringLiteral("programs")).toArray().size());
        manifest.insert(QStringLiteral("budgets"), budgets);
        REQUIRE(document.SetText(manifestFile, EncodeCanonicalJson(manifest)) == RenderGraphDocumentStatus::Ok);
        expected = RenderGraphDocumentStatus::Capacity;
    }
    SECTION("file appears on disk after load")
    {
        QFile file(QDir(package).filePath(QStringLiteral("shader_Kernel.slang")));
        REQUIRE(file.open(QIODevice::WriteOnly) == true);
        REQUIRE(file.write("// external source\n") > 0);
        expected = RenderGraphDocumentStatus::Conflict;
    }
    SECTION("malformed manifest")
    {
        REQUIRE(document.SetText(QStringLiteral("main.lushader.json"), QByteArray("{broken")) ==
                RenderGraphDocumentStatus::Ok);
        expected = RenderGraphDocumentStatus::Malformed;
    }
    SECTION("wrong existing program stage")
    {
        QJsonObject shader = shaders.at(0).toObject();
        shader.insert(QStringLiteral("program"), QStringLiteral("Geometry"));
        shaders.replace(0, shader);
    }
    SECTION("invalid name after a valid new source")
    {
        QJsonObject invalid = shaders.at(0).toObject();
        invalid.insert(QStringLiteral("id"), QStringLiteral("invalid"));
        invalid.insert(QStringLiteral("program"), QStringLiteral("../Escape"));
        shaders.append(invalid);
    }
    SECTION("duplicate shader identity")
    {
        shaders.append(shaders.at(0));
    }
    SECTION("connected port removed after preparing new sources")
    {
        node.insert(QStringLiteral("inputs"),
                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("image")},
                                           {QStringLiteral("type"), QStringLiteral("ColorTexture")},
                                           {QStringLiteral("required"), true}}});
        REQUIRE(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
        REQUIRE(document.Connect(ROOT_GRAPH,
                                 DEFAULT_CONFIGURATION,
                                 QStringLiteral("color.texture"),
                                 QStringLiteral("work.image")) == RenderGraphDocumentStatus::Ok);
        node.insert(QStringLiteral("inputs"), QJsonArray{});
    }
    node.insert(QStringLiteral("shaders"), shaders);
    const QByteArray graph = document.GetDraft(ROOT_GRAPH);
    const QByteArray manifest = document.GetDraft(QStringLiteral("main.lushader.json"));
    const QStringList files = document.GetFiles();
    const uint64 revision = document.GetRevision();
    CHECK(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == expected);
    CHECK(document.GetRevision() == revision);
    CHECK(document.GetFiles() == files);
    CHECK(document.GetDraft(ROOT_GRAPH) == graph);
    CHECK(document.GetDraft(QStringLiteral("main.lushader.json")) == manifest);
}

TEST_CASE("pipeline shader creation reuses programs and preserves edited sources",
          "[editor][render-graph][pipeline][shader-creation]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject node = MakePipelineNode(QStringLiteral("GraphicsPipeline"), QStringLiteral("work"));
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    const QJsonArray shaders{QJsonObject{{QStringLiteral("id"), QStringLiteral("vertex")},
                                         {QStringLiteral("stage"), QStringLiteral("vertex")},
                                         {QStringLiteral("program"), QStringLiteral("NewVertex")}},
                             QJsonObject{{QStringLiteral("id"), QStringLiteral("fragment")},
                                         {QStringLiteral("stage"), QStringLiteral("fragment")},
                                         {QStringLiteral("program"), QStringLiteral("NewFragment")}}};
    node.insert(QStringLiteral("shaders"), shaders);
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    const QString source = QStringLiteral("shader_NewVertex.slang");
    REQUIRE(document.SetText(source, QByteArray("// authored replacement\n")) == RenderGraphDocumentStatus::Ok);
    const QByteArray manifest = document.GetDraft(QStringLiteral("main.lushader.json"));
    // Reuse the registered programs on a different pipeline without creating files.
    node.insert(QStringLiteral("id"), QStringLiteral("other"));
    REQUIRE(document.AddNode(ROOT_GRAPH,
                             DEFAULT_CONFIGURATION,
                             MakePipelineNode(QStringLiteral("GraphicsPipeline"), QStringLiteral("other"))) ==
            RenderGraphDocumentStatus::Ok);
    const QStringList files = document.GetFiles();
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetFiles() == files);
    CHECK(document.GetDraft(QStringLiteral("main.lushader.json")) == manifest);
    CHECK(document.GetDraft(source) == QByteArray("// authored replacement\n"));
    const uint64 revision = document.GetRevision();
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetRevision() == revision);
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
        QObject::connect(&editor, &RenderGraphPipelineEditor::Edited, &editor, [&applied](const QJsonObject& node) {
            applied = node;
        });
        QPushButton* add = editor.findChild<QPushButton*>(QStringLiteral("renderGraphAdd_outputs"));
        QTableWidget* resources = editor.findChild<QTableWidget*>(QStringLiteral("outputs"));
        REQUIRE(add != nullptr);
        REQUIRE(resources != nullptr);
        CHECK(resources->property("fields").toStringList().contains(QStringLiteral("target")) == false);
        add->click();
        resources->item(0, 0)->setText(QStringLiteral("color"));
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
        CHECK(applied.value(QStringLiteral("outputs")).toArray().at(0).toObject().value(QStringLiteral("format")) ==
              QStringLiteral("Depth32Float"));
        CHECK(applied.value(QStringLiteral("inputs")).toArray().at(0).toObject().value(QStringLiteral("type")) ==
              QStringLiteral("DepthTexture"));
        QPushButton* remove = editor.findChild<QPushButton*>(QStringLiteral("renderGraphRemove_outputs"));
        REQUIRE(remove != nullptr);
        remove->click();
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
    QObject::connect(&editor, &RenderGraphPipelineEditor::Edited, &editor, [&applied](const QJsonObject& candidate) {
        applied = candidate;
    });
    QTableWidget* unchangedResources = editor.findChild<QTableWidget*>(QStringLiteral("outputs"));
    REQUIRE(unchangedResources != nullptr);
    QComboBox* load = qobject_cast<QComboBox*>(unchangedResources->cellWidget(0, 3));
    REQUIRE(load != nullptr);
    const QString originalLoad = load->currentText();
    load->setCurrentText(originalLoad == QStringLiteral("Clear") ? QStringLiteral("Load") : QStringLiteral("Clear"));
    load->setCurrentText(originalLoad);
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
    QObject::connect(&editor, &RenderGraphPipelineEditor::Edited, &editor, [&applied](const QJsonObject& node) {
        applied = node;
    });
    QPushButton* add = editor.findChild<QPushButton*>(QStringLiteral("renderGraphAdd_outputs"));
    QTableWidget* resources = editor.findChild<QTableWidget*>(QStringLiteral("outputs"));
    QLabel* error = editor.findChild<QLabel*>(QStringLiteral("renderGraphPipelineError"));
    REQUIRE(add != nullptr);
    REQUIRE(resources != nullptr);
    REQUIRE(error != nullptr);
    add->click();
    add->click();
    const QJsonObject lastValid = applied;
    resources->item(1, 0)->setText(QStringLiteral("resource"));
    CHECK(applied == lastValid);
    CHECK(error->text().contains(QStringLiteral("resource")) == true);
    CHECK(error->text().contains(QStringLiteral("unique name")) == true);
    resources->item(1, 0)->setText(QStringLiteral("invalid name"));
    CHECK(applied == lastValid);
    CHECK(error->text().contains(QStringLiteral("starting with a letter")) == true);
    resources->item(1, 0)->setText(QString());
    CHECK(applied == lastValid);
    resources->item(1, 0)->setText(QStringLiteral("other"));
    CHECK(applied.isEmpty() == false);
    CHECK(error->text().isEmpty() == true);
}

TEST_CASE("same-name writable ports repair a stale target on a completed edit", "[editor][render-graph][pipeline]")
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
    QObject::connect(&editor, &RenderGraphPipelineEditor::Edited, &editor, [&applied](const QJsonObject& candidate) {
        applied = candidate;
    });
    CHECK(node == original);
    CHECK(applied.isEmpty() == true);
    QTableWidget* resources = editor.findChild<QTableWidget*>(QStringLiteral("outputs"));
    REQUIRE(resources != nullptr);
    QComboBox* load = qobject_cast<QComboBox*>(resources->cellWidget(0, 3));
    REQUIRE(load != nullptr);
    load->setCurrentText(QStringLiteral("Load"));
    CHECK(applied.value(QStringLiteral("inputs")) == node.value(QStringLiteral("inputs")));
    CHECK(applied.value(QStringLiteral("outputs")).toArray().at(0).toObject().value(QStringLiteral("target")) ==
          QStringLiteral("color"));
}

TEST_CASE("shader renames and stage changes preserve one source per assignment",
          "[editor][render-graph][pipeline][shader-creation]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString manifestFile = QStringLiteral("main.lushader.json");
    const qsizetype initialFiles = document.GetFiles().size();
    QJsonObject node = MakePipelineNode(QStringLiteral("RayTracingPipeline"), QStringLiteral("trace"));
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    QJsonArray shaders{QJsonObject{{QStringLiteral("id"), QStringLiteral("shader")},
                                   {QStringLiteral("stage"), QStringLiteral("ray_generation")},
                                   {QStringLiteral("program"), QStringLiteral("Program")}}};
    node.insert(QStringLiteral("shaders"), shaders);
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    const QString originalSource = QStringLiteral("shader_Shader_trace_shader.slang");
    REQUIRE(document.GetFiles().contains(originalSource) == true);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    const QByteArray authored("// authored lighting function: preserve this text\n");
    REQUIRE(document.SetText(originalSource, authored) == RenderGraphDocumentStatus::Ok);
    QJsonObject shader = shaders.at(0).toObject();
    shader.insert(QStringLiteral("program"), QStringLiteral("Shader_trace_shader"));
    shader.insert(QStringLiteral("id"), QStringLiteral("raygen"));
    shaders.replace(0, shader);
    node.insert(QStringLiteral("shaders"), shaders);
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    const QString renamedSource = QStringLiteral("shader_Shader_trace_raygen.slang");
    CHECK(document.GetFiles().contains(originalSource) == false);
    CHECK(document.GetDraft(renamedSource) == authored);
    CHECK(document.GetFiles().size() == initialFiles + 1);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(originalSource) == authored);
    CHECK(document.GetFiles().contains(renamedSource) == false);
    REQUIRE(document.Redo() == true);
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    CHECK(QFileInfo::exists(QDir(package).filePath(originalSource)) == false);
    CHECK(QFileInfo::exists(QDir(package).filePath(renamedSource)) == true);
    // A stage edit updates the same manifest entry and preserves authored code.
    shader.insert(QStringLiteral("program"), QStringLiteral("Shader_trace_raygen"));
    shader.insert(QStringLiteral("stage"), QStringLiteral("miss"));
    shaders.replace(0, shader);
    node.insert(QStringLiteral("shaders"), shaders);
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetDraft(renamedSource) == authored);
    CHECK(document.GetFiles().size() == initialFiles + 1);
    const QJsonArray programs =
        QJsonDocument::fromJson(document.GetDraft(manifestFile)).object().value(QStringLiteral("programs")).toArray();
    const QJsonObject program = programs.last().toObject();
    CHECK(program.value(QStringLiteral("id")).toString() == QStringLiteral("Shader_trace_raygen"));
    CHECK(program.value(QStringLiteral("stage")).toString() == QStringLiteral("miss"));
    CHECK(program.value(QStringLiteral("input")).toString() == QStringLiteral("Ray"));
    // Renaming Program explicitly moves the existing source as well.
    shader.insert(QStringLiteral("program"), QStringLiteral("SkyMiss"));
    shaders.replace(0, shader);
    node.insert(QStringLiteral("shaders"), shaders);
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetFiles().contains(renamedSource) == false);
    CHECK(document.GetDraft(QStringLiteral("shader_SkyMiss.slang")) == authored);
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reloaded;
    REQUIRE(reloaded.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reloaded.GetFiles() == document.GetFiles());
    CHECK(reloaded.GetDraft(manifestFile) == document.GetDraft(manifestFile));
}

TEST_CASE("shader edits reclaim only untouched unused generated sources",
          "[editor][render-graph][pipeline][shader-creation]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject node = MakePipelineNode(QStringLiteral("RayTracingPipeline"), QStringLiteral("trace"));
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    QJsonArray shaders;
    for (const QString& identity :
         {QStringLiteral("raygen"), QStringLiteral("closestHit"), QStringLiteral("unused"), QStringLiteral("authored")})
    {
        shaders.append(QJsonObject{{QStringLiteral("id"), identity},
                                   {QStringLiteral("stage"), QStringLiteral("ray_generation")},
                                   {QStringLiteral("program"), QStringLiteral("Program")}});
    }
    node.insert(QStringLiteral("shaders"), shaders);
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    const QString retained = QStringLiteral("shader_Shader_trace_authored.slang");
    REQUIRE(document.SetText(retained, QByteArray("// custom work must survive\n")) == RenderGraphDocumentStatus::Ok);
    const QJsonArray assigned = GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION)
                                    .value(QStringLiteral("nodes"))
                                    .toArray()
                                    .last()
                                    .toObject()
                                    .value(QStringLiteral("shaders"))
                                    .toArray();
    node.insert(QStringLiteral("shaders"), QJsonArray{assigned.at(0), assigned.at(1)});
    const QStringList before = document.GetFiles();
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetFiles().size() == before.size() - 1);
    CHECK(document.GetFiles().contains(QStringLiteral("shader_Shader_trace_unused.slang")) == false);
    CHECK(document.GetDraft(retained) == QByteArray("// custom work must survive\n"));
    REQUIRE(document.Undo() == true);
    CHECK(document.GetFiles() == before);
    REQUIRE(document.Redo() == true);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    const uint64 revision = document.GetRevision();
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetRevision() == revision);
}

TEST_CASE("shared shader renames update all assignments and protect stages and external files",
          "[editor][render-graph][pipeline][shader-creation]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject node = MakePipelineNode(QStringLiteral("RayTracingPipeline"), QStringLiteral("trace"));
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    QJsonObject shader{{QStringLiteral("id"), QStringLiteral("callback")},
                       {QStringLiteral("stage"), QStringLiteral("ray_generation")},
                       {QStringLiteral("program"), QStringLiteral("CameraRay")}};
    node.insert(QStringLiteral("shaders"), QJsonArray{shader});
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    QJsonObject other = node;
    other.insert(QStringLiteral("id"), QStringLiteral("other"));
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, other) == RenderGraphDocumentStatus::Ok);
    shader.insert(QStringLiteral("stage"), QStringLiteral("miss"));
    node.insert(QStringLiteral("shaders"), QJsonArray{shader});
    const uint64 revision = document.GetRevision();
    CHECK(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::InvalidEdit);
    CHECK(document.GetRevision() == revision);
    shader.insert(QStringLiteral("stage"), QStringLiteral("ray_generation"));
    shader.insert(QStringLiteral("program"), QStringLiteral("RenamedRay"));
    node.insert(QStringLiteral("shaders"), QJsonArray{shader});
    SECTION("shared references")
    {
        REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
        const QJsonArray nodes =
            GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION).value(QStringLiteral("nodes")).toArray();
        CHECK(nodes.at(nodes.size() - 2).toObject().value(QStringLiteral("shaders")) == QJsonArray{shader});
        CHECK(nodes.last().toObject().value(QStringLiteral("shaders")) == QJsonArray{shader});
        CHECK(document.GetFiles().contains(QStringLiteral("shader_CameraRay.slang")) == false);
        CHECK(document.GetFiles().contains(QStringLiteral("shader_RenamedRay.slang")) == true);
    }
    SECTION("external destination")
    {
        QFile external(QDir(package).filePath(QStringLiteral("shader_RenamedRay.slang")));
        REQUIRE(external.open(QIODevice::WriteOnly) == true);
        REQUIRE(external.write("// external file\n") > 0);
        external.close();
        CHECK(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Conflict);
        CHECK(document.GetRevision() == revision);
    }
    SECTION("external source changes before Save")
    {
        QStringList saved;
        REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
        REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
        QFile external(QDir(package).filePath(QStringLiteral("shader_CameraRay.slang")));
        REQUIRE(external.open(QIODevice::WriteOnly) == true);
        REQUIRE(external.write("// changed in external editor\n") > 0);
        external.close();
        CHECK(document.Save(saved) == RenderGraphDocumentStatus::Conflict);
        CHECK(QFileInfo::exists(QDir(package).filePath(QStringLiteral("shader_RenamedRay.slang"))) == false);
    }
}

TEST_CASE("renaming a registered shader updates techniques and local include references",
          "[editor][render-graph][pipeline][shader-creation]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString original = QStringLiteral("main_shaders/geometry.slang");
    const QByteArray source = document.GetDraft(original);
    const QString consumer = QStringLiteral("main_shaders/color.slang");
    REQUIRE(document.SetText(consumer, QByteArray("#include \"geometry.slang\"\n// keep this comment\n")) ==
            RenderGraphDocumentStatus::Ok);
    QJsonObject node = MakePipelineNode(QStringLiteral("GraphicsPipeline"), QStringLiteral("work"));
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    QJsonObject shader{{QStringLiteral("id"), QStringLiteral("vertex")},
                       {QStringLiteral("stage"), QStringLiteral("vertex")},
                       {QStringLiteral("program"), QStringLiteral("Geometry")}};
    node.insert(QStringLiteral("shaders"), QJsonArray{shader});
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    shader.insert(QStringLiteral("program"), QStringLiteral("CustomGeometry"));
    node.insert(QStringLiteral("shaders"), QJsonArray{shader});
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    const QJsonObject manifest =
        QJsonDocument::fromJson(document.GetDraft(QStringLiteral("main.lushader.json"))).object();
    for (const QJsonValueConstRef technique : manifest.value(QStringLiteral("techniques")).toArray())
    {
        CHECK(technique.toObject().value(QStringLiteral("vertex")).toString() == QStringLiteral("CustomGeometry"));
    }
    CHECK(document.GetFiles().contains(original) == false);
    CHECK(document.GetDraft(QStringLiteral("main_shaders/shader_CustomGeometry.slang")) == source);
    CHECK(document.GetDraft(consumer) ==
          QByteArray("#include \"shader_CustomGeometry.slang\"\n// keep this comment\n"));
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(original) == source);
    CHECK(document.GetDraft(consumer).contains("geometry.slang") == true);
}

TEST_CASE("ray state defaults are automatic and explicit overrides survive reset undo and save",
          "[editor][render-graph][pipeline]")
{
    QJsonObject node = MakePipelineNode(QStringLiteral("RayTracingPipeline"), QStringLiteral("trace"));
    CHECK(node.value(QStringLiteral("state")).toObject().isEmpty() == true);
    RenderGraphPipelineEditor automatic(node);
    QGroupBox* defaultAdvanced = automatic.findChild<QGroupBox*>(QStringLiteral("renderGraphRayAdvanced"));
    REQUIRE(defaultAdvanced != nullptr);
    CHECK(defaultAdvanced->isChecked() == false);
    node.insert(QStringLiteral("state"),
                QJsonObject{{QStringLiteral("max_recursion"), 1},
                            {QStringLiteral("payload_bytes"), 0},
                            {QStringLiteral("attribute_bytes"), 0}});
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    const QByteArray original = document.GetDraft(ROOT_GRAPH);
    RenderGraphPipelineEditor editor(node);
    QJsonObject applied;
    QObject::connect(&editor,
                     &RenderGraphPipelineEditor::Edited,
                     &editor,
                     [&document, &applied](const QJsonObject& candidate) {
                         applied = candidate;
                         REQUIRE(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, candidate) ==
                                 RenderGraphDocumentStatus::Ok);
                     });
    QGroupBox* advanced = editor.findChild<QGroupBox*>(QStringLiteral("renderGraphRayAdvanced"));
    QSpinBox* attributes = editor.findChild<QSpinBox*>(QStringLiteral("renderGraphRay_attribute_bytes"));
    QPushButton* reset = editor.findChild<QPushButton*>(QStringLiteral("renderGraphRayAutomatic"));
    REQUIRE(advanced != nullptr);
    REQUIRE(attributes != nullptr);
    REQUIRE(reset != nullptr);
    CHECK(advanced->isChecked() == true);
    CHECK(attributes->value() == 0); // Opening must preserve the old invalid value until the author resets it.
    advanced->setChecked(false);
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
    const uint64 revision = document.GetRevision();
    reset->click();
    CHECK(document.GetRevision() == revision + 1);
    CHECK(applied.value(QStringLiteral("state")).toObject().isEmpty() == true);
    CHECK(attributes->value() == attributes->minimum());
    CHECK(advanced->isChecked() == false);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(ROOT_GRAPH) == original);
    REQUIRE(document.Redo() == true);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reloaded;
    REQUIRE(reloaded.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(reloaded.GetDraft(ROOT_GRAPH) == document.GetDraft(ROOT_GRAPH));
    advanced->setChecked(true);
    constexpr int32 TRIANGLE_ATTRIBUTE_BYTES = 8;
    attributes->setValue(TRIANGLE_ATTRIBUTE_BYTES);
    CHECK(applied.value(QStringLiteral("state")).toObject() ==
          QJsonObject{{QStringLiteral("attribute_bytes"), TRIANGLE_ATTRIBUTE_BYTES}});
    attributes->setValue(attributes->minimum());
    CHECK(applied.value(QStringLiteral("state")).toObject().isEmpty() == true);
}
