#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/graphics/renderer/renderer.hpp>

namespace ludus::graphics::renderer::internal
{
/// One immutable ray invocation's scene inputs and one-shot per-frame commands.
/// The borrowed context/device outlive this owner. All calls use the RHI owner
/// thread; initialize/poll/prepare/reset are between frames, Encode is in a frame.
class RenderGraphRayPass final
{
public:
    RenderGraphRayPass() noexcept = default;
    ~RenderGraphRayPass() noexcept;
    RenderGraphRayPass(const RenderGraphRayPass&) = delete;
    RenderGraphRayPass& operator=(const RenderGraphRayPass&) = delete;
    [[nodiscard]] rhi::RasterStatus Initialize(Renderer& inoutContext,
                                               rhi::DeviceHandle device,
                                               Snapshot snapshot,
                                               const ViewDescription& camera,
                                               foundation::uint32 width,
                                               foundation::uint32 height,
                                               const RenderGraphSceneDescription* sceneOrNull = nullptr) noexcept;
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    [[nodiscard]] rhi::RasterStatus Prepare(rhi::TextureHandle target) noexcept;
    [[nodiscard]] rhi::RasterStatus Encode(rhi::SurfaceHandle surface) noexcept;
    [[nodiscard]] LUDUS_INLINE bool IsPrepared() const noexcept
    {
        return mIsPrepared;
    }

private:
    Renderer* mContextOrNull = nullptr;
    rhi::DeviceHandle mDevice{};
    GpuPreview mJob{};
    rhi::OrderedGraph mGraph{};
    bool mIsPrepared = false;
};
} // namespace ludus::graphics::renderer::internal
