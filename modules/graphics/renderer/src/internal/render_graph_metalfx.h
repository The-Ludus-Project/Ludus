#pragma once

#include "render_graph_upscale.h"
#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/spatial_upscale.h>

namespace ludus::graphics::renderer::internal
{
/// Immutable size-generation effect; shared by scene and camera replacements. Owner thread only.
class RenderGraphMetalFxPass final
{
public:
    RenderGraphMetalFxPass() noexcept = default;
    ~RenderGraphMetalFxPass() noexcept;
    RenderGraphMetalFxPass(const RenderGraphMetalFxPass&) = delete;
    RenderGraphMetalFxPass& operator=(const RenderGraphMetalFxPass&) = delete;
    /// Prepare once between frames. Surface output uses a private intermediate and shared composite program.
    [[nodiscard]] rhi::RasterStatus Initialize(rhi::DeviceHandle device,
                                               const rhi::SpatialUpscalerDescription& description,
                                               rhi::TextureHandle source,
                                               rhi::TextureHandle destination,
                                               RenderGraphUpscaleProgram* surfaceProgramOrNull) noexcept;
    /// Advance intermediate/source-view preparation; never encodes GPU work.
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    /// Encode only after Ready, with no setup allocation or pipeline creation.
    [[nodiscard]] rhi::RasterStatus Draw() noexcept;

private:
    rhi::DeviceHandle mDevice{};
    rhi::SpatialUpscalerHandle mUpscaler{};
    rhi::TextureHandle mSource{};
    rhi::TextureHandle mDestination{};
    rhi::TextureHandle mIntermediate{};
    RenderGraphUpscaleProgram* mSurfaceProgramOrNull = nullptr;
    RenderGraphUpscalePass mCopy;
    bool mHasCopy = false;
};
/// Prepared effects belonging to one graph attachment generation, including resize candidates.
struct RenderGraphMetalFxResources final
{
    foundation::StaticArray<RenderGraphMetalFxPass, content::render_graph_format::MAX_PASSES> Passes{};
    foundation::StaticArray<bool, content::render_graph_format::MAX_PASSES> IsPrepared{};
};
} // namespace ludus::graphics::renderer::internal
