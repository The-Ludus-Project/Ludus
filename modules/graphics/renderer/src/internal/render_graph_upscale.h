#pragma once

#include <ludus/foundation/base/compiler.h>

#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer::internal
{
struct RenderGraphUpscaleDependencies;
/// One graph's linear-clamp color resampling program for one target. Owner thread only.
/// Shaders, layout, sampler and quad are leased from a RenderGraphProgramCache, and the raster pipeline
/// is an owned rhi::RequestPipeline request, so programs with equal stages and target share one native
/// pipeline across graph instances and generations. Input bindings and targets stay with each pass.
class RenderGraphUpscaleProgram final
{
public:
    RenderGraphUpscaleProgram() noexcept = default;
    ~RenderGraphUpscaleProgram() noexcept;
    RenderGraphUpscaleProgram(const RenderGraphUpscaleProgram&) = delete;
    RenderGraphUpscaleProgram& operator=(const RenderGraphUpscaleProgram&) = delete;
    /// Lease the installed composite recipe's dependencies and request the surface or RGBA8 pipeline outside
    /// frames. Ready/Pending owns a lease; failure requires Reset or destruction. The cache outlives the program.
    [[nodiscard]] rhi::RasterStatus Initialize(RenderGraphProgramCache& inoutCache,
                                               const rhi::RasterShaderDescription& vertex,
                                               const rhi::RasterShaderDescription& fragment,
                                               bool isSurface) noexcept;
    /// Advance asynchronous setup; no wait or submission. Failure never publishes a drawable replacement.
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    /// Encode into the acquired frame. Destination is distinct RGBA8 or null for the surface.
    /// No allocation, readback, implicit tone mapping, CPU pixel copies or pipeline creation.
    [[nodiscard]] rhi::RasterStatus Draw(rhi::BindingSetHandle bindings, rhi::TextureHandle destination) noexcept;
    /// Release the pipeline request, then the dependency lease, and return to Empty. Accepted frames keep
    /// the released pipeline and dependencies through RHI completion. Every pass using it is gone.
    void Reset() noexcept;
    /// Whether Initialize acquired a pipeline request that Reset has not released.
    [[nodiscard]] LUDUS_INLINE bool HasRequest() const noexcept
    {
        return mHasRequest;
    }
    /// The owned pipeline request; valid only while HasRequest.
    [[nodiscard]] LUDUS_INLINE rhi::PipelineRequest GetRequest() const noexcept
    {
        return mRequest;
    }

private:
    friend class RenderGraphUpscalePass;
    enum class Phase : foundation::uint8
    {
        Empty,
        Dependencies,
        PipelineRequest,
        Pipeline,
        Ready,
        Failed
    };
    rhi::DeviceHandle mDevice{};
    RenderGraphProgramCache* mCacheOrNull = nullptr;
    RenderGraphUpscaleDependencies* mDependenciesOrNull = nullptr;
    rhi::PipelineRequest mRequest{};
    /// Borrowed from mRequest while Ready; never destroyed here.
    rhi::RasterPipelineHandle mPipeline{};
    Phase mPhase = Phase::Empty;
    rhi::RasterStatus mStatus = rhi::RasterStatus::InvalidState;
    bool mIsSurface = false;
    bool mHasRequest = false;
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
