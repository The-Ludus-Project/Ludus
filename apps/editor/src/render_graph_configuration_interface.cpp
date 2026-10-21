#include "internal/render_graph_canvas.h"
#include "internal/render_graph_configuration_editor.h"
#include "internal/render_graph_document.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

namespace ludus::editor
{
using namespace foundation;
namespace
{
QString FindPortType(const QJsonArray& ports, const QString& identity)
{
    for (const QJsonValueConstRef value : ports)
    {
        const QJsonObject port = value.toObject();
        if (port.value(QStringLiteral("id")).toString() == identity)
        {
            return port.value(QStringLiteral("type")).toString();
        }
    }
    return {};
}

bool IsPortPreserved(const QString& endpoint,
                     const QString& node,
                     const QJsonArray& previous,
                     const QJsonArray& candidate)
{
    if (endpoint.section(QLatin1Char('.'), 0, 0) != node)
    {
        return true;
    }
    const QString identity = endpoint.section(QLatin1Char('.'), 1);
    const QString type = FindPortType(candidate, identity);
    return type.isEmpty() == false && type == FindPortType(previous, identity);
}

bool ValidatePorts(const QJsonArray& ports, int32 version, bool isOutput, QString& outError)
{
    if (ports.size() > MAX_CONFIGURATION_PORTS)
    {
        outError =
            QStringLiteral("A configuration supports at most %1 ports in each direction.").arg(MAX_CONFIGURATION_PORTS);
        return false;
    }
    const QRegularExpression identifier(QStringLiteral("\\A[A-Za-z][A-Za-z0-9_-]{0,62}\\z"));
    QSet<QString> identities;
    for (const QJsonValueConstRef value : ports)
    {
        const QJsonObject port = value.toObject();
        const QString identity = port.value(QStringLiteral("id")).toString();
        if (identifier.match(identity).hasMatch() == false || identities.contains(identity) == true)
        {
            outError = QStringLiteral("Port names must be unique in each direction: 1–63 ASCII letters, digits, "
                                      "'_' or '-', beginning with a letter.");
            return false;
        }
        identities.insert(identity);
        if (GetConfigurationPortTypes(version, isOutput).contains(port.value(QStringLiteral("type")).toString()) ==
            false)
        {
            outError = QStringLiteral("%1 has an unsupported port type for this graph version.").arg(identity);
            return false;
        }
        if (isOutput == true && (port.value(QStringLiteral("source")).isString() == false ||
                                 port.value(QStringLiteral("required")).isBool() == false))
        {
            outError = QStringLiteral("Output %1 needs a source and a required-consumer flag.").arg(identity);
            return false;
        }
    }
    return true;
}
} // namespace

QStringList GetConfigurationPortTypes(int32 version, bool isOutput)
{
    QStringList types{QStringLiteral("ColorTexture"), QStringLiteral("DepthTexture")};
    if (version == 2)
    {
        types.append(QStringLiteral("Buffer"));
        types.append(QStringLiteral("RenderSize"));
    }
    if (isOutput == false)
    {
        types += {QStringLiteral("Scene"), QStringLiteral("View")};
        if (version == 2)
        {
            types += {QStringLiteral("AccelerationStructure"),
                      QStringLiteral("Float32"),
                      QStringLiteral("Float2"),
                      QStringLiteral("Float3"),
                      QStringLiteral("Float4"),
                      QStringLiteral("UInt32")};
        }
    }
    return types;
}

// File and configuration are distinct authored identities.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus RenderGraphDocument::ReplaceConfigurationInterface(const QString& file,
                                                                             const QString& configuration,
                                                                             const QJsonArray& inputs,
                                                                             const QJsonArray& outputs,
                                                                             QString& outError)
{
    outError.clear();
    QJsonObject graph;
    if (GetGraph(file, graph) == false)
    {
        outError = QStringLiteral("The graph source is unavailable or malformed.");
        return RenderGraphDocumentStatus::NotFound;
    }
    const int32 version = graph.value(QStringLiteral("version")).toInt();
    if (ValidatePorts(inputs, version, false, outError) == false ||
        ValidatePorts(outputs, version, true, outError) == false)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    qsizetype selected = -1;
    for (qsizetype i = 0; i < configurations.size(); ++i)
    {
        if (configurations.at(i).toObject().value(QStringLiteral("id")).toString() == configuration)
        {
            selected = i;
            break;
        }
    }
    if (selected < 0)
    {
        outError = QStringLiteral("The configuration no longer exists.");
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QJsonObject record = configurations.at(selected).toObject();
    const QJsonArray previousInputs = record.value(QStringLiteral("inputs")).toArray();
    const QJsonArray previousOutputs = record.value(QStringLiteral("outputs")).toArray();
    const QJsonArray edges = record.value(QStringLiteral("edges")).toArray();
    for (const QJsonValueConstRef edge : edges)
    {
        if (IsPortPreserved(edge.toObject().value(QStringLiteral("from")).toString(),
                            QStringLiteral("input"),
                            previousInputs,
                            inputs) == false)
        {
            outError =
                QStringLiteral("Disconnect an input's internal links before removing, renaming or changing its type.");
            return RenderGraphDocumentStatus::InvalidEdit;
        }
    }
    // Callers of this library share this interface. Never strand their existing links.
    for (const QString& source : GetFiles())
    {
        QJsonObject parent;
        if (GetGraph(source, parent) == false)
        {
            continue;
        }
        const QJsonArray parents = parent.value(QStringLiteral("configurations")).toArray();
        for (const QJsonValueConstRef parentValue : parents)
        {
            const QJsonObject parentConfiguration = parentValue.toObject();
            const QJsonArray nodes = parentConfiguration.value(QStringLiteral("nodes")).toArray();
            for (const QJsonValueConstRef nodeValue : nodes)
            {
                const QJsonObject node = nodeValue.toObject();
                if (node.value(QStringLiteral("kind")).toString() != QStringLiteral("Subgraph") ||
                    node.value(QStringLiteral("asset")).toString() != file ||
                    node.value(QStringLiteral("configuration")).toString() != configuration)
                {
                    continue;
                }
                const QString identity = node.value(QStringLiteral("id")).toString();
                const QJsonArray parentEdges = parentConfiguration.value(QStringLiteral("edges")).toArray();
                for (const QJsonValueConstRef edge : parentEdges)
                {
                    const QJsonObject link = edge.toObject();
                    if (IsPortPreserved(link.value(QStringLiteral("from")).toString(),
                                        identity,
                                        previousOutputs,
                                        outputs) == false ||
                        IsPortPreserved(link.value(QStringLiteral("to")).toString(),
                                        identity,
                                        previousInputs,
                                        inputs) == false)
                    {
                        outError =
                            QStringLiteral("Disconnect affected ports on %1#%2/%3 before changing this interface.")
                                .arg(source, parentConfiguration.value(QStringLiteral("id")).toString(), identity);
                        return RenderGraphDocumentStatus::InvalidEdit;
                    }
                }
                const QJsonArray parentOutputs = parentConfiguration.value(QStringLiteral("outputs")).toArray();
                for (const QJsonValueConstRef output : parentOutputs)
                {
                    if (IsPortPreserved(output.toObject().value(QStringLiteral("source")).toString(),
                                        identity,
                                        previousOutputs,
                                        outputs) == false)
                    {
                        outError =
                            QStringLiteral("Disconnect configuration outputs using %1 before changing its ports.")
                                .arg(identity);
                        return RenderGraphDocumentStatus::InvalidEdit;
                    }
                }
            }
        }
    }
    const QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    for (const QJsonValueConstRef value : outputs)
    {
        const QJsonObject output = value.toObject();
        const QString endpoint = output.value(QStringLiteral("source")).toString();
        if (endpoint.isEmpty() == true)
        {
            continue; // An unconnected output is a saved draft; validation diagnoses it before builds.
        }
        QString type;
        const QString producer = endpoint.section(QLatin1Char('.'), 0, 0);
        const QString port = endpoint.section(QLatin1Char('.'), 1);
        if (producer == QStringLiteral("input"))
        {
            type = FindPortType(inputs, port);
        }
        else
        {
            for (const QJsonValueConstRef node : nodes)
            {
                if (node.toObject().value(QStringLiteral("id")).toString() == producer)
                {
                    RenderGraphNodePorts ports;
                    if (GetRenderGraphNodePorts(*this, node.toObject(), ports) == true)
                    {
                        for (const RenderGraphPort& candidate : ports.Outputs)
                        {
                            if (candidate.Id == port)
                            {
                                type = candidate.Type;
                            }
                        }
                    }
                }
            }
        }
        if (endpoint.count(QLatin1Char('.')) != 1 || type.isEmpty() == true ||
            type != output.value(QStringLiteral("type")).toString())
        {
            outError = QStringLiteral("Output %1 needs a source of type %2, or Unconnected while drafting.")
                           .arg(output.value(QStringLiteral("id")).toString(),
                                output.value(QStringLiteral("type")).toString());
            return RenderGraphDocumentStatus::InvalidEdit;
        }
    }
    if (inputs == previousInputs && outputs == previousOutputs)
    {
        return RenderGraphDocumentStatus::Ok;
    }
    record.insert(QStringLiteral("inputs"), inputs);
    record.insert(QStringLiteral("outputs"), outputs);
    configurations.replace(selected, record);
    graph.insert(QStringLiteral("configurations"), configurations);
    return SetText(file, EncodeCanonicalJson(graph));
}
} // namespace ludus::editor
