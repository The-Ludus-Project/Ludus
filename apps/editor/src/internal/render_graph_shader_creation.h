#pragma once

#include "internal/render_graph_document.h"

namespace ludus::editor
{
/// Prepare new sources and manifest entries for a pipeline in caller-owned candidates.
/// Reconcile previous assignments, preserving authored bytes on rename and stage edits.
/// Program requests an independent generated name, including for an existing assignment.
/// Generated creation/row renames avoid package-wide program IDs and source paths;
/// unreferenced untouched generated drafts are removed.
/// The document commits all files together. No disk writes occur here.
/// Creating programs grows budgets.programs to fit, retaining a larger authored value. Target artifacts
/// (programs times profiles) have no authored budget; the supported ceiling still rejects, and outErrorOrNull
/// receives the repair diagnostic when supplied.
[[nodiscard]] RenderGraphDocumentStatus PreparePipelineShaders(RenderGraphSnapshot& inoutSnapshot,
                                                               QJsonObject& inoutNode,
                                                               const QJsonObject& previousNode,
                                                               const QString& manifestFile,
                                                               const QString& packageRoot,
                                                               QString* outErrorOrNull = nullptr);
/// Generate the scene-aware callback, or the C03 kernel for a compute stage, and record its manifest
/// input/output types. Returns empty bytes for stages without a scene-aware form.
[[nodiscard]] QByteArray MakeSceneRaySource(QJsonObject& inoutProgram);
/// True when the shared ray technique declares the scene-aware interface.
[[nodiscard]] bool HasSceneRayInterface(const QJsonObject& manifest);
/// Initialize scene callbacks for a new ray pipeline, or the scene interface and automatic triangle
/// recipe for a scene compute pass, when no ray technique exists.
/// Caller owns the unpublished candidate; existing techniques and sources are preserved.
[[nodiscard]] RenderGraphDocumentStatus PrepareSceneRayDefaults(RenderGraphSnapshot& inoutSnapshot,
                                                                QJsonObject& inoutManifest,
                                                                const QJsonObject& node,
                                                                const QString& packageRoot);
/// Enable builder-managed legacy adapters and default payload without creating project helper sources.
/// Reuses program records and IDs, preserving graph references and authored admission budgets.
/// Caller discards the unpublished candidate on failure.
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
