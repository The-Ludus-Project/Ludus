#pragma once

#include "internal/render_graph_shader_creation.h"

#include <QVector>

namespace ludus::editor
{
struct RenderGraphPort;

/// Add the typed members of each declared Scene input; member IDs use _geometry/_materials/_lights.
void AppendSceneBundleInputs(QVector<RenderGraphPort>& inoutPorts);
/// True for a Scene source or a node declaring a full Scene input.
[[nodiscard]] bool HasSceneBundle(const QJsonObject& node);
/// Resolve a pipeline's implicit Scene member ID, or return empty for other ports.
[[nodiscard]] QString GetSceneBundlePortType(const QJsonObject& node, const QString& port);
/// True for a component type. Derived ports may be hidden while disconnected.
[[nodiscard]] bool IsSceneBundleMember(const RenderGraphPort& port);
/// Upgrade complete legacy ray callbacks when their Scene bundle is connected. Candidate-only; no disk writes.
[[nodiscard]] RenderGraphDocumentStatus PrepareConnectedSceneShaders(RenderGraphSnapshot& inoutSnapshot,
                                                                     const QString& rootGraph,
                                                                     const QString& packageRoot,
                                                                     QString* outErrorOrNull = nullptr);
} // namespace ludus::editor
