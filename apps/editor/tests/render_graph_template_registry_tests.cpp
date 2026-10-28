// The built-in template catalog and the pickers that read it.
#include "internal/render_graph_template_registry.h"
#include "internal/render_graph_upscale_node.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVector>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
QByteArray ReadCatalog()
{
    QFile file(QStringLiteral(":/render-graph/render_graph_templates.json"));
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

QStringList GetIdentities(const QVector<RenderGraphTemplate>& templates)
{
    QStringList identities;
    for (const RenderGraphTemplate& entry : templates)
    {
        identities.append(entry.Identity);
    }
    return identities;
}

/// A minimal valid catalog with one entry of each kind; cases replace one entry to break it.
QJsonObject MakeCatalog(const QJsonObject& replacement = {}, qsizetype index = -1)
{
    QJsonArray templates{
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("graph")},
                    {QStringLiteral("identity"), QStringLiteral("raster")},
                    {QStringLiteral("label"), QStringLiteral("Raster")},
                    {QStringLiteral("category"), QStringLiteral("raster")},
                    {QStringLiteral("description"), QStringLiteral("Raster graph.")}},
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("snippet")},
                    {QStringLiteral("identity"), QStringLiteral("visibility-buffer")},
                    {QStringLiteral("label"), QStringLiteral("Visibility")},
                    {QStringLiteral("category"), QStringLiteral("visibility")},
                    {QStringLiteral("defaultConfiguration"), QStringLiteral("VisibilityBuffer")},
                    {QStringLiteral("description"), QStringLiteral("Visibility snippet.")}},
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("node")},
                    {QStringLiteral("identity"), QStringLiteral("Upscale")},
                    {QStringLiteral("algorithm"), QStringLiteral("fsr-2")},
                    {QStringLiteral("isDraft"), true},
                    {QStringLiteral("category"), QStringLiteral("upscaling")}},
    };
    if (index >= 0)
    {
        templates.replace(index, replacement);
    }
    return QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("templates"), templates}};
}

QByteArray Encode(const QJsonObject& catalog)
{
    return QJsonDocument(catalog).toJson(QJsonDocument::Compact);
}
} // namespace

TEST_CASE("the built-in template catalog keeps the editor's existing template lists",
          "[editor][render-graph][templates]")
{
    RenderGraphTemplateRegistry registry;
    QString error;
    REQUIRE(RenderGraphTemplateRegistry::Parse(ReadCatalog(), registry, error) == true);
    CAPTURE(error.toStdString());
    CHECK(RenderGraphTemplateRegistry::GetBuiltIn().IsEmpty() == false);

    const QVector<RenderGraphTemplate> graphs = registry.GetTemplates(RenderGraphTemplateKind::Graph);
    CHECK(GetIdentities(graphs) == QStringList{QStringLiteral("raster"),
                                               QStringLiteral("forward-lit"),
                                               QStringLiteral("visibility-buffer"),
                                               QStringLiteral("visibility-secondary-rays"),
                                               QStringLiteral("restir-di"),
                                               QStringLiteral("tiled-vbuffer-nee"),
                                               QStringLiteral("compute-ray-tracing"),
                                               QStringLiteral("simple-compute-ray-tracing"),
                                               QStringLiteral("veach-guibas-mis"),
                                               QStringLiteral("line-sampling"),
                                               QStringLiteral("salesin-jarosz-lines")});
    CHECK(graphs.at(0).Label == QStringLiteral("Raster"));
    CHECK(graphs.at(4).Label == QStringLiteral("ReSTIR DI — mesh emitter lighting"));
    CHECK(graphs.at(6).Category == QStringLiteral("ray-traced"));
    for (const RenderGraphTemplate& graph : graphs)
    {
        CAPTURE(graph.Identity.toStdString());
        CHECK(graph.Description.isEmpty() == false);
        CHECK(registry.HasTemplate(RenderGraphTemplateKind::Graph, graph.Identity) == true);
        const bool isRayTraced = graph.Identity == QStringLiteral("restir-di") ||
                                 graph.Identity == QStringLiteral("visibility-secondary-rays") ||
                                 graph.Identity == QStringLiteral("tiled-vbuffer-nee") ||
                                 graph.Identity == QStringLiteral("compute-ray-tracing") ||
                                 graph.Identity == QStringLiteral("simple-compute-ray-tracing") ||
                                 graph.Identity == QStringLiteral("veach-guibas-mis");
        CHECK(graph.Requirements.contains(QStringLiteral("native-ray-tracing")) == isRayTraced);
        // Triangle-loop templates run on Metal or Vulkan compute and must not claim ray hardware.
        CHECK(graph.Requirements.contains(QStringLiteral("compute-ray-tracing")) ==
              (graph.Identity == QStringLiteral("line-sampling") ||
               graph.Identity == QStringLiteral("salesin-jarosz-lines")));
    }
    CHECK(GetRenderGraphTemplateRequirementLabel(QStringLiteral("compute-ray-tracing")) ==
          QStringLiteral("Metal or Vulkan compute"));
    CHECK(GetRenderGraphTemplateRequirementLabel(QStringLiteral("native-ray-tracing")) ==
          QStringLiteral("Native ray tracing"));

    const QVector<RenderGraphTemplate> snippets = registry.GetTemplates(RenderGraphTemplateKind::Snippet);
    CHECK(GetIdentities(snippets) == QStringList{QStringLiteral("restir-di"),
                                                 QStringLiteral("tiled-vbuffer-nee"),
                                                 QStringLiteral("compute-ray-tracing"),
                                                 QStringLiteral("simple-compute-ray-tracing"),
                                                 QStringLiteral("veach-guibas-mis"),
                                                 QStringLiteral("line-sampling"),
                                                 QStringLiteral("salesin-jarosz-lines"),
                                                 QStringLiteral("visibility-buffer"),
                                                 QStringLiteral("visibility-secondary-rays")});
    QStringList defaults;
    for (const RenderGraphTemplate& snippet : snippets)
    {
        defaults.append(snippet.DefaultConfiguration);
    }
    CHECK(defaults == QStringList{QStringLiteral("RestirDi"),
                                  QStringLiteral("TiledVbufferNee"),
                                  QStringLiteral("ComputeRayTracing"),
                                  QStringLiteral("SimpleComputeRayTracing"),
                                  QStringLiteral("VeachGuibasMis"),
                                  QStringLiteral("LineSampling"),
                                  QStringLiteral("PointLineMis"),
                                  QStringLiteral("VisibilityBuffer"),
                                  QStringLiteral("VisibilitySecondaryRays")});
    CHECK(registry.HasTemplate(RenderGraphTemplateKind::Snippet, QStringLiteral("raster")) == false);

    // Upscale presets follow the plain node kinds, once per algorithm, in GetUpscaleAlgorithms() order.
    const QVector<RenderGraphTemplate> nodes = registry.GetTemplates(RenderGraphTemplateKind::Node);
    QStringList kinds;
    QStringList algorithms;
    for (const RenderGraphTemplate& node : nodes)
    {
        // Quick Add explains and finds every node by its description and keywords.
        CAPTURE(node.Identity.toStdString(), node.Algorithm.toStdString());
        CHECK(node.Description.isEmpty() == false);
        CHECK(node.Keywords.isEmpty() == false);
        if (node.Algorithm.isEmpty() == true)
        {
            CHECK(node.Label == node.Identity);
            kinds.append(node.Identity);
            continue;
        }
        CHECK(node.Identity == QStringLiteral("Upscale"));
        const bool isExecutable =
            node.Algorithm == QStringLiteral("bilinear") || node.Algorithm == QStringLiteral("metalfx-spatial");
        CHECK(node.IsDraft == (isExecutable == false));
        CHECK(node.Label ==
              (isExecutable == true ? QStringLiteral("Upscale — %1") : QStringLiteral("Upscale — %1 (draft)"))
                  .arg(GetUpscaleLabel(node.Algorithm)));
        algorithms.append(node.Algorithm);
    }
    CHECK(kinds == QStringList{QStringLiteral("Scene"),
                               QStringLiteral("Camera"),
                               QStringLiteral("Target"),
                               QStringLiteral("Buffer"),
                               QStringLiteral("StructuredBuffer"),
                               QStringLiteral("OutputSize"),
                               QStringLiteral("RenderSize"),
                               QStringLiteral("GraphicsPipeline"),
                               QStringLiteral("ComputePipeline"),
                               QStringLiteral("RayTracingPipeline"),
                               QStringLiteral("Accumulate"),
                               QStringLiteral("ToneMap"),
                               QStringLiteral("Denoise"),
                               QStringLiteral("Subgraph"),
                               QStringLiteral("Export"),
                               QStringLiteral("Present")});
    CHECK(algorithms == GetUpscaleAlgorithms());
}

TEST_CASE("the Add Node list shows every catalog node in catalog order", "[editor][render-graph][templates]")
{
    RendererWorkspace workspace({});
    QComboBox* kindsOrNull = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    REQUIRE(kindsOrNull != nullptr);
    const QVector<RenderGraphTemplate> nodes =
        RenderGraphTemplateRegistry::GetBuiltIn().GetTemplates(RenderGraphTemplateKind::Node);
    REQUIRE(kindsOrNull->count() == nodes.size());
    for (int32 i = 0; i < kindsOrNull->count(); ++i)
    {
        CAPTURE(i);
        CHECK(kindsOrNull->itemText(i) == nodes.at(i).Label);
        // Plain kinds carry no item data; addNode treats data as an Upscale algorithm.
        CHECK(kindsOrNull->itemData(i).toString() == nodes.at(i).Algorithm);
        CHECK(kindsOrNull->itemData(i, Qt::ToolTipRole).isValid() ==
              (nodes.at(i).Identity == QStringLiteral("Denoise")));
    }
}

TEST_CASE("template catalogs reject entries the pickers cannot offer", "[editor][render-graph][templates]")
{
    RenderGraphTemplateRegistry registry;
    QString error;
    REQUIRE(RenderGraphTemplateRegistry::Parse(Encode(MakeCatalog()), registry, error) == true);
    CHECK(registry.GetTemplates(RenderGraphTemplateKind::Node).at(0).Label ==
          QStringLiteral("Upscale — FSR 2 temporal (draft)"));

    const QJsonObject graph = MakeCatalog().value(QStringLiteral("templates")).toArray().at(0).toObject();
    const QJsonObject snippet = MakeCatalog().value(QStringLiteral("templates")).toArray().at(1).toObject();
    const QJsonObject upscale = MakeCatalog().value(QStringLiteral("templates")).toArray().at(2).toObject();
    const auto with = [](QJsonObject entry, const QString& field, const QJsonValue& value) {
        entry.insert(field, value);
        return entry;
    };
    const auto without = [](QJsonObject entry, const QString& field) {
        entry.remove(field);
        return entry;
    };
    struct Case final
    {
        const char* Name;
        QByteArray Source;
        QString Message;
    };
    QJsonObject twoGraphs = MakeCatalog();
    QJsonArray repeated = twoGraphs.value(QStringLiteral("templates")).toArray();
    repeated.append(graph);
    twoGraphs.insert(QStringLiteral("templates"), repeated);
    QJsonObject wrongVersion = MakeCatalog();
    wrongVersion.insert(QStringLiteral("version"), 2);
    const QVector<Case> cases = {
        {"not JSON", QByteArrayLiteral("{"), QStringLiteral("not a JSON object")},
        {"wrong version", Encode(wrongVersion), QStringLiteral("version 1")},
        {"unknown field",
         Encode(MakeCatalog(with(graph, QStringLiteral("tags"), QJsonArray{}), 0)),
         QStringLiteral("unknown field 'tags'")},
        {"unknown kind",
         Encode(MakeCatalog(with(graph, QStringLiteral("kind"), QStringLiteral("pass")), 0)),
         QStringLiteral("kind of graph")},
        {"bad graph identity",
         Encode(MakeCatalog(with(graph, QStringLiteral("identity"), QStringLiteral("Raster")), 0)),
         QStringLiteral("lowercase identity")},
        {"graph category",
         Encode(MakeCatalog(with(graph, QStringLiteral("category"), QStringLiteral("lighting")), 0)),
         QStringLiteral("category from")},
        {"missing description",
         Encode(MakeCatalog(without(graph, QStringLiteral("description")), 0)),
         QStringLiteral("label and a description")},
        {"graph default configuration",
         Encode(MakeCatalog(with(graph, QStringLiteral("defaultConfiguration"), QStringLiteral("Main")), 0)),
         QStringLiteral("only snippets")},
        {"snippet without default",
         Encode(MakeCatalog(without(snippet, QStringLiteral("defaultConfiguration")), 1)),
         QStringLiteral("defaultConfiguration")},
        {"unknown requirement",
         Encode(MakeCatalog(with(snippet, QStringLiteral("requirements"), QJsonArray{QStringLiteral("cuda")}), 1)),
         QStringLiteral("requirements")},
        {"repeated requirement",
         Encode(MakeCatalog(with(snippet,
                                 QStringLiteral("requirements"),
                                 QJsonArray{QStringLiteral("denoiser"), QStringLiteral("denoiser")}),
                            1)),
         QStringLiteral("distinct")},
        {"node label",
         Encode(MakeCatalog(with(upscale, QStringLiteral("label"), QStringLiteral("Upscale")), 2)),
         QStringLiteral("labelled by their kind")},
        {"upscale without algorithm",
         Encode(MakeCatalog(without(upscale, QStringLiteral("algorithm")), 2)),
         QStringLiteral("needs an algorithm")},
        {"unknown algorithm",
         Encode(MakeCatalog(with(upscale, QStringLiteral("algorithm"), QStringLiteral("taa")), 2)),
         QStringLiteral("needs an algorithm")},
        {"algorithm on another node",
         Encode(MakeCatalog(with(upscale, QStringLiteral("identity"), QStringLiteral("ToneMap")), 2)),
         QStringLiteral("only Upscale nodes")},
        {"draft not boolean",
         Encode(MakeCatalog(with(upscale, QStringLiteral("isDraft"), QStringLiteral("yes")), 2)),
         QStringLiteral("true or false")},
        {"empty node description",
         Encode(MakeCatalog(with(upscale, QStringLiteral("description"), QStringLiteral(" ")), 2)),
         QStringLiteral("description must be non-empty")},
        {"uppercase keyword",
         Encode(MakeCatalog(with(graph, QStringLiteral("keywords"), QJsonArray{QStringLiteral("HDR")}), 0)),
         QStringLiteral("lowercase")},
        {"repeated keyword",
         Encode(MakeCatalog(
             with(graph, QStringLiteral("keywords"), QJsonArray{QStringLiteral("hdr"), QStringLiteral("hdr")}),
             0)),
         QStringLiteral("distinct")},
        {"repeated entry", Encode(twoGraphs), QStringLiteral("repeats an earlier entry")},
    };
    for (const Case& test : cases)
    {
        CAPTURE(test.Name);
        RenderGraphTemplateRegistry preserved = registry;
        QString message;
        CHECK(RenderGraphTemplateRegistry::Parse(test.Source, preserved, message) == false);
        CAPTURE(message.toStdString());
        CHECK(message.contains(test.Message) == true);
        // Failure leaves the previous registry in place.
        CHECK(GetIdentities(preserved.GetTemplates(RenderGraphTemplateKind::Graph)) ==
              QStringList{QStringLiteral("raster")});
    }
}
