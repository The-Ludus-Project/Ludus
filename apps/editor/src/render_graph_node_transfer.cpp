#include "internal/render_graph_document.h"
#include "internal/render_graph_pipeline_node.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace ludus::editor
{
using namespace foundation;
namespace
{
const QString TRANSFER_KIND = QStringLiteral("ludus.render-graph.nodes");

bool IsIdentifier(const QString& value)
{
    static const QRegularExpression expression(
        QStringLiteral("\\A[A-Za-z][A-Za-z0-9_-]{0,%1}\\z").arg(RenderGraphDocument::MAX_IDENTIFIER_LENGTH - 1));
    return expression.match(value).hasMatch();
}

qsizetype FindConfiguration(const QJsonArray& configurations, const QString& identity)
{
    for (qsizetype i = 0; i < configurations.size(); ++i)
    {
        if (configurations.at(i).toObject().value(QStringLiteral("id")).toString() == identity)
        {
            return i;
        }
    }
    return -1;
}

QString GetEndpointNode(const QString& endpoint)
{
    return endpoint.section(QLatin1Char('.'), 0, 0);
}

QByteArray ComputeDigest(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

bool CollectDependencies(const RenderGraphDocument& document,
                         const QJsonArray& nodes,
                         QJsonObject& inoutDependencies,
                         QSet<QString>& inoutVisited)
{
    for (const QJsonValueConstRef value : nodes)
    {
        const QJsonObject node = value.toObject();
        const QString kind = node.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("Subgraph"))
        {
            const QString asset = node.value(QStringLiteral("asset")).toString();
            if (asset.isEmpty() == true)
            {
                continue;
            }
            QJsonObject graph;
            if (document.GetGraph(asset, graph) == false)
            {
                return false;
            }
            inoutDependencies.insert(asset, QString::fromLatin1(ComputeDigest(document.GetDraft(asset))));
            const QString key = asset + QLatin1Char('#') + node.value(QStringLiteral("configuration")).toString();
            if (inoutVisited.contains(key) == true)
            {
                continue;
            }
            inoutVisited.insert(key);
            const QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
            const qsizetype index =
                FindConfiguration(configurations, node.value(QStringLiteral("configuration")).toString());
            if (index < 0 ||
                CollectDependencies(document,
                                    configurations.at(index).toObject().value(QStringLiteral("nodes")).toArray(),
                                    inoutDependencies,
                                    inoutVisited) == false)
            {
                return false;
            }
        }
        if (IsPipelineNode(kind) == true && node.value(QStringLiteral("shaders")).toArray().isEmpty() == true)
        {
            continue;
        }
        if (IsPipelineNode(kind) == true || kind == QStringLiteral("Raster"))
        {
            // Programs remain shared references. Across packages require the same manifest
            // contract and shader bytes, rather than silently rebinding equal program names.
            QString manifest = document.GetRootGraph();
            manifest.chop(QStringLiteral(".lurender.json").size());
            manifest += QStringLiteral(".lushader.json");
            if (document.GetDraft(manifest).isEmpty() == true)
            {
                return false;
            }
            for (const QString& file : document.GetFiles())
            {
                if (file == manifest || file.endsWith(QStringLiteral(".slang")) == true)
                {
                    inoutDependencies.insert(file, QString::fromLatin1(ComputeDigest(document.GetDraft(file))));
                }
            }
        }
    }
    return true;
}
} // namespace

RenderGraphDocumentStatus RenderGraphDocument::RenameNode(const QString& file,
                                                          const QString& configuration,
                                                          const QString& node,
                                                          const QString& name,
                                                          QString& outError)
{
    outError = QStringLiteral("Select an existing node and configuration before renaming it.");
    QJsonObject graph;
    if (GetGraph(file, graph) == false)
    {
        return RenderGraphDocumentStatus::Malformed;
    }
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    const qsizetype configurationIndex = FindConfiguration(configurations, configuration);
    if (configurationIndex < 0)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QJsonObject record = configurations.at(configurationIndex).toObject();
    QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    qsizetype nodeIndex = -1;
    for (qsizetype index = 0; index < nodes.size(); ++index)
    {
        const QString identity = nodes.at(index).toObject().value(QStringLiteral("id")).toString();
        if (identity == node)
        {
            if (nodeIndex >= 0)
            {
                outError = QStringLiteral("Resolve the duplicate node ID in Source before renaming it.");
                return RenderGraphDocumentStatus::InvalidEdit;
            }
            nodeIndex = index;
        }
        if (identity == name && name != node)
        {
            outError = QStringLiteral("A node named '%1' already exists in this configuration.").arg(name);
            return RenderGraphDocumentStatus::InvalidEdit;
        }
    }
    if (nodeIndex < 0)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    if (IsIdentifier(name) == false || name == QStringLiteral("input"))
    {
        outError = QStringLiteral("Use 1–%1 ASCII letters, digits, underscores or hyphens, starting with a letter. "
                                  "The name 'input' is reserved.")
                       .arg(MAX_IDENTIFIER_LENGTH);
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    if (name == node)
    {
        outError.clear();
        return RenderGraphDocumentStatus::Ok;
    }
    QJsonObject renamed = nodes.at(nodeIndex).toObject();
    renamed.insert(QStringLiteral("id"), name);
    nodes.replace(nodeIndex, renamed);
    record.insert(QStringLiteral("nodes"), nodes);
    const auto renameEndpoint = [&node, &name](const QString& endpoint) {
        return GetEndpointNode(endpoint) == node && endpoint.contains(QLatin1Char('.')) == true
                   ? name + endpoint.mid(node.size())
                   : endpoint;
    };
    QJsonArray edges = record.value(QStringLiteral("edges")).toArray();
    for (qsizetype index = 0; index < edges.size(); ++index)
    {
        QJsonObject edge = edges.at(index).toObject();
        for (const QString& field : {QStringLiteral("from"), QStringLiteral("to")})
        {
            edge.insert(field, renameEndpoint(edge.value(field).toString()));
        }
        edges.replace(index, edge);
    }
    record.insert(QStringLiteral("edges"), edges);
    QJsonArray outputs = record.value(QStringLiteral("outputs")).toArray();
    for (qsizetype index = 0; index < outputs.size(); ++index)
    {
        QJsonObject output = outputs.at(index).toObject();
        if (output.contains(QStringLiteral("source")) == true)
        {
            output.insert(QStringLiteral("source"), renameEndpoint(output.value(QStringLiteral("source")).toString()));
            outputs.replace(index, output);
        }
    }
    record.insert(QStringLiteral("outputs"), outputs);
    if (record.contains(QStringLiteral("layout")) == true)
    {
        QJsonObject layout = record.value(QStringLiteral("layout")).toObject();
        QJsonArray positions = layout.value(QStringLiteral("positions")).toArray();
        for (qsizetype index = 0; index < positions.size(); ++index)
        {
            QJsonObject position = positions.at(index).toObject();
            if (position.value(QStringLiteral("node")).toString() == node)
            {
                position.insert(QStringLiteral("node"), name);
                positions.replace(index, position);
            }
        }
        layout.insert(QStringLiteral("positions"), positions);
        record.insert(QStringLiteral("layout"), layout);
    }
    configurations.replace(configurationIndex, record);
    graph.insert(QStringLiteral("configurations"), configurations);
    RenderGraphSnapshot candidate = mDraft;
    candidate.Files.insert(file, EncodeCanonicalJson(graph));
    const RenderGraphDocumentStatus status =
        commit(candidate, false, {}, {}, {file, configuration, node}, {file, configuration, name});
    if (status == RenderGraphDocumentStatus::Ok)
    {
        mLastMove.clear();
        outError.clear();
    }
    else
    {
        outError = QStringLiteral("The renamed graph exceeds the source or package limits. The draft was preserved.");
    }
    return status;
}

RenderGraphDocumentStatus RenderGraphDocument::AddConfiguration(const QString& file, const QString& name)
{
    QJsonObject graph;
    if (GetGraph(file, graph) == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    if (IsIdentifier(name) == false || FindConfiguration(configurations, name) >= 0)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    if (configurations.size() >= MAX_CONFIGURATIONS)
    {
        return RenderGraphDocumentStatus::Capacity;
    }
    configurations.append(QJsonObject{{QStringLiteral("id"), name},
                                      {QStringLiteral("inputs"), QJsonArray{}},
                                      {QStringLiteral("outputs"), QJsonArray{}},
                                      {QStringLiteral("nodes"), QJsonArray{}},
                                      {QStringLiteral("edges"), QJsonArray{}}});
    graph.insert(QStringLiteral("configurations"), configurations);
    if (configurations.size() == 1)
    {
        graph.insert(QStringLiteral("default"), name);
    }
    RenderGraphSnapshot candidate = mDraft;
    candidate.Files.insert(file, EncodeCanonicalJson(graph));
    mLastMove.clear();
    return commit(candidate, false);
}

RenderGraphDocumentStatus RenderGraphDocument::CopyNodes(const QString& file,
                                                         const QString& configuration,
                                                         const QStringList& nodes,
                                                         const QVector<RenderGraphNodePosition>& positions,
                                                         QJsonObject& outPayload,
                                                         QString& outError) const
{
    outError = QStringLiteral("Select authored nodes from one configuration to copy.");
    QJsonObject graph;
    if (nodes.isEmpty() == true || GetGraph(file, graph) == false)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    const QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    const qsizetype index = FindConfiguration(configurations, configuration);
    if (index < 0)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    const QJsonObject record = configurations.at(index).toObject();
    QJsonArray copied;
    QJsonArray layout;
    QSet<QString> identities;
    const QJsonArray originalNodes = record.value(QStringLiteral("nodes")).toArray();
    for (const QJsonValueConstRef value : originalNodes)
    {
        const QString identity = value.toObject().value(QStringLiteral("id")).toString();
        if (nodes.contains(identity) == true)
        {
            copied.append(value);
            identities.insert(identity);
        }
    }
    if (identities.size() != nodes.size())
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QSet<QString> positioned;
    for (const RenderGraphNodePosition& position : positions)
    {
        if (identities.contains(position.Node) == true)
        {
            if (positioned.contains(position.Node) == true || position.X < -MAX_COORDINATE ||
                position.X > MAX_COORDINATE || position.Y < -MAX_COORDINATE || position.Y > MAX_COORDINATE)
            {
                return RenderGraphDocumentStatus::InvalidEdit;
            }
            positioned.insert(position.Node);
            layout.append(QJsonObject{{QStringLiteral("node"), position.Node},
                                      {QStringLiteral("x"), position.X},
                                      {QStringLiteral("y"), position.Y}});
        }
    }
    if (positioned.size() != identities.size())
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QJsonArray edges;
    const QJsonArray originalEdges = record.value(QStringLiteral("edges")).toArray();
    for (const QJsonValueConstRef value : originalEdges)
    {
        const QJsonObject edge = value.toObject();
        if (identities.contains(GetEndpointNode(edge.value(QStringLiteral("from")).toString())) == true &&
            identities.contains(GetEndpointNode(edge.value(QStringLiteral("to")).toString())) == true)
        {
            edges.append(edge);
        }
    }
    QJsonObject dependencies;
    QSet<QString> visited;
    if (CollectDependencies(*this, copied, dependencies, visited) == false)
    {
        outError = QStringLiteral("A copied node references an unavailable subgraph or shader manifest.");
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QJsonObject payload{{QStringLiteral("kind"), TRANSFER_KIND},
                        {QStringLiteral("version"), 1},
                        {QStringLiteral("package"), mPackageRoot},
                        {QStringLiteral("nodes"), copied},
                        {QStringLiteral("edges"), edges},
                        {QStringLiteral("positions"), layout},
                        {QStringLiteral("dependencies"), dependencies}};
    if (QJsonDocument(payload).toJson(QJsonDocument::Compact).size() > MAX_FILE_BYTES)
    {
        outError = QStringLiteral("The selected group exceeds the clipboard payload limit.");
        return RenderGraphDocumentStatus::Capacity;
    }
    outPayload = payload;
    outError.clear();
    return RenderGraphDocumentStatus::Ok;
}

RenderGraphDocumentStatus RenderGraphDocument::PasteNodes(const QString& file,
                                                          const QString& configuration,
                                                          const QJsonObject& payload,
                                                          int32 x,
                                                          int32 y,
                                                          QStringList& outNodes,
                                                          QString& outError)
{
    outError = QStringLiteral("The clipboard does not contain a valid render-graph node group.");
    const QJsonArray copied = payload.value(QStringLiteral("nodes")).toArray();
    const QJsonArray copiedEdges = payload.value(QStringLiteral("edges")).toArray();
    const QJsonArray copiedPositions = payload.value(QStringLiteral("positions")).toArray();
    if (payload.value(QStringLiteral("kind")).toString() != TRANSFER_KIND ||
        payload.value(QStringLiteral("version")).toInt() != 1 || copied.isEmpty() == true ||
        copied.size() > MAX_NODES || copiedEdges.size() > MAX_EDGES || copiedPositions.size() != copied.size() ||
        QJsonDocument(payload).toJson(QJsonDocument::Compact).size() > MAX_FILE_BYTES)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QJsonObject graph;
    if (GetGraph(file, graph) == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    const qsizetype index = FindConfiguration(configurations, configuration);
    if (index < 0)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    QJsonObject record = configurations.at(index).toObject();
    QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    QJsonArray edges = record.value(QStringLiteral("edges")).toArray();
    if (nodes.size() + copied.size() > MAX_NODES || edges.size() + copiedEdges.size() > MAX_EDGES)
    {
        outError = QStringLiteral("The pasted group exceeds this configuration's node or connection limit.");
        return RenderGraphDocumentStatus::Capacity;
    }
    const bool isSamePackage = payload.value(QStringLiteral("package")).toString() == mPackageRoot;
    const QJsonObject dependencies = payload.value(QStringLiteral("dependencies")).toObject();
    for (QJsonObject::const_iterator dependency = dependencies.begin(); dependency != dependencies.end(); ++dependency)
    {
        if (mDraft.Files.contains(dependency.key()) == false ||
            (isSamePackage == false &&
             QString::fromLatin1(ComputeDigest(mDraft.Files.value(dependency.key()))) != dependency.value().toString()))
        {
            outError = QStringLiteral("Paste needs matching project asset %1. Sources are reused, not imported.")
                           .arg(dependency.key());
            return RenderGraphDocumentStatus::InvalidEdit;
        }
    }
    QSet<QString> used;
    used.insert(QStringLiteral("input"));
    for (const QJsonValueConstRef value : nodes)
    {
        used.insert(value.toObject().value(QStringLiteral("id")).toString());
    }
    QMap<QString, QString> mapping;
    QStringList created;
    for (const QJsonValueConstRef value : copied)
    {
        QJsonObject node = value.toObject();
        const QString previous = node.value(QStringLiteral("id")).toString();
        if (IsIdentifier(previous) == false || previous == QStringLiteral("input") ||
            mapping.contains(previous) == true || node.value(QStringLiteral("kind")).toString().isEmpty() == true)
        {
            return RenderGraphDocumentStatus::InvalidEdit;
        }
        QString identity = previous;
        for (int32 suffix = 2; used.contains(identity) == true; ++suffix)
        {
            const QString ending = QLatin1Char('_') + QString::number(suffix);
            identity = previous.left(MAX_IDENTIFIER_LENGTH - ending.size()) + ending;
        }
        mapping.insert(previous, identity);
        used.insert(identity);
        created.append(identity);
        node.insert(QStringLiteral("id"), identity);
        nodes.append(node);
        if (IsPipelineNode(node.value(QStringLiteral("kind")).toString()) == true)
        {
            graph.insert(QStringLiteral("version"), 2);
        }
    }
    for (const QJsonValueConstRef value : copiedEdges)
    {
        QJsonObject edge = value.toObject();
        for (const QString& field : {QStringLiteral("from"), QStringLiteral("to")})
        {
            const QString endpoint = edge.value(field).toString();
            const QString identity = GetEndpointNode(endpoint);
            const QString port = endpoint.section(QLatin1Char('.'), 1);
            if (mapping.contains(identity) == false || IsIdentifier(port) == false)
            {
                return RenderGraphDocumentStatus::InvalidEdit;
            }
            edge.insert(field, mapping.value(identity) + QLatin1Char('.') + port);
        }
        edges.append(edge);
    }
    int32 minimumX = MAX_COORDINATE;
    int32 minimumY = MAX_COORDINATE;
    QSet<QString> positioned;
    for (const QJsonValueConstRef value : copiedPositions)
    {
        const QJsonObject position = value.toObject();
        const QString identity = position.value(QStringLiteral("node")).toString();
        const QJsonValue horizontal = position.value(QStringLiteral("x"));
        const QJsonValue vertical = position.value(QStringLiteral("y"));
        if (mapping.contains(identity) == false || positioned.contains(identity) == true ||
            horizontal.isDouble() == false || vertical.isDouble() == false ||
            horizontal.toDouble() != horizontal.toInt() || vertical.toDouble() != vertical.toInt() ||
            horizontal.toInt() < -MAX_COORDINATE || horizontal.toInt() > MAX_COORDINATE ||
            vertical.toInt() < -MAX_COORDINATE || vertical.toInt() > MAX_COORDINATE)
        {
            return RenderGraphDocumentStatus::InvalidEdit;
        }
        positioned.insert(identity);
        minimumX = qMin(minimumX, horizontal.toInt());
        minimumY = qMin(minimumY, vertical.toInt());
    }
    QJsonObject layout = record.value(QStringLiteral("layout")).toObject();
    QJsonArray positions = layout.value(QStringLiteral("positions")).toArray();
    for (const QJsonValueConstRef value : copiedPositions)
    {
        QJsonObject position = value.toObject();
        const int64 horizontal = int64{x} + position.value(QStringLiteral("x")).toInt() - minimumX;
        const int64 vertical = int64{y} + position.value(QStringLiteral("y")).toInt() - minimumY;
        if (horizontal < -MAX_COORDINATE || horizontal > MAX_COORDINATE || vertical < -MAX_COORDINATE ||
            vertical > MAX_COORDINATE)
        {
            outError = QStringLiteral("Paste would place nodes outside the canvas coordinate limits.");
            return RenderGraphDocumentStatus::InvalidEdit;
        }
        position.insert(QStringLiteral("node"), mapping.value(position.value(QStringLiteral("node")).toString()));
        position.insert(QStringLiteral("x"), static_cast<int32>(horizontal));
        position.insert(QStringLiteral("y"), static_cast<int32>(vertical));
        positions.append(position);
    }
    layout.insert(QStringLiteral("positions"), positions);
    record.insert(QStringLiteral("layout"), layout);
    record.insert(QStringLiteral("nodes"), nodes);
    record.insert(QStringLiteral("edges"), edges);
    configurations.replace(index, record);
    graph.insert(QStringLiteral("configurations"), configurations);
    RenderGraphSnapshot candidate = mDraft;
    candidate.Files.insert(file, EncodeCanonicalJson(graph));
    mLastMove.clear();
    const RenderGraphDocumentStatus status = commit(candidate, false);
    if (status == RenderGraphDocumentStatus::Ok)
    {
        outNodes = created;
        outError.clear();
    }
    else
    {
        outError = QStringLiteral("The pasted group cannot fit in the current graph source or package limits.");
    }
    return status;
}
} // namespace ludus::editor
