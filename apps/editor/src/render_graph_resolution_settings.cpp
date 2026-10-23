#include "internal/render_graph_resolution_settings.h"
#include "internal/render_graph_document.h"

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/base/types.h>

#include <QJsonDocument>
#include <QJsonObject>

namespace ludus::editor
{
using namespace foundation;
namespace
{
qsizetype FindRecord(const QJsonArray& records, const QString& identity)
{
    qsizetype found = -1;
    for (qsizetype index = 0; index < records.size(); ++index)
    {
        if (records.at(index).toObject().value(QStringLiteral("id")).toString() == identity)
        {
            if (found >= 0)
            {
                return -1;
            }
            found = index;
        }
    }
    return found;
}

QString FindSource(const QJsonArray& edges, const QString& destination)
{
    QString source;
    for (const QJsonValueConstRef value : edges)
    {
        const QJsonObject edge = value.toObject();
        if (edge.value(QStringLiteral("to")).toString() == destination)
        {
            if (source.isEmpty() == false)
            {
                return {};
            }
            source = edge.value(QStringLiteral("from")).toString();
        }
    }
    return source;
}

bool IsBoundedPair(const QJsonArray& values)
{
    if (values.size() != 2)
    {
        return false;
    }
    for (const QJsonValueConstRef value : values)
    {
        if (value.isDouble() == false || value.toDouble() != static_cast<float64>(value.toInteger()) ||
            value.toInteger() < 1 || value.toInteger() > content::render_graph_format::MAX_TARGET_EXTENT)
        {
            return false;
        }
    }
    return true;
}

bool ReadPair(const QJsonObject& configuration,
              const RenderGraphResolutionSelection& selection,
              qsizetype& outOutputIndex,
              qsizetype& outRenderIndex)
{
    const QJsonArray nodes = configuration.value(QStringLiteral("nodes")).toArray();
    const qsizetype outputIndex = FindRecord(nodes, selection.OutputNode);
    const qsizetype renderIndex = FindRecord(nodes, selection.RenderNode);
    if (outputIndex < 0 || renderIndex < 0)
    {
        return false;
    }
    const QJsonObject output = nodes.at(outputIndex).toObject();
    const QJsonObject render = nodes.at(renderIndex).toObject();
    const QJsonValue extent = output.value(QStringLiteral("extent"));
    if (output.value(QStringLiteral("kind")) != QStringLiteral("OutputSize") ||
        render.value(QStringLiteral("kind")) != QStringLiteral("RenderSize") ||
        render.value(QStringLiteral("extent")) != QStringLiteral("OutputSize") ||
        (extent.toString() != QStringLiteral("Viewport") && IsBoundedPair(extent.toArray()) == false) ||
        IsBoundedPair(render.value(QStringLiteral("scale")).toArray()) == false ||
        FindSource(configuration.value(QStringLiteral("edges")).toArray(),
                   selection.RenderNode + QStringLiteral(".output")) != selection.OutputNode + QStringLiteral(".size"))
    {
        return false;
    }
    outOutputIndex = outputIndex;
    outRenderIndex = renderIndex;
    return true;
}

QString AllocateIdentity(const QJsonArray& nodes, const QString& stem)
{
    QString identity = stem;
    qsizetype suffix = 2;
    while (true)
    {
        bool hasIdentity = false;
        for (const QJsonValueConstRef entry : nodes)
        {
            if (entry.toObject().value(QStringLiteral("id")).toString() == identity)
            {
                hasIdentity = true;
                break;
            }
        }
        if (hasIdentity == false)
        {
            return identity;
        }
        identity = stem + QString::number(suffix);
        ++suffix;
    }
}
} // namespace

QVector<RenderGraphResolutionSelection> FindRenderGraphResolutionSettings(const RenderGraphDocument& document)
{
    QVector<RenderGraphResolutionSelection> result;
    for (const QString& file : document.GetFiles())
    {
        QJsonObject graph;
        if (document.GetGraph(file, graph) == false)
        {
            continue;
        }
        for (const QJsonValueConstRef value : graph.value(QStringLiteral("configurations")).toArray())
        {
            const QJsonObject configuration = value.toObject();
            for (const QJsonValueConstRef entry : configuration.value(QStringLiteral("nodes")).toArray())
            {
                const QJsonObject node = entry.toObject();
                if (node.value(QStringLiteral("kind")) != QStringLiteral("RenderSize") ||
                    node.value(QStringLiteral("extent")) != QStringLiteral("OutputSize"))
                {
                    continue;
                }
                const QString render = node.value(QStringLiteral("id")).toString();
                const QString source = FindSource(configuration.value(QStringLiteral("edges")).toArray(),
                                                  render + QStringLiteral(".output"));
                RenderGraphResolutionSelection selection{file,
                                                         configuration.value(QStringLiteral("id")).toString(),
                                                         source.section(QLatin1Char('.'), 0, 0),
                                                         render};
                qsizetype outputIndex = -1;
                qsizetype renderIndex = -1;
                if (ReadPair(configuration, selection, outputIndex, renderIndex) == true)
                {
                    result.append(selection);
                }
            }
        }
    }
    return result;
}

bool ReadRenderGraphResolutionSettings(const RenderGraphDocument& document,
                                       const RenderGraphResolutionSelection& selection,
                                       QJsonValue& outExtent,
                                       QJsonArray& outScale)
{
    QJsonObject graph;
    if (document.GetGraph(selection.File, graph) == false)
    {
        return false;
    }
    const QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    const qsizetype index = FindRecord(configurations, selection.Configuration);
    if (index < 0)
    {
        return false;
    }
    const QJsonObject configuration = configurations.at(index).toObject();
    qsizetype outputIndex = -1;
    qsizetype renderIndex = -1;
    if (ReadPair(configuration, selection, outputIndex, renderIndex) == false)
    {
        return false;
    }
    const QJsonArray nodes = configuration.value(QStringLiteral("nodes")).toArray();
    outExtent = nodes.at(outputIndex).toObject().value(QStringLiteral("extent"));
    outScale = nodes.at(renderIndex).toObject().value(QStringLiteral("scale")).toArray();
    return true;
}

bool WriteRenderGraphResolutionSettings(RenderGraphDocument& inoutDocument,
                                        const RenderGraphResolutionSelection& selection,
                                        const QJsonValue& extent,
                                        const QJsonArray& scale,
                                        QString& outError)
{
    outError =
        QStringLiteral("The renderer's resolution providers changed. Select the current configuration and retry.");
    if ((extent.toString() != QStringLiteral("Viewport") && IsBoundedPair(extent.toArray()) == false) ||
        IsBoundedPair(scale) == false)
    {
        outError =
            QStringLiteral("Use positive pixel dimensions and render scale values within the renderer's size limit.");
        return false;
    }
    QJsonObject graph;
    if (inoutDocument.GetGraph(selection.File, graph) == false)
    {
        return false;
    }
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    const qsizetype index = FindRecord(configurations, selection.Configuration);
    if (index < 0)
    {
        return false;
    }
    QJsonObject configuration = configurations.at(index).toObject();
    qsizetype outputIndex = -1;
    qsizetype renderIndex = -1;
    if (ReadPair(configuration, selection, outputIndex, renderIndex) == false)
    {
        return false;
    }
    QJsonArray nodes = configuration.value(QStringLiteral("nodes")).toArray();
    QJsonObject output = nodes.at(outputIndex).toObject();
    QJsonObject render = nodes.at(renderIndex).toObject();
    output.insert(QStringLiteral("extent"), extent);
    render.insert(QStringLiteral("scale"), scale);
    nodes.replace(outputIndex, output);
    nodes.replace(renderIndex, render);
    configuration.insert(QStringLiteral("nodes"), nodes);
    configurations.replace(index, configuration);
    graph.insert(QStringLiteral("configurations"), configurations);
    if (inoutDocument.SetText(selection.File, EncodeCanonicalJson(graph)) != RenderGraphDocumentStatus::Ok)
    {
        outError = QStringLiteral("The renderer could not accept this edit. Its draft and saved files were preserved.");
        return false;
    }
    outError.clear();
    return true;
}

bool EnableRenderGraphResolutionSettings(RenderGraphDocument& inoutDocument, QString& outError)
{
    outError = QStringLiteral("This renderer needs an exposed OutputSize/RenderSize pair. Automatic setup supports one "
                              "root color target with fixed or view-relative sizing.");
    QJsonObject graph;
    if (inoutDocument.GetGraph(inoutDocument.GetRootGraph(), graph) == false)
    {
        return false;
    }
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    const qsizetype configurationIndex = FindRecord(configurations, graph.value(QStringLiteral("default")).toString());
    if (configurationIndex < 0)
    {
        return false;
    }
    QJsonObject configuration = configurations.at(configurationIndex).toObject();
    QJsonArray nodes = configuration.value(QStringLiteral("nodes")).toArray();
    QJsonArray edges = configuration.value(QStringLiteral("edges")).toArray();
    qsizetype colorIndex = -1;
    for (qsizetype index = 0; index < nodes.size(); ++index)
    {
        const QJsonObject node = nodes.at(index).toObject();
        if (node.value(QStringLiteral("kind")) == QStringLiteral("Target") &&
            node.value(QStringLiteral("type")) == QStringLiteral("ColorTexture"))
        {
            if (colorIndex >= 0)
            {
                return false;
            }
            colorIndex = index;
        }
    }
    if (colorIndex < 0)
    {
        return false;
    }
    const QJsonObject color = nodes.at(colorIndex).toObject();
    QJsonValue extent = color.value(QStringLiteral("extent"));
    QJsonArray scale{1, 1};
    qsizetype renderIndex = -1;
    if (extent.toString() == QStringLiteral("RenderSize"))
    {
        const QString source =
            FindSource(edges, color.value(QStringLiteral("id")).toString() + QStringLiteral(".size"));
        renderIndex = FindRecord(nodes, source.section(QLatin1Char('.'), 0, 0));
        if (renderIndex < 0 || source.section(QLatin1Char('.'), 1) != QStringLiteral("size"))
        {
            return false;
        }
        const QJsonObject size = nodes.at(renderIndex).toObject();
        if (size.value(QStringLiteral("kind")) != QStringLiteral("RenderSize"))
        {
            return false;
        }
        extent = size.value(QStringLiteral("extent"));
        if (extent.isObject() == true)
        {
            const QJsonObject policy = extent.toObject();
            scale = policy.value(QStringLiteral("scale")).toArray();
            if (policy.value(QStringLiteral("reference")) != QStringLiteral("View") || IsBoundedPair(scale) == false)
            {
                return false;
            }
            extent = QStringLiteral("Viewport");
        }
    }
    if (extent.toString() != QStringLiteral("Viewport") && IsBoundedPair(extent.toArray()) == false)
    {
        return false;
    }
    const QString outputIdentity = AllocateIdentity(nodes, QStringLiteral("outputSize"));
    nodes.append(QJsonObject{{QStringLiteral("id"), outputIdentity},
                             {QStringLiteral("kind"), QStringLiteral("OutputSize")},
                             {QStringLiteral("extent"), extent}});
    QString renderIdentity;
    if (renderIndex >= 0)
    {
        QJsonObject size = nodes.at(renderIndex).toObject();
        renderIdentity = size.value(QStringLiteral("id")).toString();
        size.insert(QStringLiteral("extent"), QStringLiteral("OutputSize"));
        size.insert(QStringLiteral("scale"), scale);
        nodes.replace(renderIndex, size);
    }
    else
    {
        renderIdentity = AllocateIdentity(nodes, QStringLiteral("renderSize"));
        nodes.append(QJsonObject{{QStringLiteral("id"), renderIdentity},
                                 {QStringLiteral("kind"), QStringLiteral("RenderSize")},
                                 {QStringLiteral("extent"), QStringLiteral("OutputSize")},
                                 {QStringLiteral("scale"), scale}});
        for (qsizetype index = 0; index < nodes.size(); ++index)
        {
            QJsonObject target = nodes.at(index).toObject();
            if (target.value(QStringLiteral("kind")) == QStringLiteral("Target") &&
                target.value(QStringLiteral("extent")) == extent)
            {
                target.insert(QStringLiteral("extent"), QStringLiteral("RenderSize"));
                nodes.replace(index, target);
                edges.append(QJsonObject{
                    {QStringLiteral("from"), renderIdentity + QStringLiteral(".size")},
                    {QStringLiteral("to"), target.value(QStringLiteral("id")).toString() + QStringLiteral(".size")}});
            }
        }
    }
    edges.append(QJsonObject{{QStringLiteral("from"), outputIdentity + QStringLiteral(".size")},
                             {QStringLiteral("to"), renderIdentity + QStringLiteral(".output")}});
    if (nodes.size() > RenderGraphDocument::MAX_NODES || edges.size() > RenderGraphDocument::MAX_EDGES)
    {
        return false;
    }
    configuration.insert(QStringLiteral("nodes"), nodes);
    configuration.insert(QStringLiteral("edges"), edges);
    configurations.replace(configurationIndex, configuration);
    graph.insert(QStringLiteral("configurations"), configurations);
    graph.insert(QStringLiteral("version"), 2);
    if (inoutDocument.SetText(inoutDocument.GetRootGraph(), EncodeCanonicalJson(graph)) !=
        RenderGraphDocumentStatus::Ok)
    {
        return false;
    }
    outError.clear();
    return true;
}
} // namespace ludus::editor
