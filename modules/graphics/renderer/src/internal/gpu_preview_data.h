#pragma once

#include "../../shaders/renderer_gpu_preview_layout.h"
#include "reference_data.h"

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/gpu_preview.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/graph.h>

namespace ludus::graphics::renderer::internal
{
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
    uint64 Generation = 1;
    bool Used = false;
    bool Owned = false;
    rhi::BufferHandle Data{};
    rhi::BufferHandle Texels{};
    rhi::BufferHandle Parameters{};
    rhi::BufferHandle Accumulation{};
    rhi::BufferHandle Colors{};
    rhi::BufferHandle ImageParameters{};
    rhi::BindingSetHandle ComputeBindings{};
    rhi::BindingSetHandle ImageBindings{};
    GpuFrameRecord Frames[2];
    GpuPreviewInfo Info;
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
    bool IsGraphRayScene = false;
    uint32 Phase = 0;
    bool QuadCreated = false;
    bool IndicesCreated = false;
    rhi::RasterStatus Setup = rhi::RasterStatus::Pending;
    rhi::RasterShaderHandle ComputeShader{};
    rhi::RasterShaderHandle VertexShader{};
    rhi::RasterShaderHandle FragmentShader{};
    rhi::BindingLayoutHandle ComputeLayout{};
    rhi::BindingLayoutHandle ImageLayout{};
    rhi::ComputePipelineHandle ComputePipeline{};
    rhi::RasterPipelineHandle ImagePipeline{};
    rhi::BufferHandle Quad{};
    rhi::BufferHandle Indices{};
    GpuPreviewRecord Jobs[4];
    usize GpuBytes = 0;
};
} // namespace ludus::graphics::renderer::internal
