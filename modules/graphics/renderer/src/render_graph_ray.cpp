#include <ludus/graphics/renderer/render_graph.hpp>

#include "../shaders/renderer_gpu_preview_layout.h"
#include "internal/render_graph_ray.h"

#include <ludus/foundation/containers/static_array.hpp>

// Thanks to the Slang project, "Supported Compilation Targets", Metal target,
// https://shader-slang.org/slang/user-guide/targets: the graph links authored
// ray functions into the portable compute target with an explicit resource ABI.
namespace ludus::graphics::renderer
{
void SetRenderGraphRayInterface(rhi::RasterShaderDescription& inoutDescription,
                                rhi::ShaderStage stage,
                                bool hasPersistentBuffers,
                                bool hasVisibilityBuffer) noexcept
{
    using namespace foundation;
    constexpr usize COMPUTE_BINDING_COUNT = LUDUS_GPU_PREVIEW_BINDING_COUNT;
    constexpr usize IMAGE_INPUT_COUNT = 1;
    constexpr usize IMAGE_BINDING_COUNT = 2;
    constexpr usize PARAMETERS_BYTES = LUDUS_GPU_PREVIEW_PARAMETERS_BYTES;
    constexpr usize VECTOR_BYTES = LUDUS_GPU_PREVIEW_VECTOR_BYTES;
    constexpr uint32 WORKGROUP_WIDTH = LUDUS_GPU_PREVIEW_GROUP_WIDTH;
    static constexpr StaticArray<rhi::RasterBinding, COMPUTE_BINDING_COUNT> COMPUTE_BINDINGS{
        {
            {0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Compute, PARAMETERS_BYTES},
            {1, rhi::RasterBindingKind::StorageRead, rhi::RasterVisibility::Compute, VECTOR_BYTES},
            {2, rhi::RasterBindingKind::StorageReadWrite, rhi::RasterVisibility::Compute, VECTOR_BYTES},
            {3, rhi::RasterBindingKind::StorageReadWrite, rhi::RasterVisibility::Compute, VECTOR_BYTES},
            {4, rhi::RasterBindingKind::StorageRead, rhi::RasterVisibility::Compute, VECTOR_BYTES},
            {LUDUS_GPU_PREVIEW_RAY_SCENE_BINDING, rhi::RasterBindingKind::RayScene, rhi::RasterVisibility::Compute, 0},
        },
    };
    static constexpr StaticArray<rhi::RasterBinding, IMAGE_BINDING_COUNT> IMAGE_BINDINGS{
        {
            {0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Fragment, VECTOR_BYTES},
            {1, rhi::RasterBindingKind::StorageRead, rhi::RasterVisibility::Fragment, VECTOR_BYTES},
        },
    };
    static constexpr StaticArray<rhi::RasterShaderInput, IMAGE_INPUT_COUNT> IMAGE_INPUTS{
        {
            {0, rhi::RasterVertexFormat::Float2},
        },
    };
    static constexpr StaticArray<rhi::RasterBinding, LUDUS_GRAPH_VISIBILITY_BINDING_COUNT> VISIBILITY_BINDINGS{{
        COMPUTE_BINDINGS[0],
        COMPUTE_BINDINGS[1],
        COMPUTE_BINDINGS[2],
        COMPUTE_BINDINGS[3],
        COMPUTE_BINDINGS[4],
        COMPUTE_BINDINGS[5],
        {LUDUS_GRAPH_VISIBILITY_BINDING, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Compute, 0},
        {LUDUS_GRAPH_VISIBILITY_DEPTH_BINDING,
         rhi::RasterBindingKind::TextureDepth2D,
         rhi::RasterVisibility::Compute,
         0},
    }};
    static constexpr StaticArray<rhi::RasterBinding, LUDUS_GRAPH_COMPUTE_BINDING_COUNT> PERSISTENT_BINDINGS{
        {{0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Compute, PARAMETERS_BYTES},
         {1, rhi::RasterBindingKind::StorageRead, rhi::RasterVisibility::Compute, VECTOR_BYTES},
         {2, rhi::RasterBindingKind::StorageReadWrite, rhi::RasterVisibility::Compute, VECTOR_BYTES},
         {3, rhi::RasterBindingKind::StorageReadWrite, rhi::RasterVisibility::Compute, VECTOR_BYTES},
         {4, rhi::RasterBindingKind::StorageRead, rhi::RasterVisibility::Compute, VECTOR_BYTES},
         {LUDUS_GPU_PREVIEW_RAY_SCENE_BINDING, rhi::RasterBindingKind::RayScene, rhi::RasterVisibility::Compute, 0},
         {LUDUS_GRAPH_BUFFER0_BINDING,
          rhi::RasterBindingKind::StorageReadWrite,
          rhi::RasterVisibility::Compute,
          VECTOR_BYTES},
         {LUDUS_GRAPH_BUFFER1_BINDING,
          rhi::RasterBindingKind::StorageReadWrite,
          rhi::RasterVisibility::Compute,
          VECTOR_BYTES}}};
    static constexpr StaticArray<rhi::RasterBinding,
                                 LUDUS_GRAPH_COMPUTE_BINDING_COUNT + LUDUS_GRAPH_VISIBILITY_TEXTURE_COUNT>
        PERSISTENT_VISIBILITY_BINDINGS{{PERSISTENT_BINDINGS[0],
                                        PERSISTENT_BINDINGS[1],
                                        PERSISTENT_BINDINGS[2],
                                        PERSISTENT_BINDINGS[3],
                                        PERSISTENT_BINDINGS[4],
                                        PERSISTENT_BINDINGS[5],
                                        PERSISTENT_BINDINGS[6],
                                        PERSISTENT_BINDINGS[7],
                                        VISIBILITY_BINDINGS[LUDUS_GPU_PREVIEW_BINDING_COUNT],
                                        VISIBILITY_BINDINGS[LUDUS_GPU_PREVIEW_BINDING_COUNT + 1]}};
    inoutDescription.Artifact.Stage = stage;
    inoutDescription.Bindings = {};
    inoutDescription.Inputs = {};
    if (stage == rhi::ShaderStage::Compute)
    {
        inoutDescription.Bindings =
            hasVisibilityBuffer == true
                ? (hasPersistentBuffers == true ? PERSISTENT_VISIBILITY_BINDINGS.AsSpan()
                                                : VISIBILITY_BINDINGS.AsSpan())
                : (hasPersistentBuffers == true ? PERSISTENT_BINDINGS.AsSpan() : COMPUTE_BINDINGS.AsSpan());
        inoutDescription.WorkgroupSize[0] = WORKGROUP_WIDTH;
        inoutDescription.WorkgroupSize[1] = 1;
        inoutDescription.WorkgroupSize[2] = 1;
    }
    else if (stage == rhi::ShaderStage::Vertex)
    {
        inoutDescription.Inputs = IMAGE_INPUTS.AsSpan();
    }
    else if (stage == rhi::ShaderStage::Fragment)
    {
        inoutDescription.Bindings = IMAGE_BINDINGS.AsSpan();
    }
}
} // namespace ludus::graphics::renderer

namespace ludus::graphics::renderer::internal
{
RenderGraphRayPass::~RenderGraphRayPass() noexcept
{
    if (mContextOrNull != nullptr)
    {
        static_cast<void>(mContextOrNull->Release(mJob));
    }
}
rhi::RasterStatus RenderGraphRayPass::Initialize(Renderer& inoutContext,
                                                 Snapshot snapshot,
                                                 const ViewDescription& camera,
                                                 // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): extent
                                                 foundation::uint32 width,
                                                 foundation::uint32 height,
                                                 const RenderGraphSceneDescription* sceneOrNull,
                                                 const GpuPreviewGraphInvocation* invocationOrNull) noexcept
{
    if (mContextOrNull != nullptr)
    {
        return rhi::RasterStatus::InvalidState;
    }
    if (camera.Depth != DepthConvention::Conventional || camera.InfiniteFar == true)
    {
        return rhi::RasterStatus::Unsupported;
    }
    ReferenceCaptureDescription description;
    description.WorldToClip = camera.WorldToClip;
    description.CameraPosition = camera.CameraPosition;
    description.OrthographicCamera = camera.OrthographicCamera;
    description.Width = width;
    description.Height = height;
    description.TargetSamples = invocationOrNull != nullptr && invocationOrNull->AccumulationSamples != 0
                                    ? invocationOrNull->AccumulationSamples
                                    : content::render_graph_format::MAX_ACCUMULATION_SAMPLES;
    description.MaxDepth = 1;
    description.RouletteDepth = 1;
    description.CenterRays = true;
    // A camera/scene edit restarts accumulation, but must not replay the same per-pixel stream.
    description.Seed = sceneOrNull != nullptr ? sceneOrNull->SourceGeneration : 0;
    const rhi::RasterStatus status =
        inoutContext.BeginGpuPreview(snapshot, description, mJob, sceneOrNull, invocationOrNull);
    if (status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending)
    {
        mContextOrNull = &inoutContext;
    }
    return status;
}
rhi::RasterStatus RenderGraphRayPass::Poll() noexcept
{
    if (mContextOrNull == nullptr)
    {
        return rhi::RasterStatus::InvalidState;
    }
    GpuPreviewInfo information;
    return mContextOrNull->GetGpuPreviewInfo(mJob, information);
}
} // namespace ludus::graphics::renderer::internal

namespace ludus::graphics::renderer::internal
{
rhi::RasterStatus RenderGraphRayPass::PrepareCamera(const ViewDescription& camera, foundation::uint64 revision) noexcept
{
    return mContextOrNull->prepareGpuPreviewCamera(mJob, camera, revision);
}
rhi::RasterStatus RenderGraphRayPass::PollCamera() noexcept
{
    return mContextOrNull->pollGpuPreviewCamera(mJob);
}
void RenderGraphRayPass::CommitCamera() noexcept
{
    mContextOrNull->commitGpuPreviewCamera(mJob);
}
void RenderGraphRayPass::DiscardCamera() noexcept
{
    mContextOrNull->discardGpuPreviewCamera(mJob);
}
} // namespace ludus::graphics::renderer::internal
