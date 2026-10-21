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

namespace
{
using RenameMap = QMap<QString, QString>;
constexpr int32 MAX_REPAIR_DETAILS = 128;
struct RepairLog final
{
    QStringList Details;
    int32 Omitted = 0;
    void Add(const QString& detail)
    {
        if (Details.size() < MAX_REPAIR_DETAILS)
        {
            Details.append(detail);
        }
        else
        {
            ++Omitted;
        }
    }
};
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
bool ValidateRenames(const RenameMap& renames, const QJsonArray& previous, const QJsonArray& next, QString& outError)
{
    QSet<QString> destinations;
    for (RenameMap::const_iterator rename = renames.constBegin(); rename != renames.constEnd(); ++rename)
    {
        if (FindPortType(previous, rename.key()).isEmpty() == true ||
            FindPortType(next, rename.value()).isEmpty() == true || destinations.contains(rename.value()) == true ||
            (rename.key() != rename.value() && FindPortType(previous, rename.value()).isEmpty() == false &&
             renames.contains(rename.value()) == false))
        {
            outError = QStringLiteral("Port rename %1 → %2 is missing, ambiguous or overwrites an existing identity.")
                           .arg(rename.key(), rename.value());
            return false;
        }
        destinations.insert(rename.value());
    }
    for (RenameMap::const_iterator rename = renames.constBegin(); rename != renames.constEnd(); ++rename)
    {
        if (rename.key() != rename.value() && FindPortType(next, rename.key()).isEmpty() == false &&
            destinations.contains(rename.key()) == false)
        {
            outError = QStringLiteral("Port name %1 cannot be reused for a new port in the same rename edit.")
                           .arg(rename.key());
            return false;
        }
    }
    return true;
}
QString RenameEndpoint(const QString& endpoint, const QString& node, const RenameMap& renames)
{
    if (endpoint.section(QLatin1Char('.'), 0, 0) != node || endpoint.count(QLatin1Char('.')) != 1)
    {
        return endpoint;
    }
    const QString port = endpoint.section(QLatin1Char('.'), 1);
    return node + QLatin1Char('.') + renames.value(port, port);
}
/// Read only direct port declarations: no expansion, recursion, evaluation or new document history.
QString GetEndpointType(const RenderGraphDocument& document,
                        const QMap<QString, QJsonObject>& graphs,
                        const QJsonObject& configuration,
                        const QString& endpoint,
                        bool isOutput)
{
    if (endpoint.count(QLatin1Char('.')) != 1)
    {
        return {};
    }
    const QString identity = endpoint.section(QLatin1Char('.'), 0, 0);
    const QString port = endpoint.section(QLatin1Char('.'), 1);
    if (identity == QStringLiteral("input") && isOutput == true)
    {
        return FindPortType(configuration.value(QStringLiteral("inputs")).toArray(), port);
    }
    for (const QJsonValueConstRef value : configuration.value(QStringLiteral("nodes")).toArray())
    {
        const QJsonObject node = value.toObject();
        if (node.value(QStringLiteral("id")).toString() != identity)
        {
            continue;
        }
        if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph"))
        {
            const QJsonObject child = FindConfiguration(graphs.value(node.value(QStringLiteral("asset")).toString()),
                                                        node.value(QStringLiteral("configuration")).toString());
            return FindPortType(
                child.value(isOutput == true ? QStringLiteral("outputs") : QStringLiteral("inputs")).toArray(),
                port);
        }
        RenderGraphNodePorts ports;
        if (GetRenderGraphNodePorts(document, node, ports) == false)
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
    }
    return {};
}
bool AreTypesCompatible(const QString& source, const QString& destination)
{
    return source.isEmpty() == false && destination.isEmpty() == false &&
           (source == destination ||
            (destination == QStringLiteral("Texture") &&
             (source == QStringLiteral("ColorTexture") || source == QStringLiteral("DepthTexture"))));
}
QSet<QString> FindChangedPorts(const QJsonArray& previous, const QJsonArray& next, const RenameMap& renames)
{
    QSet<QString> changed;
    for (const QJsonValueConstRef value : previous)
    {
        const QJsonObject port = value.toObject();
        const QString identity = port.value(QStringLiteral("id")).toString();
        if (renames.value(identity, identity) != identity ||
            FindPortType(next, identity) != port.value(QStringLiteral("type")).toString())
        {
            changed.insert(identity);
        }
    }
    return changed;
}
/// Rewrite both ends before checking types, including links between two instances of the same library.
void RepairUses(const RenderGraphDocument& document,
                const QMap<QString, QJsonObject>& graphs,
                QJsonObject& inoutConfiguration,
                const QString& location,
                const QSet<QString>& nodes,
                const RenameMap& inputRenames,
                const RenameMap& outputRenames,
                const QSet<QString>& changedInputs,
                const QSet<QString>& changedOutputs,
                RepairLog& inoutLog,
                bool isInternal = false)
{
    const auto isAffected = [&nodes](const QString& endpoint, const QSet<QString>& changed) {
        return nodes.contains(endpoint.section(QLatin1Char('.'), 0, 0)) == true &&
               changed.contains(endpoint.section(QLatin1Char('.'), 1)) == true;
    };
    const auto remap = [&nodes](const QString& endpoint, const RenameMap& renames) {
        const QString node = endpoint.section(QLatin1Char('.'), 0, 0);
        return nodes.contains(node) == true ? RenameEndpoint(endpoint, node, renames) : endpoint;
    };
    const QSet<QString>& changedSources = isInternal == true ? changedInputs : changedOutputs;
    const RenameMap& sourceRenames = isInternal == true ? inputRenames : outputRenames;
    QJsonArray edges;
    for (const QJsonValueConstRef value : inoutConfiguration.value(QStringLiteral("edges")).toArray())
    {
        QJsonObject edge = value.toObject();
        const QString from = edge.value(QStringLiteral("from")).toString();
        const QString to = edge.value(QStringLiteral("to")).toString();
        const bool hasAffectedSource = isAffected(from, changedSources);
        const bool hasAffectedDestination = isInternal == false && isAffected(to, changedInputs) == true;
        if (hasAffectedSource == false && hasAffectedDestination == false)
        {
            edges.append(value);
            continue;
        }
        const QString nextFrom = remap(from, sourceRenames);
        const QString nextTo = isInternal == false ? remap(to, inputRenames) : to;
        const QString sourceType = GetEndpointType(document, graphs, inoutConfiguration, nextFrom, true);
        const QString destinationType = GetEndpointType(document, graphs, inoutConfiguration, nextTo, false);
        if (AreTypesCompatible(sourceType, destinationType) == false)
        {
            inoutLog.Add(
                QStringLiteral("%1: disconnect %2 → %3 (removed or incompatible port).").arg(location, from, to));
            continue;
        }
        edge.insert(QStringLiteral("from"), nextFrom);
        edge.insert(QStringLiteral("to"), nextTo);
        edges.append(edge);
        if (from != nextFrom || to != nextTo)
        {
            inoutLog.Add(QStringLiteral("%1: rewire %2 → %3 to %4 → %5.").arg(location, from, to, nextFrom, nextTo));
        }
    }
    inoutConfiguration.insert(QStringLiteral("edges"), edges);
    QJsonArray outputs = inoutConfiguration.value(QStringLiteral("outputs")).toArray();
    for (qsizetype index = 0; index < outputs.size(); ++index)
    {
        QJsonObject output = outputs.at(index).toObject();
        const QString source = output.value(QStringLiteral("source")).toString();
        if (isAffected(source, changedSources) == false)
        {
            continue;
        }
        const QString next = remap(source, sourceRenames);
        const QString type = GetEndpointType(document, graphs, inoutConfiguration, next, true);
        if (type.isEmpty() == true || type != output.value(QStringLiteral("type")).toString())
        {
            output.insert(QStringLiteral("source"), QString());
            inoutLog.Add(QStringLiteral("%1: clear output %2 source %3 (removed or incompatible port).")
                             .arg(location, output.value(QStringLiteral("id")).toString(), source));
        }
        else
        {
            output.insert(QStringLiteral("source"), next);
            if (source != next)
            {
                inoutLog.Add(QStringLiteral("%1: rewire output %2 source %3 to %4.")
                                 .arg(location, output.value(QStringLiteral("id")).toString(), source, next));
            }
        }
        outputs.replace(index, output);
    }
    inoutConfiguration.insert(QStringLiteral("outputs"), outputs);
}
} // namespace

RenderGraphDocumentStatus
RenderGraphDocument::prepareConfigurationInterface(const QString& file,
                                                   const QString& configuration,
                                                   const QJsonArray& inputs,
                                                   const QJsonArray& outputs,
                                                   const QMap<QString, QString>& inputRenames,
                                                   const QMap<QString, QString>& outputRenames,
                                                   RenderGraphSnapshot& outSnapshot,
                                                   QStringList& outRepairs,
                                                   QString& outError) const
{
    outError.clear();
    QMap<QString, QJsonObject> graphs;
    for (const QString& source : GetFiles())
    {
        if (source.endsWith(QStringLiteral(".lurender.json")) == false)
        {
            continue;
        }
        QJsonObject graph;
        if (GetGraph(source, graph) == false)
        {
            outError = QStringLiteral("Cannot inspect callers while %1 has malformed graph JSON. Fix its Source first.")
                           .arg(source);
            return RenderGraphDocumentStatus::Malformed;
        }
        graphs.insert(source, graph);
    }
    if (graphs.contains(file) == false)
    {
        outError = QStringLiteral("The graph source is unavailable.");
        return RenderGraphDocumentStatus::NotFound;
    }
    QJsonObject graph = graphs.value(file);
    QJsonObject record = FindConfiguration(graph, configuration);
    if (record.isEmpty() == true)
    {
        outError = QStringLiteral("The configuration no longer exists.");
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    const int32 version = graph.value(QStringLiteral("version")).toInt();
    const QJsonArray previousInputs = record.value(QStringLiteral("inputs")).toArray();
    const QJsonArray previousOutputs = record.value(QStringLiteral("outputs")).toArray();
    if (ValidatePorts(inputs, version, false, outError) == false ||
        ValidatePorts(outputs, version, true, outError) == false ||
        ValidateRenames(inputRenames, previousInputs, inputs, outError) == false ||
        ValidateRenames(outputRenames, previousOutputs, outputs, outError) == false)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    const QSet<QString> changedInputs = FindChangedPorts(previousInputs, inputs, inputRenames);
    const QSet<QString> changedOutputs = FindChangedPorts(previousOutputs, outputs, outputRenames);
    record.insert(QStringLiteral("inputs"), inputs);
    record.insert(QStringLiteral("outputs"), outputs);
    RepairLog log;
    // Publish declarations into the local read-only graph table before inspecting references.
    const auto replaceRecord = [](QJsonObject& inoutGraph, const QJsonObject& next) {
        QJsonArray records = inoutGraph.value(QStringLiteral("configurations")).toArray();
        for (qsizetype index = 0; index < records.size(); ++index)
        {
            if (records.at(index).toObject().value(QStringLiteral("id")) == next.value(QStringLiteral("id")))
            {
                records.replace(index, next);
                break;
            }
        }
        inoutGraph.insert(QStringLiteral("configurations"), records);
    };
    replaceRecord(graph, record);
    graphs.insert(file, graph);
    RepairUses(*this,
               graphs,
               record,
               file + QLatin1Char('#') + configuration,
               {QStringLiteral("input")},
               inputRenames,
               {},
               changedInputs,
               {},
               log,
               true);
    // Explicitly authored bad sources still reject; repairs are limited to changed old contracts.
    QJsonArray checkedOutputs = record.value(QStringLiteral("outputs")).toArray();
    for (qsizetype index = 0; index < checkedOutputs.size(); ++index)
    {
        QJsonObject output = checkedOutputs.at(index).toObject();
        const QString source = output.value(QStringLiteral("source")).toString();
        if (source.isEmpty() == true)
        {
            continue;
        }
        const QString type = GetEndpointType(*this, graphs, record, source, true);
        if (type == output.value(QStringLiteral("type")).toString() && type.isEmpty() == false)
        {
            continue;
        }
        QJsonObject previous;
        for (const QJsonValueConstRef value : previousOutputs)
        {
            const QJsonObject old = value.toObject();
            const QString oldId = old.value(QStringLiteral("id")).toString();
            if (outputRenames.value(oldId, oldId) == output.value(QStringLiteral("id")).toString())
            {
                previous = old;
            }
        }
        if (previous.value(QStringLiteral("source")).toString() == source &&
            previous.value(QStringLiteral("type")) != output.value(QStringLiteral("type")))
        {
            output.insert(QStringLiteral("source"), QString());
            checkedOutputs.replace(index, output);
            log.Add(QStringLiteral("%1#%2: clear output %3 source %4 (changed type).")
                        .arg(file, configuration, output.value(QStringLiteral("id")).toString(), source));
            continue;
        }
        outError =
            QStringLiteral("Output %1 needs a source of type %2, or Unconnected while drafting.")
                .arg(output.value(QStringLiteral("id")).toString(), output.value(QStringLiteral("type")).toString());
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    record.insert(QStringLiteral("outputs"), checkedOutputs);
    replaceRecord(graph, record);
    graphs.insert(file, graph);
    for (QMap<QString, QJsonObject>::iterator source = graphs.begin(); source != graphs.end(); ++source)
    {
        QJsonArray configurations = source.value().value(QStringLiteral("configurations")).toArray();
        for (qsizetype index = 0; index < configurations.size(); ++index)
        {
            QJsonObject caller = configurations.at(index).toObject();
            QSet<QString> instances;
            for (const QJsonValueConstRef value : caller.value(QStringLiteral("nodes")).toArray())
            {
                const QJsonObject node = value.toObject();
                if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph") &&
                    node.value(QStringLiteral("asset")).toString() == file &&
                    node.value(QStringLiteral("configuration")).toString() == configuration)
                {
                    instances.insert(node.value(QStringLiteral("id")).toString());
                }
            }
            if (instances.isEmpty() == false)
            {
                RepairUses(*this,
                           graphs,
                           caller,
                           source.key() + QLatin1Char('#') + caller.value(QStringLiteral("id")).toString(),
                           instances,
                           inputRenames,
                           outputRenames,
                           changedInputs,
                           changedOutputs,
                           log);
            }
            if (instances.isEmpty() == false)
            {
                configurations.replace(index, caller);
            }
        }
        if (source.value().value(QStringLiteral("configurations")).toArray() != configurations)
        {
            source.value().insert(QStringLiteral("configurations"), configurations);
        }
    }
    RenderGraphSnapshot candidate = mDraft;
    for (QMap<QString, QJsonObject>::const_iterator source = graphs.constBegin(); source != graphs.constEnd(); ++source)
    {
        QJsonObject original;
        if (GetGraph(source.key(), original) == true && original != source.value())
        {
            candidate.Files.insert(source.key(), EncodeCanonicalJson(source.value()));
        }
    }
    qsizetype total = 0;
    for (const QByteArray& bytes : candidate.Files)
    {
        if (bytes.size() > MAX_FILE_BYTES)
        {
            outError = QStringLiteral("The repaired package exceeds a file byte limit.");
            return RenderGraphDocumentStatus::Capacity;
        }
        total += bytes.size();
    }
    if (total > MAX_TOTAL_BYTES)
    {
        outError = QStringLiteral("The repaired package exceeds its total byte limit.");
        return RenderGraphDocumentStatus::Capacity;
    }
    if (log.Omitted > 0)
    {
        log.Details.append(
            QStringLiteral("%1 additional repairs; all are included in the same undo step.").arg(log.Omitted));
    }
    outSnapshot = candidate;
    outRepairs = log.Details;
    return RenderGraphDocumentStatus::Ok;
}
RenderGraphDocumentStatus
RenderGraphDocument::PreviewConfigurationInterface(const QString& file,
                                                   const QString& configuration,
                                                   const QJsonArray& inputs,
                                                   const QJsonArray& outputs,
                                                   QStringList& outRepairs,
                                                   QString& outError,
                                                   const QMap<QString, QString>& inputRenames,
                                                   const QMap<QString, QString>& outputRenames) const
{
    RenderGraphSnapshot candidate;
    return prepareConfigurationInterface(file,
                                         configuration,
                                         inputs,
                                         outputs,
                                         inputRenames,
                                         outputRenames,
                                         candidate,
                                         outRepairs,
                                         outError);
}
RenderGraphDocumentStatus
RenderGraphDocument::ReplaceConfigurationInterface(const QString& file,
                                                   const QString& configuration,
                                                   const QJsonArray& inputs,
                                                   const QJsonArray& outputs,
                                                   QString& outError,
                                                   const QMap<QString, QString>& inputRenames,
                                                   const QMap<QString, QString>& outputRenames)
{
    RenderGraphSnapshot candidate;
    QStringList repairs;
    const RenderGraphDocumentStatus status = prepareConfigurationInterface(file,
                                                                           configuration,
                                                                           inputs,
                                                                           outputs,
                                                                           inputRenames,
                                                                           outputRenames,
                                                                           candidate,
                                                                           repairs,
                                                                           outError);
    if (status != RenderGraphDocumentStatus::Ok || candidate == mDraft)
    {
        return status;
    }
    mLastMove.clear();
    return commit(candidate, false);
}
} // namespace ludus::editor
