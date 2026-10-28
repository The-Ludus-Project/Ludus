#include "internal/render_graph_quick_add_graphs.h"

#include "internal/render_graph_document.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace ludus::editor
{
namespace
{
QJsonObject FindConfiguration(const QJsonObject& graph, const QString& identity)
{
    for (const QJsonValueConstRef value : graph.value(QStringLiteral("configurations")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == identity)
        {
            return value.toObject();
        }
    }
    return {};
}
} // namespace

QString CheckRenderGraphQuickAddGraph(const RenderGraphDocument& document,
                                      const QString& file,
                                      const QString& configuration,
                                      const RenderGraphTemplate& entry)
{
    QJsonObject graph;
    if (document.GetGraph(entry.Identity, graph) == false)
    {
        return QStringLiteral("Graph source is unavailable or invalid. Fix it in Source and reopen Quick Add.");
    }
    if (graph.value(QStringLiteral("default")).toString() != entry.DefaultConfiguration)
    {
        return QStringLiteral("The graph's default configuration changed. Reopen Quick Add.");
    }
    const QJsonObject selected = FindConfiguration(graph, entry.DefaultConfiguration);
    if (entry.DefaultConfiguration.isEmpty() == true || selected.isEmpty() == true ||
        selected.value(QStringLiteral("inputs")).isArray() == false ||
        selected.value(QStringLiteral("outputs")).isArray() == false)
    {
        return QStringLiteral("Graph configuration %1#%2 has no valid interface. Fix it in Source.")
            .arg(entry.Identity, entry.DefaultConfiguration);
    }
    struct Visit final
    {
        QString File;
        bool IsLeaving = false;
    };
    QVector<Visit> pending{{entry.Identity, false}};
    QStringList active;
    QStringList visited;
    // The tooling loader imports every configuration and rejects recursion between files,
    // even when the recursive configuration is not the currently selected/default one.
    while (pending.isEmpty() == false)
    {
        const Visit visit = pending.takeLast();
        const QString dependency = visit.File;
        if (visit.IsLeaving == true)
        {
            active.removeAll(dependency);
            visited.append(dependency);
            continue;
        }
        if (active.contains(dependency) == true)
        {
            return QStringLiteral("Cannot insert this graph: its dependencies contain a cycle through %1.")
                .arg(dependency);
        }
        if (dependency == file)
        {
            return QStringLiteral("Cannot insert this graph: its dependencies would create a cycle through %1#%2.")
                .arg(file, configuration);
        }
        if (visited.contains(dependency) == true)
        {
            continue;
        }
        active.append(dependency);
        pending.append({dependency, true});
        QJsonObject dependencyGraph;
        if (document.GetGraph(dependency, dependencyGraph) == false)
        {
            return QStringLiteral("Graph dependency %1 is unavailable or invalid.").arg(dependency);
        }
        for (const QJsonValueConstRef value : dependencyGraph.value(QStringLiteral("configurations")).toArray())
        {
            const QJsonObject record = value.toObject();
            if (record.value(QStringLiteral("nodes")).isArray() == false)
            {
                return QStringLiteral("Graph configuration %1#%2 has no valid nodes array. Fix it in Source.")
                    .arg(dependency, record.value(QStringLiteral("id")).toString());
            }
            for (const QJsonValueConstRef nodeValue : record.value(QStringLiteral("nodes")).toArray())
            {
                const QJsonObject node = nodeValue.toObject();
                if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph"))
                {
                    const QString asset = node.value(QStringLiteral("asset")).toString();
                    QJsonObject imported;
                    if (document.GetGraph(asset, imported) == false ||
                        FindConfiguration(imported, node.value(QStringLiteral("configuration")).toString()).isEmpty() ==
                            true)
                    {
                        return QStringLiteral("Graph dependency %1#%2 is unavailable or invalid.")
                            .arg(asset, node.value(QStringLiteral("configuration")).toString());
                    }
                    pending.append({asset, false});
                }
            }
        }
    }
    return {};
}

QVector<RenderGraphQuickAddGraph>
BuildRenderGraphQuickAddGraphs(const RenderGraphDocument& document, const QString& file, const QString& configuration)
{
    QVector<RenderGraphQuickAddGraph> entries;
    for (const QString& asset : document.GetFiles())
    {
        if (asset.endsWith(QStringLiteral(".lurender.json")) == false)
        {
            continue;
        }
        QJsonObject graph;
        const bool hasGraph = document.GetGraph(asset, graph);
        RenderGraphQuickAddGraph entry;
        entry.Template.Kind = RenderGraphTemplateKind::Graph;
        entry.Template.Identity = asset;
        entry.Template.Label = graph.value(QStringLiteral("id")).toString();
        if (entry.Template.Label.isEmpty() == true)
        {
            entry.Template.Label = QFileInfo(asset).fileName().chopped(QStringLiteral(".lurender.json").size());
        }
        entry.Template.Description = QStringLiteral("Insert a reference to this graph's default configuration.");
        entry.Template.Category = QStringLiteral("structure");
        entry.Template.DefaultConfiguration = graph.value(QStringLiteral("default")).toString();
        const QJsonObject selected = FindConfiguration(graph, entry.Template.DefaultConfiguration);
        entry.Ports.Inputs = ReadRenderGraphInterfacePorts(selected.value(QStringLiteral("inputs")).toArray());
        entry.Ports.Outputs = ReadRenderGraphInterfacePorts(selected.value(QStringLiteral("outputs")).toArray());
        for (const QJsonValueConstRef value : graph.value(QStringLiteral("configurations")).toArray())
        {
            entry.Template.Keywords.append(value.toObject().value(QStringLiteral("id")).toString().toLower());
        }
        for (const RenderGraphPort& port : entry.Ports.Inputs + entry.Ports.Outputs)
        {
            entry.Template.Keywords.append(port.Id.toLower());
            entry.Template.Keywords.append(port.Type.toLower());
        }
        entry.UnavailableReason = hasGraph == true
                                      ? CheckRenderGraphQuickAddGraph(document, file, configuration, entry.Template)
                                      : QStringLiteral("Graph source is invalid. Fix it in Source.");
        entries.append(entry);
    }
    return entries;
}
} // namespace ludus::editor
