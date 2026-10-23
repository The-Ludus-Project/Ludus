#pragma once

#include "internal/render_graph_document.h"

namespace ludus::editor
{
/// Prepare a reusable ReSTIR configuration and its shader/manifest closure in an unpublished snapshot.
/// The destination must be absent or empty. Existing shader bytes, configurations and setting values
/// are retained; incompatible shared ray interfaces fail with a repair message. GUI thread, no disk writes.
[[nodiscard]] RenderGraphDocumentStatus PrepareRestirConfiguration(RenderGraphSnapshot& inoutSnapshot,
                                                                   const QString& rootGraph,
                                                                   const QString& file,
                                                                   const QString& configuration,
                                                                   QString& outError);
/// Prepare library insertion from a root canvas, or configuration application inside a library.
/// Parent selection is consulted only for root insertion. The caller owns/discards the candidate;
/// no disk writes occur, and existing on-disk assets are never replaced.
[[nodiscard]] RenderGraphDocumentStatus PrepareRestirTemplate(RenderGraphSnapshot& inoutSnapshot,
                                                              const QString& rootGraph,
                                                              const QString& file,
                                                              const QString& configuration,
                                                              const QString& parentConfiguration,
                                                              const QString& packageRoot,
                                                              QString& outError);
} // namespace ludus::editor
