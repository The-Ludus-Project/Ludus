#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace ludus::editor
{
class RenderGraphDocument;

/// One typed node port, including graph-only RenderSize values and texture inputs.
struct RenderGraphPort final
{
    QString Id;
    QString Type;
};

/// Inputs (consumed) and outputs (produced) of one authored node or configuration interface.
struct RenderGraphNodePorts final
{
    QVector<RenderGraphPort> Inputs;
    QVector<RenderGraphPort> Outputs;
};

/// Read authored id/type pairs without changing declarations; storage is owned by the result.
[[nodiscard]] QVector<RenderGraphPort> ReadRenderGraphInterfacePorts(const QJsonArray& ports);

/// Ports of one node, mirroring ludus_tools.render_graph `node_ports`. A Subgraph's ports
/// are its referenced configuration's interface, resolved from the package draft.
/// @return False when a Subgraph's asset or configuration is not in the draft.
[[nodiscard]] bool
GetRenderGraphNodePorts(const RenderGraphDocument& document, const QJsonObject& node, RenderGraphNodePorts& outPorts);

} // namespace ludus::editor
