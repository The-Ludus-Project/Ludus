#pragma once

#include "render_graph_atmosphere.h"

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/graphics/renderer/deferred_layout.h>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/gpu_scope.h>

namespace ludus::graphics::renderer::internal
{
/// Skin diffusion targets of one view: radiance without skin diffuse, skin diffuse and its horizontal blur.
inline constexpr foundation::uint32 DEFERRED_SKIN_TARGET_COUNT = 3;
/// Borrowed immutable resources from the graph's common scene-lighting material.
struct DeferredMaterialBindings final
{
    foundation::StaticArray<rhi::RasterBindingResource, CLUSTERED_AUTHORED_BINDING_COUNT> Resources{};
    bool HasMaterial = false;
    /// An opaque packet of the view draws skin with a nonzero scattering distance, so the view runs the skin
    /// diffusion passes.
    bool HasSkin = false;
    /// Scattering distance in metres (xyz) of each skin profile slot, in slot order; free slots keep stale values,
    /// which no pixel references.
    foundation::StaticArray<foundation::math::Vector4, DEFERRED_MAX_SKIN_PROFILES> SkinProfiles{};
};
/// Skin diffusion programs a test draws in place of the built-in renderer_skin_blur and renderer_skin_composite
/// recipes, such as their retained Slang originals (LUSL L5d wave 2).
struct SkinDiffusionShaders final
{
    rhi::RasterShaderDescription Vertex{};    ///< Fullscreen vertex stage shared by every skin stage.
    rhi::RasterShaderDescription Blur{};      ///< Horizontal blur fragment.
    rhi::RasterShaderDescription Composite{}; ///< Vertical blur and composite fragment.
};
/// Test builds only (LUDUS_RENDERER_TEST_READBACK): deferred programs whose skin resources are created after this call
/// draw with shadersOrNull's programs instead of the built-in recipes; nullptr restores the recipes. The programs must
/// keep the recipes' binding slots (renderer_skin_composite_slots.h). The descriptions are copied; the artifacts they
/// borrow must outlive the programs. Owner thread only, between graph preparations. Without offline shader tools
/// (LUDUS_RENDERER_SKIN_SHADERS undefined) the renderer has no skin programs and the call does nothing.
void SetSkinDiffusionShadersForTesting(const SkinDiffusionShaders* shadersOrNull) noexcept;
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
    /// Create the skin diffusion shaders, kernel and pipelines on first use, then poll them. Unsupported, and stays
    /// so, in builds without the renderer's built-in shaders; a view with skin then shades it as metallic/roughness.
    [[nodiscard]] rhi::RasterStatus PrepareSkin() noexcept;

private:
    friend class RenderGraphDeferredTargets;
    friend class RenderGraphDeferredPass;
    /// Built-in skin stages: the two resolve variants, the horizontal blur and the vertical blur with composite.
    enum class SkinStage : foundation::uint8
    {
        Lighting,
        AtmosphereLighting,
        Blur,
        Composite,
        Count,
    };
    struct SkinStageResources final
    {
        rhi::RasterShaderHandle Shader{};
        rhi::BindingLayoutHandle Layout{};
        rhi::RasterPipelineHandle Pipeline{};
        foundation::BorrowedView<const rhi::RasterBinding> Bindings;
    };
    [[nodiscard]] rhi::RasterStatus draw(rhi::RasterPipelineHandle pipeline, rhi::BindingSetHandle bindings) noexcept;
    [[nodiscard]] const SkinStageResources& getSkinStage(SkinStage stage) const noexcept;
    RenderGraphAtmosphereProgram mAtmosphere;
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
    rhi::GpuScopeDescription mSkinBlurScope{};
    rhi::GpuScopeDescription mSkinCompositeScope{};
    foundation::StaticArray<SkinStageResources, static_cast<foundation::usize>(SkinStage::Count)> mSkinStages{};
    rhi::RasterShaderHandle mSkinVertex{};
    /// Uniform SkinDiffusionKernel words, shared by every view.
    rhi::BufferHandle mSkinKernel{};
    rhi::RasterStatus mSkinStatus = rhi::RasterStatus::NotReady;
    bool mIsInitialized = false;
    bool mHasPipelines = false;
    bool mHasSkinResources = false;
    bool mHasSkinPipelines = false;
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
    /// Allocate the skin diffusion targets on first use, then poll them: the resolve's radiance without skin diffuse,
    /// the skin diffuse and its horizontal blur, each RGBA16F with private depth at the G-buffer's extent.
    [[nodiscard]] rhi::RasterStatus PrepareSkin() noexcept;
    /// Logical color plus private depth bytes of the skin targets; zero until PrepareSkin allocates them.
    [[nodiscard]] foundation::uint64 GetSkinAttachmentBytes() const noexcept;

private:
    friend class RenderGraphDeferredPass;
    /// Skin targets, in the order the passes write them.
    enum class SkinTarget : foundation::uint8
    {
        Radiance,
        Diffuse,
        Blurred,
        Count,
    };
    static constexpr foundation::usize SKIN_TARGET_COUNT = static_cast<foundation::usize>(SkinTarget::Count);
    static_assert(SKIN_TARGET_COUNT == DEFERRED_SKIN_TARGET_COUNT);
    RenderGraphDeferredProgram* mProgramOrNull = nullptr;
    foundation::StaticArray<rhi::TextureHandle, DEFERRED_TARGET_COUNT> mTextures{};
    foundation::StaticArray<rhi::TextureViewHandle, DEFERRED_TARGET_COUNT> mSampled{};
    foundation::StaticArray<rhi::TextureViewHandle, DEFERRED_TARGET_COUNT> mAttachments{};
    rhi::TextureViewHandle mDepth{};
    rhi::TextureViewHandle mOutputDepth{};
    rhi::TextureHandle mOutput{};
    rhi::BindingSetHandle mDepthBindings{};
    foundation::StaticArray<rhi::TextureHandle, SKIN_TARGET_COUNT> mSkinTextures{};
    foundation::StaticArray<rhi::TextureViewHandle, SKIN_TARGET_COUNT> mSkinSampled{};
    foundation::StaticArray<rhi::TextureViewHandle, SKIN_TARGET_COUNT> mSkinAttachments{};
    foundation::uint32 mWidth = 0;
    foundation::uint32 mHeight = 0;
    bool mHasViews = false;
    bool mHasBindings = false;
    bool mHasSkinTextures = false;
    bool mHasSkinViews = false;
    rhi::RasterStatus mStatus = rhi::RasterStatus::NotReady;
    rhi::RasterStatus mSkinStatus = rhi::RasterStatus::NotReady;
};
/// One-based indices into the resolve's packed light array, zero for none.
struct DeferredResolveLights final
{
    /// The directional light an Atmosphere lights as its Sun.
    foundation::uint32 AtmosphereSunIndex = 0;
    /// The light ViewDescription::LightingView isolates.
    foundation::uint32 LightingViewLightIndex = 0;
    foundation::uint32 NearTableWord = 0; ///< Zero or the first per-light near record in the scene storage buffer.
};
/// Per-view camera uniform and common light bindings; owns no mutable scene data. The operation's
/// ToneMap exposure and curve, when nonzero, are applied by the lighting resolve before RGBA8 output. A view with skin
/// resolves into the targets' skin targets, spreads the skin diffuse horizontally, then vertically while compositing
/// and tone mapping into the output (renderer_skin_diffusion.luslh).
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
                                               foundation::uint32 meshLightCount,
                                               foundation::uint32 meshLightGridWord,
                                               bool hasShadowAtlas,
                                               rhi::TextureViewHandle shadowAtlas,
                                               const content::render_graph_format::Pass& operation,
                                               const DeferredResolveLights& lights) noexcept;
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    [[nodiscard]] rhi::RasterStatus
    Draw(Renderer& inoutRenderer, PreparedView view, const rhi::RasterPassDescription& description) noexcept;

private:
    [[nodiscard]] rhi::RasterStatus pollSkin() noexcept;
    [[nodiscard]] rhi::RasterStatus drawSkin(const rhi::RasterPassDescription& lighting) noexcept;
    RenderGraphDeferredTargets* mTargetsOrNull = nullptr;
    rhi::BufferHandle mParameters{};
    rhi::BindingSetHandle mBindings{};
    rhi::RasterPipelineHandle mLightingPipeline{};
    DeferredMaterialBindings mMaterial;
    RenderGraphAtmosphereView mAtmosphere;
    ViewDescription mCamera{};
    foundation::float32 mExposure = 0;
    foundation::uint32 mToneCurve = 0;
    rhi::RasterBindingResource mMeshLights{};
    /// Sampled directional and mesh-light shadow atlas bound in place of the unused material base-colour texture, when
    /// present.
    rhi::TextureViewHandle mShadowAtlas{};
    bool mHasShadowAtlas = false;

    bool mHasBindings = false;
    /// Bindings of the skin resolve variant, blur and composite, and the selected resolve variant.
    rhi::BindingSetHandle mSkinLightingBindings{};
    rhi::BindingSetHandle mSkinBlurBindings{};
    rhi::BindingSetHandle mSkinCompositeBindings{};
    rhi::RasterPipelineHandle mSkinLightingPipeline{};
    /// Whole-target resolve bindings by slot, kept to select the skin variant's subset.
    foundation::StaticArray<rhi::RasterBindingResource, DEFERRED_BINDING_COUNT> mResolveResources{};
    /// Per-view skin profile table, SkinProfiles of the view's material bindings, while the view spreads skin.
    rhi::BufferHandle mSkinProfiles{};
    /// The view draws skin in its final colour, so it prepares the skin passes.
    bool mShouldSpreadSkin = false;
    bool mHasSkin = false;
    bool mHasSkinBindings = false;
    /// The resolve uses the atmosphere's lighting variant, so the skin resolve uses its atmosphere variant too.
    bool mIsAtmosphereLit = false;
    foundation::float32 mClearDepth = 1;
    rhi::RasterStatus mStatus = rhi::RasterStatus::NotReady;
};
} // namespace ludus::graphics::renderer::internal
