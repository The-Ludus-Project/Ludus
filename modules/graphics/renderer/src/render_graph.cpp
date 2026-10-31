#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>

#include "../shaders/renderer_gpu_preview_layout.h"
#include "internal/gpu_denoiser.h"
#include "internal/ltc_table_data.h"
#include "internal/render_graph_deferred.h"
#include "internal/render_graph_metalfx.h"
#include "internal/render_graph_pool.h"
#include "internal/render_graph_ray.h"
#include "internal/render_graph_ray_path.h"
#include "internal/render_graph_scene_resources.h"
#include "internal/render_graph_upscale.h"

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/queries.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <new>

// Thanks to Donald Revie, "Designing a Data-Driven Renderer", GPU Pro 3,
// V.4, sections 4.4-4.8, and Arseny Kapoulkine, "Writing an Efficient Vulkan
// Renderer", GPU Zen 2, IV.4, sections 4.4-4.6: keep scene interpretation separate
// from authored techniques and prewarm complete immutable state. This original
// adapter uses retained Ludus snapshots, not their mutable context stacks.
// Per-draw uniform values follow Patrick Cozzi, "A Framework for GLSL Engine
// Uniforms", Game Engine Gems 2, chapter 6, sections 6.2-6.6: typed values are
// resolved before encoding and cached once per distinct value set, rather than
// becoming shader configurations; we use plan layout records, not string maps.
// Contract: docs/architecture/render-graph-authoring.md.
namespace ludus::graphics::renderer
{
using namespace foundation;
namespace format = content::render_graph_format;
namespace math = foundation::math;
using Status = rhi::RasterStatus;
namespace
{
constexpr usize ROLE_COUNT = 2;
constexpr usize UPSCALE_TARGET_COUNT = 2;
constexpr usize MAX_COVERAGE = format::MAX_ITEMS * format::MAX_CAMERAS * ROLE_COUNT;
constexpr usize BOX_CORNER_COUNT = 8;
constexpr usize BOX_INDEX_COUNT = 36;
constexpr usize FLAT_INPUT_COUNT = 6;
constexpr usize MAX_RETAINED_SCENES = 3;
/// Frozen ray compute ABI, visibility textures, structured storage and the diagnostic capture slot.
constexpr usize RAY_COMPUTE_BINDING_CAPACITY = LUDUS_GRAPH_VISIBILITY_BINDING_COUNT + format::MAX_STORAGE_BUFFERS + 1 +
                                               LUDUS_GRAPH_LTC_TABLE_BINDING_COUNT + LUDUS_GRAPH_FAILURE_BINDING_COUNT;
constexpr usize MAX_COMPONENTS = 4;
/// Largest packed uniform bytes of one technique: every slot at its largest block.
constexpr usize MAX_TECHNIQUE_UNIFORM_BYTES = format::UNIFORM_SLOT_COUNT * format::MAX_UNIFORM_BLOCK_BYTES;
constexpr usize NO_SET = ~usize{0};
constexpr uint32 MAX_TECHNIQUE = static_cast<uint32>(format::TECHNIQUE_COUNT - 1);
constexpr uint32 MAX_TARGET = static_cast<uint32>(format::ShaderTarget::GlslEs);
bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
bool IsColorTechnique(format::Technique technique) noexcept
{
    return format::GetTechniqueFamily(technique) == format::Technique::ColorAndDepth ||
           format::GetTechniqueFamily(technique) == format::Technique::MaskedColorAndDepth ||
           technique == format::Technique::RayTracing;
}
bool IsMaskedTechnique(format::Technique technique) noexcept
{
    return format::GetTechniqueFamily(technique) == format::Technique::MaskedColorAndDepth ||
           format::GetTechniqueFamily(technique) == format::Technique::MaskedDepthOnly;
}
/// Whether a technique's authored family covers the item's declared static coverage contract.
bool IsCompatible(format::Technique technique, const RenderGraphItem& item, const RenderGraphMesh& mesh) noexcept
{
    return format::IsImageTechnique(technique) == false && mesh.Layout == RenderGraphLayout::Static &&
           ((item.Coverage == RenderGraphMaterial::Opaque && IsMaskedTechnique(technique) == false) ||
            (item.Coverage == RenderGraphMaterial::Masked && IsMaskedTechnique(technique) == true));
}
Status MapDeviceStatus(rhi::DeviceStatus status) noexcept
{
    switch (status)
    {
        case rhi::DeviceStatus::Ready:
            return Status::Ready;
        case rhi::DeviceStatus::Pending:
        case rhi::DeviceStatus::Skipped:
        case rhi::DeviceStatus::NotReady:
            return Status::NotReady;
        case rhi::DeviceStatus::InvalidHandle:
            return Status::InvalidHandle;
        case rhi::DeviceStatus::InvalidState:
            return Status::InvalidState;
        case rhi::DeviceStatus::InvalidDescription:
            return Status::InvalidDescription;
        case rhi::DeviceStatus::Unsupported:
            return Status::Unsupported;
        case rhi::DeviceStatus::Failed:
            return Status::Failed;
        case rhi::DeviceStatus::DeviceLost:
            return Status::DeviceLost;
        case rhi::DeviceStatus::IdentityExhausted:
            return Status::IdentityExhausted;
    }
    return Status::InvalidDescription;
}
Status CheckDevice(rhi::DeviceHandle device) noexcept
{
    rhi::DeviceInfo information;
    const rhi::DeviceStatus status = rhi::GetDeviceInfo(device, information);
    if (status != rhi::DeviceStatus::Ready)
    {
        return MapDeviceStatus(status);
    }
    return information.FrameOpen == true ? Status::InvalidState : Status::Ready;
}
/// Artifact target whose uniform layout records describe the selected backend's emitted shaders.
format::ShaderTarget GetTarget(rhi::Backend backend) noexcept
{
    switch (backend)
    {
        case rhi::Backend::Metal:
            return format::ShaderTarget::Metal;
        case rhi::Backend::WebGPU:
            return format::ShaderTarget::Wgsl;
        case rhi::Backend::WebGL2:
            return format::ShaderTarget::GlslEs;
        // No DXIL target yet: the Direct3D 12 RHI rejects every shader artifact.
        case rhi::Backend::Direct3D12:
        case rhi::Backend::Vulkan:
            break;
    }
    return format::ShaderTarget::Spirv;
}
/// Largest carried block of one binding: the uniform range the layout must admit on every target.
usize GetBindingBytes(const RenderGraphPlan& plan, usize binding) noexcept
{
    usize bytes = 0;
    for (usize i = 0; i < plan.UniformBlocks.GetSize(); ++i)
    {
        const format::UniformBlock& block = plan.UniformBlocks[i];
        if (block.Binding == binding && block.Size > bytes)
        {
            bytes = block.Size;
        }
    }
    return bytes;
}
bool IsReadByStage(format::Visibility stages, rhi::ShaderStage stage) noexcept
{
    return stages == format::Visibility::Both ||
           (stages == format::Visibility::Vertex && stage == rhi::ShaderStage::Vertex) ||
           (stages == format::Visibility::Fragment && stage == rhi::ShaderStage::Fragment);
}
/// One stage must declare exactly the plan's bindings for its technique, as read-only uniform buffers.
bool HasPlanBindings(const rhi::RasterShaderDescription& description,
                     const RenderGraphPlan& plan,
                     format::Technique technique) noexcept
{
    usize expected = 0;
    for (usize i = 0; i < plan.UniformBindings.GetSize(); ++i)
    {
        const format::UniformBinding& binding = plan.UniformBindings[i];
        if (binding.Program != technique || IsReadByStage(binding.Stages, description.Artifact.Stage) == false)
        {
            continue;
        }
        ++expected;
        const rhi::RasterVisibility visibility = description.Artifact.Stage == rhi::ShaderStage::Vertex
                                                     ? rhi::RasterVisibility::Vertex
                                                     : rhi::RasterVisibility::Fragment;
        bool isFound = false;
        for (const rhi::RasterBinding& declared : description.Bindings)
        {
            isFound = isFound == true ||
                      (declared.Binding == binding.Slot && declared.Kind == rhi::RasterBindingKind::UniformBuffer &&
                       declared.Visibility == visibility && declared.MinSize == GetBindingBytes(plan, i) &&
                       declared.Count == 1 && declared.HasDynamicOffset == false &&
                       declared.Indexing == rhi::RasterBindingIndexing::Constant && declared.IsRuntimeSized == false &&
                       declared.HasPartialBinding == false);
        }
        if (isFound == false)
        {
            return false;
        }
    }
    return description.Bindings.size() == expected;
}
bool HasFlatInterface(const rhi::RasterShaderDescription& vertex,
                      const rhi::RasterShaderDescription& fragment,
                      const RenderGraphPlan& plan,
                      format::Technique technique) noexcept
{
    if (vertex.Artifact.Stage != rhi::ShaderStage::Vertex || fragment.Artifact.Stage != rhi::ShaderStage::Fragment ||
        (vertex.Inputs.size() != FLAT_INPUT_COUNT && vertex.Inputs.size() != format::VISIBILITY_INPUT_COUNT) ||
        HasPlanBindings(vertex, plan, technique) == false || HasPlanBindings(fragment, plan, technique) == false)
    {
        return false;
    }
    for (usize i = 0; i < vertex.Inputs.size(); ++i)
    {
        const rhi::RasterShaderInput& input = vertex.Inputs[i];
        if (input.Location != i || input.Format != (i == 0                 ? rhi::RasterVertexFormat::Float3
                                                    : i < FLAT_INPUT_COUNT ? rhi::RasterVertexFormat::Float4
                                                                           : rhi::RasterVertexFormat::Float2))
        {
            return false;
        }
    }
    return true;
}
bool AreArtifactsEqual(const rhi::ShaderDescription& left, const rhi::ShaderDescription& right) noexcept
{
    if (left.Wgsl != right.Wgsl || left.Msl != right.Msl || left.GlslEs != right.GlslEs ||
        left.WgslEntry != right.WgslEntry || left.MslEntry != right.MslEntry || left.GlslEsEntry != right.GlslEsEntry ||
        left.SpirvEntry != right.SpirvEntry || left.Spirv.size() != right.Spirv.size())
    {
        return false;
    }
    for (usize i = 0; i < left.Spirv.size(); ++i)
    {
        if (left.Spirv[i] != right.Spirv[i])
        {
            return false;
        }
    }
    return true;
}
/// The authored vertex and fragment of one technique; Composite entries are not techniques.
const rhi::RasterShaderDescription&
GetStage(const RenderGraphPrograms& programs, format::Technique technique, rhi::ShaderStage stage) noexcept
{
    const bool isVertex = stage == rhi::ShaderStage::Vertex;
    const usize index = static_cast<usize>(technique);
    if (index >= format::LEGACY_TECHNIQUE_COUNT && index < format::SCENE_TECHNIQUE_COUNT)
    {
        return programs
            .AdditionalRasterStages[(index - format::LEGACY_TECHNIQUE_COUNT) * format::TECHNIQUE_STAGE_COUNT +
                                    (isVertex == true ? 0 : 1)];
    }
    switch (technique)
    {
        case format::Technique::ColorAndDepth:
        case format::Technique::RayTracing:
            return isVertex == true ? programs.ColorVertex : programs.ColorFragment;
        case format::Technique::DepthOnly:
            return isVertex == true ? programs.DepthVertex : programs.DepthFragment;
        case format::Technique::MaskedColorAndDepth:
            return isVertex == true ? programs.MaskedColorVertex : programs.MaskedColorFragment;
        case format::Technique::MaskedDepthOnly:
            break;
        case format::Technique::MetalFxSpatialUpscale:
        case format::Technique::MetalFxSpatialPerceptualUpscale:
        case format::Technique::BilinearUpscale:
            return isVertex == true ? programs.CompositeVertex : programs.CompositeFragment;
    }
    return isVertex == true ? programs.MaskedDepthVertex : programs.MaskedDepthFragment;
}
/// The display curve a pass's fused ToneMap applies.
ToneCurve GetToneCurve(const format::Pass& pass) noexcept
{
    return pass.ToneMappingCurve == format::DisplayCurve::PbrNeutral ? ToneCurve::PbrNeutral : ToneCurve::Reinhard;
}
/// Editor overlays belong to display color, never the visibility codec's triangle/instance IDs.
ViewDescription MakeRasterPassView(const ViewDescription& source,
                                   const RenderGraphPrograms& programs,
                                   format::Technique technique) noexcept
{
    ViewDescription view = source;
    if (GetStage(programs, technique, rhi::ShaderStage::Vertex).Inputs.size() == format::VISIBILITY_INPUT_COUNT ||
        format::GetTechniqueFamily(technique) == format::Technique::DepthOnly)
    {
        view.WorldGrid.IsVisible = false;
    }
    return view;
}
Status ValidateUniforms(const RenderGraphPlan& plan, StaticArray<bool, format::TECHNIQUE_COUNT> isUsed) noexcept
{
    const format::Header& header = plan.Description;
    if (plan.UniformBindings.IsValid() == false || plan.UniformBlocks.IsValid() == false ||
        plan.UniformFields.IsValid() == false || header.UniformBindingCount != plan.UniformBindings.GetSize() ||
        header.UniformBlockCount != plan.UniformBlocks.GetSize() ||
        header.UniformFieldCount != plan.UniformFields.GetSize() ||
        header.UniformValueCount > format::MAX_UNIFORM_VALUES ||
        plan.UniformBindings.GetSize() > format::MAX_UNIFORM_BINDINGS ||
        plan.UniformBlocks.GetSize() > format::MAX_UNIFORM_BLOCKS ||
        plan.UniformFields.GetSize() > format::MAX_UNIFORM_FIELDS ||
        (header.UniformValueCount == 0) != plan.UniformBindings.IsEmpty())
    {
        return Status::InvalidDescription;
    }
    for (usize i = 0; i < plan.UniformBindings.GetSize(); ++i)
    {
        const format::UniformBinding& binding = plan.UniformBindings[i];
        const uint32 program = static_cast<uint32>(binding.Program);
        const uint32 stages = static_cast<uint32>(binding.Stages);
        // Strict (technique, slot) order matches the Renderer's ascending layout concatenation.
        const bool isOrdered =
            i == 0 || static_cast<uint32>(plan.UniformBindings[i - 1].Program) < program ||
            (plan.UniformBindings[i - 1].Program == binding.Program && plan.UniformBindings[i - 1].Slot < binding.Slot);
        if (program > MAX_TECHNIQUE || isUsed[program] == false || binding.Slot >= format::UNIFORM_SLOT_COUNT ||
            stages == 0 || stages > static_cast<uint32>(format::Visibility::Both) || binding.ValueCount == 0 ||
            binding.ValueOffset > header.UniformValueCount ||
            binding.ValueCount > header.UniformValueCount - binding.ValueOffset || isOrdered == false)
        {
            return Status::InvalidDescription;
        }
    }
    for (usize i = 0; i < plan.UniformBlocks.GetSize(); ++i)
    {
        const format::UniformBlock& block = plan.UniformBlocks[i];
        if (block.Binding >= plan.UniformBindings.GetSize() || static_cast<uint32>(block.Target) > MAX_TARGET ||
            block.Size == 0 || block.Size > format::MAX_UNIFORM_BLOCK_BYTES || block.Size % sizeof(float32) != 0)
        {
            return Status::InvalidDescription;
        }
        for (usize earlier = 0; earlier < i; ++earlier)
        {
            if (plan.UniformBlocks[earlier].Binding == block.Binding &&
                plan.UniformBlocks[earlier].Target == block.Target)
            {
                return Status::InvalidDescription;
            }
        }
    }
    for (usize i = 0; i < plan.UniformFields.GetSize(); ++i)
    {
        const format::UniformField& field = plan.UniformFields[i];
        if (field.Binding >= plan.UniformBindings.GetSize() || static_cast<uint32>(field.Target) > MAX_TARGET ||
            field.Components == 0 || field.Components > MAX_COMPONENTS || field.ByteOffset % sizeof(float32) != 0 ||
            field.ValueOffset > plan.UniformBindings[field.Binding].ValueCount ||
            field.Components > plan.UniformBindings[field.Binding].ValueCount - field.ValueOffset)
        {
            return Status::InvalidDescription;
        }
        bool hasBlock = false;
        for (usize block = 0; block < plan.UniformBlocks.GetSize(); ++block)
        {
            const format::UniformBlock& candidate = plan.UniformBlocks[block];
            hasBlock = hasBlock == true || (candidate.Binding == field.Binding && candidate.Target == field.Target &&
                                            field.ByteOffset <= candidate.Size &&
                                            field.Components * sizeof(float32) <= candidate.Size - field.ByteOffset);
        }
        if (hasBlock == false)
        {
            return Status::InvalidDescription;
        }
    }
    return Status::Ready;
}
/// The selected target must carry a complete layout: one block per binding and fields
/// that write every logical value exactly once, in schema order.
Status ValidateTargetUniforms(const RenderGraphPlan& plan, format::ShaderTarget target) noexcept
{
    for (usize binding = 0; binding < plan.UniformBindings.GetSize(); ++binding)
    {
        bool hasBlock = false;
        for (usize i = 0; i < plan.UniformBlocks.GetSize(); ++i)
        {
            hasBlock = hasBlock == true ||
                       (plan.UniformBlocks[i].Binding == binding && plan.UniformBlocks[i].Target == target);
        }
        uint32 covered = 0;
        for (usize i = 0; i < plan.UniformFields.GetSize(); ++i)
        {
            const format::UniformField& field = plan.UniformFields[i];
            if (field.Binding != binding || field.Target != target)
            {
                continue;
            }
            if (field.ValueOffset != covered)
            {
                return Status::Unsupported;
            }
            covered += field.Components;
        }
        if (hasBlock == false || covered != plan.UniformBindings[binding].ValueCount)
        {
            return Status::Unsupported;
        }
    }
    return Status::Ready;
}
Status ValidatePlan(const RenderGraphPlan& plan) noexcept
{
    if (plan.Description.Version != format::VERSION || plan.Resources.IsValid() == false ||
        plan.Passes.IsValid() == false || plan.Cameras.IsValid() == false || plan.PassNames.IsValid() == false ||
        (plan.PassNames.IsEmpty() == false && plan.PassNames.GetSize() != plan.Passes.GetSize()) ||
        plan.Description.ResourceCount != plan.Resources.GetSize() ||
        plan.Description.PassCount != plan.Passes.GetSize() || plan.Resources.IsEmpty() == true ||
        plan.Passes.IsEmpty() == true || plan.Resources.GetSize() > format::MAX_RESOURCES ||
        plan.Passes.GetSize() > format::MAX_PASSES || plan.Description.CameraCount != plan.Cameras.GetSize() ||
        plan.Description.CameraCount == 0 || plan.Description.CameraCount > format::MAX_CAMERAS)
    {
        return Status::InvalidDescription;
    }
    uint32 surfaceOwner = format::ABSENT;
    usize rayPassCount = 0;
    bool hasEndedRaySequence = false;
    bool isVisibilitySequence = false;
    StaticArray<bool, format::MAX_RESOURCES> colorDefined{};
    StaticArray<bool, format::MAX_RESOURCES> depthDefined{};
    StaticArray<bool, format::MAX_CAMERAS> hasCameraRoute{};
    StaticArray<bool, format::TECHNIQUE_COUNT> isUsed{};
    for (usize i = 0; i < plan.Resources.GetSize(); ++i)
    {
        const format::Resource& resource = plan.Resources[i];
        if (resource.Width == 0 || resource.Height == 0 || resource.Backing >= plan.Resources.GetSize() ||
            resource.IsSurface > 1 || resource.IsExported > 1 ||
            (resource.Kind != format::Aspect::Color && resource.Kind != format::Aspect::Depth &&
             resource.Kind != format::Aspect::Buffer))
        {
            return Status::InvalidDescription;
        }
        if (resource.Kind == format::Aspect::Buffer)
        {
            if (resource.Backing != i || resource.IsSurface != 0 || resource.IsExported != 0 || resource.Height != 1 ||
                resource.Width % format::RAY_BUFFER_WORD_BYTES != 0 || resource.Width > format::MAX_RAY_BUFFER_BYTES ||
                (resource.BytesPerPixel == 0 && resource.ViewNumerator != 0) ||
                resource.BytesPerPixel > format::MAX_RAY_BUFFER_PIXEL_BYTES ||
                resource.BytesPerPixel % format::RAY_BUFFER_WORD_BYTES != 0)
            {
                return Status::InvalidDescription;
            }
            continue;
        }
        if (resource.IsSurface == 1)
        {
            if (surfaceOwner != format::ABSENT && surfaceOwner != resource.Backing)
            {
                return Status::Unsupported;
            }
            surfaceOwner = resource.Backing;
        }
        for (usize earlier = 0; earlier < i; ++earlier)
        {
            if (resource.Kind == format::Aspect::Depth && plan.Resources[earlier].Kind == format::Aspect::Depth &&
                plan.Resources[earlier].Backing == resource.Backing)
            {
                return Status::Unsupported;
            }
        }
        if (resource.BytesPerPixel != 0)
        {
            return Status::InvalidDescription;
        }
        const format::Resource& backing = plan.Resources[resource.Backing];
        if (backing.Backing != resource.Backing || backing.Width != resource.Width ||
            backing.Height != resource.Height || backing.ViewNumerator != resource.ViewNumerator ||
            backing.ViewDenominator != resource.ViewDenominator || backing.IsSurface != resource.IsSurface ||
            (resource.Kind == format::Aspect::Color && resource.Backing != i) ||
            (resource.IsExported == 1 && resource.IsSurface == 1))
        {
            return Status::Unsupported;
        }
    }
    for (usize camera = 0; camera < plan.Cameras.GetSize(); ++camera)
    {
        const uint32 target = plan.Cameras[camera].Target;
        if (target >= plan.Resources.GetSize() ||
            (plan.Resources[target].IsExported == 0 &&
             (plan.Resources[target].IsSurface == 0 || plan.Resources[target].Kind != format::Aspect::Color)))
        {
            return Status::InvalidDescription;
        }
    }
    for (usize passIndex = 0; passIndex < plan.Passes.GetSize(); ++passIndex)
    {
        const format::Pass& pass = plan.Passes[passIndex];
        if (format::IsImageTechnique(pass.Program) == true)
        {
            if (pass.Camera >= plan.Cameras.GetSize() || pass.Color >= plan.Resources.GetSize() ||
                pass.SourceColor >= plan.Resources.GetSize() || pass.SourceColor == pass.Color ||
                plan.Resources[pass.SourceColor].Kind != format::Aspect::Color ||
                plan.Resources[pass.SourceColor].IsSurface != 0 || colorDefined[pass.SourceColor] == false ||
                plan.Resources[pass.Color].Kind != format::Aspect::Color || pass.Depth != format::ABSENT ||
                pass.ColorLoad != format::Load::Clear || pass.ColorStore != format::Store::Store ||
                pass.DepthLoad != format::Load::Discard || pass.DepthStore != format::Store::Discard ||
                pass.AllowsRepeatedCoverage != 0 || pass.SceneData != 0 || pass.RayProgram != 0 ||
                pass.Buffer0 != format::ABSENT || pass.Buffer1 != format::ABSENT || pass.Visibility != format::ABSENT ||
                pass.VisibilityDepth != format::ABSENT || pass.AccumulationSamples != 0 ||
                pass.ToneMappingExposure != 0 || pass.ToneMappingCurve != format::DisplayCurve::Reinhard ||
                pass.DenoisingQuality != 0)
            {
                return Status::InvalidDescription;
            }
            if (pass.Program != format::Technique::BilinearUpscale &&
                (plan.Resources[pass.Color].Width < plan.Resources[pass.SourceColor].Width ||
                 plan.Resources[pass.Color].Height < plan.Resources[pass.SourceColor].Height))
            {
                return Status::InvalidDescription;
            }
            colorDefined[pass.Color] = true;
            depthDefined[pass.Color] = false;
            hasCameraRoute[pass.Camera] =
                hasCameraRoute[pass.Camera] == true || plan.Cameras[pass.Camera].Target == pass.Color;
            hasEndedRaySequence = rayPassCount != 0;
            continue;
        }
        if (pass.SourceColor != format::ABSENT)
        {
            return Status::InvalidDescription;
        }
        if (static_cast<uint32>(pass.Program) > MAX_TECHNIQUE || pass.Camera >= plan.Description.CameraCount ||
            (pass.Program != format::Technique::RayTracing && pass.Depth >= plan.Resources.GetSize()) ||
            pass.AllowsRepeatedCoverage > 1 ||
            static_cast<uint32>(pass.ColorLoad) > static_cast<uint32>(format::Load::Discard) ||
            static_cast<uint32>(pass.DepthLoad) > static_cast<uint32>(format::Load::Discard) ||
            static_cast<uint32>(pass.ColorStore) > static_cast<uint32>(format::Store::Discard) ||
            static_cast<uint32>(pass.DepthStore) > static_cast<uint32>(format::Store::Discard))
        {
            return Status::InvalidDescription;
        }
        isUsed[static_cast<usize>(pass.Program)] = true;
        const uint32 requiredTarget = plan.Cameras[pass.Camera].Target;
        hasCameraRoute[pass.Camera] =
            hasCameraRoute[pass.Camera] == true || pass.Color == requiredTarget || pass.Depth == requiredTarget;
        if ((pass.SceneData & ~::ludus::content::render_graph_format::SCENE_DATA_ALL) != 0 ||
            (pass.SceneData & ::ludus::content::render_graph_format::SCENE_DATA_GEOMETRY) == 0)
        {
            return Status::InvalidDescription;
        }
        if (pass.DenoisingQuality > static_cast<uint32>(format::ProcessingQuality::High) ||
            pass.AccumulationSamples > format::MAX_ACCUMULATION_SAMPLES ||
            (pass.Program != format::Technique::RayTracing &&
             (pass.AccumulationSamples != 0 || pass.DenoisingQuality != 0)) ||
            format::IsToneMappingAdmitted(pass) == false)
        {
            return Status::InvalidDescription;
        }
        if (static_cast<uint32>(pass.Traversal) > static_cast<uint32>(format::RayTraversal::ComputeBvh) ||
            (pass.Program != format::Technique::RayTracing && pass.Traversal != format::RayTraversal::NativeQuery))
        {
            return Status::InvalidDescription;
        }
        if (pass.Program == format::Technique::RayTracing)
        {
            if (hasEndedRaySequence == true || pass.RayProgram >= format::MAX_RAY_PASSES)
            {
                return Status::Unsupported;
            }
            const StaticArray<uint32, format::MAX_RAY_BUFFERS> buffers{{pass.Buffer0, pass.Buffer1}};
            for (const uint32 buffer : buffers)
            {
                if (buffer != format::ABSENT &&
                    (buffer >= plan.Resources.GetSize() || plan.Resources[buffer].Kind != format::Aspect::Buffer))
                {
                    return Status::InvalidDescription;
                }
            }
            if (pass.Buffer0 != format::ABSENT && pass.Buffer0 == pass.Buffer1)
            {
                return Status::InvalidDescription;
            }
            ++rayPassCount;
            if (rayPassCount > format::MAX_RAY_PASSES || pass.Color >= plan.Resources.GetSize() ||
                pass.Depth != format::ABSENT || plan.Resources[pass.Color].Kind != format::Aspect::Color ||
                plan.Resources[pass.Color].IsSurface != 0 || pass.ColorLoad != format::Load::Clear ||
                pass.ColorStore != format::Store::Store || pass.DepthLoad != format::Load::Discard ||
                pass.DepthStore != format::Store::Discard)
            {
                return Status::Unsupported;
            }
            if ((pass.Visibility == format::ABSENT) != (pass.VisibilityDepth == format::ABSENT))
            {
                return Status::InvalidDescription;
            }
            // One ray sequence is either wholly ordinary or wholly visibility-driven.
            const bool hasVisibility = pass.Visibility != format::ABSENT;
            if (rayPassCount > 1 && hasVisibility != isVisibilitySequence)
            {
                return Status::Unsupported;
            }
            isVisibilitySequence = hasVisibility;
            if (hasVisibility == true)
            {
                if (passIndex == 0 || pass.Visibility >= plan.Resources.GetSize() ||
                    pass.VisibilityDepth >= plan.Resources.GetSize() || pass.Visibility == pass.Color)
                {
                    return Status::InvalidDescription;
                }
                const format::Pass& previous = plan.Passes[passIndex - 1];
                const format::Resource& visibility = plan.Resources[pass.Visibility];
                const format::Resource& depth = plan.Resources[pass.VisibilityDepth];
                const format::Resource& target = plan.Resources[pass.Color];
                if (visibility.Kind != format::Aspect::Color || depth.Kind != format::Aspect::Depth ||
                    depth.Backing != pass.Visibility || visibility.IsSurface != 0 || visibility.Width != target.Width ||
                    visibility.Height != target.Height || visibility.ViewNumerator != target.ViewNumerator ||
                    visibility.ViewDenominator != target.ViewDenominator || colorDefined[pass.Visibility] == false ||
                    depthDefined[pass.Visibility] == false)
                {
                    return Status::Unsupported;
                }
                // Later passes reread the image the sequence's raster producer wrote; ray passes
                // write only their own color and buffers, so no pass in between can change it.
                if (previous.Program == format::Technique::RayTracing)
                {
                    if (previous.Camera != pass.Camera || previous.Visibility != pass.Visibility ||
                        previous.VisibilityDepth != pass.VisibilityDepth)
                    {
                        return Status::Unsupported;
                    }
                }
                else if (format::GetTechniqueFamily(previous.Program) != format::Technique::ColorAndDepth ||
                         previous.Camera != pass.Camera || previous.Color != pass.Visibility ||
                         previous.Depth != pass.VisibilityDepth || previous.ColorLoad != format::Load::Clear ||
                         previous.DepthLoad != format::Load::Clear || previous.ColorStore != format::Store::Store ||
                         previous.DepthStore != format::Store::Store)
                {
                    return Status::Unsupported;
                }
            }
            colorDefined[pass.Color] = true;
            depthDefined[pass.Color] = false;
            continue;
        }
        hasEndedRaySequence = rayPassCount != 0;
        if (pass.RayProgram != 0 || pass.Buffer0 != format::ABSENT || pass.Buffer1 != format::ABSENT ||
            pass.Visibility != format::ABSENT || pass.VisibilityDepth != format::ABSENT)
        {
            return Status::InvalidDescription;
        }
        const format::Resource& depth = plan.Resources[pass.Depth];
        if (depth.Kind != format::Aspect::Depth ||
            (pass.DepthLoad == format::Load::Load && depthDefined[depth.Backing] == false))
        {
            return Status::InvalidDescription;
        }
        depthDefined[depth.Backing] = pass.DepthStore == format::Store::Store &&
                                      (pass.DepthLoad == format::Load::Clear ||
                                       (pass.DepthLoad == format::Load::Load && depthDefined[depth.Backing] == true));
        if (IsColorTechnique(pass.Program) == true)
        {
            if (pass.Color >= plan.Resources.GetSize())
            {
                return Status::InvalidDescription;
            }
            const format::Resource& color = plan.Resources[pass.Color];
            if (color.Kind != format::Aspect::Color || depth.Backing != pass.Color ||
                (pass.ColorLoad == format::Load::Load && colorDefined[pass.Color] == false))
            {
                return Status::Unsupported;
            }
            colorDefined[pass.Color] = pass.ColorStore == format::Store::Store &&
                                       (pass.ColorLoad == format::Load::Clear ||
                                        (pass.ColorLoad == format::Load::Load && colorDefined[pass.Color] == true));
        }
        else
        {
            if (pass.Color != format::ABSENT ||
                (pass.ColorLoad == format::Load::Load && colorDefined[depth.Backing] == false))
            {
                return Status::InvalidDescription;
            }
            colorDefined[depth.Backing] =
                pass.ColorStore == format::Store::Store &&
                (pass.ColorLoad == format::Load::Clear ||
                 (pass.ColorLoad == format::Load::Load && colorDefined[depth.Backing] == true));
        }
    }
    for (usize camera = 0; camera < plan.Description.CameraCount; ++camera)
    {
        if (hasCameraRoute[camera] == false)
        {
            return Status::InvalidDescription;
        }
    }
    for (usize i = 0; i < plan.Resources.GetSize(); ++i)
    {
        const format::Resource& resource = plan.Resources[i];
        const bool isDefined =
            resource.Kind == format::Aspect::Depth ? depthDefined[resource.Backing] : colorDefined[i];
        if ((resource.IsExported == 1 || (resource.Kind == format::Aspect::Color && resource.IsSurface == 1)) &&
            isDefined == false)
        {
            return Status::InvalidDescription;
        }
    }
    usize usedCount = 0;
    for (const bool hasProgram : isUsed)
    {
        usedCount += hasProgram == true ? 1 : 0;
    }
    if (usedCount > format::MAX_AUTHORED_TECHNIQUES)
    {
        return Status::Unsupported;
    }
    return ValidateUniforms(plan, isUsed);
}
/// The complete RHI description of a backing: RGBA8 color with private Depth32, one sample.
/// A depth-only backing keeps this color image: RHI attachments always pair color with private
/// depth, and every raster pipeline targets at least one color format.
rhi::TextureDescription MakeAttachmentDescription(const format::Resource& backing) noexcept
{
    rhi::TextureDescription description;
    description.Width = backing.Width;
    description.Height = backing.Height;
    description.Attachment = true;
    return description;
}
/// Whether any logical resource exported from the plan lives in this backing allocation.
bool IsExportBacking(BorrowedView<const format::Resource> resources, usize backing) noexcept
{
    bool isExported = false;
    for (usize i = 0; i < resources.GetSize(); ++i)
    {
        isExported = isExported == true || (resources[i].Backing == backing && resources[i].IsExported == 1);
    }
    return isExported;
}
Status CreateZeroedGraphBuffer(rhi::DeviceHandle device, uint32 capacity, rhi::BufferHandle& outBuffer) noexcept
{
    Array<uint8> zeros;
    if (zeros.TryResize(capacity) == false)
    {
        return Status::OutOfMemory;
    }
    return rhi::CreateBuffer(device, {rhi::BufferRole::Storage, capacity}, zeros.AsSpan(), outBuffer);
}
} // namespace
Status ResolveRenderGraphResource(const format::Resource& resource,
                                  uint32 referenceWidth,
                                  uint32 referenceHeight,
                                  format::Resource& outResource) noexcept
{
    if (resource.ViewDenominator == 0 || resource.ViewDenominator > format::MAX_TARGET_EXTENT ||
        resource.ViewNumerator > format::MAX_TARGET_EXTENT)
    {
        return Status::InvalidDescription;
    }
    const bool isBuffer = resource.Kind == format::Aspect::Buffer;
    if ((isBuffer == false && resource.BytesPerPixel != 0) ||
        (isBuffer == true && (resource.Width == 0 || resource.Width > format::MAX_RAY_BUFFER_BYTES ||
                              resource.Width % format::RAY_BUFFER_WORD_BYTES != 0 || resource.Height != 1 ||
                              resource.BytesPerPixel > format::MAX_RAY_BUFFER_PIXEL_BYTES ||
                              resource.BytesPerPixel % format::RAY_BUFFER_WORD_BYTES != 0 ||
                              (resource.BytesPerPixel == 0 && resource.ViewNumerator != 0))))
    {
        return Status::InvalidDescription;
    }
    format::Resource resolved = resource;
    if (resource.ViewNumerator != 0)
    {
        if (referenceWidth == 0 || referenceHeight == 0)
        {
            return Status::NotReady;
        }
        const uint64 width =
            (uint64{referenceWidth} * resource.ViewNumerator + resource.ViewDenominator - 1) / resource.ViewDenominator;
        const uint64 height = (uint64{referenceHeight} * resource.ViewNumerator + resource.ViewDenominator - 1) /
                              resource.ViewDenominator;
        if (width > format::MAX_TARGET_EXTENT || height > format::MAX_TARGET_EXTENT)
        {
            return Status::CapacityExceeded;
        }
        if (isBuffer == true)
        {
            const uint64 capacity = width * height * resource.BytesPerPixel;
            if (capacity > format::MAX_RAY_BUFFER_BYTES)
            {
                return Status::CapacityExceeded;
            }
            resolved.Width = static_cast<uint32>(capacity);
            resolved.Height = 1;
        }
        else
        {
            resolved.Width = static_cast<uint32>(width);
            resolved.Height = static_cast<uint32>(height);
        }
    }
    if (isBuffer == true)
    {
        outResource = resolved;
        return Status::Ready;
    }
    if (resolved.Width == 0 || resolved.Height == 0)
    {
        return Status::InvalidDescription;
    }
    if (resolved.Width > format::MAX_TARGET_EXTENT || resolved.Height > format::MAX_TARGET_EXTENT)
    {
        return Status::CapacityExceeded;
    }
    outResource = resolved;
    return Status::Ready;
}
struct RenderGraph::State final
{
    rhi::DeviceHandle Device{};
    format::Header Header{};
    format::ShaderTarget Target = format::ShaderTarget::Spirv;
    StaticArray<format::Resource, format::MAX_RESOURCES> Resources{};
    StaticArray<format::Pass, format::MAX_PASSES> Passes{};
    StaticArray<rhi::GpuScopeDescription, format::MAX_PASSES> PassScopes{};
    // Only the validated active prefix is copied and read; unused capacity has no stage.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    StaticArray<format::UniformBinding, format::MAX_UNIFORM_BINDINGS> UniformBindings{};
    StaticArray<format::StorageBuffer, format::MAX_STORAGE_BUFFERS> StorageBuffers{};
    usize StorageBufferCount = 0;
    StaticArray<format::UniformBlock, format::MAX_UNIFORM_BLOCKS> UniformBlocks{};
    StaticArray<format::UniformField, format::MAX_UNIFORM_FIELDS> UniformFields{};
    /// Uniform range of each binding: its largest carried block, matching the stage MinSize.
    StaticArray<usize, format::MAX_UNIFORM_BINDINGS> BindingBytes{};
    StaticArray<rhi::TextureHandle, format::MAX_RESOURCES> Textures{};
    StaticArray<rhi::BufferHandle, format::MAX_RESOURCES> Buffers{};
    /// Backings leased from PoolOrNull and returned to it on destruction; exports never are.
    StaticArray<bool, format::MAX_RESOURCES> IsPooled{};
    RenderGraphAttachmentPool* PoolOrNull = nullptr;
    RenderGraphAllocationReport Allocation{};
    struct AttachmentReplacement final
    {
        rhi::DeviceHandle Device{};
        RenderGraphAttachmentPool* PoolOrNull = nullptr;
        core::UniquePtr<internal::RenderGraphMetalFxResources> MetalFxResources;
        uint32 Count = 0;
        StaticArray<format::Resource, format::MAX_RESOURCES> Resources{};
        StaticArray<rhi::TextureHandle, format::MAX_RESOURCES> Textures{};
        StaticArray<rhi::BufferHandle, format::MAX_RESOURCES> Buffers{};
        StaticArray<bool, format::MAX_RESOURCES> IsPooled{};
        ~AttachmentReplacement() noexcept
        {
            for (usize i = 0; i < Count; ++i)
            {
                if (IsPooled[i] == true && internal::RenderGraphPoolAccess::IsBound(*PoolOrNull) == true)
                {
                    internal::RenderGraphPoolAccess::Release(*PoolOrNull,
                                                             MakeAttachmentDescription(Resources[i]),
                                                             Textures[i]);
                }
                else
                {
                    static_cast<void>(rhi::Destroy(Device, Textures[i]));
                }
                static_cast<void>(rhi::Destroy(Device, Buffers[i]));
            }
        }
    };
    core::UniquePtr<AttachmentReplacement> Resizing;
    core::UniquePtr<internal::RenderGraphMetalFxResources> MetalFxResources;
    /// Commit only beside the scene prepared for these dimensions. The local retired set
    /// releases old leases through RHI completion, using the same pool ownership ledger.
    void PublishResize() noexcept
    {
        if (Resizing.Get() == nullptr)
        {
            return;
        }
        for (usize i = 0; i < Header.ResourceCount; ++i)
        {
            const format::Resource resource = Resources[i];
            if (Resizing->Buffers[i] != rhi::BufferHandle{})
            {
                const rhi::BufferHandle retired = Buffers[i];
                Buffers[i] = Resizing->Buffers[i];
                Resizing->Buffers[i] = retired;
            }
            const rhi::TextureHandle texture = Textures[i];
            const bool isPooled = IsPooled[i];
            Resources[i] = Resizing->Resources[i];
            Textures[i] = Resizing->Textures[i];
            IsPooled[i] = Resizing->IsPooled[i];
            Resizing->Resources[i] = resource;
            Resizing->Textures[i] = texture;
            Resizing->IsPooled[i] = isPooled;
        }
        MetalFxResources.Swap(Resizing->MetalFxResources);
        Resizing.Reset();
        HasValidOutput = false;
        OutputRevision = 0;
    }
    StaticArray<Renderer, format::TECHNIQUE_COUNT> Contexts{};
    StaticArray<bool, format::TECHNIQUE_COUNT> IsUsed{};
    StaticArray<bool, format::TECHNIQUE_COUNT> HasViews{};
    bool HasRaySetup = false;
    StaticArray<bool, format::TECHNIQUE_COUNT> IsBound{};
    RenderGraphPrograms Programs{};
    /// Capture-variant RayCompute bindings with the appended capture slot; Programs borrows them.
    // RasterBinding initializes Visibility to Both for every nested element.
    StaticArray<rhi::RasterBinding, RAY_COMPUTE_BINDING_CAPACITY>
        RayComputeBindings{}; // NOLINT(bugprone-invalid-enum-default-initialization)
    internal::RenderGraphRayPathCapture RayPathCapture;
    /// Capture-only replacement for every positive authored Accumulate limit; zero keeps the authored limits.
    uint32 AccumulationLimitOverride = 0;
    /// Capture-only row bands per ray sample (SetRayTileCount), each rendered by its own submission.
    uint32 RayTileCount = 1;
    /// Failure summary read-back of PollRayFailures, in flight while HasFailureReadback.
    rhi::ReadbackTicket FailureReadback{};
    bool HasFailureReadback = false;
    uint32 FailurePass = format::ABSENT;
    void ReleaseFailureReadback() noexcept
    {
        if (HasFailureReadback == true)
        {
            static_cast<void>(rhi::Release(Device, FailureReadback));
            HasFailureReadback = false;
        }
    }
    StaticArray<internal::RenderGraphUpscaleProgram, UPSCALE_TARGET_COUNT> UpscalePrograms{};
    StaticArray<bool, UPSCALE_TARGET_COUNT> HasUpscalePrograms{};
    /// Packed bytes of the distinct value sets of the technique being prepared; reused per Update.
    StaticArray<StaticArray<uint8, MAX_TECHNIQUE_UNIFORM_BYTES>, format::MAX_UNIFORM_SETS> Packed{};
    StaticArray<uint8, MAX_TECHNIQUE_UNIFORM_BYTES> Candidate{};
    internal::ForwardSceneResources ForwardResources;
    MaterialProgram ForwardProgram{};
    internal::RenderGraphDeferredProgram DeferredProgram;
    bool HasDeferredProgram = false;
    bool HasForwardLighting = false;
    bool HasForwardProgram = false;
    bool HasForwardResources = false;
    /// The sample limit an Accumulate on operation averages to: the authored limit, or the capture override
    /// when one is set and the pass accumulates at all. Zero outputs individual samples.
    [[nodiscard]] LUDUS_INLINE uint32 GetAccumulationSamples(const format::Pass& operation) const noexcept
    {
        return operation.AccumulationSamples != 0 && AccumulationLimitOverride != 0 ? AccumulationLimitOverride
                                                                                    : operation.AccumulationSamples;
    }
    /// The largest accumulation limit of this graph, which bounds the submitted-sample counter.
    [[nodiscard]] LUDUS_INLINE uint32 GetMaximumAccumulationSamples() const noexcept
    {
        return AccumulationLimitOverride != 0 ? AccumulationLimitOverride : format::MAX_ACCUMULATION_SAMPLES;
    }
    /// Count one valid ray frame: the next band, and a sample once its last band was submitted.
    void CountRayFrame() noexcept
    {
        if (HasRaySetup == false)
        {
            return;
        }
        Current->NextRayTile = Current->NextRayTile + 1 < RayTileCount ? Current->NextRayTile + 1 : 0;
        if (Current->NextRayTile == 0 && Current->SubmittedRaySamples < GetMaximumAccumulationSamples())
        {
            ++Current->SubmittedRaySamples;
        }
    }
    [[nodiscard]] LUDUS_INLINE Renderer& GetContext(format::Technique technique) noexcept
    {
        const format::Technique context =
            Programs.HasClusteredForward == true && technique == format::Technique::DepthOnly
                ? format::Technique::ColorAndDepth
                : technique;
        return Contexts[static_cast<usize>(context)];
    }
    [[nodiscard]] LUDUS_INLINE bool HasForwardProfile() const noexcept
    {
        return Programs.ForwardVertex.Inputs.empty() == false;
    }
    struct Scene final
    {
        State* OwnerOrNull = nullptr;
        core::UniquePtr<internal::ForwardSceneGeneration> ForwardGeneration;
        StaticArray<StaticArray<Mesh, format::MAX_MESHES>, format::TECHNIQUE_COUNT> Meshes{};
        StaticArray<bool, format::MAX_MESHES> HasMeshes{};
        StaticArray<StaticArray<UniformSet, format::MAX_UNIFORM_SETS>, format::TECHNIQUE_COUNT> UniformSets{};
        StaticArray<usize, format::TECHNIQUE_COUNT> UniformSetCounts{};
        /// Distinct value set drawn for each item by each bound technique, or NO_SET.
        StaticArray<StaticArray<usize, format::MAX_ITEMS>, format::TECHNIQUE_COUNT> ItemSets{};
        StaticArray<Snapshot, format::MAX_PASSES> Snapshots{};
        StaticArray<PreparedView, format::MAX_PASSES> Views{};
        StaticArray<core::UniquePtr<internal::RenderGraphDeferredPass>, format::MAX_PASSES> DeferredPasses{};
        StaticArray<internal::RenderGraphRayPass, format::MAX_PASSES> RayPasses{};
        StaticArray<internal::RenderGraphUpscalePass, format::MAX_PASSES> UpscalePasses{};
        StaticArray<core::UniquePtr<internal::RenderGraphDeferredTargets>, format::MAX_PASSES> DeferredTargets{};
        rhi::OrderedGraph RayGraph{};
        bool IsRayGraphPrepared = false;
        StaticArray<ViewDescription, format::MAX_CAMERAS> Cameras{};
        StaticArray<StaticArray<SceneItem, format::MAX_ITEMS>, format::MAX_PASSES> Items{};
        StaticArray<usize, format::MAX_PASSES> Counts{};
        StaticArray<bool, format::MAX_PASSES> HasSnapshots{};
        StaticArray<bool, format::MAX_PASSES> HasViews{};
        usize MeshCount = 0;
        uint64 Revision = 0;
        uint64 ContentRevision = 0;
        uint64 DeferredAttachmentBytes = 0;
        RenderGraphSceneRevisions Revisions{};
        RenderGraphCoveragePolicy CoveragePolicy = RenderGraphCoveragePolicy::RejectIncomplete;
        /// Samples whose every band was submitted; one per frame when untiled.
        uint32 SubmittedRaySamples = 0;
        /// Band the next ray frame dispatches; zero at a sample boundary.
        uint32 NextRayTile = 0;
        ~Scene() noexcept
        {
            if (OwnerOrNull != nullptr)
            {
                static_cast<void>(rhi::DiscardOrderedGraph(OwnerOrNull->Device, RayGraph));
                for (usize i = 0; i < OwnerOrNull->Header.PassCount; ++i)
                {
                    DeferredPasses[i].Reset();
                    DeferredTargets[i].Reset();
                    Renderer& context = OwnerOrNull->GetContext(OwnerOrNull->Passes[i].Program);
                    static_cast<void>(context.Release(Views[i]));
                    static_cast<void>(context.Release(Snapshots[i]));
                }
                for (usize i = 0; i < format::TECHNIQUE_COUNT; ++i)
                {
                    for (usize mesh = 0; mesh < MeshCount; ++mesh)
                    {
                        static_cast<void>(OwnerOrNull->Contexts[i].Release(Meshes[i][mesh]));
                    }
                    for (usize set = 0; set < UniformSetCounts[i]; ++set)
                    {
                        static_cast<void>(OwnerOrNull->Contexts[i].Release(UniformSets[i][set]));
                    }
                }
            }
        }
    };
    core::UniquePtr<Scene> Current;
    core::UniquePtr<Scene> Pending;
    struct CameraUpdate final
    {
        State* OwnerOrNull = nullptr;
        Scene* SourceOrNull = nullptr;
        StaticArray<PreparedView, format::MAX_PASSES> Views{};
        StaticArray<core::UniquePtr<internal::RenderGraphDeferredPass>, format::MAX_PASSES> DeferredPasses{};
        StaticArray<bool, format::MAX_PASSES> HasRayCamera{};
        StaticArray<ViewDescription, format::MAX_CAMERAS> Cameras{};
        uint64 Revision = 0;
        ~CameraUpdate() noexcept
        {
            if (OwnerOrNull == nullptr)
            {
                return;
            }
            for (usize i = 0; i < OwnerOrNull->Header.PassCount; ++i)
            {
                if (HasRayCamera[i] == true)
                {
                    SourceOrNull->RayPasses[i].DiscardCamera();
                }
                DeferredPasses[i].Reset();
                static_cast<void>(OwnerOrNull->GetContext(OwnerOrNull->Passes[i].Program).Release(Views[i]));
            }
        }
    };
    core::UniquePtr<CameraUpdate> PendingCamera;
    Status PrepareDeferredView(core::UniquePtr<internal::RenderGraphDeferredPass>& inoutPass,
                               internal::RenderGraphDeferredTargets& inoutTargets,
                               PreparedView view,
                               const ViewDescription& camera,
                               const internal::ForwardSceneGeneration& lighting,
                               const format::Pass& operation) noexcept
    {
        if (inoutPass.Get() == nullptr)
        {
            internal::RenderGraphDeferredPass* allocationOrNull = new (std::nothrow) internal::RenderGraphDeferredPass;
            if (allocationOrNull == nullptr)
            {
                return Status::OutOfMemory;
            }
            inoutPass = core::UniquePtr<internal::RenderGraphDeferredPass>{allocationOrNull};
            const Status status = inoutPass->Initialize(inoutTargets,
                                                        GetContext(format::Technique::ColorAndDepth),
                                                        view,
                                                        camera,
                                                        lighting.GetMeshLights(),
                                                        lighting.GetMeshLightCount(),
                                                        lighting.GetMeshLightGridWord(),
                                                        operation);
            if (IsAccepted(status) == false)
            {
                return status;
            }
        }
        return inoutPass->Poll();
    }
    Status PrepareCameras(BorrowedView<const ViewDescription> cameras, uint64 revision) noexcept
    {
        CameraUpdate* allocationOrNull = new (std::nothrow) CameraUpdate;
        core::UniquePtr<CameraUpdate> candidate{allocationOrNull};
        if (candidate.Get() == nullptr)
        {
            return Status::OutOfMemory;
        }
        candidate->OwnerOrNull = this;
        candidate->SourceOrNull = Current.Get();
        candidate->Revision = revision;
        for (usize i = 0; i < cameras.GetSize(); ++i)
        {
            candidate->Cameras[i] = cameras[i];
            candidate->Cameras[i].OverlayToClip = cameras[i].WorldToClip;
        }
        for (usize i = 0; i < Header.PassCount; ++i)
        {
            const format::Pass& pass = Passes[i];
            if (format::IsImageTechnique(pass.Program) == true)
            {
                continue;
            }
            Status status = Status::NotReady;
            if (pass.Program == format::Technique::RayTracing)
            {
                status = Current->RayPasses[i].PrepareCamera(cameras[pass.Camera], revision);
                candidate->HasRayCamera[i] = IsAccepted(status);
            }
            else
            {
                ViewReport viewReport;
                const ViewDescription view =
                    MakeRasterPassView(candidate->Cameras[pass.Camera], Programs, pass.Program);
                status =
                    GetContext(pass.Program).PrepareView(Current->Snapshots[i], view, candidate->Views[i], viewReport);
            }
            if (IsAccepted(status) == false)
            {
                return status;
            }
        }
        PendingCamera.Swap(candidate);
        return Status::Pending;
    }
    Status PollCameras() noexcept
    {
        if (Producer.IsNull() == false)
        {
            const Status producer = rhi::GetStatus(Device, Producer);
            if (producer != Status::Ready)
            {
                return producer;
            }
        }
        CameraUpdate& update = *PendingCamera;
        for (usize i = 0; i < Header.PassCount; ++i)
        {
            if (format::IsImageTechnique(Passes[i].Program) == true)
            {
                continue;
            }
            Status status = update.HasRayCamera[i] == true ? Current->RayPasses[i].PollCamera()
                                                           : GetContext(Passes[i].Program).GetStatus(update.Views[i]);
            if (status == Status::Ready && Programs.HasClusteredDeferred == true &&
                Passes[i].Program == format::Technique::ColorAndDepth)
            {
                status = PrepareDeferredView(update.DeferredPasses[i],
                                             *Current->DeferredTargets[i],
                                             update.Views[i],
                                             update.Cameras[Passes[i].Camera],
                                             *Current->ForwardGeneration,
                                             Passes[i]);
            }
            if (status != Status::Ready)
            {
                if (status != Status::Pending)
                {
                    PendingCamera.Reset();
                }
                return status;
            }
        }
        // Every replacement is ready. Retire the old binding snapshots before swapping them.
        static_cast<void>(rhi::DiscardOrderedGraph(Device, Current->RayGraph));
        Current->IsRayGraphPrepared = false;
        // Publication cannot allocate or fail, and no producer is open.
        for (usize i = 0; i < Header.PassCount; ++i)
        {
            if (format::IsImageTechnique(Passes[i].Program) == true)
            {
                continue;
            }
            if (update.HasRayCamera[i] == true)
            {
                Current->RayPasses[i].CommitCamera();
                update.HasRayCamera[i] = false;
            }
            else
            {
                static_cast<void>(GetContext(Passes[i].Program).Release(Current->Views[i]));
                Current->DeferredPasses[i].Swap(update.DeferredPasses[i]);
                Current->Views[i] = update.Views[i];
                update.Views[i] = {};
            }
        }
        Current->Cameras = update.Cameras;
        Current->Revision = update.Revision;
        Current->SubmittedRaySamples = 0;
        Current->NextRayTile = 0;
        if (Report.SceneRevision == update.Revision)
        {
            Report.IsReady = true;
        }
        PendingCamera.Reset();
        return Status::Ready;
    }
    StaticArray<RenderGraphCoverageRecord, MAX_COVERAGE> Coverage{};
    usize CoverageCount = 0;
    RenderGraphReport Report{};
    bool IsReady = false;
    bool HasOpenFrame = false;
    bool HasExternalRecording = false;
    bool HasValidOutput = false;
    bool HasAttemptedDraw = false;
    uint64 RecordingRevision = 0;
    uint64 OutputRevision = 0;
    Status RecordingStatus = Status::Ready;
    rhi::SurfaceHandle RecordingSurface{};
    rhi::SubmissionToken Producer{};
    /// Whether the opaque color context can draw the editor's magenta marker: it reads instance color
    /// and binds no uniforms. Bound or masked-only color techniques never pretend to provide markers.
    [[nodiscard]] bool HasDiagnosticContext() const noexcept
    {
        constexpr usize COLOR = static_cast<usize>(format::Technique::ColorAndDepth);
        return IsUsed[COLOR] == true && IsBound[COLOR] == false;
    }
    /// Pack one item's logical values into the technique's concatenated target blocks.
    usize PackUniforms(format::Technique technique, BorrowedView<const float32> values) noexcept
    {
        usize size = 0;
        for (usize binding = 0; binding < Header.UniformBindingCount; ++binding)
        {
            if (UniformBindings[binding].Program != technique)
            {
                continue;
            }
            for (usize i = 0; i < BindingBytes[binding]; ++i)
            {
                Candidate[size + i] = 0;
            }
            for (usize index = 0; index < Header.UniformFieldCount; ++index)
            {
                const format::UniformField& field = UniformFields[index];
                if (field.Binding != binding || field.Target != Target)
                {
                    continue;
                }
                for (usize component = 0; component < field.Components; ++component)
                {
                    const float32 value = values[UniformBindings[binding].ValueOffset + field.ValueOffset + component];
                    // Native-endian float32, matching the RHI upload and generated packers.
                    const uint8* bytes = reinterpret_cast<const uint8*>(&value);
                    for (usize byte = 0; byte < sizeof(float32); ++byte)
                    {
                        Candidate[size + field.ByteOffset + component * sizeof(float32) + byte] = bytes[byte];
                    }
                }
            }
            size += BindingBytes[binding];
        }
        return size;
    }
    Status PollScene(Scene& inoutScene) noexcept
    {
        if (Resizing.Get() != nullptr)
        {
            for (usize i = 0; i < Resizing->Count; ++i)
            {
                if (Resizing->Buffers[i] != rhi::BufferHandle{})
                {
                    const Status ready = rhi::GetStatus(Device, Resizing->Buffers[i]);
                    if (ready != Status::Ready)
                    {
                        return ready;
                    }
                }
                if (Resizing->Resources[i].Backing == i && Resizing->Resources[i].IsSurface == 0 &&
                    Resizing->Resources[i].Kind != format::Aspect::Buffer)
                {
                    const Status ready = rhi::GetStatus(Device, Resizing->Textures[i]);
                    if (ready != Status::Ready)
                    {
                        return ready;
                    }
                }
            }
        }
        const StaticArray<format::Resource, format::MAX_RESOURCES>& preparingResources =
            Resizing.Get() != nullptr ? Resizing->Resources : Resources;
        if (inoutScene.ForwardGeneration.Get() != nullptr)
        {
            const Status ready = inoutScene.ForwardGeneration->Poll();
            if (ready != Status::Ready)
            {
                return ready;
            }
        }
        for (usize context = 0; context < format::TECHNIQUE_COUNT; ++context)
        {
            if (IsUsed[context] == false || context == static_cast<usize>(format::Technique::RayTracing))
            {
                continue;
            }
            for (usize mesh = 0; mesh < inoutScene.MeshCount; ++mesh)
            {
                if (inoutScene.HasMeshes[mesh] == true)
                {
                    const Status status = Contexts[context].GetStatus(inoutScene.Meshes[context][mesh]);
                    if (status != Status::Ready)
                    {
                        return status;
                    }
                }
            }
            for (usize set = 0; set < inoutScene.UniformSetCounts[context]; ++set)
            {
                const Status status = Contexts[context].GetStatus(inoutScene.UniformSets[context][set]);
                if (status != Status::Ready)
                {
                    return status;
                }
            }
        }
        for (usize i = 0; i < Header.PassCount; ++i)
        {
            if (Passes[i].Program == format::Technique::MetalFxSpatialUpscale ||
                Passes[i].Program == format::Technique::MetalFxSpatialPerceptualUpscale)
            {
                core::UniquePtr<internal::RenderGraphMetalFxResources>& effects =
                    Resizing.Get() != nullptr ? Resizing->MetalFxResources : MetalFxResources;
                if (effects.Get() == nullptr)
                {
                    effects.Reset(new (std::nothrow) internal::RenderGraphMetalFxResources);
                    if (effects.Get() == nullptr)
                    {
                        return Status::OutOfMemory;
                    }
                }
                if (effects->IsPrepared[i] == false)
                {
                    const bool isSurface = preparingResources[Passes[i].Color].IsSurface == 1;
                    if (isSurface == true)
                    {
                        internal::RenderGraphUpscaleProgram& program = UpscalePrograms[0];
                        if (HasUpscalePrograms[0] == false)
                        {
                            const Status created =
                                program.Initialize(Device, Programs.CompositeVertex, Programs.CompositeFragment, true);
                            if (IsAccepted(created) == false)
                            {
                                return created;
                            }
                            HasUpscalePrograms[0] = true;
                        }
                        const Status ready = program.Poll();
                        if (ready != Status::Ready)
                        {
                            return ready;
                        }
                    }
                    const format::Resource& source = preparingResources[Passes[i].SourceColor];
                    const format::Resource& destination = preparingResources[Passes[i].Color];
                    rhi::SpatialUpscalerDescription description;
                    description.InputWidth = source.Width;
                    description.InputHeight = source.Height;
                    description.OutputWidth = destination.Width;
                    description.OutputHeight = destination.Height;
                    description.ColorProcessing = Passes[i].Program == format::Technique::MetalFxSpatialUpscale
                                                      ? rhi::SpatialUpscaleColorProcessing::Linear
                                                      : rhi::SpatialUpscaleColorProcessing::Perceptual;
                    const StaticArray<rhi::TextureHandle, format::MAX_RESOURCES>& textures =
                        Resizing.Get() != nullptr ? Resizing->Textures : Textures;
                    const Status created =
                        effects->Passes[i].Initialize(Device,
                                                      description,
                                                      textures[Passes[i].SourceColor],
                                                      textures[Passes[i].Color],
                                                      isSurface == true ? &UpscalePrograms[0] : nullptr);
                    if (IsAccepted(created) == false)
                    {
                        return created;
                    }
                    effects->IsPrepared[i] = true;
                }
                const Status ready = effects->Passes[i].Poll();
                if (ready != Status::Ready)
                {
                    return ready;
                }
                continue;
            }
            if (format::IsImageTechnique(Passes[i].Program) == true)
            {
                const StaticArray<rhi::TextureHandle, format::MAX_RESOURCES>& textures =
                    Resizing.Get() != nullptr ? Resizing->Textures : Textures;
                if (inoutScene.HasViews[i] == false)
                {
                    const usize programIndex = preparingResources[Passes[i].Color].IsSurface == 1 ? 0 : 1;
                    internal::RenderGraphUpscaleProgram& program = UpscalePrograms[programIndex];
                    if (HasUpscalePrograms[programIndex] == false)
                    {
                        const Status created = program.Initialize(Device,
                                                                  Programs.CompositeVertex,
                                                                  Programs.CompositeFragment,
                                                                  programIndex == 0);
                        if (IsAccepted(created) == false)
                        {
                            return created;
                        }
                        HasUpscalePrograms[programIndex] = true;
                    }
                    const Status ready = program.Poll();
                    if (ready != Status::Ready)
                    {
                        return ready;
                    }
                    const Status created =
                        inoutScene.UpscalePasses[i].Initialize(program, textures[Passes[i].SourceColor]);
                    if (IsAccepted(created) == false)
                    {
                        return created;
                    }
                    inoutScene.HasViews[i] = true;
                }
                const Status ready = inoutScene.UpscalePasses[i].Poll();
                if (ready != Status::Ready)
                {
                    return ready;
                }
                continue;
            }
            Renderer& context = GetContext(Passes[i].Program);
            if (Passes[i].Program == format::Technique::RayTracing && inoutScene.HasViews[i] == true)
            {
                const Status ready = inoutScene.RayPasses[i].Poll();
                if (ready != Status::Ready)
                {
                    return ready;
                }
                continue;
            }
            if (inoutScene.HasSnapshots[i] == false)
            {
                const BorrowedView<const SceneItem> items =
                    inoutScene.ForwardGeneration.Get() != nullptr
                        ? inoutScene.ForwardGeneration->GetItems()
                        : BorrowedView<const SceneItem>{inoutScene.Items[i].GetData(), inoutScene.Counts[i]};
                const Status created =
                    context.CreateSnapshot(items.GetDataOrNull(), items.GetSize(), inoutScene.Snapshots[i]);
                if (IsAccepted(created) == false)
                {
                    return created;
                }
                inoutScene.HasSnapshots[i] = true;
            }
            if (Passes[i].Program == format::Technique::RayTracing)
            {
                if (inoutScene.HasViews[i] == false)
                {
                    GpuPreviewGraphInvocation invocation{0,
                                                         {},
                                                         GetAccumulationSamples(Passes[i]),
                                                         Passes[i].ToneMappingExposure,
                                                         Passes[i].DenoisingQuality,
                                                         {},
                                                         Passes[i].Traversal == format::RayTraversal::HardwarePipeline};
                    invocation.ToneMappingCurve = GetToneCurve(Passes[i]);
                    const Status created =
                        inoutScene.RayPasses[i].Initialize(context,
                                                           inoutScene.Snapshots[i],
                                                           inoutScene.Cameras[Passes[i].Camera],
                                                           preparingResources[Passes[i].Color].Width,
                                                           preparingResources[Passes[i].Color].Height,
                                                           nullptr,
                                                           &invocation);
                    if (IsAccepted(created) == false)
                    {
                        return created;
                    }
                    inoutScene.HasViews[i] = true;
                }
                const Status ready = inoutScene.RayPasses[i].Poll();
                if (ready != Status::Ready)
                {
                    return ready;
                }
                continue;
            }
            if (inoutScene.HasViews[i] == false)
            {
                ViewReport report;
                const ViewDescription view =
                    MakeRasterPassView(inoutScene.Cameras[Passes[i].Camera], Programs, Passes[i].Program);
                const Status prepared = context.PrepareView(inoutScene.Snapshots[i], view, inoutScene.Views[i], report);
                if (IsAccepted(prepared) == false)
                {
                    return prepared;
                }
                inoutScene.HasViews[i] = true;
            }
            const Status ready = context.GetStatus(inoutScene.Views[i]);
            if (ready != Status::Ready)
            {
                return ready;
            }
            if (Programs.HasClusteredDeferred == true && Passes[i].Program == format::Technique::ColorAndDepth)
            {
                if (inoutScene.DeferredTargets[i].Get() == nullptr)
                {
                    internal::RenderGraphDeferredTargets* allocationOrNull =
                        new (std::nothrow) internal::RenderGraphDeferredTargets;
                    if (allocationOrNull == nullptr)
                    {
                        return Status::OutOfMemory;
                    }
                    inoutScene.DeferredTargets[i] =
                        core::UniquePtr<internal::RenderGraphDeferredTargets>{allocationOrNull};
                    const uint32 backing = preparingResources[Passes[i].Depth].Backing;
                    const StaticArray<rhi::TextureHandle, format::MAX_RESOURCES>& textures =
                        Resizing.Get() != nullptr ? Resizing->Textures : Textures;
                    const Status initialized =
                        inoutScene.DeferredTargets[i]->Initialize(DeferredProgram,
                                                                  textures[backing],
                                                                  preparingResources[backing].Width,
                                                                  preparingResources[backing].Height);
                    if (IsAccepted(initialized) == false)
                    {
                        return initialized;
                    }
                    constexpr usize COLOR_CHANNEL_COUNT = 4;
                    constexpr usize ATTACHMENT_BYTES_PER_PIXEL = COLOR_CHANNEL_COUNT * sizeof(uint16) + sizeof(float32);
                    inoutScene.DeferredAttachmentBytes = uint64{preparingResources[backing].Width} *
                                                         preparingResources[backing].Height * DEFERRED_TARGET_COUNT *
                                                         ATTACHMENT_BYTES_PER_PIXEL;
                }
                const Status targets = inoutScene.DeferredTargets[i]->Poll();
                if (targets != Status::Ready)
                {
                    return targets;
                }
                const Status deferred = PrepareDeferredView(inoutScene.DeferredPasses[i],
                                                            *inoutScene.DeferredTargets[i],
                                                            inoutScene.Views[i],
                                                            inoutScene.Cameras[Passes[i].Camera],
                                                            *inoutScene.ForwardGeneration,
                                                            Passes[i]);
                if (deferred != Status::Ready)
                {
                    return deferred;
                }
            }
        }
        return Status::Ready;
    }
    Status EncodePasses(rhi::SurfaceHandle surface) noexcept
    {
        RecordingSurface = surface;
        RecordingRevision = Current->Revision;
        RecordingStatus = Status::Ready;
        HasAttemptedDraw = false;
        rhi::FrameInfo information;
        const rhi::DeviceStatus acquired = rhi::GetFrameInfo(Device, surface, information);
        if (acquired != rhi::DeviceStatus::Ready)
        {
            RecordingStatus = MapDeviceStatus(acquired);
            return RecordingStatus;
        }
        for (usize resource = 0; resource < Header.ResourceCount; ++resource)
        {
            const format::Resource& description = Resources[resource];
            if (description.IsSurface == 1 &&
                (description.Width != information.Width || description.Height != information.Height))
            {
                RecordingStatus = Status::NotReady;
                return RecordingStatus;
            }
        }
        for (usize pass = 0; pass < Header.PassCount && RecordingStatus == Status::Ready; ++pass)
        {
            const rhi::GpuScope scope(Device, PassScopes[pass]);
            const format::Pass& operation = Passes[pass];
            if (format::IsImageTechnique(operation.Program) == true)
            {
                HasAttemptedDraw = true;
                RecordingStatus = operation.Program == format::Technique::BilinearUpscale
                                      ? Current->UpscalePasses[pass].Draw(Textures[operation.Color])
                                      : MetalFxResources->Passes[pass].Draw();
                continue;
            }
            if (operation.Program == format::Technique::RayTracing)
            {
                if (pass > 0 && Passes[pass - 1].Program == format::Technique::RayTracing)
                {
                    continue;
                }
                if (Current->IsRayGraphPrepared == false)
                {
                    return RecordingStatus = Status::NotReady;
                }
                HasAttemptedDraw = true;
                RecordingStatus = rhi::EncodeOrderedGraph(Device, surface, Current->RayGraph);
                Current->IsRayGraphPrepared = false;
                continue;
            }
            if (pass + 1 < Header.PassCount && Passes[pass + 1].Program == format::Technique::RayTracing &&
                Passes[pass + 1].Visibility == operation.Color)
            {
                continue;
            }
            const uint32 backing = Resources[operation.Depth].Backing;
            rhi::RasterPassDescription description;
            description.Color = Textures[backing];
            description.ColorLoad = static_cast<rhi::RasterLoad>(operation.ColorLoad);
            description.ColorStore = static_cast<rhi::RasterStore>(operation.ColorStore);
            description.DepthLoad = static_cast<rhi::RasterLoad>(operation.DepthLoad);
            description.DepthStore = static_cast<rhi::RasterStore>(operation.DepthStore);
            if (GetStage(Programs, operation.Program, rhi::ShaderStage::Vertex).Inputs.size() ==
                format::VISIBILITY_INPUT_COUNT)
            {
                // Raster-only visibility exports share the codec's zero-background contract.
                constexpr usize ALPHA_CHANNEL = 3;
                description.Clear[ALPHA_CHANNEL] = 0;
            }
            HasAttemptedDraw = true;
            RecordingStatus =
                Programs.HasClusteredDeferred == true && operation.Program == format::Technique::ColorAndDepth
                    ? Current->DeferredPasses[pass]->Draw(GetContext(operation.Program),
                                                          Current->Views[pass],
                                                          description)
                : Programs.HasClusteredForward == true && operation.Program == format::Technique::DepthOnly
                    ? GetContext(operation.Program).DrawDepthView(Current->Views[pass], description)
                    : GetContext(operation.Program).DrawView(Current->Views[pass], description);
        }
        return RecordingStatus;
    }
    Status FinishFrame(rhi::SubmissionToken& outCompletion) noexcept
    {
        const Status submitted = rhi::EndFrame(Device, outCompletion);
        if (outCompletion.IsNull() == false)
        {
            Producer = outCompletion;
        }
        rhi::FrameInfo information;
        HasOpenFrame = rhi::GetFrameInfo(Device, RecordingSurface, information) == rhi::DeviceStatus::Ready;
        if (HasOpenFrame == true)
        {
            return submitted;
        }
        if (HasAttemptedDraw == true)
        {
            HasValidOutput = submitted == Status::Ready && RecordingStatus == Status::Ready;
            OutputRevision = HasValidOutput == true ? RecordingRevision : 0;
            if (HasValidOutput == true)
            {
                CountRayFrame();
            }
        }
        return RecordingStatus == Status::Ready ? submitted : RecordingStatus;
    }
    ~State() noexcept
    {
        ReleaseFailureReadback();
        MetalFxResources.Reset();
        PendingCamera.Reset();
        Pending.Reset();
        Resizing.Reset();
        Current.Reset();
        ForwardResources.Reset();
        static_cast<void>(Contexts[static_cast<usize>(format::Technique::ColorAndDepth)].Release(ForwardProgram));
        for (usize i = 0; i < Header.ResourceCount; ++i)
        {
            static_cast<void>(rhi::Destroy(Device, Buffers[i]));
            // A pool reset early (a broken precondition, asserted there) no longer takes leases back.
            if (IsPooled[i] == true && internal::RenderGraphPoolAccess::IsBound(*PoolOrNull) == true)
            {
                internal::RenderGraphPoolAccess::Release(*PoolOrNull,
                                                         MakeAttachmentDescription(Resources[i]),
                                                         Textures[i]);
            }
            else
            {
                static_cast<void>(rhi::Destroy(Device, Textures[i]));
            }
        }
    }
};
RenderGraph::RenderGraph() noexcept = default;
RenderGraph::~RenderGraph() noexcept = default;
void RenderGraph::Reset() noexcept
{
    mState.Reset();
}
Status RenderGraph::SetAccumulationLimitOverride(uint32 samples) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    if (samples == 0 || samples > MAX_RENDER_GRAPH_CAPTURE_SAMPLES)
    {
        return Status::InvalidDescription;
    }
    if (state.IsReady == false || state.HasOpenFrame == true || state.HasExternalRecording == true ||
        state.Current.Get() != nullptr || state.Pending.Get() != nullptr)
    {
        return Status::InvalidState;
    }
    state.AccumulationLimitOverride = samples;
    return Status::Ready;
}
Status RenderGraph::SetRayTileCount(uint32 tileCount) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    if (tileCount == 0 || tileCount > MAX_RENDER_GRAPH_RAY_TILES)
    {
        return Status::InvalidDescription;
    }
    if (state.IsReady == false || state.HasOpenFrame == true || state.HasExternalRecording == true ||
        (state.Current.Get() != nullptr && state.Current->NextRayTile != 0))
    {
        return Status::InvalidState;
    }
    if (tileCount > 1 && state.Programs.HasRayTiles == false)
    {
        return Status::Unsupported;
    }
    state.RayTileCount = tileCount;
    if (state.Current.Get() != nullptr)
    {
        // The prepared sequence dispatches the previous band layout.
        state.Current->IsRayGraphPrepared = false;
    }
    return Status::Ready;
}
uint32 RenderGraph::GetSubmittedRaySamples() const noexcept
{
    return mState.Get() == nullptr || mState->Current.Get() == nullptr ? 0 : mState->Current->SubmittedRaySamples;
}
Status RenderGraph::PollRayFailures(uint32 pass, RenderGraphRayFailures& outFailures) noexcept
{
    if (mState.Get() == nullptr || mState->Current.Get() == nullptr || mState->HasRaySetup == false)
    {
        return Status::NotReady;
    }
    State& state = *mState;
    if (pass >= state.Header.PassCount || state.Passes[pass].Program != format::Technique::RayTracing)
    {
        return Status::InvalidDescription;
    }
    if (state.HasFailureReadback == true && state.FailurePass != pass)
    {
        state.ReleaseFailureReadback();
    }
    if (state.HasFailureReadback == true)
    {
        const Status status = rhi::GetStatus(state.Device, state.FailureReadback);
        if (status == Status::Pending)
        {
            return Status::Pending;
        }
        StaticArray<uint32, MAX_RENDER_GRAPH_FAILURE_REASONS> pixels{};
        const Status copied = status == Status::Ready ? rhi::CopyReadback(state.Device,
                                                                          state.FailureReadback,
                                                                          reinterpret_cast<uint8*>(pixels.GetData()),
                                                                          sizeof(pixels))
                                                      : status;
        state.ReleaseFailureReadback();
        if (copied != Status::Ready)
        {
            return copied;
        }
        RenderGraphRayFailures result;
        result.Pixels = pixels;
        for (uint32 reason = 0; reason < MAX_RENDER_GRAPH_FAILURE_REASONS; ++reason)
        {
            result.TotalPixels += pixels[reason];
            result.MainReason = pixels[reason] > pixels[result.MainReason] ? reason : result.MainReason;
        }
        outFailures = result;
        return Status::Ready;
    }
    // Read the counts after the frames that wrote them.
    if (state.Producer.IsNull() == false)
    {
        const Status completed = rhi::GetStatus(state.Device, state.Producer);
        if (completed != Status::Ready)
        {
            return completed;
        }
    }
    rhi::BufferHandle failures;
    const Status found = state.Current->RayPasses[pass].GetFailures(failures);
    if (found != Status::Ready)
    {
        return found;
    }
    const Status requested =
        rhi::RequestBufferReadback(state.Device, failures, 0, LUDUS_GRAPH_FAILURE_BYTES, state.FailureReadback);
    if (requested != Status::Pending && requested != Status::Ready)
    {
        return requested;
    }
    state.HasFailureReadback = true;
    state.FailurePass = pass;
    return Status::Pending;
}
Status RenderGraph::GetRayWorkload(RenderGraphRayWorkload& outWorkload) const noexcept
{
    if (mState.Get() == nullptr || mState->Current.Get() == nullptr || mState->HasRaySetup == false)
    {
        return Status::NotReady;
    }
    const State& state = *mState;
    RenderGraphRayWorkload workload;
    for (usize pass = 0; pass < state.Header.PassCount; ++pass)
    {
        if (state.Passes[pass].Program != format::Technique::RayTracing)
        {
            continue;
        }
        const format::Resource& color = state.Resources[state.Passes[pass].Color];
        workload.Pixels += uint64{color.Width} * color.Height;
        workload.Lights = math::Max(workload.Lights, state.Current->RayPasses[pass].GetLightCount());
    }
    outWorkload = workload;
    return Status::Ready;
}
Status RenderGraph::GetAccumulation(uint32 pass, rhi::BufferHandle& outBuffer) const noexcept
{
    if (mState.Get() == nullptr || mState->Current.Get() == nullptr)
    {
        return Status::NotReady;
    }
    const State& state = *mState;
    if (pass >= state.Header.PassCount || state.Passes[pass].Program != format::Technique::RayTracing)
    {
        return Status::InvalidDescription;
    }
    if (state.HasValidOutput == false || state.Producer.IsNull() == true ||
        rhi::GetStatus(state.Device, state.Producer) != Status::Ready)
    {
        return Status::NotReady;
    }
    // The lookup reads the context's job table without changing it; the internal bridge takes a mutable context.
    return internal::RenderGraphRayPass::GetAccumulation(
        const_cast<Renderer&>(state.Contexts[static_cast<usize>(format::Technique::RayTracing)]),
        state.Current->RayPasses[pass].GetJob(),
        outBuffer);
}
Status RenderGraph::RequestRayPathCapture(const RayPathCaptureDescription& description) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    if (state.Programs.HasRayPathCapture == false)
    {
        return Status::Unsupported;
    }
    const Status admitted = CheckDevice(state.Device);
    if (admitted != Status::Ready)
    {
        return admitted;
    }
    if (state.IsReady == false || state.HasOpenFrame == true || state.HasExternalRecording == true ||
        state.Current.Get() != nullptr || state.Pending.Get() != nullptr)
    {
        return Status::InvalidState;
    }
    // Records carry the cooked callback-set index; map it back to the one plan pass using it.
    internal::RayPathPassTable passes{};
    for (uint32& pass : passes)
    {
        pass = format::ABSENT;
    }
    for (usize pass = 0; pass < state.Header.PassCount; ++pass)
    {
        const format::Pass& operation = state.Passes[pass];
        if (operation.Program != format::Technique::RayTracing)
        {
            continue;
        }
        if (operation.RayProgram >= passes.GetSize() || passes[operation.RayProgram] != format::ABSENT)
        {
            return Status::Unsupported;
        }
        passes[operation.RayProgram] = static_cast<uint32>(pass);
    }
    return state.RayPathCapture.Request(state.Device, description, passes);
}
Status RenderGraph::PollRayPathCapture(RayPathCaptureReport& outReport) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    if (state.RayPathCapture.IsRequested() == false || state.HasOpenFrame == true || state.HasExternalRecording == true)
    {
        return Status::InvalidState;
    }
    const Status admitted = CheckDevice(state.Device);
    if (admitted != Status::Ready)
    {
        return admitted;
    }
    if (state.Producer.IsNull() == false)
    {
        const Status completed = rhi::GetStatus(state.Device, state.Producer);
        if (completed != Status::Ready)
        {
            return completed;
        }
    }
    return state.RayPathCapture.Poll(outReport);
}
BorrowedView<const RayPathEvent> RenderGraph::GetRayPathEvents() const noexcept
{
    return mState.Get() != nullptr ? mState->RayPathCapture.GetEvents() : BorrowedView<const RayPathEvent>{};
}
BorrowedView<const RayPathSummary> RenderGraph::GetRayPathSummaries() const noexcept
{
    return mState.Get() != nullptr ? mState->RayPathCapture.GetSummaries() : BorrowedView<const RayPathSummary>{};
}
Status RenderGraph::Initialize(rhi::DeviceHandle device,
                               const RenderGraphPlan& sourcePlan,
                               const RenderGraphPrograms& programs,
                               RenderGraphAttachmentPool* poolOrNull,
                               uint32 referenceWidth,
                               uint32 referenceHeight) noexcept
{
    if (mState.Get() != nullptr)
    {
        return Status::InvalidState;
    }
    Status status = CheckDevice(device);
    if (status != Status::Ready)
    {
        return status;
    }
    if (poolOrNull != nullptr && internal::RenderGraphPoolAccess::IsUsable(*poolOrNull) == false)
    {
        return Status::InvalidDescription;
    }
    StaticArray<format::Resource, format::MAX_RESOURCES> resolvedResources{};
    if (sourcePlan.Description.Version != format::VERSION || sourcePlan.Resources.IsValid() == false ||
        sourcePlan.Resources.GetSize() > format::MAX_RESOURCES)
    {
        return Status::InvalidDescription;
    }
    for (usize i = 0; i < sourcePlan.Resources.GetSize(); ++i)
    {
        status =
            ResolveRenderGraphResource(sourcePlan.Resources[i], referenceWidth, referenceHeight, resolvedResources[i]);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    RenderGraphPlan plan = sourcePlan;
    plan.Resources = {resolvedResources.GetData(), sourcePlan.Resources.GetSize()};
    status = ValidatePlan(plan);
    if (status != Status::Ready)
    {
        return status;
    }
    for (usize passIndex = 0; passIndex < plan.Passes.GetSize(); ++passIndex)
    {
        const format::Pass& pass = plan.Passes[passIndex];
        if ((pass.Buffer0 != format::ABSENT || pass.Buffer1 != format::ABSENT || pass.RayProgram != 0) &&
            programs.HasPersistentRayBuffers == false)
        {
            return Status::Unsupported;
        }
    }
    if (RequiresNativeRayPipeline(plan) == true)
    {
        if (programs.NativeRayStages.Artifact.HasNativeRayStages == false)
        {
            return Status::InvalidDescription;
        }
        rhi::RayCapabilities capabilities;
        const Status capabilityStatus = rhi::GetRayCapabilities(device, capabilities);
        if (capabilityStatus != Status::Ready || capabilities.RayPipelines == false)
        {
            return capabilityStatus == Status::Ready ? Status::Unsupported : capabilityStatus;
        }
    }
    bool hasNativeRayBinding = false;
    for (const rhi::RasterBinding& binding : programs.RayCompute.Bindings)
    {
        hasNativeRayBinding = hasNativeRayBinding == true || binding.Kind == rhi::RasterBindingKind::RayScene;
    }
    if (UsesRayTracing(plan) == true && RequiresHardwareRayTracing(plan) != hasNativeRayBinding)
    {
        return Status::InvalidDescription;
    }
    const StaticArray<const rhi::RasterShaderDescription*, format::FORWARD_STAGE_ROLE_COUNT> forwardStages{
        {&programs.ForwardVertex,
         &programs.ForwardFragment,
         &programs.ForwardShadow,
         &programs.LightingVertex,
         &programs.LightingFragment,
         &programs.LightingShadow},
    };
    const bool hasForwardProfile = programs.ForwardVertex.Inputs.empty() == false;
    for (const rhi::RasterShaderDescription* stage : forwardStages)
    {
        const bool hasArtifact = stage->Artifact.Wgsl.empty() == false || stage->Artifact.Msl.empty() == false ||
                                 stage->Artifact.Spirv.empty() == false || stage->Artifact.GlslEs.empty() == false;
        if (hasArtifact != hasForwardProfile)
        {
            return Status::InvalidDescription;
        }
    }
    const bool hasDepthArtifact =
        programs.ForwardDepth.Artifact.Wgsl.empty() == false || programs.ForwardDepth.Artifact.Msl.empty() == false ||
        programs.ForwardDepth.Artifact.Spirv.empty() == false || programs.ForwardDepth.Artifact.GlslEs.empty() == false;
    if (hasDepthArtifact != programs.HasClusteredForward ||
        (programs.HasClusteredForward == true && hasForwardProfile == false))
    {
        return Status::InvalidDescription;
    }
    const StaticArray<const rhi::RasterShaderDescription*, format::DEFERRED_STAGE_ROLE_COUNT> deferredStages{
        {&programs.DeferredGBufferFragment,
         &programs.DeferredLightingVertex,
         &programs.DeferredLightingFragment,
         &programs.DeferredDepthCopyFragment}};
    for (const rhi::RasterShaderDescription* stage : deferredStages)
    {
        const bool hasArtifact = stage->Artifact.Wgsl.empty() == false || stage->Artifact.Msl.empty() == false ||
                                 stage->Artifact.Spirv.empty() == false || stage->Artifact.GlslEs.empty() == false;
        if (hasArtifact != programs.HasClusteredDeferred)
        {
            return Status::InvalidDescription;
        }
    }
    for (usize index = 0; index < plan.Passes.GetSize(); ++index)
    {
        const format::Pass& pass = plan.Passes[index];
        // Raster display conversion is fused into the clustered deferred lighting resolve; no other raster
        // profile has a stage that could apply it.
        if (pass.Program != format::Technique::RayTracing && pass.ToneMappingExposure != 0 &&
            programs.HasClusteredDeferred == false)
        {
            return Status::Unsupported;
        }
    }
    if (programs.HasClusteredDeferred == true)
    {
        if (programs.HasClusteredForward == false)
        {
            return Status::InvalidDescription;
        }
        usize colorCount = 0;
        usize depthCount = 0;
        for (usize index = 0; index < plan.Passes.GetSize(); ++index)
        {
            const format::Pass& pass = plan.Passes[index];
            if (pass.Program == format::Technique::ColorAndDepth)
            {
                ++colorCount;
                if (pass.DepthStore != format::Store::Store || pass.ColorStore != format::Store::Store ||
                    pass.ColorLoad != format::Load::Clear)
                {
                    return Status::Unsupported;
                }
                if (pass.DepthLoad == format::Load::Load &&
                    (index == 0 || plan.Passes[index - 1].Program != format::Technique::DepthOnly ||
                     plan.Passes[index - 1].Depth != pass.Depth || plan.Passes[index - 1].Camera != pass.Camera))
                {
                    return Status::Unsupported;
                }
            }
            else if (pass.Program == format::Technique::DepthOnly)
            {
                ++depthCount;
                if (index + 1 >= plan.Passes.GetSize() ||
                    plan.Passes[index + 1].Program != format::Technique::ColorAndDepth ||
                    plan.Passes[index + 1].DepthLoad != format::Load::Load ||
                    plan.Passes[index + 1].Depth != pass.Depth || plan.Passes[index + 1].Camera != pass.Camera)
                {
                    return Status::Unsupported;
                }
            }
        }
        if (colorCount != 1 || depthCount > 1)
        {
            return Status::Unsupported;
        }
        rhi::DeviceInfo information;
        rhi::FormatCapabilities formatCapabilities;
        if (rhi::GetDeviceInfo(device, information) != rhi::DeviceStatus::Ready)
        {
            return Status::InvalidHandle;
        }
        const Status formatStatus =
            rhi::GetFormatCapabilities(device, rhi::RasterFormat::Rgba16Float, formatCapabilities);
        if (formatStatus != Status::Ready)
        {
            return formatStatus;
        }
        if (information.Profile.Interface.MaxColorAttachments < DEFERRED_TARGET_COUNT ||
            information.Profile.Interface.MaxBindings < DEFERRED_BINDING_COUNT ||
            information.Profile.Compute.MaxStorageBindings < 2 ||
            information.Profile.Compute.MaxStorageRange < sizeof(internal::LTC_TABLE) ||
            formatCapabilities.DepthSampled == false || formatCapabilities.AttachmentViews == false ||
            formatCapabilities.Uses.ColorAttachment == false)
        {
            return Status::Unsupported;
        }
    }
    if (hasForwardProfile == true)
    {
        for (usize pass = 0; pass < plan.Passes.GetSize(); ++pass)
        {
            if (format::IsImageTechnique(plan.Passes[pass].Program) == true)
            {
                continue;
            }
            if ((plan.Passes[pass].Program != format::Technique::ColorAndDepth &&
                 (programs.HasClusteredForward == false ||
                  plan.Passes[pass].Program != format::Technique::DepthOnly)) ||
                plan.Passes[pass].SceneData != format::SCENE_DATA_ALL)
            {
                return Status::Unsupported;
            }
        }
    }
    for (usize passIndex = 0; passIndex < plan.Passes.GetSize(); ++passIndex)
    {
        const format::Pass& pass = plan.Passes[passIndex];
        if (pass.Program == format::Technique::MetalFxSpatialUpscale ||
            pass.Program == format::Technique::MetalFxSpatialPerceptualUpscale)
        {
            rhi::SpatialUpscaleCapabilities capabilities;
            const Status capabilityStatus = rhi::GetSpatialUpscaleCapabilities(device, capabilities);
            if (capabilityStatus != Status::Ready)
            {
                return capabilityStatus;
            }
            if (capabilities.IsMetalFxSpatialSupported == false)
            {
                return Status::Unsupported;
            }
        }
        // Denoising is an optional build feature: reject it here, not later at scene preparation.
        if (pass.DenoisingQuality != 0 && internal::IsGpuDenoisingAvailable() == false)
        {
            return Status::Unsupported;
        }
    }
    StaticArray<bool, format::TECHNIQUE_COUNT> isUsed{};
    for (usize pass = 0; pass < plan.Passes.GetSize(); ++pass)
    {
        if (format::IsImageTechnique(plan.Passes[pass].Program) == false)
        {
            isUsed[static_cast<usize>(plan.Passes[pass].Program)] = true;
        }
    }
    if (programs.HasClusteredForward == true && isUsed[static_cast<usize>(format::Technique::ColorAndDepth)] == false)
    {
        return Status::Unsupported;
    }
    for (usize i = 0; i < format::TECHNIQUE_COUNT; ++i)
    {
        const format::Technique technique = static_cast<format::Technique>(i);
        if (isUsed[i] == true && HasFlatInterface(GetStage(programs, technique, rhi::ShaderStage::Vertex),
                                                  GetStage(programs, technique, rhi::ShaderStage::Fragment),
                                                  plan,
                                                  technique) == false)
        {
            return Status::Unsupported;
        }
    }
    // A depth-only technique shares its family's exact geometry stage, and masked coverage
    // is never the opaque void depth function: both are checked, not inferred from names.
    constexpr usize COLOR = static_cast<usize>(format::Technique::ColorAndDepth);
    constexpr usize DEPTH = static_cast<usize>(format::Technique::DepthOnly);
    constexpr usize MASKED_COLOR = static_cast<usize>(format::Technique::MaskedColorAndDepth);
    constexpr usize MASKED_DEPTH = static_cast<usize>(format::Technique::MaskedDepthOnly);
    if ((isUsed[COLOR] == true && isUsed[DEPTH] == true &&
         AreArtifactsEqual(programs.ColorVertex.Artifact, programs.DepthVertex.Artifact) == false) ||
        (isUsed[MASKED_COLOR] == true && isUsed[MASKED_DEPTH] == true &&
         AreArtifactsEqual(programs.MaskedColorVertex.Artifact, programs.MaskedDepthVertex.Artifact) == false) ||
        (isUsed[DEPTH] == true && isUsed[MASKED_DEPTH] == true &&
         AreArtifactsEqual(programs.DepthFragment.Artifact, programs.MaskedDepthFragment.Artifact) == true))
    {
        return Status::Unsupported;
    }
    rhi::DeviceInfo information;
    if (rhi::GetDeviceInfo(device, information) != rhi::DeviceStatus::Ready)
    {
        return Status::NotReady;
    }
    const format::ShaderTarget target = GetTarget(information.Startup.SelectedBackend);
    status = ValidateTargetUniforms(plan, target);
    if (status != Status::Ready)
    {
        return status;
    }
    for (usize i = 0; i < plan.Resources.GetSize(); ++i)
    {
        if (plan.Resources[i].IsExported == 1 && plan.Resources[i].Kind == format::Aspect::Depth &&
            information.Profile.Formats[static_cast<usize>(rhi::RasterFormat::Rgba8Unorm)].DepthSampled == false)
        {
            return Status::Unsupported;
        }
    }
    bool hasVisibility = false;
    for (usize index = 0; index < plan.Passes.GetSize(); ++index)
    {
        const format::Pass& pass = plan.Passes[index];
        // ValidatePlan pairs later sequence passes with the first pass's raster producer.
        if (pass.Visibility != format::ABSENT && plan.Passes[index - 1].Program != format::Technique::RayTracing)
        {
            hasVisibility = true;
            const format::Technique producer = plan.Passes[index - 1].Program;
            if (GetStage(programs, producer, rhi::ShaderStage::Vertex).Inputs.size() !=
                    format::VISIBILITY_INPUT_COUNT ||
                GetStage(programs, producer, rhi::ShaderStage::Vertex).Bindings.empty() == false ||
                GetStage(programs, producer, rhi::ShaderStage::Fragment).Bindings.empty() == false)
            {
                return Status::Unsupported;
            }
        }
    }
    if (hasVisibility != programs.HasVisibilityBuffer ||
        (hasVisibility == true && programs.HasSceneShaderInterface == false))
    {
        return Status::InvalidDescription;
    }
    // A graph ray kernel's failure summary is its last binding; the LTC table, when the programs declare it,
    // precedes it.
    const std::span<const rhi::RasterBinding> allRayBindings = programs.RayCompute.Bindings;
    const bool hasFailureBinding =
        allRayBindings.empty() == false && allRayBindings.back().Binding == LUDUS_GRAPH_FAILURE_BINDING;
    if (hasFailureBinding == true && (allRayBindings.back().Kind != rhi::RasterBindingKind::StorageReadWrite ||
                                      allRayBindings.back().MinSize != sizeof(uint32)))
    {
        return Status::InvalidDescription;
    }
    const std::span<const rhi::RasterBinding> rayBindings = allRayBindings.first(
        allRayBindings.size() - (hasFailureBinding == true ? LUDUS_GRAPH_FAILURE_BINDING_COUNT : 0));
    const bool hasLtcBinding =
        rayBindings.empty() == false && rayBindings.back().Binding == LUDUS_GRAPH_LTC_TABLE_BINDING;
    if (hasLtcBinding != programs.HasLtcTable ||
        (hasLtcBinding == true &&
         (programs.HasSceneShaderInterface == false || rayBindings.back().Kind != rhi::RasterBindingKind::StorageRead ||
          rayBindings.back().MinSize != LUDUS_GPU_PREVIEW_VECTOR_BYTES)))
    {
        return Status::InvalidDescription;
    }
    State* allocationOrNull = new (std::nothrow) State;
    core::UniquePtr<State> candidate{allocationOrNull};
    if (candidate.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    candidate->Device = device;
    candidate->Header = plan.Description;
    candidate->Target = target;
    candidate->Programs = programs;
    if (programs.HasRayPathCapture == true)
    {
        // Runtime-package stage metadata never names the diagnostic slot; generated headers reflect it.
        // The capture slot precedes the trailing LTC table and failure summary bindings, keeping slot order.
        const std::span<const rhi::RasterBinding> bindings = programs.RayCompute.Bindings;
        const usize kernelCount = bindings.size() - (hasLtcBinding == true ? LUDUS_GRAPH_LTC_TABLE_BINDING_COUNT : 0) -
                                  (hasFailureBinding == true ? LUDUS_GRAPH_FAILURE_BINDING_COUNT : 0);
        const bool hasCaptureBinding =
            kernelCount != 0 && bindings[kernelCount - 1].Binding == LUDUS_RAY_PATH_CAPTURE_BINDING;
        const usize count = bindings.size() + (hasCaptureBinding == true ? 0 : 1);
        if (isUsed[static_cast<usize>(format::Technique::RayTracing)] == false ||
            count > candidate->RayComputeBindings.GetSize())
        {
            return Status::InvalidDescription;
        }
        for (usize i = 0; i < kernelCount; ++i)
        {
            candidate->RayComputeBindings[i] = bindings[i];
        }
        usize next = kernelCount;
        if (hasCaptureBinding == false)
        {
            candidate->RayComputeBindings[next++] = {LUDUS_RAY_PATH_CAPTURE_BINDING,
                                                     rhi::RasterBindingKind::StorageReadWrite,
                                                     rhi::RasterVisibility::Compute,
                                                     LUDUS_RAY_PATH_CAPTURE_WORD_BYTES};
        }
        for (usize i = kernelCount; i < bindings.size(); ++i)
        {
            candidate->RayComputeBindings[next++] = bindings[i];
        }
        candidate->Programs.RayCompute.Bindings = {candidate->RayComputeBindings.GetData(), count};
    }
    if (plan.StorageBuffers.IsValid() == false || plan.StorageBuffers.GetSize() > format::MAX_STORAGE_BUFFERS ||
        (plan.StorageBuffers.IsEmpty() == false &&
         (isUsed[static_cast<usize>(format::Technique::RayTracing)] == false ||
          programs.HasPersistentRayBuffers == true)))
    {
        return Status::InvalidDescription;
    }
    candidate->StorageBufferCount = plan.StorageBuffers.GetSize();
    for (usize i = 0; i < candidate->StorageBufferCount; ++i)
    {
        candidate->StorageBuffers[i] = plan.StorageBuffers[i];
    }
    candidate->IsUsed = isUsed;
    if (programs.HasClusteredForward == true)
    {
        candidate->IsUsed[static_cast<usize>(format::Technique::DepthOnly)] = false;
    }
    candidate->PoolOrNull = poolOrNull;
    candidate->Allocation.IsPooled = poolOrNull != nullptr;
    for (usize i = 0; i < plan.Resources.GetSize(); ++i)
    {
        candidate->Resources[i] = plan.Resources[i];
    }
    for (usize i = 0; i < plan.Passes.GetSize(); ++i)
    {
        candidate->Passes[i] = plan.Passes[i];

        const BorrowedView<const char> name =
            plan.PassNames.IsEmpty() == true
                ? BorrowedView<const char>{"RenderGraph/Pass", sizeof("RenderGraph/Pass") - 1}
                : plan.PassNames[i];
        if (rhi::MakeGpuScope(i + 1, name, candidate->PassScopes[i]) == false)
        {
            return Status::InvalidDescription;
        }
    }
    for (usize i = 0; i < plan.UniformBindings.GetSize(); ++i)
    {
        candidate->UniformBindings[i] = plan.UniformBindings[i];
        candidate->BindingBytes[i] = GetBindingBytes(plan, i);
        candidate->IsBound[static_cast<usize>(plan.UniformBindings[i].Program)] = true;
    }
    for (usize i = 0; i < plan.UniformBlocks.GetSize(); ++i)
    {
        candidate->UniformBlocks[i] = plan.UniformBlocks[i];
    }
    for (usize i = 0; i < plan.UniformFields.GetSize(); ++i)
    {
        candidate->UniformFields[i] = plan.UniformFields[i];
    }
    for (usize i = 0; i < format::TECHNIQUE_COUNT; ++i)
    {
        if (candidate->IsUsed[i] == false)
        {
            continue;
        }
        const format::Technique technique = static_cast<format::Technique>(i);
        Limits limits;
        limits.Objects = static_cast<uint32>(format::MAX_ITEMS);
        limits.Meshes = static_cast<uint32>(
            (candidate->HasForwardProfile() == true ? format::MAX_ITEMS : format::MAX_MESHES) * MAX_RETAINED_SCENES);
        limits.Snapshots = static_cast<uint32>(format::MAX_PASSES * MAX_RETAINED_SCENES);
        limits.Views = static_cast<uint32>(format::MAX_PASSES * MAX_RETAINED_SCENES);
        if (candidate->HasForwardProfile() == true)
        {
            limits.Materials = static_cast<uint32>(format::MAX_ITEMS * MAX_RETAINED_SCENES);
        }
        limits.UniformSets = static_cast<uint32>(format::MAX_UNIFORM_SETS * MAX_RETAINED_SCENES);
        status = candidate->Contexts[i].Initialize(device,
                                                   GetStage(programs, technique, rhi::ShaderStage::Vertex),
                                                   GetStage(programs, technique, rhi::ShaderStage::Fragment),
                                                   limits);
        if (IsAccepted(status) == false)
        {
            return status;
        }
    }
    for (usize i = 0; i < candidate->Header.ResourceCount; ++i)
    {
        const format::Resource& resource = candidate->Resources[i];
        if (resource.Backing != i || resource.IsSurface == 1)
        {
            continue;
        }
        if (resource.Kind == format::Aspect::Buffer)
        {
            status = CreateZeroedGraphBuffer(device, resource.Width, candidate->Buffers[i]);
            if (IsAccepted(status) == false)
            {
                return status;
            }
            continue;
        }
        const rhi::TextureDescription description = MakeAttachmentDescription(resource);
        // Exports are never pooled: a consumer may retain views or bindings beyond the graph.
        const bool isExportBacking = IsExportBacking(plan.Resources, i);
        if (isExportBacking == true)
        {
            ++candidate->Allocation.ExportAllocations;
        }
        else
        {
            ++candidate->Allocation.TransientAllocations;
        }
        if (poolOrNull != nullptr && isExportBacking == false)
        {
            bool isReused = false;
            status =
                internal::RenderGraphPoolAccess::Acquire(*poolOrNull, description, candidate->Textures[i], isReused);
            if (IsAccepted(status) == false)
            {
                return status;
            }
            candidate->IsPooled[i] = true;
            if (isReused == true)
            {
                ++candidate->Allocation.ReusedAllocations;
            }
            else
            {
                ++candidate->Allocation.CreatedAllocations;
            }
            continue;
        }
        status = rhi::CreateTexture(device, description, {}, candidate->Textures[i]);
        if (IsAccepted(status) == false)
        {
            return status;
        }
        ++candidate->Allocation.CreatedAllocations;
    }
    mState.Swap(candidate);
    status = Poll();
    if (IsAccepted(status) == false)
    {
        Reset();
    }
    return status;
}
Status RenderGraph::Resize(uint32 referenceWidth, uint32 referenceHeight) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    if (state.HasOpenFrame == true || state.HasExternalRecording == true || HasUnconfirmedEncoding() == true)
    {
        return Status::InvalidState;
    }
    if (referenceWidth == 0 || referenceHeight == 0)
    {
        return Status::NotReady;
    }
    StaticArray<format::Resource, format::MAX_RESOURCES> resources{};
    bool hasChanges = false;
    bool isSameRequest = state.Resizing.Get() != nullptr;
    for (usize i = 0; i < state.Header.ResourceCount; ++i)
    {
        const Status resolved =
            ResolveRenderGraphResource(state.Resources[i], referenceWidth, referenceHeight, resources[i]);
        if (resolved != Status::Ready)
        {
            state.Pending.Reset();
            state.Resizing.Reset();
            return resolved;
        }
        hasChanges = hasChanges == true || resources[i].Width != state.Resources[i].Width ||
                     resources[i].Height != state.Resources[i].Height;
        isSameRequest = isSameRequest == true && resources[i].Width == state.Resizing->Resources[i].Width &&
                        resources[i].Height == state.Resizing->Resources[i].Height;
    }
    if (isSameRequest == true)
    {
        return Status::Pending;
    }
    if (hasChanges == false)
    {
        if (state.Resizing.Get() != nullptr)
        {
            state.Pending.Reset();
            state.Resizing.Reset();
        }
        return Status::Ready;
    }
    state.Pending.Reset();
    state.Resizing.Reset();
    State::AttachmentReplacement* allocationOrNull = new (std::nothrow) State::AttachmentReplacement;
    core::UniquePtr<State::AttachmentReplacement> candidate{allocationOrNull};
    if (candidate.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    candidate->Device = state.Device;
    candidate->PoolOrNull = state.PoolOrNull;
    candidate->Count = state.Header.ResourceCount;
    candidate->Resources = resources;
    const BorrowedView<const format::Resource> descriptions{resources.GetData(), state.Header.ResourceCount};
    for (usize i = 0; i < state.Header.ResourceCount; ++i)
    {
        if (resources[i].Kind == format::Aspect::Buffer)
        {
            if (resources[i].Width != state.Resources[i].Width)
            {
                const Status allocated =
                    CreateZeroedGraphBuffer(state.Device, resources[i].Width, candidate->Buffers[i]);
                if (IsAccepted(allocated) == false)
                {
                    return allocated;
                }
            }
            continue;
        }
        if (resources[i].Backing != i || resources[i].IsSurface == 1)
        {
            continue;
        }
        const rhi::TextureDescription description = MakeAttachmentDescription(resources[i]);
        const bool isExported = IsExportBacking(descriptions, i);
        Status allocated = Status::Ready;
        if (state.PoolOrNull != nullptr && isExported == false)
        {
            bool isReused = false;
            allocated = internal::RenderGraphPoolAccess::Acquire(*state.PoolOrNull,
                                                                 description,
                                                                 candidate->Textures[i],
                                                                 isReused);
            candidate->IsPooled[i] = IsAccepted(allocated);
            state.Allocation.ReusedAllocations += isReused == true ? 1 : 0;
            state.Allocation.CreatedAllocations += isReused == false && IsAccepted(allocated) == true ? 1 : 0;
        }
        else
        {
            allocated = rhi::CreateTexture(state.Device, description, {}, candidate->Textures[i]);
            state.Allocation.CreatedAllocations += IsAccepted(allocated) == true ? 1 : 0;
        }
        if (IsAccepted(allocated) == false)
        {
            return allocated;
        }
    }
    state.Resizing.Swap(candidate);
    return Status::Pending;
}
bool RenderGraph::HasPendingResize() const noexcept
{
    return mState.Get() != nullptr && mState->Resizing.Get() != nullptr;
}
bool RenderGraph::GetResourceDescription(uint32 resource, format::Resource& outResource) const noexcept
{
    if (mState.Get() == nullptr || resource >= mState->Header.ResourceCount)
    {
        return false;
    }
    outResource = mState->Resources[resource];
    return true;
}
Status RenderGraph::Poll() noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    if (state.HasExternalRecording == true)
    {
        return Status::InvalidState;
    }
    const Status admitted = CheckDevice(state.Device);
    if (admitted != Status::Ready || state.HasOpenFrame == true)
    {
        return admitted == Status::Ready ? Status::InvalidState : admitted;
    }
    for (usize i = 0; i < format::TECHNIQUE_COUNT; ++i)
    {
        if (state.IsUsed[i] == false)
        {
            continue;
        }
        Status status = state.Contexts[i].Poll();
        if (status != Status::Ready)
        {
            return status;
        }
        if (i == static_cast<usize>(format::Technique::RayTracing))
        {
            if (state.HasRaySetup == false)
            {
                status = state.Contexts[i].EnableTriangleRayPipeline(
                    state.Programs.RayCompute,
                    state.Programs.RayImageVertex,
                    state.Programs.RayImageFragment,
                    {RENDER_GRAPH_RAY_CPU_BYTES,
                     RENDER_GRAPH_RAY_GPU_BYTES,
                     1,
                     {state.StorageBuffers.GetData(), state.StorageBufferCount},
                     state.Programs.NativeRayStages.Artifact.HasNativeRayStages == true
                         ? &state.Programs.NativeRayStages
                         : nullptr});
                if (IsAccepted(status) == false)
                {
                    return status;
                }
                state.HasRaySetup = true;
            }
            status = state.Contexts[i].PollGpuPreview();
            if (status != Status::Ready)
            {
                return status;
            }
            continue;
        }
        if (state.HasViews[i] == false)
        {
            status =
                state.Contexts[i].InitializeViews(state.Programs.CompositeVertex, state.Programs.CompositeFragment);
            if (IsAccepted(status) == false)
            {
                return status;
            }
            state.HasViews[i] = true;
        }
        status = state.Contexts[i].PollViews();
        if (status != Status::Ready)
        {
            return status;
        }
    }
    if (state.HasForwardProfile() == true)
    {
        constexpr usize COLOR_CONTEXT = static_cast<usize>(format::Technique::ColorAndDepth);
        Renderer& context = state.Contexts[COLOR_CONTEXT];
        if (state.HasForwardLighting == false)
        {
            const Status status = context.InitializeLighting(state.Programs.LightingVertex,
                                                             state.Programs.LightingFragment,
                                                             state.Programs.LightingShadow);
            if (IsAccepted(status) == false)
            {
                return status;
            }
            state.HasForwardLighting = true;
        }
        Status status = context.PollLighting();
        if (status != Status::Ready)
        {
            return status;
        }
        if (state.HasForwardProgram == false)
        {
            status = context.prewarmProgram(
                state.Programs.ForwardVertex,
                state.Programs.ForwardFragment,
                &state.Programs.ForwardShadow,
                state.ForwardProgram,
                state.Programs.HasClusteredForward == true ? &state.Programs.ForwardDepth : nullptr,
                state.Programs.HasClusteredDeferred == true ? &state.Programs.DeferredGBufferFragment : nullptr);
            if (IsAccepted(status) == false)
            {
                return status;
            }
            state.HasForwardProgram = true;
        }
        status = context.GetStatus(state.ForwardProgram);
        if (status != Status::Ready)
        {
            return status;
        }
        if (state.HasForwardResources == false)
        {
            status = state.ForwardResources.Initialize(state.Device,
                                                       context,
                                                       state.ForwardProgram,
                                                       state.Programs.HasClusteredForward,
                                                       state.Programs.HasClusteredDeferred);
            if (status != Status::Ready)
            {
                return status;
            }
            state.HasForwardResources = true;
        }
    }
    for (usize i = 0; i < state.Header.ResourceCount; ++i)
    {
        if (state.Resources[i].Backing != i || state.Resources[i].IsSurface == 1)
        {
            continue;
        }
        const Status ready = state.Resources[i].Kind == format::Aspect::Buffer
                                 ? rhi::GetStatus(state.Device, state.Buffers[i])
                                 : rhi::GetStatus(state.Device, state.Textures[i]);
        if (ready != Status::Ready)
        {
            return ready;
        }
    }
    if (state.Programs.HasClusteredDeferred == true)
    {
        if (state.HasDeferredProgram == false)
        {
            const Status status = state.DeferredProgram.Initialize(state.Device,
                                                                   state.Programs.DeferredLightingVertex,
                                                                   state.Programs.DeferredLightingFragment,
                                                                   state.Programs.DeferredDepthCopyFragment);
            if (IsAccepted(status) == false)
            {
                return status;
            }
            state.HasDeferredProgram = true;
        }
        const Status status = state.DeferredProgram.Poll();
        if (status != Status::Ready)
        {
            return status;
        }
    }
    state.IsReady = true;
    if (state.PendingCamera.Get() != nullptr)
    {
        return state.PollCameras();
    }
    if (state.Pending.Get() != nullptr)
    {
        const Status ready = state.PollScene(*state.Pending);
        if (ready == Status::Ready)
        {
            state.PublishResize();
            state.Current.Swap(state.Pending);
            state.Pending.Reset();
            if (state.Report.IsAccepted == true && state.Report.SceneRevision == state.Current->Revision)
            {
                state.Report.IsReady = true;
            }
        }
        else if (ready != Status::Pending)
        {
            if (state.Report.IsAccepted == true && state.Report.SceneRevision == state.Pending->Revision)
            {
                state.Report.IsReady = false;
            }
            state.Pending.Reset();
            state.Resizing.Reset();
        }
        return ready;
    }
    return Status::Ready;
}
Status RenderGraph::Update(BorrowedView<const RenderGraphMesh> meshes,
                           BorrowedView<const RenderGraphItem> items,
                           BorrowedView<const ViewDescription> cameras,
                           uint64 sceneRevision,
                           RenderGraphCoveragePolicy policy,
                           RenderGraphReport& outReport,
                           const SceneShaderInputs* sceneInputsOrNull,
                           uint64 contentRevision,
                           const RenderGraphSceneRevisions& revisions) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    if (state.HasExternalRecording == true)
    {
        return Status::InvalidState;
    }
    const Status admitted = CheckDevice(state.Device);
    if (admitted != Status::Ready || state.HasOpenFrame == true || state.IsReady == false)
    {
        return admitted != Status::Ready ? admitted : Status::NotReady;
    }
    // A capture graph traces one extraction: a replacement would record samples [0, N) again.
    if (state.Programs.HasRayPathCapture == true && (state.RayPathCapture.IsRequested() == false ||
                                                     state.Current.Get() != nullptr || state.Pending.Get() != nullptr))
    {
        return Status::InvalidState;
    }
    if (meshes.IsValid() == false || items.IsValid() == false || cameras.IsValid() == false ||
        meshes.GetSize() > format::MAX_MESHES || items.GetSize() > format::MAX_ITEMS ||
        cameras.GetSize() != state.Header.CameraCount || sceneRevision == 0 ||
        (policy != RenderGraphCoveragePolicy::RejectIncomplete && policy != RenderGraphCoveragePolicy::DiagnosticColor))
    {
        return Status::InvalidDescription;
    }
    for (usize cameraIndex = 0; cameraIndex < cameras.GetSize(); ++cameraIndex)
    {
        const ViewDescription& camera = cameras[cameraIndex];
        if (camera.Depth != DepthConvention::Conventional)
        {
            return Status::Unsupported;
        }
    }
    const bool hasRevisions = revisions.Geometry != 0 && revisions.Instances != 0 && revisions.Materials != 0 &&
                              revisions.Lights != 0 && revisions.Environment != 0;
    if (hasRevisions == false && revisions != RenderGraphSceneRevisions{})
    {
        return Status::InvalidDescription;
    }
    const bool canReuse =
        state.Current.Get() != nullptr && state.Current->CoveragePolicy == policy &&
        (hasRevisions == true ? state.Current->Revisions == revisions
                              : contentRevision != 0 && state.Current->ContentRevision == contentRevision);
    if (canReuse == true && state.Resizing.Get() == nullptr && state.Pending.Get() == nullptr)
    {
        if (state.PendingCamera.Get() != nullptr)
        {
            return Status::NotReady;
        }
        const Status prepared = state.PrepareCameras(cameras, sceneRevision);
        if (prepared != Status::IdentityExhausted || meshes.IsEmpty() == true)
        {
            if (IsAccepted(prepared) == true)
            {
                state.Report.SceneRevision = sceneRevision;
                state.Report.IsAccepted = true;
                state.Report.IsReady = false;
            }
            outReport = state.Report;
            return prepared;
        }
    }
    for (usize itemIndex = 0; itemIndex < items.GetSize(); ++itemIndex)
    {
        const RenderGraphItem& item = items[itemIndex];
        if (item.SourceId == 0 || item.MeshIndex >= meshes.GetSize() || item.Uniforms.IsValid() == false)
        {
            return Status::InvalidDescription;
        }
        // Every drawn item supplies the complete logical table; an excluded item may omit it.
        const bool hasValues = item.Uniforms.IsEmpty() == false;
        if ((hasValues == true && item.Uniforms.GetSize() != state.Header.UniformValueCount) ||
            (hasValues == false && state.Header.UniformValueCount != 0 && item.IsIntentionallyExcluded == false))
        {
            return Status::InvalidDescription;
        }
        for (usize value = 0; value < item.Uniforms.GetSize(); ++value)
        {
            if (math::IsFinite(item.Uniforms[value]) == false)
            {
                return Status::InvalidDescription;
            }
        }
    }
    state.CoverageCount = 0;
    state.Report = {};
    state.Report.SceneRevision = sceneRevision;
    for (usize camera = 0; camera < cameras.GetSize(); ++camera)
    {
        for (usize role = 0; role < ROLE_COUNT; ++role)
        {
            usize routeCount = 0;
            for (usize pass = 0; pass < state.Header.PassCount; ++pass)
            {
                const format::Pass& operation = state.Passes[pass];
                routeCount += format::IsImageTechnique(operation.Program) == false && operation.Camera == camera &&
                                      ((role == 1 && operation.Program != format::Technique::RayTracing) ||
                                       (role == 0 && IsColorTechnique(operation.Program) == true))
                                  ? 1U
                                  : 0U;
            }
            if (routeCount == 0)
            {
                continue;
            }
            for (usize i = 0; i < items.GetSize(); ++i)
            {
                const RenderGraphItem& item = items[i];
                const RenderGraphMesh& mesh = meshes[item.MeshIndex];
                // Only routes of the item's own material family count; another family's route
                // is neither coverage nor a conflicting repetition.
                usize compatibleCount = 0;
                bool isRepeatedAllowed = true;
                for (usize pass = 0; pass < state.Header.PassCount; ++pass)
                {
                    const format::Pass& operation = state.Passes[pass];
                    if (operation.Camera == camera &&
                        ((role == 1 && operation.Program != format::Technique::RayTracing) ||
                         (role == 0 && IsColorTechnique(operation.Program) == true)) &&
                        IsCompatible(operation.Program, item, mesh) == true)
                    {
                        ++compatibleCount;
                        isRepeatedAllowed =
                            isRepeatedAllowed == true && (operation.Program == format::Technique::RayTracing ||
                                                          operation.AllowsRepeatedCoverage == 1);
                    }
                }
                RenderGraphCoverageRecord record;
                record.SourceId = item.SourceId;
                record.Camera = static_cast<uint32>(camera);
                record.Role = role == 0 ? format::Aspect::Color : format::Aspect::Depth;
                bool hasDuplicate = false;
                for (usize other = 0; other < items.GetSize(); ++other)
                {
                    hasDuplicate = hasDuplicate == true || (other != i && items[other].SourceId == item.SourceId);
                }
                if (item.IsIntentionallyExcluded == true)
                {
                    record.Status = RenderGraphCoverage::IntentionallyExcluded;
                    ++state.Report.Excluded;
                }
                else if (hasDuplicate == true || (compatibleCount > 1 && isRepeatedAllowed == false))
                {
                    record.Status = RenderGraphCoverage::Conflicting;
                    record.Reason = RenderGraphReason::Duplicate;
                    ++state.Report.Conflicting;
                }
                else if (compatibleCount == 0)
                {
                    record.Status = RenderGraphCoverage::Unsupported;
                    record.Reason = mesh.Layout != RenderGraphLayout::Static ? RenderGraphReason::Layout
                                                                             : RenderGraphReason::Material;
                    record.HasDiagnosticRoute =
                        role == 0 && policy == RenderGraphCoveragePolicy::DiagnosticColor &&
                        state.HasDiagnosticContext() == true &&
                        (mesh.Layout == RenderGraphLayout::Static || mesh.HasDiagnosticBounds == true);
                    ++state.Report.Unsupported;
                }
                else
                {
                    record.Status = RenderGraphCoverage::Covered;
                    ++state.Report.Covered;
                }
                state.Coverage[state.CoverageCount++] = record;
            }
        }
    }
    outReport = state.Report;
    if (state.Report.Conflicting != 0 ||
        (state.Report.Unsupported != 0 && policy == RenderGraphCoveragePolicy::RejectIncomplete))
    {
        return Status::Unsupported;
    }
    State::Scene* allocationOrNull = new (std::nothrow) State::Scene;
    core::UniquePtr<State::Scene> candidate{allocationOrNull};
    if (candidate.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    candidate->OwnerOrNull = &state;
    candidate->MeshCount = meshes.GetSize();
    candidate->Revision = sceneRevision;
    candidate->ContentRevision = contentRevision;
    candidate->Revisions = revisions;
    candidate->CoveragePolicy = policy;
    for (usize camera = 0; camera < cameras.GetSize(); ++camera)
    {
        candidate->Cameras[camera] = cameras[camera];
        candidate->Cameras[camera].OverlayToClip = cameras[camera].WorldToClip;
    }
    const auto drawsItem = [&state](const RenderGraphItem& item, const RenderGraphMesh& mesh) noexcept {
        bool isDrawn = false;
        for (usize pass = 0; pass < state.Header.PassCount; ++pass)
        {
            isDrawn = isDrawn == true || IsCompatible(state.Passes[pass].Program, item, mesh) == true;
        }
        return isDrawn;
    };
    const auto hasColorRoute = [&state](const RenderGraphItem& item,
                                        const RenderGraphMesh& mesh,
                                        uint32 camera) noexcept {
        bool hasRoute = false;
        for (usize pass = 0; pass < state.Header.PassCount; ++pass)
        {
            const format::Pass& operation = state.Passes[pass];
            hasRoute = hasRoute == true || (operation.Camera == camera && IsColorTechnique(operation.Program) == true &&
                                            IsCompatible(operation.Program, item, mesh) == true);
        }
        return hasRoute;
    };
    const bool hasDiagnostic =
        policy == RenderGraphCoveragePolicy::DiagnosticColor && state.HasDiagnosticContext() == true;
    if (state.HasForwardProfile() == true)
    {
        bool hasMaskedMaterials = false;
        if (sceneInputsOrNull != nullptr)
        {
            if (sceneInputsOrNull->Materials.IsValid() == false ||
                sceneInputsOrNull->Materials.GetSize() > format::MAX_ITEMS)
            {
                return Status::InvalidDescription;
            }
            for (usize materialIndex = 0; materialIndex < sceneInputsOrNull->Materials.GetSize(); ++materialIndex)
            {
                hasMaskedMaterials =
                    hasMaskedMaterials == true || sceneInputsOrNull->Materials[materialIndex].IsAlphaMasked == true;
            }
        }
        for (usize pass = 0; pass < state.Header.PassCount; ++pass)
        {
            if ((state.Passes[pass].Program != format::Technique::ColorAndDepth &&
                 (state.Programs.HasClusteredForward == false ||
                  state.Passes[pass].Program != format::Technique::DepthOnly)) ||
                state.Passes[pass].SceneData != format::SCENE_DATA_ALL)
            {
                return Status::Unsupported;
            }
            // The authored opaque void prepass has no texture bindings. Reject its
            // mask combination before publication instead of sealing cut-out holes.
            if (state.Passes[pass].Program == format::Technique::DepthOnly && hasMaskedMaterials == true)
            {
                return Status::Unsupported;
            }
        }
        RenderGraphSceneDescription source;
        source.Meshes = meshes;
        source.Items = items;
        source.Inputs = sceneInputsOrNull != nullptr ? *sceneInputsOrNull : SceneShaderInputs{};
        source.HasMaterialInterface = sceneInputsOrNull != nullptr;
        source.SourceGeneration = sceneRevision;
        const Status prepared = state.ForwardResources.Prepare(source, candidate->ForwardGeneration);
        if (IsAccepted(prepared) == false)
        {
            return prepared;
        }
    }
    for (usize meshIndex = 0; state.HasForwardProfile() == false && meshIndex < meshes.GetSize(); ++meshIndex)
    {
        const RenderGraphMesh& mesh = meshes[meshIndex];
        bool isRequired = false;
        for (usize itemIndex = 0; itemIndex < items.GetSize(); ++itemIndex)
        {
            const RenderGraphItem& item = items[itemIndex];
            const bool isDiagnostic =
                hasDiagnostic == true && (mesh.Layout == RenderGraphLayout::Static || mesh.HasDiagnosticBounds == true);
            isRequired = isRequired == true || (item.MeshIndex == meshIndex && item.IsIntentionallyExcluded == false &&
                                                (drawsItem(item, mesh) == true || isDiagnostic == true));
        }
        if (isRequired == false)
        {
            continue;
        }

        StaticArray<math::Vector3, BOX_CORNER_COUNT> corners{};
        const StaticArray<uint32, BOX_INDEX_COUNT> indices{{0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                                                            2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5}};
        MeshDescription description = mesh.Geometry;
        if (mesh.Layout != RenderGraphLayout::Static)
        {
            if (mesh.HasDiagnosticBounds == false || hasDiagnostic == false)
            {
                continue;
            }
            if (math::IsFinite(mesh.BoundsMesh) == false || mesh.BoundsMesh.Min.X > mesh.BoundsMesh.Max.X ||
                mesh.BoundsMesh.Min.Y > mesh.BoundsMesh.Max.Y || mesh.BoundsMesh.Min.Z > mesh.BoundsMesh.Max.Z)
            {
                return Status::InvalidDescription;
            }
            for (usize corner = 0; corner < BOX_CORNER_COUNT; ++corner)
            {
                corners[corner] = {(corner & 1) == 0 ? mesh.BoundsMesh.Min.X : mesh.BoundsMesh.Max.X,
                                   (corner & 2) == 0 ? mesh.BoundsMesh.Min.Y : mesh.BoundsMesh.Max.Y,
                                   (corner & 4) == 0 ? mesh.BoundsMesh.Min.Z : mesh.BoundsMesh.Max.Z};
            }
            description = {};
            description.Positions = corners.GetData();
            description.VertexCount = corners.GetSize();
            description.Indices = indices.GetData();
            description.IndexCount = indices.GetSize();
        }
        candidate->HasMeshes[meshIndex] = true;
        for (usize context = 0; context < format::TECHNIQUE_COUNT; ++context)
        {
            if (state.IsUsed[context] == false || context == static_cast<usize>(format::Technique::RayTracing))
            {
                continue;
            }
            const Status created =
                state.Contexts[context].CreateMesh(description, candidate->Meshes[context][meshIndex]);
            if (IsAccepted(created) == false)
            {
                return created;
            }
        }
    }
    // Pack and deduplicate per-draw values for every bound technique. Each distinct value
    // set becomes one immutable uniform set; equal values share it and stay instanced.
    for (usize context = 0; context < format::TECHNIQUE_COUNT; ++context)
    {
        StaticArray<usize, format::MAX_ITEMS>& itemSets = candidate->ItemSets[context];
        for (usize itemIndex = 0; itemIndex < items.GetSize(); ++itemIndex)
        {
            itemSets[itemIndex] = NO_SET;
        }
        if (state.IsUsed[context] == false || state.IsBound[context] == false)
        {
            continue;
        }
        const format::Technique technique = static_cast<format::Technique>(context);
        for (usize itemIndex = 0; itemIndex < items.GetSize(); ++itemIndex)
        {
            const RenderGraphItem& item = items[itemIndex];
            if (item.IsIntentionallyExcluded == true || IsCompatible(technique, item, meshes[item.MeshIndex]) == false)
            {
                continue;
            }
            const usize size = state.PackUniforms(technique, item.Uniforms);
            usize& count = candidate->UniformSetCounts[context];
            usize match = NO_SET;
            for (usize set = 0; set < count && match == NO_SET; ++set)
            {
                bool isEqual = true;
                for (usize byte = 0; byte < size && isEqual == true; ++byte)
                {
                    isEqual = state.Packed[set][byte] == state.Candidate[byte];
                }
                match = isEqual == true ? set : NO_SET;
            }
            if (match == NO_SET)
            {
                if (count == format::MAX_UNIFORM_SETS)
                {
                    return Status::CapacityExceeded;
                }
                const Status created = state.Contexts[context].CreateUniformSet({state.Candidate.GetData(), size},
                                                                                candidate->UniformSets[context][count]);
                if (IsAccepted(created) == false)
                {
                    return created;
                }
                for (usize byte = 0; byte < size; ++byte)
                {
                    state.Packed[count][byte] = state.Candidate[byte];
                }
                match = count++;
            }
            itemSets[itemIndex] = match;
        }
    }
    for (usize pass = 0; state.HasForwardProfile() == false && pass < state.Header.PassCount; ++pass)
    {
        const format::Pass& operation = state.Passes[pass];
        const usize context = static_cast<usize>(operation.Program);
        for (usize itemIndex = 0; itemIndex < items.GetSize(); ++itemIndex)
        {
            const RenderGraphItem& item = items[itemIndex];
            const RenderGraphMesh& mesh = meshes[item.MeshIndex];
            if (item.IsIntentionallyExcluded == true)
            {
                continue;
            }
            const bool isCompatible = IsCompatible(operation.Program, item, mesh);
            const bool isDiagnostic = isCompatible == false && hasDiagnostic == true &&
                                      operation.Program == format::Technique::ColorAndDepth &&
                                      candidate->HasMeshes[item.MeshIndex] == true &&
                                      hasColorRoute(item, mesh, operation.Camera) == false;
            if (isCompatible == false && isDiagnostic == false)
            {
                continue;
            }
            SceneItem drawing;
            drawing.SourceId = item.SourceId;
            drawing.DrawIndex = item.MeshIndex;
            drawing.InstanceIndex = static_cast<uint32>(itemIndex);
            drawing.Geometry = candidate->Meshes[context][item.MeshIndex];
            drawing.Transform = item.MeshToWorld;
            drawing.Material = item.Material;
            if (candidate->ItemSets[context][itemIndex] != NO_SET)
            {
                drawing.Uniforms = candidate->UniformSets[context][candidate->ItemSets[context][itemIndex]];
            }
            if (isCompatible == false)
            {
                // Existing authored flat color is runtime data, not a new shader.
                // Overlay pipeline is prewarmed and cannot establish required depth.
                drawing.Material.Color = {1, 0, 1, 1};
                drawing.Pass = Layer::Overlay;
                // OverlayToClip is the extracted camera matrix in this graph
                // profile, so world-space diagnostic geometry follows navigation.
            }
            candidate->Items[pass][candidate->Counts[pass]++] = drawing;
        }
    }
    const StaticArray<format::Resource, format::MAX_RESOURCES>& preparingResources =
        state.Resizing.Get() != nullptr ? state.Resizing->Resources : state.Resources;
    RenderGraphSceneDescription rayScene{meshes,
                                         items,
                                         sceneInputsOrNull != nullptr && state.Programs.HasSceneShaderInterface == true
                                             ? *sceneInputsOrNull
                                             : SceneShaderInputs{},
                                         state.Programs.HasSceneShaderInterface,
                                         policy == RenderGraphCoveragePolicy::DiagnosticColor,
                                         &state.Report.SceneDiagnostics,
                                         sceneRevision};
    for (usize pass = 0; pass < state.Header.PassCount; ++pass)
    {
        if (state.Passes[pass].Program != format::Technique::RayTracing)
        {
            continue;
        }
        const format::Pass& operation = state.Passes[pass];
        rayScene.SceneData = operation.SceneData;
        StaticArray<GpuPreviewGraphBuffer, format::MAX_RAY_BUFFERS> buffers{};
        const StaticArray<uint32, format::MAX_RAY_BUFFERS> indices{{operation.Buffer0, operation.Buffer1}};
        for (usize slot = 0; slot < indices.GetSize(); ++slot)
        {
            if (indices[slot] != format::ABSENT)
            {
                const uint32 index = indices[slot];
                const rhi::BufferHandle buffer =
                    state.Resizing.Get() != nullptr && state.Resizing->Buffers[index] != rhi::BufferHandle{}
                        ? state.Resizing->Buffers[index]
                        : state.Buffers[index];
                buffers[slot] = {buffer, preparingResources[index].Width};
            }
        }
        // Ray passes reading the same scene data of this update share the first one's uploaded
        // scene, so a graph stores and charges each distinct packed scene once, not once per pass.
        GpuPreview sceneSource{};
        bool hasSceneSource = false;
        for (usize earlier = 0; earlier < pass && hasSceneSource == false; ++earlier)
        {
            if (state.Passes[earlier].Program == format::Technique::RayTracing &&
                candidate->HasViews[earlier] == true && state.Passes[earlier].SceneData == operation.SceneData)
            {
                sceneSource = candidate->RayPasses[earlier].GetJob();
                hasSceneSource = true;
            }
        }
        GpuPreviewGraphInvocation invocation{
            operation.RayProgram,
            state.Programs.HasPersistentRayBuffers == true
                ? BorrowedView<const GpuPreviewGraphBuffer>{buffers.GetData(), buffers.GetSize()}
                : BorrowedView<const GpuPreviewGraphBuffer>{},
            state.GetAccumulationSamples(operation),
            operation.ToneMappingExposure,
            operation.DenoisingQuality,
            state.RayPathCapture.GetBuffer(),
            operation.Traversal == format::RayTraversal::HardwarePipeline,
            hasSceneSource == true ? &sceneSource : nullptr};
        invocation.ToneMappingCurve = GetToneCurve(operation);
        const Status created = candidate->RayPasses[pass].Initialize(state.GetContext(operation.Program),
                                                                     {},
                                                                     cameras[operation.Camera],
                                                                     preparingResources[operation.Color].Width,
                                                                     preparingResources[operation.Color].Height,
                                                                     &rayScene,
                                                                     &invocation);
        if (IsAccepted(created) == false)
        {
            outReport = state.Report;
            return created;
        }
        candidate->HasViews[pass] = true;
    }
    const Status prepared = state.PollScene(*candidate);
    if (IsAccepted(prepared) == false)
    {
        return prepared;
    }
    state.PendingCamera.Reset();
    state.Report.IsAccepted = true;
    state.Report.IsReady = prepared == Status::Ready;
    if (prepared == Status::Ready)
    {
        state.PublishResize();
        state.Current.Swap(candidate);
        state.Pending.Reset();
    }
    else
    {
        state.Pending.Swap(candidate);
    }
    outReport = state.Report;
    return prepared;
}
RenderGraphAllocationReport RenderGraph::GetAllocationReport() const noexcept
{
    if (mState.Get() == nullptr)
    {
        return {};
    }
    RenderGraphAllocationReport report = mState->Allocation;
    for (const State::Scene* sceneOrNull : {mState->Current.Get(), mState->Pending.Get()})
    {
        if (sceneOrNull != nullptr && sceneOrNull->DeferredAttachmentBytes != 0)
        {
            report.DeferredAllocations += DEFERRED_TARGET_COUNT;
            report.DeferredAttachmentBytes += sceneOrNull->DeferredAttachmentBytes;
        }
    }
    return report;
}
BorrowedView<const RenderGraphCoverageRecord> RenderGraph::GetCoverage() const noexcept
{
    if (mState.Get() == nullptr)
    {
        return {};
    }
    return {mState->Coverage.GetData(), mState->CoverageCount};
}
RenderGraphReport RenderGraph::GetReport() const noexcept
{
    RenderGraphReport report = mState.Get() == nullptr ? RenderGraphReport{} : mState->Report;
    if (mState.Get() != nullptr)
    {
        report.Resources = mState->ForwardResources.GetReport();
    }
    return report;
}
uint64 RenderGraph::GetDrawableSceneRevision() const noexcept
{
    return mState.Get() == nullptr || mState->Current.Get() == nullptr ? 0 : mState->Current->Revision;
}
bool RenderGraph::HasPendingSamples() const noexcept
{
    if (mState.Get() == nullptr || mState->Current.Get() == nullptr || mState->HasRaySetup == false)
    {
        return false;
    }
    const State& state = *mState;
    for (usize pass = 0; pass < state.Header.PassCount; ++pass)
    {
        const format::Pass& operation = state.Passes[pass];
        bool hasLaterWriter = false;
        for (usize next = pass + 1; next < state.Header.PassCount; ++next)
        {
            hasLaterWriter = hasLaterWriter == true || (state.Passes[next].Program == format::Technique::RayTracing &&
                                                        state.Passes[next].Color == operation.Color);
        }
        const uint32 limit = state.GetAccumulationSamples(operation);
        if (hasLaterWriter == false && operation.Program == format::Technique::RayTracing &&
            (limit == 0 || state.Current->SubmittedRaySamples < limit))
        {
            return true;
        }
    }
    return false;
}
uint64 RenderGraph::GetOutputSceneRevision() const noexcept
{
    if (mState.Get() == nullptr || mState->HasValidOutput == false || mState->Producer.IsNull() == true ||
        rhi::GetStatus(mState->Device, mState->Producer) != Status::Ready)
    {
        return 0;
    }
    return mState->OutputRevision;
}
Status RenderGraph::PrepareEncoding() noexcept
{
    if (mState.Get() == nullptr || mState->Current.Get() == nullptr || mState->IsReady == false)
    {
        return Status::NotReady;
    }
    State& state = *mState;
    const Status admitted = CheckDevice(state.Device);
    if (admitted != Status::Ready || state.HasExternalRecording == true)
    {
        return admitted == Status::Ready ? Status::InvalidState : admitted;
    }
    if (state.Producer.IsNull() == false)
    {
        const Status completed = rhi::GetStatus(state.Device, state.Producer);
        if (completed != Status::Ready)
        {
            return completed == Status::Pending ? Status::NotReady : completed;
        }
    }
    if (state.Current->IsRayGraphPrepared == true)
    {
        return Status::Ready;
    }
    StaticArray<GpuPreview, format::MAX_RAY_PASSES> jobs{};
    StaticArray<rhi::TextureHandle, format::MAX_RAY_PASSES> targets{};
    usize count = 0;
    VisibilityRasterPass visibility;
    const VisibilityRasterPass* visibilityOrNull = nullptr;
    for (usize pass = 0; pass < state.Header.PassCount; ++pass)
    {
        if (state.Passes[pass].Program != format::Technique::RayTracing)
        {
            continue;
        }
        const format::Pass& operation = state.Passes[pass];
        // The raster producer precedes the first pass; later passes share its image.
        if (operation.Visibility != format::ABSENT && visibilityOrNull == nullptr)
        {
            const format::Pass& producer = state.Passes[pass - 1];
            visibility.ProducerOrNull = &state.GetContext(producer.Program);
            visibility.View = state.Current->Views[pass - 1];
            visibility.Attachment.Color = state.Textures[operation.Visibility];
            visibility.Attachment.ColorLoad = static_cast<rhi::RasterLoad>(producer.ColorLoad);
            visibility.Attachment.ColorStore = static_cast<rhi::RasterStore>(producer.ColorStore);
            visibility.Attachment.DepthLoad = static_cast<rhi::RasterLoad>(producer.DepthLoad);
            visibility.Attachment.DepthStore = static_cast<rhi::RasterStore>(producer.DepthStore);
            visibilityOrNull = &visibility;
        }
        // Every pass dispatches the same band in each frame, from a uniform copy prepared for that band.
        const Status tiles =
            state.Current->RayPasses[pass].PrepareTiles(state.RayTileCount, state.Current->NextRayTile);
        if (tiles != Status::Ready)
        {
            return tiles == Status::Pending ? Status::NotReady : tiles;
        }
        jobs[count] = state.Current->RayPasses[pass].GetJob();
        targets[count++] = state.Textures[state.Passes[pass].Color];
    }
    if (count != 0)
    {
        static_cast<void>(rhi::DiscardOrderedGraph(state.Device, state.Current->RayGraph));
        const Status prepared =
            state.Contexts[static_cast<usize>(format::Technique::RayTracing)].prepareGpuPreviewGraphSequence(
                {jobs.GetData(), count},
                {targets.GetData(), count},
                state.Current->NextRayTile,
                state.Current->RayGraph,
                visibilityOrNull);
        if (prepared != Status::Ready)
        {
            return prepared;
        }
        state.Current->IsRayGraphPrepared = true;
    }
    return Status::Ready;
}
Status RenderGraph::Render(rhi::SurfaceHandle surface,
                           uint32 surfaceWidth,
                           uint32 surfaceHeight,
                           rhi::SubmissionToken& outCompletion) noexcept
{
    if (mState.Get() == nullptr || outCompletion.IsNull() == false)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    if (state.HasExternalRecording == true)
    {
        return Status::InvalidState;
    }
    if (state.HasOpenFrame == true)
    {
        return state.FinishFrame(outCompletion);
    }
    const Status admitted = CheckDevice(state.Device);
    if (admitted != Status::Ready)
    {
        return admitted;
    }
    if (state.IsReady == false || state.Current.Get() == nullptr)
    {
        return Status::NotReady;
    }
    if (state.Producer.IsNull() == false)
    {
        const Status completed = rhi::GetStatus(state.Device, state.Producer);
        if (completed != Status::Ready)
        {
            return completed == Status::Pending ? Status::NotReady : completed;
        }
    }
    for (usize resource = 0; resource < state.Header.ResourceCount; ++resource)
    {
        const format::Resource& description = state.Resources[resource];
        if (description.IsSurface == 1 && (description.Width != surfaceWidth || description.Height != surfaceHeight))
        {
            return Status::NotReady;
        }
    }
    const Status prepared = PrepareEncoding();
    if (prepared != Status::Ready)
    {
        return prepared;
    }
    const rhi::DeviceStatus started = rhi::BeginFrame(state.Device, surface);
    if (started != rhi::DeviceStatus::Ready)
    {
        return MapDeviceStatus(started);
    }
    state.HasOpenFrame = true;
    static_cast<void>(state.EncodePasses(surface));
    return state.FinishFrame(outCompletion);
}
Status RenderGraph::Encode(rhi::SurfaceHandle surface) noexcept
{
    if (mState.Get() == nullptr || mState->HasOpenFrame == true || mState->HasExternalRecording == true)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    rhi::DeviceInfo information;
    const rhi::DeviceStatus owner = rhi::GetDeviceInfo(state.Device, information);
    if (owner != rhi::DeviceStatus::Ready)
    {
        return MapDeviceStatus(owner);
    }
    if (information.FrameOpen == false)
    {
        return Status::InvalidState;
    }
    if (state.IsReady == false || state.Current.Get() == nullptr)
    {
        return Status::NotReady;
    }
    if (state.Producer.IsNull() == false)
    {
        const Status completed = rhi::GetStatus(state.Device, state.Producer);
        if (completed != Status::Ready)
        {
            return completed == Status::Pending ? Status::NotReady : completed;
        }
    }
    for (usize pass = 0; pass < state.Header.PassCount; ++pass)
    {
        if (state.Passes[pass].Program == format::Technique::RayTracing && state.Current->IsRayGraphPrepared == false)
        {
            return Status::NotReady;
        }
    }
    const Status encoded = state.EncodePasses(surface);
    state.HasExternalRecording = state.HasAttemptedDraw;
    if (state.HasExternalRecording == true)
    {
        state.HasValidOutput = false;
        state.OutputRevision = 0;
    }
    return encoded;
}
bool RenderGraph::HasUnconfirmedEncoding() const noexcept
{
    return mState.Get() != nullptr && mState->HasExternalRecording == true;
}
Status RenderGraph::ConfirmSubmission(rhi::SubmissionToken completion) noexcept
{
    if (mState.Get() == nullptr || mState->HasExternalRecording == false)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    const Status owner = CheckDevice(state.Device);
    if (owner != Status::Ready && owner != Status::DeviceLost)
    {
        return owner;
    }
    if (owner == Status::Ready && completion.IsNull() == false)
    {
        const Status submitted = rhi::GetStatus(state.Device, completion);
        if (submitted != Status::Ready && submitted != Status::Pending && submitted != Status::Failed)
        {
            return submitted;
        }
        state.Producer = completion;
        state.HasValidOutput = state.RecordingStatus == Status::Ready && submitted != Status::Failed;
        state.OutputRevision = state.HasValidOutput == true ? state.RecordingRevision : 0;
        if (state.HasValidOutput == true)
        {
            state.CountRayFrame();
        }
        state.HasExternalRecording = false;
        return state.RecordingStatus == Status::Ready ? submitted : state.RecordingStatus;
    }
    state.HasValidOutput = false;
    state.OutputRevision = 0;
    state.HasExternalRecording = false;
    return owner == Status::DeviceLost ? owner : Status::Failed;
}
Status RenderGraph::GetExportAllocation(uint32 resource,
                                        rhi::TextureHandle& outTexture,
                                        rhi::TextureAspect& outAspect) const noexcept
{
    if (mState.Get() == nullptr || resource >= mState->Header.ResourceCount ||
        mState->Resources[resource].IsExported == 0)
    {
        return Status::InvalidDescription;
    }
    const State& state = *mState;
    if (state.IsReady == false)
    {
        return Status::NotReady;
    }
    const rhi::TextureHandle texture = state.Textures[state.Resources[resource].Backing];
    const Status ready = rhi::GetStatus(state.Device, texture);
    if (ready != Status::Ready)
    {
        return ready == Status::Pending ? Status::NotReady : ready;
    }
    outTexture = texture;
    outAspect =
        state.Resources[resource].Kind == format::Aspect::Color ? rhi::TextureAspect::Color : rhi::TextureAspect::Depth;
    return Status::Ready;
}
Status
RenderGraph::GetExport(uint32 resource, rhi::TextureHandle& outTexture, rhi::TextureAspect& outAspect) const noexcept
{
    if (mState.Get() == nullptr || resource >= mState->Header.ResourceCount)
    {
        return Status::InvalidDescription;
    }
    const State& state = *mState;
    if (state.Resources[resource].IsExported == 0)
    {
        return Status::InvalidDescription;
    }
    if (state.HasValidOutput == false || state.Producer.IsNull() == true ||
        rhi::GetStatus(state.Device, state.Producer) != Status::Ready)
    {
        return Status::NotReady;
    }
    outTexture = state.Textures[state.Resources[resource].Backing];
    outAspect =
        state.Resources[resource].Kind == format::Aspect::Color ? rhi::TextureAspect::Color : rhi::TextureAspect::Depth;
    return Status::Ready;
}
Status RenderGraph::GetResourceAllocation(uint32 resource,
                                          rhi::TextureHandle& outTexture,
                                          rhi::TextureAspect& outAspect) const noexcept
{
    if (mState.Get() == nullptr || resource >= mState->Header.ResourceCount)
    {
        return Status::InvalidDescription;
    }
    const State& state = *mState;
    const format::Resource& logical = state.Resources[resource];
    if (logical.Kind == format::Aspect::Buffer || logical.IsSurface == 1 ||
        state.Resources[logical.Backing].IsSurface == 1)
    {
        return Status::Unsupported;
    }
    if (state.HasValidOutput == false || state.Producer.IsNull() == true ||
        rhi::GetStatus(state.Device, state.Producer) != Status::Ready)
    {
        return Status::NotReady;
    }
    outTexture = state.Textures[logical.Backing];
    outAspect = logical.Kind == format::Aspect::Color ? rhi::TextureAspect::Color : rhi::TextureAspect::Depth;
    return Status::Ready;
}
#if defined(LUDUS_RENDERER_TEST_READBACK)
Status internal::RenderGraphRayPass::GetGraphAccumulationForTesting(RenderGraph& inoutGraph,
                                                                    uint32 pass,
                                                                    rhi::BufferHandle& outBuffer) noexcept
{
    if (inoutGraph.mState.Get() == nullptr || inoutGraph.mState->Current.Get() == nullptr)
    {
        return Status::NotReady;
    }
    RenderGraph::State& state = *inoutGraph.mState;
    if (pass >= state.Header.PassCount || state.Passes[pass].Program != format::Technique::RayTracing)
    {
        return Status::InvalidDescription;
    }
    return GetAccumulationForTesting(state.Contexts[static_cast<usize>(format::Technique::RayTracing)],
                                     state.Current->RayPasses[pass].GetJob(),
                                     outBuffer);
}
#endif
} // namespace ludus::graphics::renderer
