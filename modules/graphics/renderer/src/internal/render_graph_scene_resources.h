#pragma once

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>

#include "directional_near_cascade.h"

namespace ludus::graphics::renderer::internal
{
struct ShadowTexture;
struct PreparedShadowResources final
{
    rhi::DeviceHandle Device{};
    DeferredShadowAtlas Atlas;
    foundation::SharedPtr<ShadowTexture> Backing;
    rhi::RasterBindingResource LightWords{};
    foundation::uint32 NearTableWord = 0;
    ~PreparedShadowResources() noexcept;
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
};
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
    [[nodiscard]] foundation::usize GetSurfaceChartDrawCount() const noexcept;
    [[nodiscard]] foundation::usize GetProbeReceiverDrawCount() const noexcept;
    /// Borrowed read-only triangle storage, retained until this generation is destroyed.
    [[nodiscard]] rhi::RasterBindingResource GetMeshLights() const noexcept;
    [[nodiscard]] foundation::uint32 GetMeshLightCount() const noexcept;
    /// One-based index in the deferred resolve's packed light array, sorted by source ID across all light kinds.
    /// Returns zero if no directional light with positive intensity and a nonblack color matches the requested source.
    [[nodiscard]] foundation::uint32 FindDirectionalLightIndex(foundation::uint64 sourceId) const noexcept;
    /// One-based index of any light with this source ID in the same packed array, or zero if none matches.
    [[nodiscard]] foundation::uint32 FindLightIndex(foundation::uint64 sourceId) const noexcept;
    /// First word of the emitter culling grid inside GetMeshLights' buffer, or zero when every record is evaluated.
    [[nodiscard]] foundation::uint32 GetMeshLightGridWord() const noexcept;
    /// Mesh-light shadow casters with allocated atlas faces; empty when the scene is unshadowed.
    [[nodiscard]] foundation::BorrowedView<const MeshLightShadowCaster> GetMeshLightShadowCasters() const noexcept;
    [[nodiscard]] bool HasShadowAtlas() const noexcept;
    /// Sampled view of the shadow atlas for the deferred resolve, Ready once Poll is Ready; null without shadowed
    /// lights.
    [[nodiscard]] rhi::TextureViewHandle GetShadowAtlasView() const noexcept;
    /// The shadow atlas the frame encoder draws faces into; null without shadowed lights.
    [[nodiscard]] DeferredShadowAtlas* GetShadowAtlasOrNull() noexcept;
    void DescribeDrawableShadows(RenderGraphSceneResourceReport& inoutReport) const noexcept;
    [[nodiscard]] const PreparedShadowResources& GetShadowResources() const noexcept;
    [[nodiscard]] rhi::RasterStatus
    PrepareCameraShadows(const DirectionalNearFootprint& footprint,
                         foundation::core::UniquePtr<PreparedShadowResources>& outReplacement) const noexcept;
    void CommitCameraShadows(foundation::core::UniquePtr<PreparedShadowResources>& inoutReplacement) noexcept;
    /// Whether the admitted camera slice would use exactly the currently prepared near projections.
    [[nodiscard]] bool CanReuseDirectionalNearCascades(const DirectionalNearFootprint& footprint) const noexcept;
    /// Mesh-light faces that have yet to be submitted.
    [[nodiscard]] foundation::uint32 GetPendingMeshLightShadowFaces() const noexcept;

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
                                               bool hasMeshLights = false,
                                               MaterialProgram surfaceChartProgram = {},
                                               MaterialProgram probeProgram = {}) noexcept;
    /// Consume every input before returning. Failure preserves a null generation.
    [[nodiscard]] rhi::RasterStatus Prepare(const RenderGraphSceneDescription& scene,
                                            foundation::core::UniquePtr<ForwardSceneGeneration>& outGeneration,
                                            const DirectionalNearFootprint& footprint = {},
                                            const ForwardSceneGeneration* matchingCastersOrNull = nullptr) noexcept;
    void Reset() noexcept;
    [[nodiscard]] RenderGraphSceneResourceReport GetReport() const noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
    friend class ForwardSceneGeneration;
};
} // namespace ludus::graphics::renderer::internal
