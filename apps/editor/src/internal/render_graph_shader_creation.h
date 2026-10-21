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
/// Generate the scene-aware callback and record its manifest input/output types.
[[nodiscard]] QByteArray MakeSceneRaySource(QJsonObject& inoutProgram);
/// True when the shared ray technique declares the scene-aware interface.
[[nodiscard]] bool HasSceneRayInterface(const QJsonObject& manifest);
/// Initialize scene callbacks for a new ray pipeline when no ray technique exists.
/// Caller owns the unpublished candidate; existing techniques and sources are preserved.
[[nodiscard]] RenderGraphDocumentStatus PrepareSceneRayDefaults(RenderGraphSnapshot& inoutSnapshot,
                                                                QJsonObject& inoutManifest,
                                                                const QJsonObject& node,
                                                                const QString& packageRoot);
/// Adapt the shared legacy ray technique, preserving original sources and callback behavior.
/// Updates all package graph references in the candidate; caller discards it on failure.
[[nodiscard]] RenderGraphDocumentStatus ConvertSceneRayInterface(RenderGraphSnapshot& inoutSnapshot,
                                                                 const QString& manifestFile,
                                                                 const QString& packageRoot,
                                                                 QString& outError);
/// Reconcile complete pipeline declarations in the root import closure with its manifest.
/// Preserves incomplete drafts and authored metadata; conflicts/capacity failures change no files.
/// Call on an unpublished candidate so shader, graph and technique edits share one transaction.
[[nodiscard]] RenderGraphDocumentStatus
PrepareRenderGraphTechniques(RenderGraphSnapshot& inoutSnapshot, const QString& rootGraph, QString& outError);
} // namespace ludus::editor
