#pragma once

#include "internal/render_graph_document.h"

namespace ludus::editor
{
/// End a template error with "The draft was preserved." exactly once.
void AppendDraftPreserved(QString& inoutError);
/// Upgrade unchanged engine-default manifest budgets to the selected template's budgets.
/// Custom limits are retained. Returns false when embedded defaults are unavailable; no disk writes.
[[nodiscard]] bool UpgradeDefaultTemplateBudgets(QJsonObject& inoutManifest, const QJsonObject& source);
/// Check the merged program count and report a blocked increase through the optional request.
/// Only explicit approval raises budgets.programs to the exact required count. Programs times target
/// profiles beyond the supported artifact ceiling fail without a request, since no approval admits them.
/// On success the legacy budgets.artifacts and budgets.pipelines fields are removed; no disk writes.
[[nodiscard]] bool EnsureTemplateProgramBudget(QJsonObject& inoutManifest,
                                               foundation::int32 requiredPrograms,
                                               const QString& templateName,
                                               QString& outError,
                                               RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull);
/// Merge a template's typed Graph settings into a manifest draft. A setting already present with the
/// same definition keeps its authored value; a different definition reports Conflict, naming
/// templateName. More than LUDUS_EDITOR_MAX_SETTINGS merged settings report Capacity unless removing
/// authored settings that no `.slang` file in snapshot names makes them fit: the optional request
/// then receives those settings, and only the ones it approved are removed. The template's own
/// settings are never removed. inoutManifest changes only on Ok. GUI thread; no disk writes.
/// @param snapshot Candidate package, including the template's copied shader sources.
[[nodiscard]] RenderGraphDocumentStatus MergeTemplateSettings(const RenderGraphSnapshot& snapshot,
                                                              QJsonObject& inoutManifest,
                                                              const QJsonArray& additions,
                                                              const QString& templateName,
                                                              QString& outError,
                                                              RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull);
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
/// Prepare the compute-ray-tracing, simple-compute-ray-tracing, veach-guibas-mis, ris-projected-solid-angle,
/// polygonal-light-sampling, slice-sampling-luminaire, steinberg-plt, steinberg-physical-optics, restir-gi or
/// rao-blackwell-mh template in an absent/empty library. The simple template imports one triangle-loop kernel under
/// simple_compute_ray_tracing/ with no Buffer. The original template imports a primary-hits and a shade scene compute
/// pass sharing one render-size hit Buffer under compute_ray_tracing/. veach-guibas-mis imports the same primary-hits
/// pass with three shade passes (MIS, light sampling and BSDF sampling) under veach_guibas_mis/; its light and BSDF
/// images are optional library outputs. ris-projected-solid-angle has the same four-pass shape under
/// ris_projected_solid_angle/, with compute-BVH passes, and also enables the ray technique's
/// scene_interface.polygonal_light_sampling helpers. polygonal-light-sampling imports four compute-BVH shade passes
/// under polygonal_light_sampling/, the first of which also records the hits, with the same helper requirement and a
/// visibility budget of 16. slice-sampling-luminaire imports three compute-BVH shade passes under
/// slice_sampling_luminaire/ (defensive slice sampling, which also records the hits, slice sampling and uniform panel
/// area), each with one shadow ray. steinberg-plt imports the primary-hits pass with wave-optics, geometric-optics and
/// coherence passes under steinberg_plt/; its geometric and coherence images are optional library outputs.
/// steinberg-physical-optics has the same four-pass shape under steinberg_physical_optics/ (wave optics, radiometric
/// and coherence), with three surface and three visibility queries. restir-gi imports the initial-sample, temporal,
/// spatial and shade passes under restir_gi/, which share three render-size Buffers (initial samples, temporal and
/// spatial reservoirs), with four surface and sixteen visibility queries. rao-blackwell-mh imports three compute-BVH
/// passes (RB, standard MH, waste recycling), sharing two Buffers (208 bytes per render pixel), eight surface queries
/// and five Graph settings. Its weighted results replace successive ratios. Kernels arrive with their local include
/// closure. A template's Graph settings merge like ReSTIR's (existing values are kept). The shared ray technique gains
/// scene_interface version 1 when it lacks one, and its query budgets are raised to the template's; its callbacks,
/// payload and larger budgets are retained. Requires a Metal or Vulkan profile. Authored shader bytes are never
/// replaced: source/program collisions, unsupported scene interface versions and capacity limits report a repair.
/// Caller owns/discards the snapshot; GUI thread; no disk writes.
[[nodiscard]] RenderGraphDocumentStatus
PrepareComputeRayTracingConfiguration(RenderGraphSnapshot& inoutSnapshot,
                                      const QString& rootGraph,
                                      const QString& file,
                                      const QString& configuration,
                                      QString& outError,
                                      const QString& templateIdentity,
                                      RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull = nullptr);
/// A renderer-owned scene compute template: its registry identity (the embedded resource directory
/// and, with '-' replaced by '_', the destination source directory), the prefix that keeps its
/// imported program IDs distinct, and the display name used in repairs.
struct SceneComputeTemplate final
{
    QString Identity;
    QString ProgramPrefix;
    QString Name;
};
/// Shared import of a scene compute template, with the contract of PrepareComputeRayTracingConfiguration:
/// prefixed kernels with their include closure, Graph settings, and the ray technique's
/// scene_interface with every query budget the template declares (surface, visibility and, when
/// present, line) raised to the template's. Caller owns/discards the snapshot; GUI thread; no disk writes.
[[nodiscard]] RenderGraphDocumentStatus
PrepareSceneComputeTemplateConfiguration(RenderGraphSnapshot& inoutSnapshot,
                                         const QString& rootGraph,
                                         const QString& file,
                                         const QString& configuration,
                                         QString& outError,
                                         const SceneComputeTemplate& selectedTemplate,
                                         RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull = nullptr);
/// Prepare the line-sampling template (point-line design D8a) in an absent/empty library: a
/// primary-hits pass and the ShadeLineSampling and ShadeVisibleFraction triangle-loop passes sharing
/// one render-size hit Buffer, imported under line_sampling/ with LineSampling-prefixed program IDs;
/// visibleFraction is an optional library output. The ray technique's scene_interface gains the
/// template's line_queries budget. Requires a Metal or Vulkan profile but no native ray tracing.
/// Same transaction contract as PrepareComputeRayTracingConfiguration.
[[nodiscard]] RenderGraphDocumentStatus
PrepareLineSamplingConfiguration(RenderGraphSnapshot& inoutSnapshot,
                                 const QString& rootGraph,
                                 const QString& file,
                                 const QString& configuration,
                                 QString& outError,
                                 RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull = nullptr);
/// Prepare the salesin-jarosz-lines template (point-line design D8b) in an absent/empty library: a
/// primary-hits pass and the ShadeMultipleImportance, ShadeLineSampling and ShadePointSampling
/// triangle-loop passes sharing one render-size hit Buffer, imported under salesin_jarosz_lines/ with
/// PointLine-prefixed program IDs and the POINT_LINE_MIS_HEURISTIC and POINT_LINE_MIS_POWER_EXPONENT Graph settings;
/// lineSampling and pointSampling are optional library outputs. The ray technique's scene_interface
/// gains the template's visibility and line_queries budgets. Requires a Metal or Vulkan profile but no
/// native ray tracing. Same transaction contract as PrepareComputeRayTracingConfiguration.
[[nodiscard]] RenderGraphDocumentStatus
PreparePointLineConfiguration(RenderGraphSnapshot& inoutSnapshot,
                              const QString& rootGraph,
                              const QString& file,
                              const QString& configuration,
                              QString& outError,
                              RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull = nullptr);
/// Prepare a built-in configuration, inserting a library/Subgraph when selected on the root canvas.
/// Supports restir-di, visibility-buffer, visibility-secondary-rays, tiled-vbuffer-nee,
/// compute-ray-tracing, simple-compute-ray-tracing, veach-guibas-mis, line-sampling, ris-projected-solid-angle,
/// salesin-jarosz-lines, polygonal-light-sampling, slice-sampling-luminaire, steinberg-plt,
/// steinberg-physical-optics and restir-gi.
/// Same transaction contract as PrepareRestirTemplate; invalid identities and conflicts leave publication to the
/// caller.
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
