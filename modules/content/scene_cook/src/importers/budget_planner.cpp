#include "budget_planner.h"

#include "transform_utilities.h"

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::scene_cook::importers
{
namespace
{
namespace format = content::scene_format;
constexpr uint32 MAX_REPORTED_COUNT = ~uint32{0};

[[nodiscard]] bool IsCancelled(const Cancellation* cancelOrNull) noexcept
{
    return cancelOrNull != nullptr && cancelOrNull->Requested != nullptr &&
           cancelOrNull->Requested(cancelOrNull->ContextOrNull) == true;
}

[[nodiscard]] bool IsEmissive(const IrShape& shape) noexcept
{
    return shape.Emission[0] > 0.0 || shape.Emission[1] > 0.0 || shape.Emission[2] > 0.0;
}

[[nodiscard]] float64 ComputeLuminance(const float64 (&colour)[3]) noexcept
{
    return .2126 * colour[0] + .7152 * colour[1] + .0722 * colour[2];
}

[[nodiscard]] float64 ComputeWorldArea(const IrMesh& mesh, const math::Matrix4d& transform) noexcept
{
    float64 area = 0.0;
    const auto load = [&](uint32 index) noexcept {
        const float32* p = mesh.Positions.GetData() + usize{index} * 3;
        return TransformPoint(
            transform,
            math::Vector3d{static_cast<float64>(p[0]), static_cast<float64>(p[1]), static_cast<float64>(p[2])});
    };
    for (usize i = 0; i + 2 < mesh.Indices.GetSize(); i += 3)
    {
        const math::Vector3d a = load(mesh.Indices[i]);
        const math::Vector3d b = load(mesh.Indices[i + 1]);
        const math::Vector3d c = load(mesh.Indices[i + 2]);
        area += .5 * math::Length(math::Cross(b - a, c - a));
    }
    return math::IsFinite(area) == true ? area : 0.0;
}

/// Material terms of an IR index (IR_NONE is pbrt's default diffuse 0.5).
struct MaterialTerms final
{
    float64 BaseColour[3] = {.5, .5, .5};
    uint32 Texture = IR_NONE;
    float64 Metallic = 0.0;
    float64 Roughness = 1.0;
};

[[nodiscard]] MaterialTerms GetTerms(const SceneIr& scene, uint32 material) noexcept
{
    MaterialTerms terms;
    if (material == IR_NONE || material >= scene.Materials.GetSize())
    {
        return terms;
    }
    const IrMaterial& source = scene.Materials[material];
    for (usize k = 0; k < 3; ++k)
    {
        terms.BaseColour[k] = source.BaseColour[k];
    }
    terms.Texture = source.BaseColourTexture;
    terms.Metallic = source.Metallic;
    terms.Roughness = source.Roughness;
    return terms;
}

[[nodiscard]] float64 MeasureDistance(const MaterialTerms& left, const MaterialTerms& right) noexcept
{
    float64 distance = 0.0;
    for (usize k = 0; k < 3; ++k)
    {
        const float64 delta = left.BaseColour[k] - right.BaseColour[k];
        distance += delta * delta;
    }
    distance += (left.Metallic - right.Metallic) * (left.Metallic - right.Metallic);
    distance += .25 * (left.Roughness - right.Roughness) * (left.Roughness - right.Roughness);
    if (left.Texture != right.Texture)
    {
        distance += .1;
    }
    return distance;
}

struct EmitterUse final
{
    math::Vector3 Emission{};
    bool IsTwoSided = false;
    bool HasPower = false;
    /// Every member is decorative (IrShape::IsEmissionOptional); the group may be demoted.
    bool IsOptional = false;
    bool IsDemoted = false;
    /// Source material of a decorative group, whose surface terms and maps the slot keeps;
    /// IR_NONE for required (pbrt area-light) groups, which cook as white untextured emitters.
    uint32 Material = IR_NONE;
    float64 PowerLumens = 0.0;
    float64 Area = 0.0;
    Array<uint32> Sources;
};
// Thanks to Pharr, Jakob and Humphreys, Physically Based Rendering, fourth
// edition, section 12.5, for diffuse power = PI * area * luminance * side count.
// https://pbr-book.org/4ed/Light_Sources/Area_Lights
// The frozen RGB/Y proxy is an explicit approximation of PBRT's spectral
// photometric normalization. Resolve each bound Shape/placement independently.
[[nodiscard]] bool ResolveEmission(const IrShape& shape, float64 area, math::Vector3& outEmission) noexcept
{
    const float64 luminance = ComputeLuminance(shape.Emission);
    float64 scale = shape.EmissionScale;
    if (shape.HasEmissionPower == true)
    {
        if (area <= 0.0)
        {
            return false;
        }
        scale *= shape.EmissionPowerLumens / (math::kPi * area * (shape.IsEmissionTwoSided == true ? 2.0 : 1.0));
    }
    if (luminance <= 0.0 || math::IsFinite(luminance) == false || math::IsFinite(scale) == false)
    {
        return false;
    }
    const float64 r = shape.Emission[0] / luminance * scale;
    const float64 g = shape.Emission[1] / luminance * scale;
    const float64 b = shape.Emission[2] / luminance * scale;
    const float64 maximum = static_cast<float64>(format::MAX_EMISSION);
    if (math::IsFinite(r) == false || math::IsFinite(g) == false || math::IsFinite(b) == false || r < 0.0 || g < 0.0 ||
        b < 0.0 || r > maximum || g > maximum || b > maximum)
    {
        return false;
    }
    outEmission = {static_cast<float32>(r), static_cast<float32>(g), static_cast<float32>(b)};
    return true;
}

/// Copy a source material's surface terms (base colour and texture, metallic,
/// roughness, alpha mask) into a slot it represents.
void ApplyMaterialTerms(const SceneIr& scene, uint32 material, PlannedMaterial& inoutPlanned) noexcept
{
    const MaterialTerms terms = GetTerms(scene, material);
    inoutPlanned.Representative = material;
    for (usize k = 0; k < IR_RGB_CHANNEL_COUNT; ++k)
    {
        inoutPlanned.BaseColour[k] = terms.BaseColour[k];
    }
    inoutPlanned.Texture = terms.Texture;
    inoutPlanned.IsTextureRepeated = terms.Texture == IR_NONE || scene.Textures[terms.Texture].IsRepeated == true;
    inoutPlanned.Metallic = math::Clamp(terms.Metallic, 0.0, 1.0);
    inoutPlanned.Roughness = math::Clamp(terms.Roughness, static_cast<float64>(format::MIN_ROUGHNESS), 1.0);
    if (material != IR_NONE && scene.Materials[material].AlphaMode == IrAlphaMode::Mask)
    {
        inoutPlanned.IsAlphaMasked = true;
        inoutPlanned.AlphaCutoff = math::Clamp(scene.Materials[material].AlphaCutoff, 0.0, 1.0);
    }
}

struct MaterialUse final
{
    /// IR material index or IR_NONE.
    uint32 Material = IR_NONE;
    float64 Area = 0.0;
    uint32 Slot = IR_NONE;
};
} // namespace

Status
PlanBudget(const SceneIr& scene, DiagnosticLog& inoutLog, CookPlan& outPlan, const Cancellation* cancelOrNull) noexcept
{
    CookPlan plan;
    // 1. Expand placements.
    const auto addPlacement = [&](uint32 shape, const math::Matrix4d& transform, bool isInstanced) noexcept -> bool {
        Placement placement;
        placement.Shape = shape;
        placement.Transform = transform;
        placement.IsInstanced = isInstanced;
        return plan.Placements.TryAdd(placement);
    };
    for (const uint32 shape : scene.WorldShapes)
    {
        if (addPlacement(shape, scene.Shapes[shape].Transform, false) == false)
        {
            return Status::OutOfMemory;
        }
    }
    for (const IrInstance& instance : scene.Instances)
    {
        for (const uint32 shape : scene.Objects[instance.Object].Shapes)
        {
            if (addPlacement(shape, MultiplyMatrices(instance.Transform, scene.Shapes[shape].Transform), true) == false)
            {
                return Status::OutOfMemory;
            }
        }
    }
    if (plan.Placements.IsEmpty() == true)
    {
        (void)inoutLog.Add(DiagnosticSeverity::Error, codes::LIMIT_EXCEEDED, {}, 0, "the scene has no drawable shapes");
        return Status::Invalid;
    }
    uint64 sourceVertices = 0;
    uint64 sourceIndices = 0;
    for (Placement& placement : plan.Placements)
    {
        if (IsCancelled(cancelOrNull) == true)
        {
            return Status::Cancelled;
        }
        const IrMesh& mesh = scene.Meshes[scene.Shapes[placement.Shape].Mesh];
        placement.Area = ComputeWorldArea(mesh, placement.Transform);
        sourceVertices += mesh.SourceVertexCount == 0 ? mesh.GetVertexCount() : mesh.SourceVertexCount;
        sourceIndices += mesh.Indices.GetSize();
    }
    plan.SourceVertices =
        sourceVertices > MAX_REPORTED_COUNT ? MAX_REPORTED_COUNT : static_cast<uint32>(sourceVertices);
    plan.SourceIndices = sourceIndices > MAX_REPORTED_COUNT ? MAX_REPORTED_COUNT : static_cast<uint32>(sourceIndices);

    // 2. Rank placements by area (ties by order) for deterministic material selection.
    Array<uint32> order;
    if (order.TryResize(plan.Placements.GetSize()) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < order.GetSize(); ++i)
    {
        order[i] = static_cast<uint32>(i);
    }
    foundation::Sort(order.GetData(), order.GetSize(), [&](uint32 left, uint32 right) noexcept {
        const bool isLeftEmitter = IsEmissive(scene.Shapes[plan.Placements[left].Shape]);
        const bool isRightEmitter = IsEmissive(scene.Shapes[plan.Placements[right].Shape]);
        if (isLeftEmitter != isRightEmitter)
        {
            return isLeftEmitter;
        }
        const float64 a = plan.Placements[left].Area;
        const float64 b = plan.Placements[right].Area;
        return a > b || (a == b && left < right);
    });

    // 3. Material usage by area (emissive surfaces are tracked separately).
    Array<MaterialUse> uses;
    Array<EmitterUse> emitters;
    uint32 droppedEmitters = 0;
    for (Placement& placement : plan.Placements)
    {
        const IrShape& shape = scene.Shapes[placement.Shape];
        if (IsEmissive(shape) == true)
        {
            math::Vector3 emission;
            if (ResolveEmission(shape, placement.Area, emission) == false)
            {
                if (shape.IsEmissionOptional == true)
                {
                    ++droppedEmitters;
                    (void)inoutLog.Add(
                        DiagnosticSeverity::Warning,
                        codes::EMISSION_DROPPED,
                        {},
                        shape.Line,
                        "decorative emission exceeds the finite RGB-nits profile; cooked as a non-emitter");
                    continue;
                }
                (void)inoutLog.Add(DiagnosticSeverity::Error,
                                   codes::LIMIT_EXCEEDED,
                                   {},
                                   shape.Line,
                                   "area-light normalization exceeds the finite RGB-nits profile");
                return Status::Invalid;
            }
            const uint32 emitterMaterial = shape.IsEmissionOptional == true ? shape.Material : IR_NONE;
            for (usize i = 0; i < emitters.GetSize(); ++i)
            {
                if (emitters[i].Emission == emission && emitters[i].IsTwoSided == shape.IsEmissionTwoSided &&
                    emitters[i].HasPower == shape.HasEmissionPower &&
                    emitters[i].IsOptional == shape.IsEmissionOptional && emitters[i].Material == emitterMaterial)
                {
                    placement.EmitterGroup = static_cast<uint32>(i);
                    break;
                }
            }
            if (placement.EmitterGroup == IR_NONE)
            {
                placement.EmitterGroup = static_cast<uint32>(emitters.GetSize());
                EmitterUse* emitterOrNull = emitters.TryAddInPlace();
                if (emitterOrNull == nullptr)
                {
                    return Status::OutOfMemory;
                }
                emitterOrNull->Emission = emission;
                emitterOrNull->IsTwoSided = shape.IsEmissionTwoSided;
                emitterOrNull->HasPower = shape.HasEmissionPower;
                emitterOrNull->IsOptional = shape.IsEmissionOptional;
                emitterOrNull->Material = emitterMaterial;
            }
            EmitterUse& emitter = emitters[placement.EmitterGroup];
            emitter.Area += placement.Area;
            emitter.PowerLumens +=
                shape.HasEmissionPower == true ? shape.EmissionPowerLumens * shape.EmissionScale : 0.0;
            if (emitter.Sources.TryAdd(placement.Shape) == false)
            {
                return Status::OutOfMemory;
            }
        }
    }
    // Each cooked mesh carries exactly one material, so the slot count is bounded by the
    // version 5 import profile's mesh and material counts; within it no material merges.
    const usize slotLimit = math::Min(usize{format::IMPORT_LIMITS.Meshes}, usize{format::IMPORT_LIMITS.Materials});
    // Decorative emitter groups beyond the slots left by required emitters (and one slot
    // for non-emissive surfaces) are demoted, smallest area first.
    usize requiredEmitters = 0;
    usize optionalEmitters = 0;
    bool hasNonEmitters = false;
    for (const EmitterUse& emitter : emitters)
    {
        requiredEmitters += emitter.IsOptional == true ? 0 : 1;
        optionalEmitters += emitter.IsOptional == true ? 1 : 0;
    }
    for (const Placement& placement : plan.Placements)
    {
        hasNonEmitters = hasNonEmitters == true || placement.EmitterGroup == IR_NONE;
    }
    if (optionalEmitters != 0 && requiredEmitters + optionalEmitters + (hasNonEmitters == true ? 1 : 0) > slotLimit)
    {
        const usize reserved = requiredEmitters + 1;
        usize keep = slotLimit > reserved ? slotLimit - reserved : 0;
        Array<uint32> optionalOrder;
        for (usize i = 0; i < emitters.GetSize(); ++i)
        {
            if (emitters[i].IsOptional == true && optionalOrder.TryAdd(static_cast<uint32>(i)) == false)
            {
                return Status::OutOfMemory;
            }
        }
        foundation::Sort(optionalOrder.GetData(), optionalOrder.GetSize(), [&](uint32 left, uint32 right) noexcept {
            const float64 a = emitters[left].Area;
            const float64 b = emitters[right].Area;
            return a > b || (a == b && left < right);
        });
        for (const uint32 index : optionalOrder)
        {
            if (keep > 0)
            {
                --keep;
                continue;
            }
            emitters[index].IsDemoted = true;
        }
    }
    // Compact the surviving groups and return demoted placements to their materials.
    Array<uint32> groupRemap;
    if (groupRemap.TryResize(emitters.GetSize(), IR_NONE) == false)
    {
        return Status::OutOfMemory;
    }
    uint32 keptGroups = 0;
    for (usize i = 0; i < emitters.GetSize(); ++i)
    {
        if (emitters[i].IsDemoted == false)
        {
            groupRemap[i] = keptGroups++;
        }
    }
    for (usize i = emitters.GetSize(); i > 0; --i)
    {
        if (emitters[i - 1].IsDemoted == true)
        {
            droppedEmitters += static_cast<uint32>(emitters[i - 1].Sources.GetSize());
            emitters.RemoveAt(i - 1);
        }
    }
    for (Placement& placement : plan.Placements)
    {
        if (placement.EmitterGroup != IR_NONE)
        {
            placement.EmitterGroup = groupRemap[placement.EmitterGroup];
        }
        if (placement.EmitterGroup != IR_NONE)
        {
            continue;
        }
        const IrShape& shape = scene.Shapes[placement.Shape];
        MaterialUse* useOrNull = nullptr;
        for (MaterialUse& existing : uses)
        {
            if (existing.Material == shape.Material)
            {
                useOrNull = &existing;
                break;
            }
        }
        if (useOrNull == nullptr)
        {
            useOrNull = uses.TryAddInPlace();
            if (useOrNull == nullptr)
            {
                return Status::OutOfMemory;
            }
            useOrNull->Material = shape.Material;
        }
        useOrNull->Area += placement.Area;
    }
    if (droppedEmitters != 0)
    {
        (void)inoutLog.Add(DiagnosticSeverity::Warning,
                           codes::EMISSION_DROPPED,
                           {},
                           0,
                           "decorative emitter placements cooked as non-emitters (emitter slots exhausted)");
    }
    Array<uint32> useOrder;
    if (useOrder.TryResize(uses.GetSize()) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < useOrder.GetSize(); ++i)
    {
        useOrder[i] = static_cast<uint32>(i);
    }
    foundation::Sort(useOrder.GetData(), useOrder.GetSize(), [&](uint32 left, uint32 right) noexcept {
        const float64 a = uses[left].Area;
        const float64 b = uses[right].Area;
        return a > b || (a == b && left < right);
    });
    if (emitters.GetSize() > slotLimit || (emitters.GetSize() == slotLimit && uses.IsEmpty() == false))
    {
        (void)inoutLog.Add(DiagnosticSeverity::Error,
                           codes::LIMIT_EXCEEDED,
                           {},
                           0,
                           "distinct emitter radiance/sidedness groups exceed the material/mesh budget");
        return Status::Invalid;
    }
    const usize keptLimit = slotLimit - emitters.GetSize();
    const usize keptCount = useOrder.GetSize() < keptLimit ? useOrder.GetSize() : keptLimit;
    for (usize rank = 0; rank < keptCount; ++rank)
    {
        MaterialUse& use = uses[useOrder[rank]];
        PlannedMaterial* planned = plan.Materials.TryAddInPlace();
        if (planned == nullptr)
        {
            return Status::OutOfMemory;
        }
        ApplyMaterialTerms(scene, use.Material, *planned);
        use.Slot = static_cast<uint32>(rank);
    }
    uint32 mergedCount = 0;
    for (usize rank = keptCount; rank < useOrder.GetSize(); ++rank)
    {
        MaterialUse& use = uses[useOrder[rank]];
        const MaterialTerms terms = GetTerms(scene, use.Material);
        float64 best = 0.0;
        for (usize slot = 0; slot < keptCount; ++slot)
        {
            const float64 distance = MeasureDistance(terms, GetTerms(scene, plan.Materials[slot].Representative));
            if (use.Slot == IR_NONE || distance < best)
            {
                best = distance;
                use.Slot = static_cast<uint32>(slot);
            }
        }
        ++mergedCount;
    }
    for (const MaterialUse& use : uses)
    {
        if (plan.Materials[use.Slot].Sources.TryAdd(use.Material) == false)
        {
            return Status::OutOfMemory;
        }
    }
    const uint32 firstEmitterSlot = static_cast<uint32>(plan.Materials.GetSize());
    for (EmitterUse& emitter : emitters)
    {
        if (math::IsFinite(emitter.PowerLumens) == false || emitter.PowerLumens < 0 ||
            emitter.PowerLumens > static_cast<float64>(format::MAX_LIGHT_INTENSITY))
        {
            (void)inoutLog.Add(DiagnosticSeverity::Error,
                               codes::LIMIT_EXCEEDED,
                               {},
                               0,
                               "grouped emitter power exceeds the one-million-lumen scene profile");
            return Status::Invalid;
        }
        PlannedMaterial* plannedOrNull = plan.Materials.TryAddInPlace();
        if (plannedOrNull == nullptr)
        {
            return Status::OutOfMemory;
        }
        plannedOrNull->IsEmissive = true;
        plannedOrNull->IsEmissionTwoSided = emitter.IsTwoSided;
        plannedOrNull->HasEmissionPower = emitter.HasPower;
        plannedOrNull->EmissionPowerLumens = emitter.PowerLumens;
        plannedOrNull->EmitterSources = static_cast<Array<uint32>&&>(emitter.Sources);
        plannedOrNull->BaseColour[0] = plannedOrNull->BaseColour[1] = plannedOrNull->BaseColour[2] = 1.0;
        plannedOrNull->IsTextureRepeated = false;
        if (emitter.Material != IR_NONE)
        {
            // A decorative group keeps its material's surface, alpha and maps.
            ApplyMaterialTerms(scene, emitter.Material, *plannedOrNull);
        }
        plannedOrNull->Area = emitter.Area;
        plannedOrNull->Emission[0] = static_cast<float64>(emitter.Emission.X);
        plannedOrNull->Emission[1] = static_cast<float64>(emitter.Emission.Y);
        plannedOrNull->Emission[2] = static_cast<float64>(emitter.Emission.Z);
    }
    if (mergedCount != 0)
    {
        char text[96]{};
        usize length = 0;
        const auto append = [&](std::string_view part) noexcept {
            for (usize i = 0; i < part.size() && length < sizeof(text); ++i)
            {
                text[length++] = part[i];
            }
        };
        char digits[12]{};
        usize digitCount = 0;
        uint32 value = mergedCount;
        do
        {
            digits[digitCount++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        for (usize i = 0; i < digitCount; ++i)
        {
            text[length++] = digits[digitCount - 1 - i];
        }
        append(" source materials merged into the nearest kept slot");
        (void)inoutLog.Add(DiagnosticSeverity::Warning, codes::MATERIALS_MERGED, {}, 0, std::string_view(text, length));
    }

    // 4. Keep every placement. Geometry shares the encoded bundle budget with the
    // remaining records; reject an oversized scene before baking rather than dropping shapes.
    uint64 geometryBytes =
        sizeof(format::Header) + sizeof(format::LightingRecord) +
        scene.AnalyticLights.GetSize() * sizeof(format::LightRecord) +
        plan.Materials.GetSize() * (sizeof(format::MeshRecord) + sizeof(format::MaterialRecordV6) +
                                    format::MATERIAL_EXTENSION_V7_BYTES + sizeof(format::ObjectRecordV3));
    uint32 totalVertices = 0;
    uint32 totalIndices = 0;
    for (const uint32 index : order)
    {
        Placement& placement = plan.Placements[index];
        const IrShape& shape = scene.Shapes[placement.Shape];
        const IrMesh& mesh = scene.Meshes[shape.Mesh];
        const bool isEmitting = placement.EmitterGroup != IR_NONE;
        uint32 slot = isEmitting == false ? IR_NONE : firstEmitterSlot + placement.EmitterGroup;
        if (isEmitting == false)
        {
            for (const MaterialUse& use : uses)
            {
                if (use.Material == shape.Material)
                {
                    slot = use.Slot;
                    break;
                }
            }
        }
        PlannedMaterial& planned = plan.Materials[slot];
        const usize vertices = mesh.GetVertexCount();
        const usize indices = mesh.Indices.GetSize();
        if (vertices > format::IMPORT_LIMITS.Vertices - totalVertices ||
            indices > format::IMPORT_LIMITS.TotalIndices - totalIndices)
        {
            (void)inoutLog.Add(DiagnosticSeverity::Error,
                               codes::LIMIT_EXCEEDED,
                               {},
                               shape.Line,
                               "complete scene geometry exceeds the bundle byte budget; no placements were dropped");
            return Status::Invalid;
        }
        geometryBytes += uint64{vertices} * sizeof(format::VertexRecordV6) + uint64{indices} * sizeof(uint32);
        if (geometryBytes > format::IMPORT_LIMITS.BundleBytes)
        {
            (void)inoutLog.Add(DiagnosticSeverity::Error,
                               codes::LIMIT_EXCEEDED,
                               {},
                               shape.Line,
                               "complete scene geometry exceeds the bundle byte budget; no placements were dropped");
            return Status::Invalid;
        }
        placement.IsKept = true;
        totalVertices += static_cast<uint32>(vertices);
        totalIndices += static_cast<uint32>(indices);
        planned.VertexCount += static_cast<uint32>(vertices);
        planned.IndexCount += static_cast<uint32>(indices);
        if (planned.IsEmissive == false)
        {
            planned.Area += placement.Area;
        }
        if (planned.Placements.TryAdd(index) == false)
        {
            return Status::OutOfMemory;
        }
    }
    // Slots that received no geometry are removed (they would be empty meshes).
    for (usize slot = plan.Materials.GetSize(); slot > 0; --slot)
    {
        if (plan.Materials[slot - 1].Placements.IsEmpty() == true)
        {
            plan.Materials.RemoveAt(slot - 1);
        }
    }
    if (plan.Materials.IsEmpty() == true)
    {
        (void)inoutLog.Add(DiagnosticSeverity::Error, codes::LIMIT_EXCEEDED, {}, 0, "no placement fits the budget");
        return Status::Invalid;
    }
    // Placement lists follow source order so baking is stable and readable.
    for (PlannedMaterial& planned : plan.Materials)
    {
        foundation::Sort(planned.Placements.GetData(),
                         planned.Placements.GetSize(),
                         [](uint32 left, uint32 right) noexcept { return left < right; });
    }
    outPlan = static_cast<CookPlan&&>(plan);
    return Status::Ok;
}
} // namespace ludus::content::scene_cook::importers
