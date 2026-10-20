// Pipeline authoring regressions run without a Renderer device, including sanitizer builds.
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_pipeline_node.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
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
    CHECK(document.ReplaceNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, removed) == RenderGraphDocumentStatus::InvalidEdit);
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
