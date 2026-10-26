#pragma once

#include "internal/render_graph_document.h"

namespace ludus::editor
{
/// Upgrade unchanged engine-default manifest budgets to the selected template's budgets.
/// Custom limits are retained. Returns false when embedded defaults are unavailable; no disk writes.
[[nodiscard]] bool UpgradeDefaultTemplateBudgets(QJsonObject& inoutManifest, const QJsonObject& source);
/// Check the merged program count and report a blocked increase through the optional request.
/// Only explicit approval raises budgets.programs to the exact required count; no disk writes.
[[nodiscard]] bool EnsureTemplateProgramBudget(QJsonObject& inoutManifest,
                                               foundation::int32 requiredPrograms,
                                               const QString& templateName,
                                               QString& outError,
                                               RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull);
/// Merge a template's typed Graph settings into a manifest draft. A setting already present with the
/// same definition keeps its authored value; a different definition reports Conflict and more than
/// LUDUS_EDITOR_MAX_SETTINGS settings report Capacity, naming templateName. inoutManifest changes only
/// on Ok. GUI thread; no disk writes.
[[nodiscard]] RenderGraphDocumentStatus MergeTemplateSettings(QJsonObject& inoutManifest,
                                                              const QJsonArray& additions,
                                                              const QString& templateName,
                                                              QString& outError);
/// Prepare a reusable ReSTIR configuration and its shader/manifest closure in an unpublished snapshot.
/// templateRoot selects a bounded source directory; empty uses the embedded renderer assets.
/// The destination must be absent or empty. Existing shader bytes, configurations and setting values
/// are retained; incompatible shared ray interfaces fail with a repair message. GUI thread, no disk writes.
[[nodiscard]] RenderGraphDocumentStatus
PrepareRestirConfiguration(RenderGraphSnapshot& inoutSnapshot,
                           const QString& rootGraph,
                           const QString& file,
                           const QString& configuration,
                           QString& outError,
                           const QString& templateRoot = {},
                           RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull = nullptr);
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
/// Prepare visibility-buffer, visibility-secondary-rays or tiled-vbuffer-nee in an absent/empty
/// library configuration. Ray sources arrive with their local include closure. Caller owns/discards
/// the snapshot; compatible RendererStatic fields are extended, authored shader bytes are preserved,
/// shared ray query budgets are raised to the template's needs, and ABI/source/budget conflicts
/// report a repair. GUI thread; no disk writes.
[[nodiscard]] RenderGraphDocumentStatus
PrepareVisibilityConfiguration(RenderGraphSnapshot& inoutSnapshot,
                               const QString& rootGraph,
                               const QString& file,
                               const QString& configuration,
                               const QString& templateIdentity,
                               QString& outError,
                               RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull = nullptr);
/// Prepare the compute-ray-tracing, simple-compute-ray-tracing or veach-guibas-mis template in an
/// absent/empty library. The simple template imports one triangle-loop kernel under
/// simple_compute_ray_tracing/ with no Buffer. The original template imports a primary-hits and a shade
/// scene compute pass sharing one render-size hit Buffer under compute_ray_tracing/. veach-guibas-mis
/// imports the same primary-hits pass with three shade passes (MIS, light sampling and BSDF sampling)
/// under veach_guibas_mis/; its light and BSDF images are optional library outputs. Kernels arrive with
/// their local include closure. A template's Graph settings merge like ReSTIR's (existing values are
/// kept). The shared ray technique gains scene_interface version 1 when it
/// lacks one, and its query budgets are raised to the template's; its callbacks, payload and larger
/// budgets are retained. Requires a Metal or Vulkan profile. Authored shader bytes are never replaced:
/// source/program collisions, unsupported scene interface versions and capacity limits report a repair.
/// Caller owns/discards the snapshot; GUI thread; no disk writes.
[[nodiscard]] RenderGraphDocumentStatus
PrepareComputeRayTracingConfiguration(RenderGraphSnapshot& inoutSnapshot,
                                      const QString& rootGraph,
                                      const QString& file,
                                      const QString& configuration,
                                      QString& outError,
                                      const QString& templateIdentity,
                                      RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull = nullptr);
/// Prepare a built-in configuration, inserting a library/Subgraph when selected on the root canvas.
/// Supports restir-di, visibility-buffer, visibility-secondary-rays, tiled-vbuffer-nee,
/// compute-ray-tracing, simple-compute-ray-tracing and veach-guibas-mis. Same transaction contract
/// as PrepareRestirTemplate; invalid identities and conflicts leave publication to the caller.
[[nodiscard]] RenderGraphDocumentStatus
PrepareConfigurationTemplate(RenderGraphSnapshot& inoutSnapshot,
                             const QString& rootGraph,
                             const QString& file,
                             const QString& configuration,
                             const QString& parentConfiguration,
                             const QString& packageRoot,
                             const QString& templateIdentity,
                             QString& outError,
                             RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull = nullptr);
} // namespace ludus::editor
