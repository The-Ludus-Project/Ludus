#pragma once

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>

#include "raster_mesh_light_shadows.h"

namespace ludus::graphics::renderer::internal
{
class ForwardSceneResources;
/// One unpublished/active scene generation. Owns material incarnations and cache pins;
/// all submitted resources remain retained by Renderer/RHI after logical release.
class ForwardSceneGeneration final
{
public:
    ForwardSceneGeneration() noexcept;
    ~ForwardSceneGeneration() noexcept;
    ForwardSceneGeneration(const ForwardSceneGeneration&) = delete;
    ForwardSceneGeneration& operator=(const ForwardSceneGeneration&) = delete;
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    [[nodiscard]] foundation::BorrowedView<const SceneItem> GetItems() const noexcept;
    /// Borrowed read-only triangle storage, retained until this generation is destroyed.
    [[nodiscard]] rhi::RasterBindingResource GetMeshLights() const noexcept;
    [[nodiscard]] foundation::uint32 GetMeshLightCount() const noexcept;
    /// First word of the emitter culling grid inside GetMeshLights' buffer, or zero when every record is evaluated.
    [[nodiscard]] foundation::uint32 GetMeshLightGridWord() const noexcept;
    /// Mesh-light shadow casters with allocated atlas faces; empty when the scene is unshadowed.
    [[nodiscard]] foundation::BorrowedView<const MeshLightShadowCaster> GetMeshLightShadowCasters() const noexcept;
    /// Sampled view of the shadow atlas for the deferred resolve, Ready once Poll is Ready; null without casters.
    [[nodiscard]] rhi::TextureViewHandle GetMeshLightShadowView() const noexcept;
    /// The shadow atlas the frame encoder draws faces into; null without casters.
    [[nodiscard]] MeshLightShadowAtlas* GetMeshLightShadowAtlasOrNull() noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
    friend class ForwardSceneResources;
};
/// Device/context-local bounded immutable asset cache. Exact copied bytes qualify reuse;
/// hashes, caller pointers and revision hints never establish identity. One owner thread,
/// outside acquired frames. Generations must be destroyed before this owner.
class ForwardSceneResources final
{
public:
    ForwardSceneResources() noexcept;
    ~ForwardSceneResources() noexcept;
    ForwardSceneResources(const ForwardSceneResources&) = delete;
    ForwardSceneResources& operator=(const ForwardSceneResources&) = delete;
    [[nodiscard]] rhi::RasterStatus Initialize(rhi::DeviceHandle device,
                                               Renderer& inoutRenderer,
                                               MaterialProgram program,
                                               bool isClustered = false,
                                               bool hasMeshLights = false) noexcept;
    /// Consume every input before returning. Failure preserves a null generation.
    [[nodiscard]] rhi::RasterStatus
    Prepare(const RenderGraphSceneDescription& scene,
            foundation::core::UniquePtr<ForwardSceneGeneration>& outGeneration) noexcept;
    void Reset() noexcept;
    [[nodiscard]] RenderGraphSceneResourceReport GetReport() const noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
    friend class ForwardSceneGeneration;
};
} // namespace ludus::graphics::renderer::internal
