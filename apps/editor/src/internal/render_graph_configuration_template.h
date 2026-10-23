#pragma once

#include "internal/render_graph_document.h"

namespace ludus::editor
{
/// Prepare a reusable ReSTIR configuration and its shader/manifest closure in an unpublished snapshot.
/// templateRoot selects a bounded source directory; empty uses the embedded renderer assets.
/// The destination must be absent or empty. Existing shader bytes, configurations and setting values
/// are retained; incompatible shared ray interfaces fail with a repair message. GUI thread, no disk writes.
[[nodiscard]] RenderGraphDocumentStatus PrepareRestirConfiguration(RenderGraphSnapshot& inoutSnapshot,
                                                                   const QString& rootGraph,
                                                                   const QString& file,
                                                                   const QString& configuration,
                                                                   QString& outError,
                                                                   const QString& templateRoot = {});
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
/// Prepare visibility-buffer or visibility-secondary-rays in an absent/empty library configuration.
/// Caller owns/discards the snapshot; compatible RendererStatic fields are extended, authored shader
/// bytes are preserved, and ABI/source/budget conflicts report a repair. GUI thread; no disk writes.
[[nodiscard]] RenderGraphDocumentStatus PrepareVisibilityConfiguration(RenderGraphSnapshot& inoutSnapshot,
                                                                       const QString& rootGraph,
                                                                       const QString& file,
                                                                       const QString& configuration,
                                                                       const QString& templateIdentity,
                                                                       QString& outError);
/// Prepare a built-in configuration, inserting a library/Subgraph when selected on the root canvas.
/// Supports restir-di, visibility-buffer and visibility-secondary-rays. Same transaction contract
/// as PrepareRestirTemplate; invalid identities and conflicts leave publication to the caller.
[[nodiscard]] RenderGraphDocumentStatus PrepareConfigurationTemplate(RenderGraphSnapshot& inoutSnapshot,
                                                                     const QString& rootGraph,
                                                                     const QString& file,
                                                                     const QString& configuration,
                                                                     const QString& parentConfiguration,
                                                                     const QString& packageRoot,
                                                                     const QString& templateIdentity,
                                                                     QString& outError);
} // namespace ludus::editor
