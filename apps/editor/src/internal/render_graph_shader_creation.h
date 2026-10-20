#pragma once

#include "internal/render_graph_document.h"

namespace ludus::editor
{
/// Prepare new sources and manifest entries for a pipeline in caller-owned candidates.
/// Existing programs must match the requested stage; Program receives a generated name.
/// The document commits all files together. No disk writes occur here.
[[nodiscard]] RenderGraphDocumentStatus PreparePipelineShaders(RenderGraphSnapshot& inoutSnapshot,
                                                               QJsonObject& inoutNode,
                                                               const QString& manifestFile,
                                                               const QString& packageRoot);
} // namespace ludus::editor
