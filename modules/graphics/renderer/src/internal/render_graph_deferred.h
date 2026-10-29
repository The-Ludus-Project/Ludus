#pragma once

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/gpu_scope.h>

namespace ludus::graphics::renderer::internal
{
/// Borrowed immutable resources from the graph's common scene-lighting material.
struct DeferredMaterialBindings final
{
    foundation::StaticArray<rhi::RasterBindingResource, CLUSTERED_AUTHORED_BINDING_COUNT> Resources{};
    bool HasMaterial = false;
};
/// Graph-owned fullscreen geometry, shaders and immutable lighting/depth-copy pipelines.
class RenderGraphDeferredProgram final
{
public:
    RenderGraphDeferredProgram() noexcept = default;
    ~RenderGraphDeferredProgram() noexcept;
    RenderGraphDeferredProgram(const RenderGraphDeferredProgram&) = delete;
    RenderGraphDeferredProgram& operator=(const RenderGraphDeferredProgram&) = delete;
    [[nodiscard]] rhi::RasterStatus Initialize(rhi::DeviceHandle device,
                                               const rhi::RasterShaderDescription& vertex,
                                               const rhi::RasterShaderDescription& lighting,
                                               const rhi::RasterShaderDescription& depth) noexcept;
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;

private:
    friend class RenderGraphDeferredTargets;
    friend class RenderGraphDeferredPass;
    [[nodiscard]] rhi::RasterStatus draw(rhi::RasterPipelineHandle pipeline, rhi::BindingSetHandle bindings) noexcept;
    rhi::DeviceHandle mDevice{};
    rhi::RasterShaderHandle mVertex{};
    rhi::RasterShaderHandle mLighting{};
    rhi::RasterShaderHandle mDepth{};
    rhi::BindingLayoutHandle mLayout{};
    rhi::BindingLayoutHandle mDepthLayout{};
    rhi::BufferHandle mVertices{};
    rhi::BufferHandle mIndices{};
    rhi::BufferHandle mLtcTable{};
    rhi::RasterPipelineHandle mPipeline{};
    rhi::RasterPipelineHandle mDepthPipeline{};
    rhi::RasterStatus mStatus = rhi::RasterStatus::NotReady;
    rhi::GpuScopeDescription mGeometryScope{};
    rhi::GpuScopeDescription mLightingScope{};
    rhi::GpuScopeDescription mDepthScope{};
    rhi::GpuScopeDescription mOverlayScope{};
    bool mIsInitialized = false;
    bool mHasPipelines = false;
};
/// One fixed-extent G-buffer, shared across immutable scene/camera generations.
class RenderGraphDeferredTargets final
{
public:
    RenderGraphDeferredTargets() noexcept = default;
    ~RenderGraphDeferredTargets() noexcept;
    RenderGraphDeferredTargets(const RenderGraphDeferredTargets&) = delete;
    RenderGraphDeferredTargets& operator=(const RenderGraphDeferredTargets&) = delete;
    [[nodiscard]] rhi::RasterStatus Initialize(RenderGraphDeferredProgram& inoutProgram,
                                               rhi::TextureHandle output,
                                               foundation::uint32 width,
                                               foundation::uint32 height) noexcept;
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;

private:
    friend class RenderGraphDeferredPass;
    RenderGraphDeferredProgram* mProgramOrNull = nullptr;
    foundation::StaticArray<rhi::TextureHandle, DEFERRED_TARGET_COUNT> mTextures{};
    foundation::StaticArray<rhi::TextureViewHandle, DEFERRED_TARGET_COUNT> mSampled{};
    foundation::StaticArray<rhi::TextureViewHandle, DEFERRED_TARGET_COUNT> mAttachments{};
    rhi::TextureViewHandle mDepth{};
    rhi::TextureViewHandle mOutputDepth{};
    rhi::TextureHandle mOutput{};
    rhi::BindingSetHandle mDepthBindings{};
    foundation::uint32 mWidth = 0;
    foundation::uint32 mHeight = 0;
    bool mHasViews = false;
    bool mHasBindings = false;
    rhi::RasterStatus mStatus = rhi::RasterStatus::NotReady;
};
/// Per-view camera uniform and common light bindings; owns no mutable scene data.
class RenderGraphDeferredPass final
{
public:
    RenderGraphDeferredPass() noexcept = default;
    ~RenderGraphDeferredPass() noexcept;
    RenderGraphDeferredPass(const RenderGraphDeferredPass&) = delete;
    RenderGraphDeferredPass& operator=(const RenderGraphDeferredPass&) = delete;
    [[nodiscard]] rhi::RasterStatus Initialize(RenderGraphDeferredTargets& inoutTargets,
                                               Renderer& inoutRenderer,
                                               PreparedView view,
                                               const ViewDescription& camera,
                                               rhi::RasterBindingResource meshLights,
                                               foundation::uint32 meshLightCount) noexcept;
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    [[nodiscard]] rhi::RasterStatus
    Draw(Renderer& inoutRenderer, PreparedView view, const rhi::RasterPassDescription& description) noexcept;

private:
    RenderGraphDeferredTargets* mTargetsOrNull = nullptr;
    rhi::BufferHandle mParameters{};
    rhi::BindingSetHandle mBindings{};
    DeferredMaterialBindings mMaterial;
    rhi::RasterBindingResource mMeshLights{};
    bool mHasBindings = false;
    foundation::float32 mClearDepth = 1;
    rhi::RasterStatus mStatus = rhi::RasterStatus::NotReady;
};
} // namespace ludus::graphics::renderer::internal
