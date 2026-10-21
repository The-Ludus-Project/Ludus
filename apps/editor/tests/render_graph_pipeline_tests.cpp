// Pipeline authoring regressions run without a Renderer device, including sanitizer builds.
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_configuration_editor.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_pipeline.h"
#include "internal/render_graph_pipeline_node.h"
#include "internal/render_graph_shader_interface.h"

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

QJsonObject GetNode(const RenderGraphDocument& document, const QString& identity)
{
    for (const QJsonValueConstRef node :
         GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION).value(QStringLiteral("nodes")).toArray())
    {
        if (node.toObject().value(QStringLiteral("id")).toString() == identity)
        {
            return node.toObject();
        }
    }
    return {};
}
} // namespace

TEST_CASE("shared render sizes reject proven nested attachment mismatches before editing",
          "[editor][render-graph][render-size]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.Disconnect(ROOT_GRAPH,
                                DEFAULT_CONFIGURATION,
                                QStringLiteral("depth.texture"),
                                QStringLiteral("raster.depth")) == RenderGraphDocumentStatus::Ok);
    QJsonObject depth = GetNode(document, QStringLiteral("depth"));
    depth.insert(QStringLiteral("extent"), QJsonArray{97, 65});
    REQUIRE(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, depth) == RenderGraphDocumentStatus::Ok);
    const uint64 revision = document.GetRevision();
    const RenderGraphLinkCheck mismatch = CheckRenderGraphLink(document,
                                                               ROOT_GRAPH,
                                                               DEFAULT_CONFIGURATION,
                                                               QStringLiteral("depth.texture"),
                                                               QStringLiteral("raster.depth"));
    CHECK(mismatch.Code == QStringLiteral("AttachmentMismatch"));
    CHECK(mismatch.Reason.contains(QStringLiteral("97 x 65")) == true);
    CHECK(mismatch.Reason.contains(QStringLiteral("96 x 64")) == true);
    CHECK(mismatch.Reason.contains(QStringLiteral("RenderSize")) == true);
    CHECK(document.GetRevision() == revision);
    REQUIRE(document.Connect(ROOT_GRAPH,
                             DEFAULT_CONFIGURATION,
                             QStringLiteral("depth.texture"),
                             QStringLiteral("raster.depth")) == RenderGraphDocumentStatus::Ok);
    CHECK(CheckRenderGraphLink(document,
                               ROOT_GRAPH,
                               DEFAULT_CONFIGURATION,
                               QStringLiteral("scene.scene"),
                               QStringLiteral("raster.scene"),
                               true)
              .IsLegal() == true);
    REQUIRE(document.Disconnect(ROOT_GRAPH,
                                DEFAULT_CONFIGURATION,
                                QStringLiteral("depth.texture"),
                                QStringLiteral("raster.depth")) == RenderGraphDocumentStatus::Ok);
    depth.insert(QStringLiteral("extent"), QStringLiteral("RenderSize"));
    REQUIRE(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, depth) == RenderGraphDocumentStatus::Ok);
    // An unbound size is unknown; accepting this wire must not invent dimensions.
    CHECK(CheckRenderGraphLink(document,
                               ROOT_GRAPH,
                               DEFAULT_CONFIGURATION,
                               QStringLiteral("depth.texture"),
                               QStringLiteral("raster.depth"))
              .IsLegal() == true);
    REQUIRE(document.Connect(ROOT_GRAPH,
                             DEFAULT_CONFIGURATION,
                             QStringLiteral("depth.texture"),
                             QStringLiteral("raster.depth")) == RenderGraphDocumentStatus::Ok);
    const QJsonObject size{{QStringLiteral("id"), QStringLiteral("size")},
                           {QStringLiteral("kind"), QStringLiteral("RenderSize")},
                           {QStringLiteral("extent"), QJsonArray{97, 65}}};
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, size) == RenderGraphDocumentStatus::Ok);
    CHECK(CheckRenderGraphLink(document,
                               ROOT_GRAPH,
                               DEFAULT_CONFIGURATION,
                               QStringLiteral("size.size"),
                               QStringLiteral("depth.size"))
              .Code == QStringLiteral("AttachmentMismatch"));
    QJsonObject matching = size;
    matching.insert(QStringLiteral("extent"), QJsonArray{96, 64});
    REQUIRE(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, matching) == RenderGraphDocumentStatus::Ok);
    CHECK(CheckRenderGraphLink(document,
                               ROOT_GRAPH,
                               DEFAULT_CONFIGURATION,
                               QStringLiteral("size.size"),
                               QStringLiteral("depth.size"))
              .IsLegal() == true);
    REQUIRE(document.Connect(ROOT_GRAPH,
                             DEFAULT_CONFIGURATION,
                             QStringLiteral("size.size"),
                             QStringLiteral("depth.size")) == RenderGraphDocumentStatus::Ok);
    QJsonObject graph;
    REQUIRE(document.GetGraph(ROOT_GRAPH, graph) == true);
    CHECK(graph.value(QStringLiteral("version")).toInt() == 2);
    QString error;
    depth.insert(QStringLiteral("extent"), QJsonArray{96, 64});
    const uint64 connectedRevision = document.GetRevision();
    CHECK(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, depth, &error) ==
          RenderGraphDocumentStatus::InvalidEdit);
    CHECK(error.contains(QStringLiteral("Disconnect depth.size")) == true);
    CHECK(document.GetRevision() == connectedRevision);
    REQUIRE(document.Undo() == true);
    CHECK(
        GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION).value(QStringLiteral("edges")).toArray().size() ==
        6);
    REQUIRE(document.Redo() == true);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(GetNode(reopened, QStringLiteral("depth")).value(QStringLiteral("extent")) == QStringLiteral("RenderSize"));
}

TEST_CASE("texture-derived size ports accept color and depth but reject values and cycles",
          "[editor][render-graph][render-size]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QJsonObject size{{QStringLiteral("id"), QStringLiteral("size")},
                           {QStringLiteral("kind"), QStringLiteral("RenderSize")},
                           {QStringLiteral("extent"), QStringLiteral("Texture")}};
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, size) == RenderGraphDocumentStatus::Ok);
    for (const QString& source : {QStringLiteral("color.texture"), QStringLiteral("depth.texture")})
    {
        CHECK(CheckRenderGraphLink(document, ROOT_GRAPH, DEFAULT_CONFIGURATION, source, QStringLiteral("size.texture"))
                  .IsLegal() == true);
    }
    CHECK(CheckRenderGraphLink(document,
                               ROOT_GRAPH,
                               DEFAULT_CONFIGURATION,
                               QStringLiteral("scene.scene"),
                               QStringLiteral("size.texture"))
              .Code == QStringLiteral("TypeMismatch"));
    QJsonObject depth = GetNode(document, QStringLiteral("depth"));
    depth.insert(QStringLiteral("extent"), QStringLiteral("RenderSize"));
    REQUIRE(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, depth) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.Connect(ROOT_GRAPH,
                             DEFAULT_CONFIGURATION,
                             QStringLiteral("size.size"),
                             QStringLiteral("depth.size")) == RenderGraphDocumentStatus::Ok);
    CHECK(CheckRenderGraphLink(document,
                               ROOT_GRAPH,
                               DEFAULT_CONFIGURATION,
                               QStringLiteral("depth.texture"),
                               QStringLiteral("size.texture"))
              .Code == QStringLiteral("Cycle"));
}

TEST_CASE("pipeline texture contracts check samples and explicit ray coverage", "[editor][render-graph][render-size]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject pipeline = MakePipelineNode(QStringLiteral("GraphicsPipeline"), QStringLiteral("work"));
    pipeline.insert(QStringLiteral("inputs"),
                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("color")},
                                           {QStringLiteral("type"), QStringLiteral("ColorTexture")},
                                           {QStringLiteral("required"), true}}});
    pipeline.insert(QStringLiteral("outputs"),
                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("color")},
                                           {QStringLiteral("type"), QStringLiteral("ColorTexture")},
                                           {QStringLiteral("target"), QStringLiteral("color")},
                                           {QStringLiteral("format"), QStringLiteral("RGBA8Unorm")},
                                           {QStringLiteral("load"), QStringLiteral("Clear")},
                                           {QStringLiteral("store"), QStringLiteral("Store")}}});
    QJsonObject state = pipeline.value(QStringLiteral("state")).toObject();
    state.insert(QStringLiteral("samples"), 2);
    pipeline.insert(QStringLiteral("state"), state);
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, pipeline) == RenderGraphDocumentStatus::Ok);
    CHECK(CheckRenderGraphLink(document,
                               ROOT_GRAPH,
                               DEFAULT_CONFIGURATION,
                               QStringLiteral("color.texture"),
                               QStringLiteral("work.color"))
              .Code == QStringLiteral("AttachmentMismatch"));
    state.insert(QStringLiteral("samples"), 1);
    pipeline.insert(QStringLiteral("state"), state);
    REQUIRE(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, pipeline) == RenderGraphDocumentStatus::Ok);
    CHECK(CheckRenderGraphLink(document,
                               ROOT_GRAPH,
                               DEFAULT_CONFIGURATION,
                               QStringLiteral("color.texture"),
                               QStringLiteral("work.color"))
              .IsLegal() == true);
    QJsonObject ray = MakePipelineNode(QStringLiteral("RayTracingPipeline"), QStringLiteral("ray"));
    ray.insert(QStringLiteral("inputs"), pipeline.value(QStringLiteral("inputs")));
    ray.insert(QStringLiteral("outputs"), pipeline.value(QStringLiteral("outputs")));
    ray.insert(QStringLiteral("invocation"), QJsonObject{{QStringLiteral("dimensions"), QJsonArray{1, 2, 1}}});
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, ray) == RenderGraphDocumentStatus::Ok);
    const RenderGraphLinkCheck mismatch = CheckRenderGraphLink(document,
                                                               ROOT_GRAPH,
                                                               DEFAULT_CONFIGURATION,
                                                               QStringLiteral("color.texture"),
                                                               QStringLiteral("ray.color"));
    CHECK(mismatch.Code == QStringLiteral("AttachmentMismatch"));
    CHECK(mismatch.Reason.contains(QStringLiteral("Match color target")) == true);
    ray.insert(QStringLiteral("invocation"),
               QJsonObject{{QStringLiteral("dimensions"), QStringLiteral("ColorTarget")}});
    REQUIRE(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, ray) == RenderGraphDocumentStatus::Ok);
    CHECK(CheckRenderGraphLink(document,
                               ROOT_GRAPH,
                               DEFAULT_CONFIGURATION,
                               QStringLiteral("color.texture"),
                               QStringLiteral("ray.color"))
              .IsLegal() == true);
    const QJsonObject recursive{{QStringLiteral("id"), QStringLiteral("recursive_draft")},
                                {QStringLiteral("kind"), QStringLiteral("Subgraph")},
                                {QStringLiteral("asset"), ROOT_GRAPH},
                                {QStringLiteral("configuration"), DEFAULT_CONFIGURATION}};
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, recursive) == RenderGraphDocumentStatus::Ok);
    CHECK(CheckRenderGraphLink(document,
                               ROOT_GRAPH,
                               DEFAULT_CONFIGURATION,
                               QStringLiteral("color.texture"),
                               QStringLiteral("ray.color"))
              .IsLegal() == true);
}

TEST_CASE("ray launch sizing derives from color by default and preserves explicit drafts",
          "[editor][render-graph][pipeline]")
{
    const QJsonObject node = MakePipelineNode(QStringLiteral("RayTracingPipeline"), QStringLiteral("trace"));
    CHECK(node.value(QStringLiteral("invocation")).toObject().value(QStringLiteral("dimensions")) ==
          QStringLiteral("ColorTarget"));
    RenderGraphPipelineEditor editor(node);
    QJsonObject applied;
    QObject::connect(&editor, &RenderGraphPipelineEditor::Edited, &editor, [&applied](const QJsonObject& candidate) {
        applied = candidate;
    });
    QComboBox* mode = editor.findChild<QComboBox*>(QStringLiteral("rayLaunchSizeMode"));
    QSpinBox* width = editor.findChild<QSpinBox*>(QStringLiteral("rayLaunchDimension0"));
    REQUIRE(mode != nullptr);
    REQUIRE(width != nullptr);
    CHECK(mode->currentText() == QStringLiteral("Match color target"));
    CHECK(applied.isEmpty() == true);
    mode->setCurrentIndex(1);
    width->setValue(97);
    CHECK(applied.value(QStringLiteral("invocation")).toObject().value(QStringLiteral("dimensions")).toArray() ==
          QJsonArray{97, 1, 1});
    mode->setCurrentIndex(0);
    CHECK(applied.value(QStringLiteral("invocation")).toObject().value(QStringLiteral("dimensions")) ==
          QStringLiteral("ColorTarget"));

    QJsonObject legacy = node;
    legacy.insert(QStringLiteral("invocation"), QJsonObject{{QStringLiteral("dimensions"), QJsonArray{96, 64, 1}}});
    RenderGraphPipelineEditor legacyEditor(legacy);
    QComboBox* legacyMode = legacyEditor.findChild<QComboBox*>(QStringLiteral("rayLaunchSizeMode"));
    REQUIRE(legacyMode != nullptr);
    CHECK(legacyMode->currentText() == QStringLiteral("Explicit size"));
    QJsonObject legacyApplied;
    QObject::connect(&legacyEditor,
                     &RenderGraphPipelineEditor::Edited,
                     &legacyEditor,
                     [&legacyApplied](const QJsonObject& candidate) { legacyApplied = candidate; });
    CHECK(legacyApplied.isEmpty() == true);
    legacyMode->setCurrentIndex(0);
    CHECK(legacyApplied.value(QStringLiteral("invocation")).toObject().value(QStringLiteral("dimensions")) ==
          QStringLiteral("ColorTarget"));
    legacyMode->setCurrentIndex(1);
    CHECK(legacyApplied.value(QStringLiteral("invocation")).toObject().value(QStringLiteral("dimensions")).toArray() ==
          QJsonArray{96, 64, 1});
}

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

TEST_CASE("configuration interface removals repair internal and containing graph links",
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
    const QByteArray parent = document.GetDraft(ROOT_GRAPH);
    QString error;
    QJsonArray changedInputs = inputs;
    changedInputs.removeAt(0);
    REQUIRE(document.ReplaceConfigurationInterface(LIBRARY, configuration, changedInputs, outputs, error) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(GetConfiguration(document, LIBRARY, configuration).value(QStringLiteral("edges")).toArray().size() == 3);
    CHECK(
        GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION).value(QStringLiteral("edges")).toArray().size() ==
        5);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(LIBRARY) == original);
    CHECK(document.GetDraft(ROOT_GRAPH) == parent);
    QJsonArray changedOutputs = outputs;
    changedOutputs.removeAt(changedOutputs.size() - 1); // color is consumed by the root Export.
    REQUIRE(document.ReplaceConfigurationInterface(LIBRARY, configuration, inputs, changedOutputs, error) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(
        GetConfiguration(document, ROOT_GRAPH, DEFAULT_CONFIGURATION).value(QStringLiteral("edges")).toArray().size() ==
        5);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(LIBRARY) == original);
    CHECK(document.GetDraft(ROOT_GRAPH) == parent);
    const uint64 revision = document.GetRevision();
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

namespace
{
QString MakeInterfacePackage(const QTemporaryDir& project)
{
    const QString package = project.path();
    const QMap<QString, QByteArray> sources{{QStringLiteral("child.lurender.json"), QByteArrayLiteral(R"JSON({
          "version":2,"id":"Child","default":"Default","configurations":[{
            "id":"Default","inputs":[{"id":"color","type":"ColorTexture"},{"id":"other","type":"ColorTexture"}],
            "outputs":[{"id":"color","type":"ColorTexture","source":"input.color","required":true}],
            "nodes":[{"id":"size","kind":"RenderSize","extent":"Texture"},
                     {"id":"work","kind":"ComputePipeline","inputs":[{"id":"color","type":"ColorTexture"},{"id":"other","type":"ColorTexture"}],"outputs":[]}],
            "edges":[{"from":"input.color","to":"size.texture"},{"from":"input.color","to":"work.color"},
                     {"from":"input.other","to":"work.other"},{"from":"unrelated.missing","to":"work.missing"}]
          }]})JSON")},
                                            {QStringLiteral("caller.lurender.json"), QByteArrayLiteral(R"JSON({
          "version":2,"id":"Caller","default":"Default","configurations":[{
            "id":"Default","inputs":[{"id":"color","type":"ColorTexture"}],
            "outputs":[{"id":"exposed","type":"ColorTexture","source":"first.color","required":true}],
            "nodes":[{"id":"first","kind":"Subgraph","asset":"child.lurender.json","configuration":"Default"},
                     {"id":"second","kind":"Subgraph","asset":"child.lurender.json","configuration":"Default"},
                     {"id":"output","kind":"Export","type":"ColorTexture"},
                     {"id":"size","kind":"RenderSize","extent":"Texture"}],
            "edges":[{"from":"input.color","to":"first.color"},{"from":"first.color","to":"second.color"},
                     {"from":"second.color","to":"output.texture"},{"from":"first.color","to":"size.texture"},
                     {"from":"unrelated.missing","to":"other.missing"}]
          }]})JSON")},
                                            {ROOT_GRAPH, QByteArrayLiteral(R"JSON({
          "version":2,"id":"Root","default":"Default","configurations":[{
            "id":"Default","inputs":[],"outputs":[],
            "nodes":[{"id":"wrapper","kind":"Subgraph","asset":"caller.lurender.json","configuration":"Default"},
                     {"id":"output","kind":"Export","type":"ColorTexture"}],
            "edges":[{"from":"wrapper.exposed","to":"output.texture"}]
          }]})JSON")}};
    for (QMap<QString, QByteArray>::const_iterator source = sources.constBegin(); source != sources.constEnd();
         ++source)
    {
        QFile file(QDir(package).filePath(source.key()));
        REQUIRE(file.open(QIODevice::WriteOnly) == true);
        REQUIRE(file.write(source.value()) == source.value().size());
    }
    return package;
}
QMap<QString, QByteArray> ReadDrafts(const RenderGraphDocument& document)
{
    QMap<QString, QByteArray> drafts;
    for (const QString& file : document.GetFiles())
    {
        drafts.insert(file, document.GetDraft(file));
    }
    return drafts;
}
QJsonArray RenamePorts(const QJsonArray& ports, const QMap<QString, QString>& renames, bool shouldChangeType = false)
{
    QJsonArray result;
    for (const QJsonValueConstRef value : ports)
    {
        QJsonObject port = value.toObject();
        const QString identity = port.value(QStringLiteral("id")).toString();
        port.insert(QStringLiteral("id"), renames.value(identity, identity));
        if (shouldChangeType == true && identity == QStringLiteral("color"))
        {
            port.insert(QStringLiteral("type"), QStringLiteral("DepthTexture"));
        }
        result.append(port);
    }
    return result;
}
} // namespace

TEST_CASE("interface rename previews and repairs every caller with one history entry",
          "[editor][render-graph][interface]")
{
    QTemporaryDir project;
    const QString package = MakeInterfacePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString child = QStringLiteral("child.lurender.json");
    const QString caller = QStringLiteral("caller.lurender.json");
    // A second saved configuration and a separate source both instantiate the same child.
    QJsonObject graph;
    REQUIRE(document.GetGraph(caller, graph) == true);
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    QJsonObject second = configurations.at(0).toObject();
    second.insert(QStringLiteral("id"), QStringLiteral("Alternative"));
    configurations.append(second);
    graph.insert(QStringLiteral("configurations"), configurations);
    REQUIRE(document.SetText(caller, EncodeCanonicalJson(graph)) == RenderGraphDocumentStatus::Ok);
    REQUIRE(document.SetText(ROOT_GRAPH, EncodeCanonicalJson(graph)) == RenderGraphDocumentStatus::Ok);
    const QMap<QString, QByteArray> original = ReadDrafts(document);
    const QJsonObject record = GetConfiguration(document, child, DEFAULT_CONFIGURATION);
    // Explicit identity maps make swaps unambiguous and apply exactly once.
    const QMap<QString, QString> inputRenames{{QStringLiteral("color"), QStringLiteral("other")},
                                              {QStringLiteral("other"), QStringLiteral("color")}};
    const QMap<QString, QString> outputRenames{{QStringLiteral("color"), QStringLiteral("result")}};
    const QJsonArray inputs = RenamePorts(record.value(QStringLiteral("inputs")).toArray(), inputRenames);
    const QJsonArray outputs = RenamePorts(record.value(QStringLiteral("outputs")).toArray(), outputRenames);
    const uint64 revision = document.GetRevision();
    QStringList repairs;
    QString error;
    REQUIRE(document.PreviewConfigurationInterface(child,
                                                   DEFAULT_CONFIGURATION,
                                                   inputs,
                                                   outputs,
                                                   repairs,
                                                   error,
                                                   inputRenames,
                                                   outputRenames) == RenderGraphDocumentStatus::Ok);
    CHECK(repairs.join(QLatin1Char('\n')).contains(QStringLiteral("Alternative")) == true);
    CHECK(document.GetRevision() == revision);
    CHECK(ReadDrafts(document) == original);
    REQUIRE(document.ReplaceConfigurationInterface(child,
                                                   DEFAULT_CONFIGURATION,
                                                   inputs,
                                                   outputs,
                                                   error,
                                                   inputRenames,
                                                   outputRenames) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetRevision() == revision + 1);
    const QJsonObject repairedChild = GetConfiguration(document, child, DEFAULT_CONFIGURATION);
    CHECK(repairedChild.value(QStringLiteral("outputs"))
              .toArray()
              .at(0)
              .toObject()
              .value(QStringLiteral("source"))
              .toString() == QStringLiteral("input.other"));
    CHECK(repairedChild.value(QStringLiteral("edges"))
              .toArray()
              .at(0)
              .toObject()
              .value(QStringLiteral("from"))
              .toString() == QStringLiteral("input.other"));
    CHECK(repairedChild.value(QStringLiteral("edges"))
              .toArray()
              .at(2)
              .toObject()
              .value(QStringLiteral("from"))
              .toString() == QStringLiteral("input.color"));
    for (const QString& file : {caller, ROOT_GRAPH})
    {
        for (const QString& configuration : {DEFAULT_CONFIGURATION, QStringLiteral("Alternative")})
        {
            const QJsonObject repaired = GetConfiguration(document, file, configuration);
            const QJsonArray edges = repaired.value(QStringLiteral("edges")).toArray();
            REQUIRE(edges.size() == 5);
            CHECK(edges.at(1).toObject().value(QStringLiteral("from")).toString() == QStringLiteral("first.result"));
            CHECK(edges.at(1).toObject().value(QStringLiteral("to")).toString() == QStringLiteral("second.other"));
            CHECK(edges.at(4).toObject().value(QStringLiteral("from")).toString() ==
                  QStringLiteral("unrelated.missing"));
            CHECK(repaired.value(QStringLiteral("outputs"))
                      .toArray()
                      .at(0)
                      .toObject()
                      .value(QStringLiteral("source"))
                      .toString() == QStringLiteral("first.result"));
        }
    }
    const QMap<QString, QByteArray> repaired = ReadDrafts(document);
    REQUIRE(document.Undo() == true);
    CHECK(ReadDrafts(document) == original);
    REQUIRE(document.Redo() == true);
    CHECK(ReadDrafts(document) == repaired);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    CHECK(saved.size() == 3);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(ReadDrafts(reopened) == repaired);
    const uint64 unchanged = document.GetRevision();
    REQUIRE(document.ReplaceConfigurationInterface(child,
                                                   DEFAULT_CONFIGURATION,
                                                   repairedChild.value(QStringLiteral("inputs")).toArray(),
                                                   repairedChild.value(QStringLiteral("outputs")).toArray(),
                                                   error) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetRevision() == unchanged);
}

TEST_CASE("interface editor retains input identity through swaps and source selection",
          "[editor][render-graph][interface]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeInterfacePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString child = QStringLiteral("child.lurender.json");
    RenderGraphConfigurationEditor editor(document, child, GetConfiguration(document, child, DEFAULT_CONFIGURATION));
    QTableWidget* inputs = editor.findChild<QTableWidget*>(QStringLiteral("configurationInputs"));
    QTableWidget* outputs = editor.findChild<QTableWidget*>(QStringLiteral("configurationOutputs"));
    inputs->item(0, 0)->setText(QStringLiteral("other"));
    inputs->item(1, 0)->setText(QStringLiteral("color"));
    QComboBox* source = qobject_cast<QComboBox*>(outputs->cellWidget(0, 2));
    REQUIRE(source != nullptr);
    CHECK(source->currentText() == QStringLiteral("input.other"));
    CHECK(source->currentData().toString() == QStringLiteral("input.color"));
    source->setCurrentIndex(source->findText(QStringLiteral("input.color")));
    CHECK(source->currentData().toString() == QStringLiteral("input.other"));
    const QString repairs = editor.findChild<QLabel*>(QStringLiteral("configurationInterfaceRepairs"))->text();
    CHECK(repairs.contains(QStringLiteral("input.other to input.color")) == true);
    CHECK(repairs.contains(QStringLiteral("one undo step")) == true);
    QString error;
    REQUIRE(document.ReplaceConfigurationInterface(child,
                                                   DEFAULT_CONFIGURATION,
                                                   editor.GetInputs(),
                                                   editor.GetOutputs(),
                                                   error,
                                                   editor.GetInputRenames(),
                                                   editor.GetOutputRenames()) == RenderGraphDocumentStatus::Ok);
    CHECK(GetConfiguration(document, child, DEFAULT_CONFIGURATION)
              .value(QStringLiteral("outputs"))
              .toArray()
              .at(0)
              .toObject()
              .value(QStringLiteral("source"))
              .toString() == QStringLiteral("input.color"));
}

TEST_CASE("interface type repair preserves compatible uses and outer contracts", "[editor][render-graph][interface]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeInterfacePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString child = QStringLiteral("child.lurender.json");
    const QString caller = QStringLiteral("caller.lurender.json");
    const QMap<QString, QByteArray> original = ReadDrafts(document);
    const QJsonObject record = GetConfiguration(document, child, DEFAULT_CONFIGURATION);
    const QJsonArray inputs = RenamePorts(record.value(QStringLiteral("inputs")).toArray(), {}, true);
    const QJsonArray outputs = RenamePorts(record.value(QStringLiteral("outputs")).toArray(), {}, true);
    QString error;
    REQUIRE(document.ReplaceConfigurationInterface(child, DEFAULT_CONFIGURATION, inputs, outputs, error) ==
            RenderGraphDocumentStatus::Ok);
    const QJsonObject repaired = GetConfiguration(document, child, DEFAULT_CONFIGURATION);
    const QJsonArray internalEdges = repaired.value(QStringLiteral("edges")).toArray();
    REQUIRE(internalEdges.size() == 3);
    CHECK(internalEdges.at(0).toObject().value(QStringLiteral("to")).toString() == QStringLiteral("size.texture"));
    CHECK(repaired.value(QStringLiteral("outputs"))
              .toArray()
              .at(0)
              .toObject()
              .value(QStringLiteral("source"))
              .toString() == QStringLiteral("input.color"));
    const QJsonObject parent = GetConfiguration(document, caller, DEFAULT_CONFIGURATION);
    const QJsonArray parentEdges = parent.value(QStringLiteral("edges")).toArray();
    REQUIRE(parentEdges.size() == 3);
    CHECK(parentEdges.at(0).toObject().value(QStringLiteral("to")).toString() == QStringLiteral("second.color"));
    CHECK(parentEdges.at(1).toObject().value(QStringLiteral("to")).toString() == QStringLiteral("size.texture"));
    const QJsonObject exposed = parent.value(QStringLiteral("outputs")).toArray().at(0).toObject();
    CHECK(exposed.value(QStringLiteral("source")).toString().isEmpty() == true);
    CHECK(exposed.value(QStringLiteral("type")).toString() == QStringLiteral("ColorTexture"));
    CHECK(exposed.value(QStringLiteral("required")).toBool() == true);
    CHECK(document.GetDraft(ROOT_GRAPH) == original.value(ROOT_GRAPH));
    REQUIRE(document.Undo() == true);
    CHECK(ReadDrafts(document) == original);
    // Changing only the output type clears its old incompatible source too.
    REQUIRE(document.ReplaceConfigurationInterface(child,
                                                   DEFAULT_CONFIGURATION,
                                                   record.value(QStringLiteral("inputs")).toArray(),
                                                   outputs,
                                                   error) == RenderGraphDocumentStatus::Ok);
    CHECK(GetConfiguration(document, child, DEFAULT_CONFIGURATION)
              .value(QStringLiteral("outputs"))
              .toArray()
              .at(0)
              .toObject()
              .value(QStringLiteral("source"))
              .toString()
              .isEmpty() == true);
}

TEST_CASE("interface repairs reject ambiguous maps malformed callers and byte overflow atomically",
          "[editor][render-graph][interface]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeInterfacePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString child = QStringLiteral("child.lurender.json");
    const QJsonObject record = GetConfiguration(document, child, DEFAULT_CONFIGURATION);
    const QMap<QString, QString> renames{{QStringLiteral("color"), QStringLiteral("renamed")}};
    const QJsonArray inputs = RenamePorts(record.value(QStringLiteral("inputs")).toArray(), renames);
    const QJsonArray outputs = record.value(QStringLiteral("outputs")).toArray();
    QString error;
    SECTION("ambiguous map")
    {
        const QMap<QString, QByteArray> original = ReadDrafts(document);
        const uint64 revision = document.GetRevision();
        REQUIRE(document.ReplaceConfigurationInterface(child,
                                                       DEFAULT_CONFIGURATION,
                                                       inputs,
                                                       outputs,
                                                       error,
                                                       {{QStringLiteral("color"), QStringLiteral("renamed")},
                                                        {QStringLiteral("other"), QStringLiteral("renamed")}}) ==
                RenderGraphDocumentStatus::InvalidEdit);
        CHECK(ReadDrafts(document) == original);
        CHECK(document.GetRevision() == revision);
    }
    SECTION("old identity cannot be reused for a new port during a rename")
    {
        QJsonArray reused = inputs;
        reused.append(record.value(QStringLiteral("inputs")).toArray().at(0));
        const QMap<QString, QByteArray> original = ReadDrafts(document);
        CHECK(document.ReplaceConfigurationInterface(child, DEFAULT_CONFIGURATION, reused, outputs, error, renames) ==
              RenderGraphDocumentStatus::InvalidEdit);
        CHECK(ReadDrafts(document) == original);
    }
    SECTION("unrelated malformed records remain byte-for-byte unchanged")
    {
        const QByteArray unrelated = QByteArrayLiteral("{\"configurations\":[null,17],\"version\":2}");
        REQUIRE(document.SetText(ROOT_GRAPH, unrelated) == RenderGraphDocumentStatus::Ok);
        REQUIRE(document.ReplaceConfigurationInterface(child, DEFAULT_CONFIGURATION, inputs, outputs, error, renames) ==
                RenderGraphDocumentStatus::Ok);
        CHECK(document.GetDraft(ROOT_GRAPH) == unrelated);
    }
    SECTION("malformed caller")
    {
        REQUIRE(document.SetText(ROOT_GRAPH, QByteArrayLiteral("{")) == RenderGraphDocumentStatus::Ok);
        const QMap<QString, QByteArray> original = ReadDrafts(document);
        const uint64 revision = document.GetRevision();
        QStringList repairs{QStringLiteral("preserved")};
        CHECK(document.PreviewConfigurationInterface(child,
                                                     DEFAULT_CONFIGURATION,
                                                     inputs,
                                                     outputs,
                                                     repairs,
                                                     error,
                                                     renames) == RenderGraphDocumentStatus::Malformed);
        CHECK(repairs == QStringList{QStringLiteral("preserved")});
        CHECK(error.contains(ROOT_GRAPH) == true);
        CHECK(document.ReplaceConfigurationInterface(child, DEFAULT_CONFIGURATION, inputs, outputs, error, renames) ==
              RenderGraphDocumentStatus::Malformed);
        CHECK(ReadDrafts(document) == original);
        CHECK(document.GetRevision() == revision);
    }
    SECTION("byte overflow")
    {
        QJsonObject graph;
        REQUIRE(document.GetGraph(child, graph) == true);
        graph.insert(QStringLiteral("padding"), QString());
        const qsizetype used = QJsonDocument(graph).toJson(QJsonDocument::Compact).size();
        graph.insert(QStringLiteral("padding"), QString(RenderGraphDocument::MAX_FILE_BYTES - used, QLatin1Char('x')));
        REQUIRE(document.SetText(child, QJsonDocument(graph).toJson(QJsonDocument::Compact)) ==
                RenderGraphDocumentStatus::Ok);
        const QMap<QString, QByteArray> original = ReadDrafts(document);
        const uint64 revision = document.GetRevision();
        CHECK(document.ReplaceConfigurationInterface(child, DEFAULT_CONFIGURATION, inputs, outputs, error, renames) ==
              RenderGraphDocumentStatus::Capacity);
        CHECK(ReadDrafts(document) == original);
        CHECK(document.GetRevision() == revision);
    }
    SECTION("remove and add is not guessed as a rename")
    {
        REQUIRE(document.ReplaceConfigurationInterface(child, DEFAULT_CONFIGURATION, inputs, outputs, error) ==
                RenderGraphDocumentStatus::Ok);
        CHECK(GetConfiguration(document, child, DEFAULT_CONFIGURATION)
                  .value(QStringLiteral("outputs"))
                  .toArray()
                  .at(0)
                  .toObject()
                  .value(QStringLiteral("source"))
                  .toString()
                  .isEmpty() == true);
        const QJsonArray edges =
            GetConfiguration(document, QStringLiteral("caller.lurender.json"), DEFAULT_CONFIGURATION)
                .value(QStringLiteral("edges"))
                .toArray();
        CHECK(edges.size() == 3);
    }
}

TEST_CASE("interface repair preview bounds detail text without truncating the transaction",
          "[editor][render-graph][interface]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeInterfacePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString child = QStringLiteral("child.lurender.json");
    QJsonObject graph;
    REQUIRE(document.GetGraph(child, graph) == true);
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    QJsonObject record = configurations.at(0).toObject();
    QJsonArray edges;
    for (int32 index = 0; index < RenderGraphDocument::MAX_EDGES; ++index)
    {
        edges.append(QJsonObject{{QStringLiteral("from"), QStringLiteral("input.color")},
                                 {QStringLiteral("to"), QStringLiteral("work.color")}});
    }
    record.insert(QStringLiteral("edges"), edges);
    configurations.replace(0, record);
    graph.insert(QStringLiteral("configurations"), configurations);
    REQUIRE(document.SetText(child, EncodeCanonicalJson(graph)) == RenderGraphDocumentStatus::Ok);
    QJsonArray inputs = record.value(QStringLiteral("inputs")).toArray();
    inputs.removeAt(0);
    const QJsonArray outputs = record.value(QStringLiteral("outputs")).toArray();
    const QMap<QString, QByteArray> original = ReadDrafts(document);
    QStringList repairs;
    QString error;
    REQUIRE(document.PreviewConfigurationInterface(child, DEFAULT_CONFIGURATION, inputs, outputs, repairs, error) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(repairs.size() == 129);
    CHECK(repairs.last().contains(QStringLiteral("additional repairs")) == true);
    CHECK(ReadDrafts(document) == original);
    REQUIRE(document.ReplaceConfigurationInterface(child, DEFAULT_CONFIGURATION, inputs, outputs, error) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(GetConfiguration(document, child, DEFAULT_CONFIGURATION).value(QStringLiteral("edges")).toArray().isEmpty() ==
          true);
    REQUIRE(document.Undo() == true);
    CHECK(ReadDrafts(document) == original);
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
    CHECK(document.GetFiles().size() == initialFiles + 2);
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
    CHECK(document.GetFiles().size() == initialFiles + 2);
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

namespace
{
QJsonObject MakeCompleteRayPipeline(const QString& identity)
{
    QJsonObject node = MakePipelineNode(QStringLiteral("RayTracingPipeline"), identity);
    QJsonArray shaders;
    for (const QString& stage :
         {QStringLiteral("ray_generation"), QStringLiteral("miss"), QStringLiteral("closest_hit")})
    {
        shaders.append(QJsonObject{{QStringLiteral("id"), stage},
                                   {QStringLiteral("stage"), stage},
                                   {QStringLiteral("program"), QStringLiteral("Program")}});
    }
    node.insert(QStringLiteral("shaders"), shaders);
    return node;
}
QJsonObject GetRayTechnique(const RenderGraphDocument& document)
{
    const QJsonObject manifest =
        QJsonDocument::fromJson(document.GetDraft(QStringLiteral("main.lushader.json"))).object();
    for (const QJsonValueConstRef value : manifest.value(QStringLiteral("techniques")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")) == QStringLiteral("RendererTriangles.RayTracing"))
        {
            return value.toObject();
        }
    }
    return {};
}
} // namespace

TEST_CASE("completing an imported ray pipeline registers its technique in the same undo step",
          "[editor][render-graph][pipeline][technique]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject node = MakeCompleteRayPipeline(QStringLiteral("trace"));
    REQUIRE(document.AddNode(LIBRARY,
                             QStringLiteral("ColorAndDepth"),
                             MakePipelineNode(QStringLiteral("RayTracingPipeline"), QStringLiteral("trace"))) ==
            RenderGraphDocumentStatus::Ok);
    const QByteArray originalManifest = document.GetDraft(QStringLiteral("main.lushader.json"));
    const QStringList originalFiles = document.GetFiles();
    const uint64 revision = document.GetRevision();
    REQUIRE(document.ApplyPipeline(LIBRARY, QStringLiteral("ColorAndDepth"), node) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetRevision() == revision + 1);
    const QJsonObject technique = GetRayTechnique(document);
    REQUIRE(technique.isEmpty() == false);
    CHECK(technique.value(QStringLiteral("configuration")) == QStringLiteral("RayTracing"));
    constexpr int32 TRIANGLE_ATTRIBUTE_BYTES = 8;
    CHECK(technique.value(QStringLiteral("recipes"))
              .toArray()
              .at(0)
              .toObject()
              .value(QStringLiteral("attribute_bytes"))
              .toInt() == TRIANGLE_ATTRIBUTE_BYTES);
    const QJsonArray nodes =
        GetConfiguration(document, LIBRARY, QStringLiteral("ColorAndDepth")).value(QStringLiteral("nodes")).toArray();
    node = nodes.last().toObject();
    for (const QJsonValueConstRef shader : node.value(QStringLiteral("shaders")).toArray())
    {
        CHECK(technique.value(shader.toObject().value(QStringLiteral("stage")).toString()) ==
              shader.toObject().value(QStringLiteral("program")));
    }
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(QStringLiteral("main.lushader.json")) == originalManifest);
    CHECK(document.GetFiles() == originalFiles);
    REQUIRE(document.Redo() == true);
    CHECK(GetRayTechnique(document) == technique);
    QString error;
    const uint64 registeredRevision = document.GetRevision();
    CHECK(document.PrepareTechniques(error) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetRevision() == registeredRevision);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reloaded;
    REQUIRE(reloaded.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(GetRayTechnique(reloaded) == technique);
}

TEST_CASE("pipeline technique conflicts and budgets preserve the complete draft",
          "[editor][render-graph][pipeline][technique]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    QJsonObject node = MakeCompleteRayPipeline(QStringLiteral("trace"));
    REQUIRE(document.AddNode(ROOT_GRAPH,
                             DEFAULT_CONFIGURATION,
                             MakePipelineNode(QStringLiteral("RayTracingPipeline"), QStringLiteral("trace"))) ==
            RenderGraphDocumentStatus::Ok);
    QString error;
    SECTION("a second pipeline cannot silently replace a shared technique")
    {
        QJsonObject manifestBudget =
            QJsonDocument::fromJson(document.GetDraft(QStringLiteral("main.lushader.json"))).object();
        QJsonObject budgets = manifestBudget.value(QStringLiteral("budgets")).toObject();
        constexpr int32 SHARED_PROGRAM_BUDGET = 16;
        budgets.insert(QStringLiteral("programs"), SHARED_PROGRAM_BUDGET);
        manifestBudget.insert(QStringLiteral("budgets"), budgets);
        REQUIRE(document.SetText(QStringLiteral("main.lushader.json"), EncodeCanonicalJson(manifestBudget)) ==
                RenderGraphDocumentStatus::Ok);
        REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
        node = MakeCompleteRayPipeline(QStringLiteral("otherTrace"));
        REQUIRE(document.AddNode(ROOT_GRAPH,
                                 DEFAULT_CONFIGURATION,
                                 MakePipelineNode(QStringLiteral("RayTracingPipeline"),
                                                  QStringLiteral("otherTrace"))) == RenderGraphDocumentStatus::Ok);
        const QStringList files = document.GetFiles();
        const QByteArray manifest = document.GetDraft(QStringLiteral("main.lushader.json"));
        const uint64 revision = document.GetRevision();
        CHECK(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node, &error) ==
              RenderGraphDocumentStatus::Conflict);
        CHECK(error.contains(QStringLiteral("trace")) == true);
        CHECK(error.contains(QStringLiteral("otherTrace")) == true);
        CHECK(error.contains(QStringLiteral("RendererTriangles.RayTracing")) == true);
        CHECK(document.GetRevision() == revision);
        CHECK(document.GetFiles() == files);
        CHECK(document.GetDraft(QStringLiteral("main.lushader.json")) == manifest);
    }
    SECTION("a technique cannot exceed the manifest authoring budget")
    {
        QJsonObject manifest =
            QJsonDocument::fromJson(document.GetDraft(QStringLiteral("main.lushader.json"))).object();
        QJsonObject budgets = manifest.value(QStringLiteral("budgets")).toObject();
        budgets.insert(QStringLiteral("configurations"), manifest.value(QStringLiteral("techniques")).toArray().size());
        manifest.insert(QStringLiteral("budgets"), budgets);
        REQUIRE(document.SetText(QStringLiteral("main.lushader.json"), EncodeCanonicalJson(manifest)) ==
                RenderGraphDocumentStatus::Ok);
        const QStringList files = document.GetFiles();
        const uint64 revision = document.GetRevision();
        CHECK(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node, &error) ==
              RenderGraphDocumentStatus::Capacity);
        CHECK(error.contains(QStringLiteral("budget")) == true);
        CHECK(document.GetRevision() == revision);
        CHECK(document.GetFiles() == files);
    }
}

TEST_CASE("graphics pipelines register missing techniques and retain authored recipes",
          "[editor][render-graph][pipeline][technique]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString configuration = QStringLiteral("ColorAndDepth");
    const QJsonObject original =
        GetConfiguration(document, LIBRARY, configuration).value(QStringLiteral("nodes")).toArray().first().toObject();
    QJsonObject manifest = QJsonDocument::fromJson(document.GetDraft(QStringLiteral("main.lushader.json"))).object();
    QJsonObject node;
    REQUIRE(MigrateRasterNode(original, manifest, node) == true);
    QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
    techniques.removeFirst();
    manifest.insert(QStringLiteral("techniques"), techniques);
    REQUIRE(document.SetText(QStringLiteral("main.lushader.json"), EncodeCanonicalJson(manifest)) ==
            RenderGraphDocumentStatus::Ok);
    REQUIRE(document.ApplyPipeline(LIBRARY, configuration, node) == RenderGraphDocumentStatus::Ok);
    manifest = QJsonDocument::fromJson(document.GetDraft(QStringLiteral("main.lushader.json"))).object();
    techniques = manifest.value(QStringLiteral("techniques")).toArray();
    REQUIRE(techniques.size() == 2);
    const QJsonObject registered = techniques.last().toObject();
    CHECK(registered.value(QStringLiteral("id")) == QStringLiteral("DefaultStaticOpaque.ColorAndDepth"));
    CHECK(registered.value(QStringLiteral("vertex")) == QStringLiteral("Geometry"));
    CHECK(registered.value(QStringLiteral("fragment")) == QStringLiteral("Color"));
    QJsonObject state = node.value(QStringLiteral("state")).toObject();
    state.insert(QStringLiteral("cull"), QStringLiteral("Back"));
    node.insert(QStringLiteral("state"), state);
    REQUIRE(document.ApplyPipeline(LIBRARY, configuration, node) == RenderGraphDocumentStatus::Ok);
    manifest = QJsonDocument::fromJson(document.GetDraft(QStringLiteral("main.lushader.json"))).object();
    const QJsonArray recipes = manifest.value(QStringLiteral("techniques"))
                                   .toArray()
                                   .last()
                                   .toObject()
                                   .value(QStringLiteral("recipes"))
                                   .toArray();
    REQUIRE(recipes.size() == 2);
    CHECK(recipes.first() == registered.value(QStringLiteral("recipes")).toArray().first());
    CHECK(recipes.last().toObject().value(QStringLiteral("cull")) == QStringLiteral("Back"));
    CHECK(recipes.first().toObject().value(QStringLiteral("id")) !=
          recipes.last().toObject().value(QStringLiteral("id")));
}

TEST_CASE("new ray shaders expose scene materials without manifest edits",
          "[editor][render-graph][pipeline][scene-materials]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QJsonObject node = MakeCompleteRayPipeline(QStringLiteral("materials"));
    REQUIRE(document.AddNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    const QStringList originalFiles = document.GetFiles();
    REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, node) == RenderGraphDocumentStatus::Ok);
    const QJsonObject technique = GetRayTechnique(document);
    const QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
    CHECK(document.GetDraft(interface.value(QStringLiteral("payload")).toString()).contains("float3 Radiance;") ==
          true);
    const QByteArray manifest = document.GetDraft(QStringLiteral("main.lushader.json"));
    for (const QJsonValueConstRef value :
         QJsonDocument::fromJson(manifest).object().value(QStringLiteral("programs")).toArray())
    {
        const QJsonObject program = value.toObject();
        const QString stage = program.value(QStringLiteral("stage")).toString();
        if (technique.value(stage) != program.value(QStringLiteral("id")))
        {
            continue;
        }
        const QString source = program.value(QStringLiteral("source")).toString();
        const QByteArray text = document.GetDraft(source);
        CHECK(text.contains("SceneContext scene") == true);
        const QString description = DescribeShaderInterface(manifest, source);
        CHECK(description.contains(QStringLiteral("Scene materials enabled")) == true);
        CHECK(description.contains(QStringLiteral("Legacy ray interface")) == false);
        if (stage == QStringLiteral("closest_hit"))
        {
            CHECK(program.value(QStringLiteral("input")) == QStringLiteral("SurfaceHit"));
            CHECK(text.contains("EvaluateMaterial(scene, hit)") == true);
            CHECK(text.contains("material.BaseColor") == true);
        }
        if (stage == QStringLiteral("ray_generation"))
        {
            CHECK(program.value(QStringLiteral("output")) == QStringLiteral("Radiance"));
            CHECK(text.contains("TraceAndShade(scene, queries, launch.CameraRay, payload)") == true);
        }
    }
    SECTION("other ray stages remain authoring drafts")
    {
        QJsonObject pipeline = GetNode(document, QStringLiteral("materials"));
        QJsonArray shaders = pipeline.value(QStringLiteral("shaders")).toArray();
        shaders.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("anyHit")},
                                   {QStringLiteral("stage"), QStringLiteral("any_hit")},
                                   {QStringLiteral("program"), QStringLiteral("Program")}});
        pipeline.insert(QStringLiteral("shaders"), shaders);
        REQUIRE(document.ApplyPipeline(ROOT_GRAPH, DEFAULT_CONFIGURATION, pipeline) == RenderGraphDocumentStatus::Ok);
        const QJsonObject program = QJsonDocument::fromJson(document.GetDraft(QStringLiteral("main.lushader.json")))
                                        .object()
                                        .value(QStringLiteral("programs"))
                                        .toArray()
                                        .last()
                                        .toObject();
        CHECK(program.value(QStringLiteral("input")).toString().isEmpty() == true);
        CHECK(document.GetDraft(program.value(QStringLiteral("source")).toString()).contains("Authoring draft") ==
              true);
        REQUIRE(document.Undo() == true);
    }
    REQUIRE(document.Undo() == true);
    CHECK(document.GetFiles() == originalFiles);
    REQUIRE(document.Redo() == true);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument loaded;
    REQUIRE(loaded.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    CHECK(GetRayTechnique(loaded) == technique);
}

TEST_CASE("legacy ray conversion preserves authored callbacks and is atomic",
          "[editor][render-graph][pipeline][scene-materials]")
{
    QTemporaryDir project;
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/raytracing"));
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    for (const QString& file : {ROOT_GRAPH,
                                QStringLiteral("main.lushader.json.in"),
                                QStringLiteral("main_shaders/geometry.slang"),
                                QStringLiteral("main_shaders/color.slang"),
                                QStringLiteral("main_shaders/depth.slang"),
                                QStringLiteral("main_shaders/generate.slang"),
                                QStringLiteral("main_shaders/miss.slang"),
                                QStringLiteral("main_shaders/closest.slang")})
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
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QString manifestFile = QStringLiteral("main.lushader.json");
    const QByteArray originalManifest = document.GetDraft(manifestFile);
    const QByteArray originalGraph = document.GetDraft(ROOT_GRAPH);
    const QStringList originalFiles = document.GetFiles();
    QMap<QString, QByteArray> originalSources;
    for (const QString& file : originalFiles)
    {
        if (file.endsWith(QStringLiteral(".slang")) == true)
        {
            originalSources.insert(file, document.GetDraft(file));
        }
    }
    CHECK(DescribeShaderInterface(originalManifest, QStringLiteral("main_shaders/closest.slang"))
              .contains(QStringLiteral("material tint: return input.Color.xyz")) == true);
    QString error;
    const uint64 revision = document.GetRevision();
    SECTION("conversion")
    {
        const QJsonObject manifestBefore = QJsonDocument::fromJson(originalManifest).object();
        REQUIRE(document.EnableSceneMaterials(error) == RenderGraphDocumentStatus::Ok);
        CHECK(error.isEmpty() == true);
        CHECK(document.GetRevision() == revision + 1);
        const QJsonObject technique = GetRayTechnique(document);
        CHECK(technique.contains(QStringLiteral("scene_interface")) == true);
        CHECK(technique.value(QStringLiteral("recipes")).toArray().size() == 1);
        for (QMap<QString, QByteArray>::const_iterator file = originalSources.constBegin();
             file != originalSources.constEnd();
             ++file)
        {
            CHECK(document.GetDraft(file.key()) == file.value());
        }
        const QByteArray converted = document.GetDraft(manifestFile);
        const QJsonObject manifestAfter = QJsonDocument::fromJson(converted).object();
        CHECK(manifestAfter.value(QStringLiteral("budgets")) == manifestBefore.value(QStringLiteral("budgets")));
        CHECK(manifestAfter.value(QStringLiteral("programs")).toArray().size() ==
              manifestBefore.value(QStringLiteral("programs")).toArray().size());
        CHECK(document.GetDraft(ROOT_GRAPH) == originalGraph);
        for (const QJsonValueConstRef value :
             QJsonDocument::fromJson(converted).object().value(QStringLiteral("programs")).toArray())
        {
            const QJsonObject program = value.toObject();
            const QString stage = program.value(QStringLiteral("stage")).toString();
            if (technique.value(stage) == program.value(QStringLiteral("id")))
            {
                const QByteArray adapter = document.GetDraft(program.value(QStringLiteral("source")).toString());
                CHECK(adapter.contains("SceneShaderMain") == true);
                if (stage == QStringLiteral("closest_hit"))
                {
                    CHECK(adapter.contains("input.Primitive = hit.TriangleIndex") == true);
                    CHECK(adapter.contains("ShadeClosest(input)") == true);
                }
            }
        }
        REQUIRE(document.EnableSceneMaterials(error) == RenderGraphDocumentStatus::Ok);
        CHECK(document.GetRevision() == revision + 1);
        REQUIRE(document.Undo() == true);
        CHECK(document.GetDraft(manifestFile) == originalManifest);
        CHECK(document.GetDraft(ROOT_GRAPH) == originalGraph);
        CHECK(document.GetFiles() == originalFiles);
        REQUIRE(document.Redo() == true);
        CHECK(document.GetDraft(manifestFile) == converted);
        QStringList saved;
        REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
        RenderGraphDocument loaded;
        REQUIRE(loaded.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
        CHECK(loaded.GetDraft(manifestFile) == converted);
    }
    SECTION("legacy technique repair preserves callback bytes")
    {
        const QJsonObject technique = GetRayTechnique(document);
        QJsonObject manifest = QJsonDocument::fromJson(originalManifest).object();
        QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
        techniques.removeLast();
        manifest.insert(QStringLiteral("techniques"), techniques);
        REQUIRE(document.SetText(manifestFile, EncodeCanonicalJson(manifest)) == RenderGraphDocumentStatus::Ok);
        const uint64 beforeRepair = document.GetRevision();
        REQUIRE(document.PrepareTechniques(error) == RenderGraphDocumentStatus::Ok);
        CHECK(document.GetRevision() == beforeRepair + 1);
        const QJsonObject repaired = GetRayTechnique(document);
        CHECK(repaired.value(QStringLiteral("ray_generation")) == technique.value(QStringLiteral("ray_generation")));
        CHECK(repaired.value(QStringLiteral("miss")) == technique.value(QStringLiteral("miss")));
        CHECK(repaired.value(QStringLiteral("closest_hit")) == technique.value(QStringLiteral("closest_hit")));
        CHECK(repaired.contains(QStringLiteral("scene_interface")) == false);
        CHECK(document.GetDraft(ROOT_GRAPH) == originalGraph);
        for (QMap<QString, QByteArray>::const_iterator file = originalSources.constBegin();
             file != originalSources.constEnd();
             ++file)
        {
            CHECK(document.GetDraft(file.key()) == file.value());
        }
        REQUIRE(document.Undo() == true);
        CHECK(GetRayTechnique(document).isEmpty() == true);
    }
    SECTION("malformed callback preserves every file")
    {
        QJsonObject manifest = QJsonDocument::fromJson(originalManifest).object();
        QJsonArray programs = manifest.value(QStringLiteral("programs")).toArray();
        for (qsizetype index = 0; index < programs.size(); ++index)
        {
            QJsonObject program = programs.at(index).toObject();
            if (program.value(QStringLiteral("stage")).toString() == QStringLiteral("closest_hit"))
            {
                program.insert(QStringLiteral("input"), QStringLiteral("Ray"));
                programs.replace(index, program);
            }
        }
        manifest.insert(QStringLiteral("programs"), programs);
        const QByteArray invalidManifest = EncodeCanonicalJson(manifest);
        REQUIRE(document.SetText(manifestFile, invalidManifest) == RenderGraphDocumentStatus::Ok);
        const uint64 invalidRevision = document.GetRevision();
        REQUIRE(document.EnableSceneMaterials(error) == RenderGraphDocumentStatus::InvalidEdit);
        CHECK(error.isEmpty() == false);
        CHECK(document.GetRevision() == invalidRevision);
        CHECK(document.GetDraft(manifestFile) == invalidManifest);
        CHECK(document.GetDraft(ROOT_GRAPH) == originalGraph);
        CHECK(document.GetFiles() == originalFiles);
    }
    SECTION("conversion reuses full program and artifact budgets")
    {
        QJsonObject manifest = QJsonDocument::fromJson(originalManifest).object();
        const QJsonArray programs = manifest.value(QStringLiteral("programs")).toArray();
        QJsonObject budgets = manifest.value(QStringLiteral("budgets")).toObject();
        budgets.insert(QStringLiteral("programs"), programs.size());
        budgets.insert(QStringLiteral("artifacts"),
                       programs.size() * manifest.value(QStringLiteral("profiles")).toArray().size());
        manifest.insert(QStringLiteral("budgets"), budgets);
        const QByteArray fullManifest = EncodeCanonicalJson(manifest);
        REQUIRE(document.SetText(manifestFile, fullManifest) == RenderGraphDocumentStatus::Ok);
        const uint64 conversionRevision = document.GetRevision();
        REQUIRE(document.EnableSceneMaterials(error) == RenderGraphDocumentStatus::Ok);
        CHECK(error.isEmpty() == true);
        CHECK(document.GetRevision() == conversionRevision + 1);
        const QJsonObject converted = QJsonDocument::fromJson(document.GetDraft(manifestFile)).object();
        CHECK(converted.value(QStringLiteral("budgets")).toObject() == budgets);
        const QJsonArray convertedPrograms = converted.value(QStringLiteral("programs")).toArray();
        REQUIRE(convertedPrograms.size() == programs.size());
        for (qsizetype index = 0; index < programs.size(); ++index)
        {
            CHECK(convertedPrograms.at(index).toObject().value(QStringLiteral("id")) ==
                  programs.at(index).toObject().value(QStringLiteral("id")));
        }
        CHECK(document.GetDraft(ROOT_GRAPH) == originalGraph);
        for (QMap<QString, QByteArray>::const_iterator file = originalSources.constBegin();
             file != originalSources.constEnd();
             ++file)
        {
            CHECK(document.GetDraft(file.key()) == file.value());
        }
        REQUIRE(document.Undo() == true);
        CHECK(document.GetDraft(manifestFile) == fullManifest);
        CHECK(document.GetFiles() == originalFiles);
        REQUIRE(document.Redo() == true);
        CHECK(QJsonDocument::fromJson(document.GetDraft(manifestFile)).object() == converted);
        QStringList saved;
        REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
        RenderGraphDocument loaded;
        REQUIRE(loaded.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
        CHECK(QJsonDocument::fromJson(loaded.GetDraft(manifestFile)).object() == converted);
    }
}
