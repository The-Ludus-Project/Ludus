#include "internal/render_graph_resources.h"

#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_pipeline_node.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace ludus::editor
{
using namespace foundation;
namespace
{
// A partial draft may contain cycles/import recursion; inference must stay bounded.
constexpr int32 MAX_INFERENCE_STEPS = 4096;
constexpr int32 MAX_INFERENCE_DEPTH = 64;
constexpr int32 RAY_LAUNCH_DIMENSION_COUNT = 3;

struct ResourceContext final
{
    const RenderGraphDocument& Document;
    QJsonObject Configuration;
    const ResourceContext* ParentOrNull = nullptr;
    QJsonObject Bindings;
};

struct ResourceDescription final
{
    QString Type;
    QString Format;
    QJsonArray Extent;
    int32 Samples = 0;
    bool IsPresentation = false;
};

QJsonObject FindRecord(const QJsonArray& records, const QString& identity)
{
    for (const QJsonValueConstRef value : records)
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == identity)
        {
            return value.toObject();
        }
    }
    return {};
}

QString FindSource(const QJsonObject& configuration, const QString& endpoint)
{
    for (const QJsonValueConstRef value : configuration.value(QStringLiteral("edges")).toArray())
    {
        if (value.toObject().value(QStringLiteral("to")).toString() == endpoint)
        {
            return value.toObject().value(QStringLiteral("from")).toString();
        }
    }
    return {};
}

ResourceContext MakeChild(const ResourceContext& parent, const QJsonObject& node)
{
    QJsonObject graph;
    const bool hasGraph = parent.Document.GetGraph(node.value(QStringLiteral("asset")).toString(), graph);
    ResourceContext child{parent.Document,
                          hasGraph == true ? FindRecord(graph.value(QStringLiteral("configurations")).toArray(),
                                                        node.value(QStringLiteral("configuration")).toString())
                                           : QJsonObject{},
                          &parent,
                          {}};
    const QString prefix = node.value(QStringLiteral("id")).toString() + QLatin1Char('.');
    for (const QJsonValueConstRef input : child.Configuration.value(QStringLiteral("inputs")).toArray())
    {
        const QString identity = input.toObject().value(QStringLiteral("id")).toString();
        child.Bindings.insert(identity, FindSource(parent.Configuration, prefix + identity));
    }
    return child;
}

bool HasKnownExtent(const QJsonArray& extent)
{
    if (extent.size() != 2)
    {
        return false;
    }
    for (const QJsonValueConstRef dimension : extent)
    {
        if (dimension.isDouble() == false || dimension.toDouble() <= 0 ||
            dimension.toDouble() != static_cast<float64>(dimension.toInteger()))
        {
            return false;
        }
    }
    return true;
}

ResourceDescription
ResolveResource(const ResourceContext& context, const QString& endpoint, int32& inoutRemainingSteps, int32 depth = 0)
{
    if (inoutRemainingSteps <= 0 || depth >= MAX_INFERENCE_DEPTH || endpoint.count(QLatin1Char('.')) != 1)
    {
        return {};
    }
    --inoutRemainingSteps;
    const QString identity = endpoint.section(QLatin1Char('.'), 0, 0);
    const QString port = endpoint.section(QLatin1Char('.'), 1);
    if (identity == QStringLiteral("input"))
    {
        return context.ParentOrNull != nullptr ? ResolveResource(*context.ParentOrNull,
                                                                 context.Bindings.value(port).toString(),
                                                                 inoutRemainingSteps,
                                                                 depth + 1)
                                               : ResourceDescription{};
    }
    const QJsonObject node = FindRecord(context.Configuration.value(QStringLiteral("nodes")).toArray(), identity);
    const QString kind = node.value(QStringLiteral("kind")).toString();
    const QJsonValue extent = node.value(QStringLiteral("extent"));
    if (kind == QStringLiteral("Target") && port == QStringLiteral("texture"))
    {
        ResourceDescription result{node.value(QStringLiteral("type")).toString(),
                                   node.value(QStringLiteral("format")).toString(),
                                   extent.toArray(),
                                   node.value(QStringLiteral("samples")).toInt(),
                                   node.value(QStringLiteral("presentation")).toBool()};
        if (extent.toString() == QStringLiteral("RenderSize"))
        {
            result.Extent = ResolveResource(context,
                                            FindSource(context.Configuration, identity + QStringLiteral(".size")),
                                            inoutRemainingSteps,
                                            depth + 1)
                                .Extent;
        }
        return result;
    }
    if (kind == QStringLiteral("RenderSize") && port == QStringLiteral("size"))
    {
        ResourceDescription result;
        result.Type = QStringLiteral("RenderSize");
        result.Extent = extent.toString() == QStringLiteral("Texture")
                            ? ResolveResource(context,
                                              FindSource(context.Configuration, identity + QStringLiteral(".texture")),
                                              inoutRemainingSteps,
                                              depth + 1)
                                  .Extent
                            : extent.toArray();
        return result;
    }
    if (kind == QStringLiteral("Subgraph"))
    {
        const ResourceContext child = MakeChild(context, node);
        const QJsonObject output = FindRecord(child.Configuration.value(QStringLiteral("outputs")).toArray(), port);
        return ResolveResource(child,
                               output.value(QStringLiteral("source")).toString(),
                               inoutRemainingSteps,
                               depth + 1);
    }
    QString target;
    if (kind == QStringLiteral("Raster") && (port == QStringLiteral("color") || port == QStringLiteral("depth")))
    {
        target = port;
    }
    else if (IsPipelineNode(kind) == true)
    {
        target = FindRecord(node.value(QStringLiteral("outputs")).toArray(), port)
                     .value(QStringLiteral("target"))
                     .toString();
    }
    return ResolveResource(context,
                           FindSource(context.Configuration, identity + QLatin1Char('.') + target),
                           inoutRemainingSteps,
                           depth + 1);
}

QString DescribeTexture(const QString& port, const ResourceDescription& resource)
{
    return QStringLiteral("%1: %2 x %3, %4, %5 samples")
        .arg(port)
        .arg(resource.Extent.at(0).toInteger())
        .arg(resource.Extent.at(1).toInteger())
        .arg(resource.Format)
        .arg(resource.Samples);
}

void CollectProblems(const ResourceContext& context,
                     const QString& prefix,
                     QStringList& inoutProblems,
                     int32& inoutRemainingSteps,
                     int32 nestingDepth = 0)
{
    if (nestingDepth >= MAX_INFERENCE_DEPTH)
    {
        inoutRemainingSteps = 0;
        return;
    }
    for (const QJsonValueConstRef value : context.Configuration.value(QStringLiteral("nodes")).toArray())
    {
        if (inoutRemainingSteps <= 0)
        {
            return;
        }
        --inoutRemainingSteps;
        const QJsonObject node = value.toObject();
        const QString identity = node.value(QStringLiteral("id")).toString();
        const QString kind = node.value(QStringLiteral("kind")).toString();
        const QString where = prefix + QLatin1Char('/') + identity;
        if (kind == QStringLiteral("Subgraph"))
        {
            CollectProblems(MakeChild(context, node), where, inoutProblems, inoutRemainingSteps, nestingDepth + 1);
            continue;
        }
        if (kind != QStringLiteral("Raster") && kind != QStringLiteral("GraphicsPipeline") &&
            kind != QStringLiteral("RayTracingPipeline"))
        {
            continue;
        }
        QJsonArray outputs = node.value(QStringLiteral("outputs")).toArray();
        if (kind == QStringLiteral("Raster"))
        {
            outputs = {QJsonObject{{QStringLiteral("type"), QStringLiteral("DepthTexture")},
                                   {QStringLiteral("target"), QStringLiteral("depth")}}};
            if (node.value(QStringLiteral("mode")).toString() == QStringLiteral("ColorAndDepth"))
            {
                outputs.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("ColorTexture")},
                                           {QStringLiteral("target"), QStringLiteral("color")}});
            }
        }
        ResourceDescription color;
        ResourceDescription depth;
        QString colorPort;
        QString depthPort;
        for (const QJsonValueConstRef outputValue : outputs)
        {
            const QJsonObject output = outputValue.toObject();
            const QString port = output.value(QStringLiteral("target")).toString();
            const ResourceDescription resource =
                ResolveResource(context,
                                FindSource(context.Configuration, identity + QLatin1Char('.') + port),
                                inoutRemainingSteps);
            if (resource.Type != QStringLiteral("ColorTexture") && resource.Type != QStringLiteral("DepthTexture"))
            {
                continue;
            }
            if (output.value(QStringLiteral("type")).toString() == QStringLiteral("ColorTexture"))
            {
                color = resource;
                colorPort = port;
            }
            else if (output.value(QStringLiteral("type")).toString() == QStringLiteral("DepthTexture"))
            {
                depth = resource;
                depthPort = port;
            }
            const QString format = output.value(QStringLiteral("format")).toString();
            const int32 samples =
                node.value(QStringLiteral("state")).toObject().value(QStringLiteral("samples")).toInt();
            if ((format.isEmpty() == false && resource.Format.isEmpty() == false && format != resource.Format) ||
                (samples > 0 && resource.Samples > 0 && samples != resource.Samples))
            {
                inoutProblems.append(
                    QStringLiteral("%1.%2 requires %3 / %4 samples; connected target supplies %5 / %6 samples. "
                                   "Match the writable resource format and pipeline samples to the target.")
                        .arg(where, port, format)
                        .arg(samples)
                        .arg(resource.Format)
                        .arg(resource.Samples));
            }
            if (kind == QStringLiteral("RayTracingPipeline") &&
                ((resource.Format.isEmpty() == false && resource.Format != QStringLiteral("RGBA8Unorm")) ||
                 (resource.Samples > 0 && resource.Samples != 1) || resource.IsPresentation == true))
            {
                inoutProblems.append(where +
                                     QStringLiteral(" requires an offscreen single-sample RGBA8Unorm color target."));
            }
        }
        if (HasKnownExtent(color.Extent) == true && HasKnownExtent(depth.Extent) == true &&
            (color.Extent != depth.Extent ||
             (color.Samples > 0 && depth.Samples > 0 && color.Samples != depth.Samples)))
        {
            inoutProblems.append(
                QStringLiteral("%1 requires matching raster attachment extents and samples. %2; %3. "
                               "Connect both targets to the same RenderSize, or use matching fixed sizes.")
                    .arg(where, DescribeTexture(colorPort, color), DescribeTexture(depthPort, depth)));
        }
        const QJsonArray launch =
            node.value(QStringLiteral("invocation")).toObject().value(QStringLiteral("dimensions")).toArray();
        if (kind == QStringLiteral("RayTracingPipeline") && HasKnownExtent(color.Extent) == true &&
            launch.size() == RAY_LAUNCH_DIMENSION_COUNT &&
            launch != QJsonArray{color.Extent.at(0), color.Extent.at(1), 1})
        {
            inoutProblems.append(
                QStringLiteral(
                    "%1: explicit ray launch %2 x %3 x %4 differs from the color target's %5 x %6 x 1 extent. "
                    "Choose Match color target in Invocation or update the explicit launch dimensions.")
                    .arg(where)
                    .arg(launch.at(0).toInteger())
                    .arg(launch.at(1).toInteger())
                    .arg(launch.at(2).toInteger())
                    .arg(color.Extent.at(0).toInteger())
                    .arg(color.Extent.at(1).toInteger()));
        }
    }
}
} // namespace

RenderGraphLinkCheck CheckRenderGraphTextureContracts(const RenderGraphDocument& document,
                                                      const QString& file,
                                                      const QString& configuration,
                                                      const QString& from,
                                                      const QString& to)
{
    QJsonObject graph;
    if (document.GetGraph(file, graph) == false)
    {
        return {};
    }
    ResourceContext context{document,
                            FindRecord(graph.value(QStringLiteral("configurations")).toArray(), configuration),
                            nullptr,
                            {}};
    QStringList previous;
    int32 remainingSteps = MAX_INFERENCE_STEPS;
    CollectProblems(context, configuration, previous, remainingSteps);
    if (remainingSteps <= 0)
    {
        return {};
    }
    QJsonArray edges;
    for (const QJsonValueConstRef edge : context.Configuration.value(QStringLiteral("edges")).toArray())
    {
        if (edge.toObject().value(QStringLiteral("to")).toString() != to)
        {
            edges.append(edge);
        }
    }
    edges.append(QJsonObject{{QStringLiteral("from"), from}, {QStringLiteral("to"), to}});
    context.Configuration.insert(QStringLiteral("edges"), edges);
    QStringList candidate;
    remainingSteps = MAX_INFERENCE_STEPS;
    CollectProblems(context, configuration, candidate, remainingSteps);
    if (remainingSteps <= 0)
    {
        return {};
    }
    for (const QString& problem : candidate)
    {
        if (previous.contains(problem) == false)
        {
            return {QStringLiteral("AttachmentMismatch"), problem};
        }
    }
    return {};
}
} // namespace ludus::editor
