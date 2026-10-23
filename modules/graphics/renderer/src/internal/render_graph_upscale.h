#pragma once

#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer::internal
{
/// Shared renderer-owned linear-clamp color resampling setup. Owner thread only.
class RenderGraphUpscaleProgram final
{
public:
    RenderGraphUpscaleProgram() noexcept = default;
    ~RenderGraphUpscaleProgram() noexcept;
    RenderGraphUpscaleProgram(const RenderGraphUpscaleProgram&) = delete;
    RenderGraphUpscaleProgram& operator=(const RenderGraphUpscaleProgram&) = delete;
    /// Prepare the installed composite recipe and a full-target quad outside frames.
    /// Ready/Pending owns setup; failure requires destruction. Select the surface or RGBA8 output profile.
    [[nodiscard]] rhi::RasterStatus Initialize(rhi::DeviceHandle device,
                                               const rhi::RasterShaderDescription& vertex,
                                               const rhi::RasterShaderDescription& fragment,
                                               bool isSurface) noexcept;
    /// Advance asynchronous setup; no wait or submission. Failure never publishes a drawable replacement.
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    /// Encode into the acquired frame. Destination is distinct RGBA8 or null for the surface.
    /// No allocation, readback, implicit tone mapping, or CPU pixel copies.
    [[nodiscard]] rhi::RasterStatus Draw(rhi::BindingSetHandle bindings, rhi::TextureHandle destination) noexcept;

private:
    friend class RenderGraphUpscalePass;
    enum class Phase : foundation::uint8
    {
        Empty,
        Dependencies,
        PipelineCreation,
        Pipeline,
        Ready,
        Failed
    };
    rhi::DeviceHandle mDevice{};
    rhi::RasterShaderHandle mVertex{};
    rhi::RasterShaderHandle mFragment{};
    rhi::BindingLayoutHandle mLayout{};
    rhi::SamplerHandle mSampler{};
    rhi::BufferHandle mVertices{};
    rhi::BufferHandle mIndices{};
    rhi::RasterPipelineHandle mPipeline{};
    Phase mPhase = Phase::Empty;
    rhi::RasterStatus mStatus = rhi::RasterStatus::InvalidState;
    bool mIsSurface = false;
};
/// Scene-specific immutable source binding. The graph-owned program outlives every binding.
class RenderGraphUpscalePass final
{
public:
    RenderGraphUpscalePass() noexcept = default;
    ~RenderGraphUpscalePass() noexcept;
    RenderGraphUpscalePass(const RenderGraphUpscalePass&) = delete;
    RenderGraphUpscalePass& operator=(const RenderGraphUpscalePass&) = delete;
    /// Prepare a source snapshot against a Ready program outside frames; preserves prior generations.
    [[nodiscard]] rhi::RasterStatus Initialize(RenderGraphUpscaleProgram& inoutProgram,
                                               rhi::TextureHandle source) noexcept;
    /// Advance source-view/binding preparation without blocking.
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    /// Encode only after Ready; no allocations or shader/pipeline creation.
    [[nodiscard]] rhi::RasterStatus Draw(rhi::TextureHandle destination) noexcept;

private:
    RenderGraphUpscaleProgram* mProgramOrNull = nullptr;
    rhi::TextureViewHandle mSource{};
    rhi::BindingSetHandle mBindings{};
    bool mHasBindings = false;
    rhi::RasterStatus mStatus = rhi::RasterStatus::InvalidState;
};
} // namespace ludus::graphics::renderer::internal
