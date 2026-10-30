#pragma once

// Budget planner: fits a SceneIR to the LUSCN version 6 import profile of
// Ludus::SceneFormat (format::IMPORT_LIMITS).
//
// Placements (world shapes plus expanded instances) are ranked by world-space
// area for material selection; all geometry is retained or the import fails its
// encoded bundle byte budget. Each source material
// keeps its own slot, and each group of identical emitters gets one, while they
// fit the mesh/material count; beyond it the largest source materials by area
// keep their slot and the rest merge into their nearest kept material. Each slot
// becomes one world-space mesh and one object.

#include "import_diagnostics.h"
#include "scene_ir.h"

#include <ludus/content/scene_cook/scene_cook.h>

namespace ludus::content::scene_cook::importers
{
/// One cooked material slot.
struct PlannedMaterial final
{
    /// IR material indices merged into this slot (IR_NONE = pbrt default material).
    Array<uint32> Sources;
    /// Source material whose terms the slot uses.
    uint32 Representative = IR_NONE;
    float64 BaseColour[3] = {.5, .5, .5};
    uint32 Texture = IR_NONE;
    bool IsTextureRepeated = true;
    float64 Metallic = 0.0;
    float64 Roughness = 1.0;
    /// Representative material is an alpha mask (glTF MASK, MTL map_d); see CookTextures.
    bool IsAlphaMasked = false;
    /// Mask threshold in [0, 1]; zero for opaque slots.
    float64 AlphaCutoff = 0.0;
    /// Cooked texture indices of the slot's LUSCN v6 linear surface map and sRGB emission
    /// map, or IR_NONE; set by the texture cook, not the planner.
    uint32 CookedSurfaceTexture = IR_NONE;
    uint32 CookedEmissionTexture = IR_NONE;
    /// Tangent-normal strength of the surface map; zero without a normal map.
    float64 NormalScale = 0.0;
    float64 Emission[3] = {0.0, 0.0, 0.0};
    bool IsEmissive = false;
    bool IsEmissionTwoSided = false;
    bool HasEmissionPower = false;
    /// Sum of requested lumens after scale when all merged placements are power-authored.
    float64 EmissionPowerLumens = 0.0;
    /// Original shape indices retained for emitter provenance.
    Array<uint32> EmitterSources;
    /// World-space area of the surfaces in this slot.
    float64 Area = 0.0;
    /// Placements (indices into CookPlan::Placements) baked into this slot's mesh.
    Array<uint32> Placements;
    uint32 VertexCount = 0;
    uint32 IndexCount = 0;
};

/// One shape placement after instance expansion.
struct Placement final
{
    uint32 Shape = IR_NONE;
    uint32 EmitterGroup = IR_NONE;
    math::Matrix4d Transform = math::Matrix4d::Identity();
    float64 Area = 0.0;
    bool IsInstanced = false;
    bool IsKept = false;
};

/// Result of PlanBudget.
struct CookPlan final
{
    Array<Placement> Placements;
    Array<PlannedMaterial> Materials;
    uint32 SourceVertices = 0;
    uint32 DroppedPlacements = 0;
    uint32 DroppedVertices = 0;
};

/// Plan scene against the format limits; diagnostics go to inoutLog.
/// @return Ok, Invalid when no drawable geometry exists or the complete scene
/// exceeds the bundle budget, Cancelled or OutOfMemory. Failure preserves outPlan.
[[nodiscard]] Status
PlanBudget(const SceneIr& scene, DiagnosticLog& inoutLog, CookPlan& outPlan, const Cancellation* cancelOrNull) noexcept;
} // namespace ludus::content::scene_cook::importers
