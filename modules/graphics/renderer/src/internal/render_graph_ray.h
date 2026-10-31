#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/graphics/renderer/renderer.hpp>

namespace ludus::graphics::renderer
{
class RenderGraph;
}

namespace ludus::graphics::renderer::internal
{
/// One immutable ray invocation's scene inputs and owned GPU job.
/// The borrowed renderer context outlives this owner. All calls use the RHI owner
/// thread between frames. Sequence command ownership belongs to the graph Scene.
class RenderGraphRayPass final
{
public:
    RenderGraphRayPass() noexcept = default;
    ~RenderGraphRayPass() noexcept;
    RenderGraphRayPass(const RenderGraphRayPass&) = delete;
    RenderGraphRayPass& operator=(const RenderGraphRayPass&) = delete;
    [[nodiscard]] rhi::RasterStatus Initialize(Renderer& inoutContext,
                                               Snapshot snapshot,
                                               const ViewDescription& camera,
                                               foundation::uint32 width,
                                               foundation::uint32 height,
                                               const RenderGraphSceneDescription* sceneOrNull = nullptr,
                                               const GpuPreviewGraphInvocation* invocationOrNull = nullptr) noexcept;
    [[nodiscard]] LUDUS_INLINE GpuPreview GetJob() const noexcept
    {
        return mJob;
    }
    // Borrowed accumulation handle of a completed preview job; backs RenderGraph::GetAccumulation for frame
    // captures. Keep the completed preview/frame alive through the readback.
    [[nodiscard]] static rhi::RasterStatus
    GetAccumulation(Renderer& inoutRenderer, GpuPreview preview, rhi::BufferHandle& outBuffer) noexcept;
    // Test-only alias of GetAccumulation for fixtures that drive a Renderer preview directly.
    [[nodiscard]] static rhi::RasterStatus
    GetAccumulationForTesting(Renderer& inoutRenderer, GpuPreview preview, rhi::BufferHandle& outBuffer) noexcept;
    // Borrow a completed graph pass's raw history for parity checks. The graph and
    // completed frame remain alive; no production readback or installed API is added.
    [[nodiscard]] static rhi::RasterStatus GetGraphAccumulationForTesting(RenderGraph& inoutGraph,
                                                                          foundation::uint32 pass,
                                                                          rhi::BufferHandle& outBuffer) noexcept;
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    // Prepare band `tile` of `count` row bands of a tiled capture from the committed camera; a count of one
    // releases the band. Ready once it is bound, Pending while its uniform or bindings are created.
    [[nodiscard]] rhi::RasterStatus PrepareTiles(foundation::uint32 count, foundation::uint32 tile) noexcept;
    // Emissive triangles plus analytic lights of the ready job's scene.
    [[nodiscard]] foundation::uint32 GetLightCount() const noexcept;
    // Borrow the job's failure summary for the current camera history. Unsupported when the kernel declares none.
    [[nodiscard]] rhi::RasterStatus GetFailures(rhi::BufferHandle& outBuffer) const noexcept;
    [[nodiscard]] rhi::RasterStatus PrepareCamera(const ViewDescription& camera, foundation::uint64 revision) noexcept;
    [[nodiscard]] rhi::RasterStatus PollCamera() noexcept;
    void CommitCamera() noexcept;
    void DiscardCamera() noexcept;

private:
    Renderer* mContextOrNull = nullptr;
    GpuPreview mJob{};
};
} // namespace ludus::graphics::renderer::internal
