#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer::internal
{
/// RenderGraph's side of RenderGraphAttachmentPool; owner thread, between frames.
struct RenderGraphPoolAccess final
{
    /// Whether the pool is initialized and its device is the RHI's ready owner.
    [[nodiscard]] static bool IsUsable(const RenderGraphAttachmentPool& pool) noexcept;
    /// Whether the pool still holds a device binding, so leased allocations may return to it.
    [[nodiscard]] static bool IsBound(const RenderGraphAttachmentPool& pool) noexcept;
    /// Lease an idle allocation with an equal description whose last use has completed, or create one.
    /// @pre IsUsable; the pool's device owns the texture.
    /// @param outTexture Receives the leased allocation on Ready/Pending; preserved on failure.
    /// @param outIsReused Receives whether an idle allocation was reused; preserved on failure.
    /// @return CreateTexture's status for a new allocation, Ready for a reused one.
    [[nodiscard]] static rhi::RasterStatus Acquire(RenderGraphAttachmentPool& inoutPool,
                                                   const rhi::TextureDescription& description,
                                                   rhi::TextureHandle& outTexture,
                                                   bool& outIsReused) noexcept;
    /// Return a leased allocation; it becomes idle, or is destroyed when it can never be reused.
    /// Clears inoutTexture. The caller must not use the allocation afterwards.
    /// @pre IsBound.
    static void Release(RenderGraphAttachmentPool& inoutPool,
                        const rhi::TextureDescription& description,
                        rhi::TextureHandle& inoutTexture) noexcept;
};
} // namespace ludus::graphics::renderer::internal
