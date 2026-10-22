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
                                bool hasPersistentBuffers) noexcept
{
    using namespace foundation;
    constexpr usize COMPUTE_BINDING_COUNT = LUDUS_GPU_PREVIEW_BINDING_COUNT;
    constexpr usize IMAGE_INPUT_COUNT = 2;
    constexpr usize IMAGE_BINDING_COUNT = 1;
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
            {0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Vertex, VECTOR_BYTES},
        },
    };
    static constexpr StaticArray<rhi::RasterShaderInput, IMAGE_INPUT_COUNT> IMAGE_INPUTS{
        {
            {0, rhi::RasterVertexFormat::Float2},
            {1, rhi::RasterVertexFormat::Float4},
        },
    };
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
    inoutDescription.Artifact.Stage = stage;
    if (stage == rhi::ShaderStage::Compute)
    {
        inoutDescription.Bindings =
            hasPersistentBuffers == true ? PERSISTENT_BINDINGS.AsSpan() : COMPUTE_BINDINGS.AsSpan();
        inoutDescription.WorkgroupSize[0] = WORKGROUP_WIDTH;
        inoutDescription.WorkgroupSize[1] = 1;
        inoutDescription.WorkgroupSize[2] = 1;
    }
    else if (stage == rhi::ShaderStage::Vertex)
    {
        inoutDescription.Inputs = IMAGE_INPUTS.AsSpan();
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
    description.TargetSamples = LUDUS_GRAPH_SAMPLE_TARGET;
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
