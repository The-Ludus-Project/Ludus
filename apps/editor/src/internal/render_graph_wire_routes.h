#pragma once

#include "render_graph_layout_policy.h"

#include <QJsonArray>
#include <QJsonObject>

namespace ludus::editor
{
/// Validate bounded integer route geometry and unique endpoint pairs. When requested, each
/// pair must name an existing edge or a source-to-#outputs.port configuration output link.
[[nodiscard]] bool AreRenderGraphWireRoutesValid(const QJsonArray& routes,
                                                 const QJsonObject& configuration,
                                                 bool shouldRequireConnection = true);
/// Remove structurally valid routes whose exact connection disappeared. Malformed metadata
/// is preserved for Source repair. Returns whether configuration layout changed; no graph edits.
[[nodiscard]] bool PruneRenderGraphWireRoutes(QJsonObject& inoutConfiguration);
} // namespace ludus::editor
