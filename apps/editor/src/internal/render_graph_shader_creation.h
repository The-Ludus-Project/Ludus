#pragma once

#include "internal/render_graph_document.h"

namespace ludus::editor
{
/// Prepare new sources and manifest entries for a pipeline in caller-owned candidates.
/// Reconcile previous assignments, preserving authored bytes on rename and stage edits.
/// Program receives a generated name; unreferenced untouched generated drafts are removed.
/// The document commits all files together. No disk writes occur here.
[[nodiscard]] RenderGraphDocumentStatus PreparePipelineShaders(RenderGraphSnapshot& inoutSnapshot,
                                                               QJsonObject& inoutNode,
                                                               const QJsonObject& previousNode,
                                                               const QString& manifestFile,
                                                               const QString& packageRoot);
/// Reconcile complete pipeline declarations in the root import closure with its manifest.
/// Preserves incomplete drafts and authored metadata; conflicts/capacity failures change no files.
/// Call on an unpublished candidate so shader, graph and technique edits share one transaction.
[[nodiscard]] RenderGraphDocumentStatus
PrepareRenderGraphTechniques(RenderGraphSnapshot& inoutSnapshot, const QString& rootGraph, QString& outError);
} // namespace ludus::editor
