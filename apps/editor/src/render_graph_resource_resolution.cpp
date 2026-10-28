#include "internal/render_graph_document.h"
#include "internal/render_graph_node_ports.h"
#include "internal/render_graph_pipeline_node.h"
#include "internal/render_graph_resource_inspection.h"
#include "internal/render_graph_resources.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 MAX_RESOLUTION_DEPTH = 64;
struct ResourceInstanceContext final
{
    QJsonObject Record;
    QString Prefix;
    QString Instance;
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
QJsonObject GetConfiguration(const RenderGraphDocument& document, const QString& file, const QString& configuration)
{
    QJsonObject graph;
    return document.GetGraph(file, graph) == true
               ? FindRecord(graph.value(QStringLiteral("configurations")).toArray(), configuration)
               : QJsonObject{};
}
QString FindSource(const QJsonObject& configuration, const QString& endpoint)
{
    if (endpoint.startsWith(QStringLiteral("#outputs.")) == true)
    {
        return FindRecord(configuration.value(QStringLiteral("outputs")).toArray(),
                          endpoint.section(QLatin1Char('.'), 1))
            .value(QStringLiteral("source"))
            .toString();
    }
    QString source;
    for (const QJsonValueConstRef value : configuration.value(QStringLiteral("edges")).toArray())
    {
        const QJsonObject edge = value.toObject();
        if (edge.value(QStringLiteral("to")).toString() == endpoint)
        {
            if (source.isEmpty() == false)
            {
                return {};
            } // Never choose among multiple producers.
            source = edge.value(QStringLiteral("from")).toString();
        }
    }
    return source;
}
QString
GetPortType(const RenderGraphDocument& document, const QJsonObject& record, const QString& endpoint, bool isOutput)
{
    if (endpoint.count(QLatin1Char('.')) != 1)
    {
        return {};
    }
    const QString node = endpoint.section(QLatin1Char('.'), 0, 0);
    const QString port = endpoint.section(QLatin1Char('.'), 1);
    if ((node == QStringLiteral("input") && isOutput == true) ||
        (node == QStringLiteral("#outputs") && isOutput == false))
    {
        return FindRecord(
                   record.value(isOutput == true ? QStringLiteral("inputs") : QStringLiteral("outputs")).toArray(),
                   port)
            .value(QStringLiteral("type"))
            .toString();
    }
    RenderGraphNodePorts ports;
    if (GetRenderGraphNodePorts(document, FindRecord(record.value(QStringLiteral("nodes")).toArray(), node), ports) ==
        false)
    {
        return {};
    }
    for (const RenderGraphPort& candidate : isOutput == true ? ports.Outputs : ports.Inputs)
    {
        if (candidate.Id == port)
        {
            return candidate.Type;
        }
    }
    return {};
}
bool AreTypesCompatible(const QString& source, const QString& destination)
{
    return source == destination ||
           (destination == QStringLiteral("Texture") &&
            (source == QStringLiteral("ColorTexture") || source == QStringLiteral("DepthTexture")));
}
QString ResolveName(const RenderGraphDocument& document,
                    const QVector<ResourceInstanceContext>& contexts,
                    const QString& endpoint,
                    int32 depth)
{
    if (depth >= MAX_RESOLUTION_DEPTH || contexts.isEmpty() == true || endpoint.count(QLatin1Char('.')) != 1)
    {
        return {};
    }
    const ResourceInstanceContext& context = contexts.last();
    const QString identity = endpoint.section(QLatin1Char('.'), 0, 0);
    const QString port = endpoint.section(QLatin1Char('.'), 1);
    if (GetPortType(document, context.Record, endpoint, true).isEmpty() == true)
    {
        return {};
    }
    if (identity == QStringLiteral("input"))
    {
        if (contexts.size() < 2)
        {
            return {};
        }
        QVector<ResourceInstanceContext> parent = contexts;
        parent.removeLast();
        const QString binding = FindSource(parent.last().Record, context.Instance + QLatin1Char('.') + port);
        if (AreTypesCompatible(GetPortType(document, parent.last().Record, binding, true),
                               GetPortType(document, context.Record, endpoint, true)) == false)
        {
            return {};
        }
        return ResolveName(document, parent, binding, depth + 1);
    }
    const QJsonObject node = FindRecord(context.Record.value(QStringLiteral("nodes")).toArray(), identity);
    const QString kind = node.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("Target") || kind == QStringLiteral("Buffer") ||
        kind == QStringLiteral("StructuredBuffer"))
    {
        return context.Prefix + QLatin1Char('/') + identity;
    }
    if (kind == QStringLiteral("Subgraph"))
    {
        QVector<ResourceInstanceContext> child = contexts;
        const QJsonObject record = GetConfiguration(document,
                                                    node.value(QStringLiteral("asset")).toString(),
                                                    node.value(QStringLiteral("configuration")).toString());
        child.append({record, context.Prefix + QLatin1Char('/') + identity, identity});
        const QString source = FindRecord(record.value(QStringLiteral("outputs")).toArray(), port)
                                   .value(QStringLiteral("source"))
                                   .toString();
        return ResolveName(document, child, source, depth + 1);
    }
    QString target;
    if (kind == QStringLiteral("Raster") || kind == QStringLiteral("ToneMap") || kind == QStringLiteral("Accumulate") ||
        kind == QStringLiteral("Denoise"))
    {
        target = port;
    }
    else if (kind == QStringLiteral("Upscale"))
    {
        target = QStringLiteral("destination");
    }
    else if (IsPipelineNode(kind) == true)
    {
        target = FindRecord(node.value(QStringLiteral("outputs")).toArray(), port)
                     .value(QStringLiteral("target"))
                     .toString();
    }
    if (target.isEmpty() == true)
    {
        return {};
    }
    return ResolveName(document, contexts, FindSource(context.Record, identity + QLatin1Char('.') + target), depth + 1);
}
} // namespace

bool AreRenderGraphResourcePathsEqual(const QVector<RenderGraphHistoryLocation>& left,
                                      const QVector<RenderGraphHistoryLocation>& right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (int32 index = 0; index < left.size(); ++index)
    {
        const RenderGraphHistoryLocation& leftLocation = left.at(index);
        const RenderGraphHistoryLocation& rightLocation = right.at(index);
        if (leftLocation.File != rightLocation.File || leftLocation.Configuration != rightLocation.Configuration ||
            leftLocation.Instance != rightLocation.Instance)
        {
            return false;
        }
    }
    return true;
}

RenderGraphResourceDetails ResolveRenderGraphResourceInspection(const RenderGraphDocument& document,
                                                                const RenderGraphResourceRequest& request,
                                                                const QSize& referenceSize)
{
    RenderGraphResourceDetails result;
    const QJsonObject record = GetConfiguration(document, request.File, request.Configuration);
    result.Type = GetPortType(document, record, request.Endpoint, request.IsOutput);
    if (result.Type.isEmpty() == true)
    {
        result.Problem = QStringLiteral("The port or configuration is missing, malformed or unresolved.");
        return result;
    }
    result.Producer = request.IsOutput == true ? request.Endpoint : FindSource(record, request.Endpoint);
    if (result.Producer.isEmpty() == true)
    {
        result.Problem = QStringLiteral("Unconnected input or multiple producers; no resource can be selected.");
        return result;
    }
    if (GetPortType(document, record, result.Producer, true).isEmpty() == true)
    {
        result.Problem = QStringLiteral("The connection's producer port is unavailable.");
        return result;
    }
    const QString sourceType = GetPortType(document, record, result.Producer, true);
    if (AreTypesCompatible(sourceType, result.Type) == false)
    {
        result.Problem = QStringLiteral("Producer and consumer port types are incompatible.");
        return result;
    }
    for (const QJsonValueConstRef value : record.value(QStringLiteral("edges")).toArray())
    {
        const QJsonObject edge = value.toObject();
        if (edge.value(QStringLiteral("from")).toString() == result.Producer)
        {
            result.Consumers.append(edge.value(QStringLiteral("to")).toString());
        }
    }
    for (const QJsonValueConstRef value : record.value(QStringLiteral("outputs")).toArray())
    {
        const QJsonObject output = value.toObject();
        if (output.value(QStringLiteral("source")).toString() == result.Producer)
        {
            result.Consumers.append(QStringLiteral("#outputs.") + output.value(QStringLiteral("id")).toString());
        }
    }
    result.Consumers.removeDuplicates();
    result.Consumers.sort();
    result.Description =
        DescribeRenderGraphResource(document, request.File, request.Configuration, result.Producer, referenceSize)
            .Description;
    if (result.Type != QStringLiteral("ColorTexture") && result.Type != QStringLiteral("DepthTexture") &&
        result.Type != QStringLiteral("Texture") && result.Type != QStringLiteral("Buffer"))
    {
        result.Problem = QStringLiteral("This is a graph value, not a texture resource.");
        return result;
    }
    QVector<RenderGraphHistoryLocation> path = request.Path;
    if (path.isEmpty() == true)
    {
        path.append({request.File, request.Configuration, {}, {}, false});
    }
    if (path.size() > RenderGraphHistory::MAX_PATH_DEPTH || path.first().File != document.GetRootGraph() ||
        path.last().File != request.File || path.last().Configuration != request.Configuration)
    {
        result.Problem =
            QStringLiteral("Open this library through its root graph to establish an exact caller instance.");
        return result;
    }
    QJsonObject root;
    if (document.GetGraph(document.GetRootGraph(), root) == false)
    {
        return result;
    }
    QVector<ResourceInstanceContext> contexts;
    for (int32 index = 0; index < path.size(); ++index)
    {
        const RenderGraphHistoryLocation& location = path.at(index);
        const QJsonObject configuration = GetConfiguration(document, location.File, location.Configuration);
        QString prefix = root.value(QStringLiteral("id")).toString();
        if (index > 0)
        {
            const QJsonObject caller =
                FindRecord(contexts.last().Record.value(QStringLiteral("nodes")).toArray(), location.Instance);
            if (caller.value(QStringLiteral("kind")).toString() != QStringLiteral("Subgraph") ||
                caller.value(QStringLiteral("asset")).toString() != location.File ||
                caller.value(QStringLiteral("configuration")).toString() != location.Configuration)
            {
                result.Problem = QStringLiteral("The caller path changed; inspect the resource again from the canvas.");
                return result;
            }
            prefix = contexts.last().Prefix + QLatin1Char('/') + location.Instance;
        }
        if (configuration.isEmpty() == true)
        {
            result.Problem = QStringLiteral("The caller configuration is unavailable.");
            return result;
        }
        contexts.append({configuration, prefix, location.Instance});
    }
    result.ResourceName = ResolveName(document, contexts, result.Producer, 0);
    if (result.ResourceName.isEmpty() == true)
    {
        result.Problem = QStringLiteral(
            "Resource identity is unresolved: missing, cyclic or unsupported bindings. Validate the graph.");
    }
    return result;
}
} // namespace ludus::editor
