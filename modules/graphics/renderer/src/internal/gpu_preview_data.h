#pragma once

#include "../../shaders/renderer_gpu_preview_layout.h"
#include "gpu_denoiser.h"
#include "gpu_preview_camera.h"
#include "reference_data.h"
#include "render_graph_storage.h"

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/gpu_preview.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/graph.h>

namespace ludus::graphics::renderer::internal
{
inline constexpr usize GPU_PREVIEW_JOB_COUNT = 16; // Four passes, three retained scenes and retiring work.
inline constexpr usize GPU_GRAPH_BUFFER_COUNT = 2;
/// Every compute binding of one job: the frozen ABI, visibility textures, structured storage, the
/// ray path capture buffer of a diagnostic capture-variant kernel and, last, the LTC table.
inline constexpr usize GPU_PREVIEW_COMPUTE_BINDING_CAPACITY = LUDUS_GRAPH_VISIBILITY_BINDING_COUNT +
                                                              content::render_graph_format::MAX_STORAGE_BUFFERS + 1 +
                                                              LUDUS_GRAPH_LTC_TABLE_BINDING_COUNT;
/// The presentation quad's four float2 corners and six uint16 indices.
inline constexpr usize GPU_PREVIEW_QUAD_BYTES = 44;
struct GpuPreviewUniform final
{
    math::Matrix4 ClipToWorld;
    math::Vector4 CameraPositionWorld;
    uint32 Dimensions[4]{};
    uint32 Offsets[4]{};
    uint32 Counts[4]{};
    uint32 Sampling[4]{};
    math::Vector4 Environment;
    math::Vector4 Options;
    math::Vector4 BoundsMinimumWorld;
    math::Vector4 BoundsMaximumWorld;
    uint32 SurfaceLayout[4]{};
};
static_assert(sizeof(GpuPreviewUniform) == LUDUS_GPU_PREVIEW_PARAMETERS_BYTES);
struct GpuFrameRecord final
{
    uint64 Generation = 1;
    bool Owned = false;
    rhi::TextureHandle Texture{};
    rhi::SubmissionToken Producer{};
    uint32 Samples = 0;
};
struct GpuPreviewRecord final
{
    bool HasNativeRayInvocation = false;
    uint64 Generation = 1;
    bool Used = false;
    bool Owned = false;
    rhi::RayGeometryHandle NativeGeometry{};
    rhi::RaySceneHandle NativeScene{};
    bool HasNativeGeometry = false;
    bool IsNativeSceneCreated = false;
    RenderGraphStorage Storage;
    rhi::BufferHandle Data{};
    rhi::BufferHandle Texels{};
    rhi::BufferHandle Parameters{};
    rhi::BufferHandle Accumulation{};
    rhi::BufferHandle Colors{};
    rhi::BufferHandle ImageParameters{};
    rhi::TextureHandle VisibilityTexture{};
    rhi::TextureViewHandle VisibilityView{};
    rhi::TextureViewHandle VisibilityDepthView{};
    rhi::BindingSetHandle ComputeBindings{};
    rhi::BindingSetHandle ImageBindings{};
    foundation::StaticArray<GpuPreviewGraphBuffer, GPU_GRAPH_BUFFER_COUNT> GraphBuffers{};
    foundation::StaticArray<bool, GPU_GRAPH_BUFFER_COUNT> HasOwnedGraphBuffers{};
    GpuPreviewGraphBuffer RayPathCapture{}; // Caller-owned; bound only by capture-variant kernels.
    GpuFrameRecord Frames[2];
    GpuPreviewInfo Info;
    GpuPreviewUniform Uniform{};
    GpuPreviewCameraUpdate CameraUpdate;
    GpuDenoiser Denoiser;
    uint32 DenoisingQuality = 0;
    bool HasPendingCamera = false;
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 PendingFrame = 0;
    uint32 LatestFrame = 0;
    bool HasImage = false;
    usize DataBytes = 0;
    usize TexelBytes = 0;
    bool ComputeBindingsCreated = false;
    bool ImageBindingsCreated = false;
    rhi::RasterStatus Setup = rhi::RasterStatus::Pending;
    usize GpuBytes = 0;
};
struct GpuPreviewState final
{
    GpuPreviewOptions Options;
    StaticArray<content::render_graph_format::StorageBuffer, content::render_graph_format::MAX_STORAGE_BUFFERS>
        StorageDescriptions{};
    // RasterBinding initializes Visibility to Both for every nested element.
    StaticArray<rhi::RasterBinding, GPU_PREVIEW_COMPUTE_BINDING_CAPACITY>
        ComputeBindings{}; // NOLINT(bugprone-invalid-enum-default-initialization)
    usize ComputeBindingCount = 0;
    uint32 WorkgroupWidth = 1;
    uint32 WorkgroupHeight = 1;
    bool HasRayPathCapture = false;
    /// The kernel reads the renderer's LTC table; LtcTable holds the uploaded copy.
    bool HasLtcTable = false;
    bool IsGraphRayScene = false;
    bool HasNativeRayQueries = true;
    bool HasVisibilityBuffer = false;
    bool HasPersistentBuffers = false;
    uint32 Phase = 0;
    bool QuadCreated = false;
    bool IndicesCreated = false;
    bool IsLtcTableCreated = false;
    rhi::RasterStatus Setup = rhi::RasterStatus::Pending;
    rhi::RasterShaderHandle ComputeShader{};
    rhi::RasterShaderHandle NativeRayShader{};
    rhi::ComputePipelineHandle NativeRayPipeline{};
    bool HasNativeRayShader = false;
    bool HasCreatedNativeRayPipeline = false;
    rhi::RasterShaderHandle VertexShader{};
    rhi::RasterShaderHandle FragmentShader{};
    rhi::BindingLayoutHandle ComputeLayout{};
    rhi::BindingLayoutHandle ImageLayout{};
    rhi::ComputePipelineHandle ComputePipeline{};
    rhi::RasterPipelineHandle ImagePipeline{};
    rhi::BufferHandle Quad{};
    rhi::BufferHandle Indices{};
    rhi::BufferHandle LtcTable{};
    GpuPreviewRecord Jobs[GPU_PREVIEW_JOB_COUNT];
    usize GpuBytes = 0;
};
} // namespace ludus::graphics::renderer::internal
