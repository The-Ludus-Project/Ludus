#include "internal/render_graph_deferred.h"

#include "../shaders/renderer_atmosphere_layout.h"
#include "internal/camera_inverse.h"
#include "internal/ltc_table_data.h"
#include "internal/skin_diffusion.h"

#include <ludus/content/render_graph_format/plan.h>

#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/tone_mapping.hpp>

#include <cstddef>

#if defined(LUDUS_RENDERER_SKIN_SHADERS)
#    include "renderer_atmosphere_skin_deferred.h"
#    include "renderer_skin_blur.h"
#    include "renderer_skin_composite.h"
#    include "renderer_skin_deferred.h"
#endif
#include "renderer_skin_blur_slots.h"
#include "renderer_skin_composite_slots.h"

namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace
{
using Status = rhi::RasterStatus;
constexpr usize QUAD_VERTEX_COUNT = 4;
constexpr usize QUAD_INDEX_COUNT = 6;
constexpr usize QUAD_STREAM_COUNT = 1;
constexpr usize QUAD_VERTEX_BYTES = QUAD_VERTEX_COUNT * sizeof(math::Vector4);
constexpr usize QUAD_INDEX_BYTES = QUAD_INDEX_COUNT * sizeof(uint32);
constexpr usize VIEW_BYTES = static_cast<usize>(DEFERRED_VIEW_WORDS) * sizeof(math::Vector4);
constexpr usize ALPHA_CHANNEL = 3;
/// The skin resolve writes the radiance without skin diffuse and the skin diffuse.
constexpr usize SKIN_LIGHTING_TARGET_COUNT = 2;
constexpr usize SKIN_STAGE_COUNT = 4;
/// Column of a homogeneous transform that holds its translation.
constexpr usize TRANSLATION_COLUMN = 3;
namespace skin_slots = ludus::shaders::renderer_skin_composite::slots;
namespace blur_slots = ludus::shaders::renderer_skin_blur::slots;
// The skin recipes declare every binding in one shared order (renderer_skin_diffusion.luslh), so one binding table
// serves the blur and the composite.
static_assert(blur_slots::SKIN_SOURCE_TEXTURE == skin_slots::SKIN_SOURCE_TEXTURE &&
              blur_slots::SKIN_RADIANCE_TEXTURE == skin_slots::SKIN_RADIANCE_TEXTURE &&
              blur_slots::SKIN_NORMAL_TEXTURE == skin_slots::SKIN_NORMAL_TEXTURE &&
              blur_slots::SKIN_BASE_TEXTURE == skin_slots::SKIN_BASE_TEXTURE &&
              blur_slots::SKIN_DEPTH_TEXTURE == skin_slots::SKIN_DEPTH_TEXTURE &&
              blur_slots::SKIN_VIEW == skin_slots::SKIN_VIEW && blur_slots::SKIN_KERNEL == skin_slots::SKIN_KERNEL &&
              blur_slots::SKIN_PROFILES == skin_slots::SKIN_PROFILES);
/// The profile table is the recipes' last binding.
constexpr usize SKIN_BINDING_COUNT = skin_slots::SKIN_PROFILES + 1;
#if defined(LUDUS_RENDERER_TEST_READBACK) && defined(LUDUS_RENDERER_SKIN_SHADERS)
bool hasSkinShadersForTesting = false;
SkinDiffusionShaders skinShadersForTesting{};
#endif
constexpr float32 ORTHOGRAPHIC_EYE_FLAGS =
    3; // Valid eye plus direction-vector encoding, matching Renderer::PrepareView.
struct ViewParameters final
{
    StaticArray<math::Vector4, DEFERRED_VIEW_ROWS> ClipToWorldRows;
    math::Vector4 EyeWorld;
    math::Vector4 TargetSizePixel; // XY: dimensions in pixels. Z: mesh-light record count. W: culling grid word.
    math::Vector4 Options;
    // X: DeferredLightingView code. Y: one-based isolated light index or zero. Z: work light enabled.
    math::Vector4 LightingView{};
    math::Vector4 AtmosphereRadialUpWorld{};
    math::Vector4 AtmosphereSunWorld{};
    math::Vector4 AtmosphereIlluminance{};
    math::Vector4 AtmosphereCameraWorld{};
    CloudLayerUniform AtmosphereCloudLayer{};
    AuroraLayerUniform AtmosphereAuroraLayer{};
};
math::Matrix4 MakeTranslation(math::Vector3 offsetWorld) noexcept
{
    math::Affine3 translation;
    translation.Translation = offsetWorld;
    return math::ToMatrix4(translation);
}
// One component of a homogeneous transform applied to a point: the dot product of the transform's row with (point, 1).
float32 TransformPointComponent(float32 rowX,
                                float32 rowY,
                                float32 rowZ,
                                float32 rowTranslation,
                                math::Vector3 pointWorld) noexcept
{
    return static_cast<float32>(static_cast<float64>(rowX) * static_cast<float64>(pointWorld.X) +
                                static_cast<float64>(rowY) * static_cast<float64>(pointWorld.Y) +
                                static_cast<float64>(rowZ) * static_cast<float64>(pointWorld.Z) +
                                static_cast<float64>(rowTranslation));
}
} // namespace

// Inverts the view about the camera rather than the world origin. A camera hundreds of metres from the origin with a
// centimetre near plane makes WorldToClip itself ill-conditioned (Bistro's reciprocal condition is about 6e-8), so
// the checked inverse rejects it, although the eye-relative projection WorldToClip * T(eye) is well-conditioned
// (about 2e-3 there). Translating its inverse back gives the same clip-to-world matrix the resolve reads.
[[nodiscard]] bool TryMakeCameraClipToEye(const ViewDescription& camera, math::Matrix4& outClipToEye) noexcept
{
    const math::Vector3 eyeWorld = camera.CameraPosition;
    if (math::IsFinite(eyeWorld) == false)
    {
        return false;
    }
    // Only the translation column changes. It cancels to about zero in w, so it is accumulated in float64.
    const math::Matrix4& worldToClip = camera.WorldToClip;
    math::Matrix4 eyeRelativeToClip = worldToClip;
    eyeRelativeToClip.Columns[TRANSLATION_COLUMN] = {
        TransformPointComponent(worldToClip.Columns[0].X,
                                worldToClip.Columns[1].X,
                                worldToClip.Columns[2].X,
                                worldToClip.Columns[TRANSLATION_COLUMN].X,
                                eyeWorld),
        TransformPointComponent(worldToClip.Columns[0].Y,
                                worldToClip.Columns[1].Y,
                                worldToClip.Columns[2].Y,
                                worldToClip.Columns[TRANSLATION_COLUMN].Y,
                                eyeWorld),
        TransformPointComponent(worldToClip.Columns[0].Z,
                                worldToClip.Columns[1].Z,
                                worldToClip.Columns[2].Z,
                                worldToClip.Columns[TRANSLATION_COLUMN].Z,
                                eyeWorld),
        TransformPointComponent(worldToClip.Columns[0].W,
                                worldToClip.Columns[1].W,
                                worldToClip.Columns[2].W,
                                worldToClip.Columns[TRANSLATION_COLUMN].W,
                                eyeWorld),
    };
    return math::TryInverse(eyeRelativeToClip, outClipToEye) == math::MathStatus::Success;
}
math::Matrix4 MakeCameraClipToEye(const math::Matrix4& clipToWorld, math::Vector3 eyeWorld) noexcept
{
    math::Matrix4 clipToEye = clipToWorld;
    for (math::Vector4& column : clipToEye.Columns)
    {
        const float64 homogeneous = static_cast<float64>(column.W);
        column.X =
            static_cast<float32>(static_cast<float64>(column.X) - static_cast<float64>(eyeWorld.X) * homogeneous);
        column.Y =
            static_cast<float32>(static_cast<float64>(column.Y) - static_cast<float64>(eyeWorld.Y) * homogeneous);
        column.Z =
            static_cast<float32>(static_cast<float64>(column.Z) - static_cast<float64>(eyeWorld.Z) * homogeneous);
    }
    return clipToEye;
}
bool TryMakeCameraClipToWorld(const ViewDescription& camera, math::Matrix4& outClipToWorld) noexcept
{
    math::Matrix4 clipToEyeRelative;
    if (TryMakeCameraClipToEye(camera, clipToEyeRelative) == false)
    {
        return false;
    }
    const math::Matrix4 clipToWorld = MakeTranslation(camera.CameraPosition) * clipToEyeRelative;
    if (math::IsFinite(clipToWorld) == false)
    {
        return false;
    }
    outClipToWorld = clipToWorld;
    return true;
}
namespace
{
static_assert(offsetof(ViewParameters, AtmosphereRadialUpWorld) == VIEW_BYTES);
bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
/// Bytes per pixel of one RGBA16F attachment and its private float32 depth, as the graph's DeferredAttachmentBytes.
constexpr uint64 ATTACHMENT_BYTES_PER_PIXEL = 4 * sizeof(uint16) + sizeof(float32);
/// Pick, in a shader's reflected binding order, the resources it declares from a table indexed by slot.
Status SelectBindings(BorrowedView<const rhi::RasterBinding> declared,
                      BorrowedView<const rhi::RasterBindingResource> bySlot,
                      StaticArray<rhi::RasterBindingResource, DEFERRED_BINDING_COUNT>& outSelected,
                      usize& outCount) noexcept
{
    outCount = 0;
    for (const rhi::RasterBinding& binding : declared)
    {
        if (outCount == outSelected.GetSize() || binding.Binding >= bySlot.GetSize())
        {
            return Status::InvalidDescription;
        }
        outSelected[outCount++] = bySlot[binding.Binding];
    }
    return Status::Ready;
}
} // namespace

#if defined(LUDUS_RENDERER_TEST_READBACK)
void SetSkinDiffusionShadersForTesting([[maybe_unused]] const SkinDiffusionShaders* shadersOrNull) noexcept
{
    // Without offline shader tools the renderer builds no skin programs, so there is nothing to override.
#    if defined(LUDUS_RENDERER_SKIN_SHADERS)
    hasSkinShadersForTesting = shadersOrNull != nullptr;
    skinShadersForTesting = shadersOrNull != nullptr ? *shadersOrNull : SkinDiffusionShaders{};
#    endif
}
#endif
RenderGraphDeferredProgram::~RenderGraphDeferredProgram() noexcept
{
    for (SkinStageResources& stage : mSkinStages)
    {
        static_cast<void>(rhi::Destroy(mDevice, stage.Pipeline));
        static_cast<void>(rhi::Destroy(mDevice, stage.Layout));
        static_cast<void>(rhi::Destroy(mDevice, stage.Shader));
    }
    static_cast<void>(rhi::Destroy(mDevice, mSkinVertex));
    static_cast<void>(rhi::Destroy(mDevice, mSkinKernel));
    static_cast<void>(rhi::Destroy(mDevice, mPipeline));
    static_cast<void>(rhi::Destroy(mDevice, mDepthPipeline));
    static_cast<void>(rhi::Destroy(mDevice, mLayout));
    static_cast<void>(rhi::Destroy(mDevice, mDepthLayout));
    static_cast<void>(rhi::Destroy(mDevice, mVertices));
    static_cast<void>(rhi::Destroy(mDevice, mIndices));
    static_cast<void>(rhi::Destroy(mDevice, mLtcTable));
    static_cast<void>(rhi::Destroy(mDevice, mVertex));
    static_cast<void>(rhi::Destroy(mDevice, mLighting));
    static_cast<void>(rhi::Destroy(mDevice, mDepth));
}
Status RenderGraphDeferredProgram::Initialize(rhi::DeviceHandle device,
                                              const rhi::RasterShaderDescription& vertex,
                                              const rhi::RasterShaderDescription& lighting,
                                              const rhi::RasterShaderDescription& depth) noexcept
{
    if (mIsInitialized == true || lighting.Bindings.GetSize() != DEFERRED_BINDING_COUNT ||
        depth.Bindings.GetSize() != 1)
    {
        return Status::InvalidDescription;
    }
    constexpr uint64 GEOMETRY_SCOPE = content::render_graph_format::MAX_PASSES + 1;
    constexpr uint64 LIGHTING_SCOPE = GEOMETRY_SCOPE + 1;
    constexpr uint64 DEPTH_SCOPE = LIGHTING_SCOPE + 1;
    constexpr uint64 OVERLAY_SCOPE = DEPTH_SCOPE + 1;
    constexpr uint64 SKIN_BLUR_SCOPE = OVERLAY_SCOPE + 1;
    constexpr uint64 SKIN_COMPOSITE_SCOPE = SKIN_BLUR_SCOPE + 1;
    constexpr char GEOMETRY_NAME[] = "Deferred/Material";
    constexpr char LIGHTING_NAME[] = "Deferred/Lighting";
    constexpr char DEPTH_NAME[] = "Deferred/PrepassDepth";
    constexpr char OVERLAY_NAME[] = "Deferred/Overlays";
    constexpr char SKIN_BLUR_NAME[] = "Deferred/SkinBlur";
    constexpr char SKIN_COMPOSITE_NAME[] = "Deferred/SkinComposite";
    if (rhi::MakeGpuScope(GEOMETRY_SCOPE, {GEOMETRY_NAME, sizeof(GEOMETRY_NAME) - 1}, mGeometryScope) == false ||
        rhi::MakeGpuScope(LIGHTING_SCOPE, {LIGHTING_NAME, sizeof(LIGHTING_NAME) - 1}, mLightingScope) == false ||
        rhi::MakeGpuScope(DEPTH_SCOPE, {DEPTH_NAME, sizeof(DEPTH_NAME) - 1}, mDepthScope) == false ||
        rhi::MakeGpuScope(OVERLAY_SCOPE, {OVERLAY_NAME, sizeof(OVERLAY_NAME) - 1}, mOverlayScope) == false ||
        rhi::MakeGpuScope(SKIN_BLUR_SCOPE, {SKIN_BLUR_NAME, sizeof(SKIN_BLUR_NAME) - 1}, mSkinBlurScope) == false ||
        rhi::MakeGpuScope(SKIN_COMPOSITE_SCOPE,
                          {SKIN_COMPOSITE_NAME, sizeof(SKIN_COMPOSITE_NAME) - 1},
                          mSkinCompositeScope) == false)
    {
        return Status::InvalidDescription;
    }
    mIsInitialized = true;
    mDevice = device;
    Status status = rhi::CreateRasterShader(device, vertex, mVertex);
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateRasterShader(device, lighting, mLighting);
    }
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateRasterShader(device, depth, mDepth);
    }
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateBindingLayout(device, lighting.Bindings, mLayout);
    }
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateBindingLayout(device, depth.Bindings, mDepthLayout);
    }
    const StaticArray<math::Vector4, QUAD_VERTEX_COUNT> vertices{
        {{-1, 1, 0, 0}, {1, 1, 1, 0}, {1, -1, 1, 1}, {-1, -1, 0, 1}}};
    const StaticArray<uint32, QUAD_INDEX_COUNT> indices{{0, 1, 2, 2, 3, 0}};
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateBuffer(device,
                                   {rhi::BufferRole::Vertex, QUAD_VERTEX_BYTES},
                                   {reinterpret_cast<const uint8*>(vertices.GetData()), QUAD_VERTEX_BYTES},
                                   mVertices);
    }
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateBuffer(device,
                                   {rhi::BufferRole::Index32, QUAD_INDEX_BYTES},
                                   {reinterpret_cast<const uint8*>(indices.GetData()), QUAD_INDEX_BYTES},
                                   mIndices);
    }
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateBuffer(device,
                                   {rhi::BufferRole::Storage, sizeof(LTC_TABLE)},
                                   {reinterpret_cast<const uint8*>(LTC_TABLE.GetData()), sizeof(LTC_TABLE)},
                                   mLtcTable);
    }
    mStatus = status;
    return IsAccepted(status) == true ? Poll() : status;
}
Status RenderGraphDeferredProgram::Poll() noexcept
{
    if (mIsInitialized == false || IsAccepted(mStatus) == false)
    {
        return mStatus;
    }
    constexpr usize DEPENDENCY_COUNT = 8;
    const StaticArray<Status, DEPENDENCY_COUNT> dependencies{{rhi::GetStatus(mDevice, mVertex),
                                                              rhi::GetStatus(mDevice, mLighting),
                                                              rhi::GetStatus(mDevice, mDepth),
                                                              rhi::GetStatus(mDevice, mLayout),
                                                              rhi::GetStatus(mDevice, mDepthLayout),
                                                              rhi::GetStatus(mDevice, mVertices),
                                                              rhi::GetStatus(mDevice, mIndices),
                                                              rhi::GetStatus(mDevice, mLtcTable)}};
    for (const Status status : dependencies)
    {
        if (status != Status::Ready)
        {
            return mStatus = status;
        }
    }
    if (mHasPipelines == false)
    {
        const StaticArray<rhi::RasterVertexStream, QUAD_STREAM_COUNT> streams{{{sizeof(math::Vector4), false}}};
        const StaticArray<rhi::RasterVertexAttribute, QUAD_STREAM_COUNT> attributes{
            {{0, 0, 0, rhi::RasterVertexFormat::Float4}}};
        rhi::RasterPipelineDescription description{mVertex,
                                                   mLighting,
                                                   mLayout,
                                                   {streams.GetData(), streams.GetSize()},
                                                   {attributes.GetData(), attributes.GetSize()}};
        description.Target = rhi::RasterTarget::Rgba8Unorm;
        description.Depth = true;
        description.DepthCompare = rhi::RasterDepthCompare::Always;
        Status status = rhi::CreateRasterPipeline(mDevice, description, mPipeline);
        if (IsAccepted(status) == true)
        {
            description.Fragment = mDepth;
            description.Layout = mDepthLayout;
            description.Target = rhi::RasterTarget::Rgba16Float;
            description.ShouldWriteColor = false;
            status = rhi::CreateRasterPipeline(mDevice, description, mDepthPipeline);
        }
        if (IsAccepted(status) == false)
        {
            return mStatus = status;
        }
        mHasPipelines = true;
    }
    const Status status = rhi::GetStatus(mDevice, mPipeline);
    return mStatus = status == Status::Ready ? rhi::GetStatus(mDevice, mDepthPipeline) : status;
}
Status RenderGraphDeferredProgram::PrepareSkin() noexcept
{
    if (mIsInitialized == false || mStatus != Status::Ready)
    {
        return Status::InvalidState;
    }
    static_assert(SKIN_STAGE_COUNT == static_cast<usize>(SkinStage::Count));
    if (mHasSkinResources == false)
    {
        mHasSkinResources = true;
#if defined(LUDUS_RENDERER_SKIN_SHADERS)
        StaticArray<rhi::RasterShaderDescription, SKIN_STAGE_COUNT> descriptions{
            {shaders::renderer_skin_deferred::Fragment(),
             shaders::renderer_atmosphere_skin_deferred::Fragment(),
             shaders::renderer_skin_blur::Fragment(),
             shaders::renderer_skin_composite::Fragment()}};
        rhi::RasterShaderDescription vertex = shaders::renderer_skin_blur::Vertex();
#    if defined(LUDUS_RENDERER_TEST_READBACK)
        if (hasSkinShadersForTesting == true)
        {
            vertex = skinShadersForTesting.Vertex;
            descriptions[static_cast<usize>(SkinStage::Blur)] = skinShadersForTesting.Blur;
            descriptions[static_cast<usize>(SkinStage::Composite)] = skinShadersForTesting.Composite;
        }
#    endif
        Status status = rhi::CreateRasterShader(mDevice, vertex, mSkinVertex);
        for (usize index = 0; index < SKIN_STAGE_COUNT && IsAccepted(status) == true; ++index)
        {
            SkinStageResources& stage = mSkinStages[index];
            stage.Bindings = descriptions[index].Bindings;
            status = rhi::CreateRasterShader(mDevice, descriptions[index], stage.Shader);
            if (IsAccepted(status) == true)
            {
                status = rhi::CreateBindingLayout(mDevice, descriptions[index].Bindings, stage.Layout);
            }
        }
        if (IsAccepted(status) == true)
        {
            const SkinDiffusionKernel kernel = MakeSkinDiffusionKernel();
            status = rhi::CreateBuffer(mDevice,
                                       {rhi::BufferRole::Uniform, sizeof(kernel.Words)},
                                       {reinterpret_cast<const uint8*>(kernel.Words.GetData()), sizeof(kernel.Words)},
                                       mSkinKernel);
        }
        mSkinStatus = status;
#else
        // Builds without offline shader tools have no built-in skin shaders.
        mSkinStatus = Status::Unsupported;
#endif
    }
    if (IsAccepted(mSkinStatus) == false)
    {
        return mSkinStatus;
    }
    Status ready = rhi::GetStatus(mDevice, mSkinVertex);
    ready = ready == Status::Ready ? rhi::GetStatus(mDevice, mSkinKernel) : ready;
    for (const SkinStageResources& stage : mSkinStages)
    {
        ready = ready == Status::Ready ? rhi::GetStatus(mDevice, stage.Shader) : ready;
        ready = ready == Status::Ready ? rhi::GetStatus(mDevice, stage.Layout) : ready;
    }
    if (ready != Status::Ready)
    {
        return mSkinStatus = ready;
    }
    if (mHasSkinPipelines == false)
    {
        const StaticArray<rhi::RasterVertexStream, QUAD_STREAM_COUNT> streams{{{sizeof(math::Vector4), false}}};
        const StaticArray<rhi::RasterVertexAttribute, QUAD_STREAM_COUNT> attributes{
            {{0, 0, 0, rhi::RasterVertexFormat::Float4}}};
        static constexpr StaticArray<rhi::RasterFormat, SKIN_LIGHTING_TARGET_COUNT> LIGHTING_FORMATS{
            {rhi::RasterFormat::Rgba16Float, rhi::RasterFormat::Rgba16Float}};
        Status status = Status::Ready;
        for (usize index = 0; index < SKIN_STAGE_COUNT && IsAccepted(status) == true; ++index)
        {
            SkinStageResources& stage = mSkinStages[index];
            rhi::RasterPipelineDescription description{mSkinVertex,
                                                       stage.Shader,
                                                       stage.Layout,
                                                       {streams.GetData(), streams.GetSize()},
                                                       {attributes.GetData(), attributes.GetSize()}};
            const SkinStage kind = static_cast<SkinStage>(index);
            description.Target =
                kind == SkinStage::Composite ? rhi::RasterTarget::Rgba8Unorm : rhi::RasterTarget::Rgba16Float;
            // The resolve and the blur draw into explicit attachment views, which need explicit formats; the
            // composite draws into the graph output like the ordinary resolve.
            if (kind == SkinStage::Lighting || kind == SkinStage::AtmosphereLighting)
            {
                description.ColorFormats = {LIGHTING_FORMATS.GetData(), LIGHTING_FORMATS.GetSize()};
            }
            else if (kind == SkinStage::Blur)
            {
                description.ColorFormats = {LIGHTING_FORMATS.GetData(), 1};
            }
            // The composite copies covered G-buffer depth into the graph output, as the ordinary resolve does.
            description.Depth = kind == SkinStage::Composite;
            description.DepthCompare = rhi::RasterDepthCompare::Always;
            status = rhi::CreateRasterPipeline(mDevice, description, stage.Pipeline);
        }
        if (IsAccepted(status) == false)
        {
            return mSkinStatus = status;
        }
        mHasSkinPipelines = true;
    }
    for (const SkinStageResources& stage : mSkinStages)
    {
        const Status pipeline = rhi::GetStatus(mDevice, stage.Pipeline);
        if (pipeline != Status::Ready)
        {
            return mSkinStatus = pipeline;
        }
    }
    return mSkinStatus = Status::Ready;
}
const RenderGraphDeferredProgram::SkinStageResources&
RenderGraphDeferredProgram::getSkinStage(SkinStage stage) const noexcept
{
    return mSkinStages[static_cast<usize>(stage)];
}
Status RenderGraphDeferredProgram::draw(rhi::RasterPipelineHandle pipeline, rhi::BindingSetHandle bindings) noexcept
{
    const StaticArray<rhi::RasterVertexSlice, QUAD_STREAM_COUNT> vertices{{{mVertices, 0, QUAD_VERTEX_BYTES}}};
    return rhi::DrawIndexed(mDevice,
                            {pipeline,
                             bindings,
                             {vertices.GetData(), vertices.GetSize()},
                             mIndices,
                             0,
                             QUAD_INDEX_COUNT,
                             QUAD_VERTEX_COUNT,
                             1});
}

RenderGraphDeferredTargets::~RenderGraphDeferredTargets() noexcept
{
    if (mProgramOrNull == nullptr)
    {
        return;
    }
    const rhi::DeviceHandle device = mProgramOrNull->mDevice;
    static_cast<void>(rhi::Destroy(device, mDepthBindings));
    static_cast<void>(rhi::Destroy(device, mDepth));
    static_cast<void>(rhi::Destroy(device, mOutputDepth));
    for (usize index = 0; index < SKIN_TARGET_COUNT; ++index)
    {
        static_cast<void>(rhi::Destroy(device, mSkinSampled[index]));
        static_cast<void>(rhi::Destroy(device, mSkinAttachments[index]));
        static_cast<void>(rhi::Destroy(device, mSkinTextures[index]));
    }
    for (usize index = 0; index < DEFERRED_TARGET_COUNT; ++index)
    {
        static_cast<void>(rhi::Destroy(device, mSampled[index]));
        static_cast<void>(rhi::Destroy(device, mAttachments[index]));
        static_cast<void>(rhi::Destroy(device, mTextures[index]));
    }
}
Status RenderGraphDeferredTargets::Initialize(RenderGraphDeferredProgram& inoutProgram,
                                              rhi::TextureHandle output,
                                              uint32 width,
                                              uint32 height) noexcept
{
    if (mProgramOrNull != nullptr || inoutProgram.mStatus != Status::Ready)
    {
        return Status::InvalidState;
    }
    mProgramOrNull = &inoutProgram;
    mOutput = output;
    mWidth = width;
    mHeight = height;
    const rhi::TextureDescription description{width, height, rhi::RasterFormat::Rgba16Float, true};
    Status status = Status::Ready;
    for (usize index = 0; index < DEFERRED_TARGET_COUNT && IsAccepted(status) == true; ++index)
    {
        status = rhi::CreateTexture(inoutProgram.mDevice, description, {}, mTextures[index]);
    }
    mStatus = status;
    return IsAccepted(status) == true ? Poll() : status;
}
Status RenderGraphDeferredTargets::Poll() noexcept
{
    if (mProgramOrNull == nullptr || IsAccepted(mStatus) == false)
    {
        return mStatus;
    }
    const rhi::DeviceHandle device = mProgramOrNull->mDevice;
    for (const rhi::TextureHandle texture : mTextures)
    {
        const Status status = rhi::GetStatus(device, texture);
        if (status != Status::Ready)
        {
            return mStatus = status;
        }
    }
    if (mHasViews == false)
    {
        Status status = Status::Ready;
        for (usize index = 0; index < DEFERRED_TARGET_COUNT && IsAccepted(status) == true; ++index)
        {
            status = rhi::CreateTextureView(device, mTextures[index], mSampled[index]);
            if (IsAccepted(status) == true)
            {
                rhi::TextureViewDescription attachment;
                attachment.Attachment = true;
                status = rhi::CreateTextureView(device, mTextures[index], attachment, mAttachments[index]);
            }
        }
        rhi::TextureViewDescription depth;
        depth.Aspect = rhi::TextureAspect::Depth;
        if (IsAccepted(status) == true)
        {
            status = rhi::CreateTextureView(device, mTextures[0], depth, mDepth);
        }
        if (IsAccepted(status) == true)
        {
            status = rhi::CreateTextureView(device, mOutput, depth, mOutputDepth);
        }
        if (IsAccepted(status) == false)
        {
            return mStatus = status;
        }
        mHasViews = true;
    }
    for (usize index = 0; index < DEFERRED_TARGET_COUNT; ++index)
    {
        const Status sampled = rhi::GetStatus(device, mSampled[index]);
        const Status attachment = rhi::GetStatus(device, mAttachments[index]);
        if (sampled != Status::Ready || attachment != Status::Ready)
        {
            return mStatus = sampled != Status::Ready ? sampled : attachment;
        }
    }
    const Status depth = rhi::GetStatus(device, mDepth);
    const Status outputDepth = rhi::GetStatus(device, mOutputDepth);
    if (depth != Status::Ready || outputDepth != Status::Ready)
    {
        return mStatus = depth != Status::Ready ? depth : outputDepth;
    }
    if (mHasBindings == false)
    {
        const rhi::RasterBindingResource resource{ .Binding = 0, .Texture = mOutputDepth };
        const Status status =
            rhi::CreateBindingSet(device, mProgramOrNull->mDepthLayout, {&resource, 1}, mDepthBindings);
        if (IsAccepted(status) == false)
        {
            return mStatus = status;
        }
        mHasBindings = true;
    }
    return mStatus = rhi::GetStatus(device, mDepthBindings);
}

Status RenderGraphDeferredTargets::PrepareSkin() noexcept
{
    if (mProgramOrNull == nullptr || mStatus != Status::Ready)
    {
        return Status::InvalidState;
    }
    const rhi::DeviceHandle device = mProgramOrNull->mDevice;
    if (mHasSkinTextures == false)
    {
        mHasSkinTextures = true;
        const rhi::TextureDescription description{mWidth, mHeight, rhi::RasterFormat::Rgba16Float, true};
        Status status = Status::Ready;
        for (usize index = 0; index < SKIN_TARGET_COUNT && IsAccepted(status) == true; ++index)
        {
            status = rhi::CreateTexture(device, description, {}, mSkinTextures[index]);
        }
        mSkinStatus = status;
    }
    if (IsAccepted(mSkinStatus) == false)
    {
        return mSkinStatus;
    }
    for (const rhi::TextureHandle texture : mSkinTextures)
    {
        const Status status = rhi::GetStatus(device, texture);
        if (status != Status::Ready)
        {
            return mSkinStatus = status;
        }
    }
    if (mHasSkinViews == false)
    {
        Status status = Status::Ready;
        for (usize index = 0; index < SKIN_TARGET_COUNT && IsAccepted(status) == true; ++index)
        {
            status = rhi::CreateTextureView(device, mSkinTextures[index], mSkinSampled[index]);
            if (IsAccepted(status) == true)
            {
                rhi::TextureViewDescription attachment;
                attachment.Attachment = true;
                status = rhi::CreateTextureView(device, mSkinTextures[index], attachment, mSkinAttachments[index]);
            }
        }
        if (IsAccepted(status) == false)
        {
            return mSkinStatus = status;
        }
        mHasSkinViews = true;
    }
    for (usize index = 0; index < SKIN_TARGET_COUNT; ++index)
    {
        const Status sampled = rhi::GetStatus(device, mSkinSampled[index]);
        const Status attachment = rhi::GetStatus(device, mSkinAttachments[index]);
        if (sampled != Status::Ready || attachment != Status::Ready)
        {
            return mSkinStatus = sampled != Status::Ready ? sampled : attachment;
        }
    }
    return mSkinStatus = Status::Ready;
}
uint64 RenderGraphDeferredTargets::GetSkinAttachmentBytes() const noexcept
{
    return mHasSkinTextures == true ? uint64{mWidth} * mHeight * SKIN_TARGET_COUNT * ATTACHMENT_BYTES_PER_PIXEL : 0;
}

RenderGraphDeferredPass::~RenderGraphDeferredPass() noexcept
{
    if (mTargetsOrNull != nullptr)
    {
        const rhi::DeviceHandle device = mTargetsOrNull->mProgramOrNull->mDevice;
        static_cast<void>(rhi::Destroy(device, mBindings));
        static_cast<void>(rhi::Destroy(device, mSkinLightingBindings));
        static_cast<void>(rhi::Destroy(device, mSkinBlurBindings));
        static_cast<void>(rhi::Destroy(device, mSkinCompositeBindings));
        static_cast<void>(rhi::Destroy(device, mParameters));
        static_cast<void>(rhi::Destroy(device, mSkinProfiles));
    }
}
Status RenderGraphDeferredPass::Initialize(RenderGraphDeferredTargets& inoutTargets,
                                           Renderer& inoutRenderer,
                                           PreparedView view,
                                           const ViewDescription& camera,
                                           rhi::RasterBindingResource meshLights,
                                           uint32 meshLightCount,
                                           uint32 meshLightGridWord,
                                           bool hasShadowAtlas,
                                           rhi::TextureViewHandle shadowAtlas,
                                           const content::render_graph_format::Pass& operation,
                                           const DeferredResolveLights& lights) noexcept
{
    if (mTargetsOrNull != nullptr || inoutTargets.mStatus != Status::Ready)
    {
        return Status::InvalidState;
    }
    Status status = inoutRenderer.getDeferredMaterial(view, mMaterial);
    if (status != Status::Ready)
    {
        return status;
    }
    const AtmosphereDescription& atmosphere = camera.Atmosphere;
    const bool isCelestialBackgroundOnly =
        mMaterial.HasMaterial == false && atmosphere.IsEnabled == true && atmosphere.Celestial.IsEnabled == true;
    math::Matrix4 clipToWorld{};
    if (isCelestialBackgroundOnly == false && TryMakeCameraClipToWorld(camera, clipToWorld) == false)
    {
        return Status::InvalidDescription;
    }
    mTargetsOrNull = &inoutTargets;
    mMeshLights = meshLights;
    mHasShadowAtlas = hasShadowAtlas;
    mShadowAtlas = shadowAtlas;
    mClearDepth = camera.Depth == DepthConvention::ReverseZ ? 0.0F : 1.0F;
    ViewParameters parameters;
    const math::Matrix4 rows = math::Transpose(clipToWorld);
    for (usize row = 0; row < DEFERRED_VIEW_ROWS; ++row)
    {
        parameters.ClipToWorldRows[row] = rows.Columns[row];
    }
    parameters.EyeWorld = {camera.CameraPosition.X, camera.CameraPosition.Y, camera.CameraPosition.Z, 1};
    if (camera.OrthographicCamera == true)
    {
        math::Vector3 forwardWorld;
        if (math::TryNormalize(camera.CameraForward, forwardWorld) != math::MathStatus::Success)
        {
            return Status::InvalidDescription;
        }
        parameters.EyeWorld = {-forwardWorld.X, -forwardWorld.Y, -forwardWorld.Z, ORTHOGRAPHIC_EYE_FLAGS};
    }
    parameters.TargetSizePixel = {static_cast<float32>(inoutTargets.mWidth),
                                  static_cast<float32>(inoutTargets.mHeight),
                                  static_cast<float32>(meshLightCount),
                                  static_cast<float32>(meshLightGridWord)};
    mCamera = camera;
    // A zero multiplier marks a disabled ToneMap, whose raw linear output physical exposure must not change.
    const float32 baseExposure = camera.Exposure == ExposureMode::Physical && operation.ToneMappingExposure > 0
                                     ? ComputeExposureMultiplier(camera.ExposureValue100)
                                     : operation.ToneMappingExposure;
    mExposure = baseExposure * static_cast<float32>(math::Pow(2, static_cast<float64>(camera.RasterExposureStops)));
    mToneCurve = static_cast<uint32>(
        operation.ToneMappingCurve == content::render_graph_format::DisplayCurve::PbrNeutral ? ToneCurve::PbrNeutral
                                                                                             : ToneCurve::Reinhard);
    parameters.Options = {
        camera.Reference == true || camera.DirectLighting == true ? 1.0F : 0.0F,
        mClearDepth,
        mExposure,
        static_cast<float32>(mToneCurve),
    };
    const bool shouldShowWorkLight = camera.ShouldShowWorkLight == true && camera.Reference == false &&
                                     camera.LightingView == DeferredLightingView::Final;
    parameters.LightingView = {static_cast<float32>(camera.LightingView),
                               static_cast<float32>(lights.LightingViewLightIndex),
                               shouldShowWorkLight == true ? 1.0F : 0.0F,
                               static_cast<float32>(lights.NearTableWord)};
    if (atmosphere.IsEnabled == true && atmosphere.ShouldLightScene == true)
    {
        constexpr float64 METRES_PER_KILOMETRE = LUDUS_ATMOSPHERE_METRES_PER_KILOMETRE;
        math::Vector3 upWorld;
        math::Vector3 sunWorld;
        float64 altitude = 0;
        if (TryGetAtmosphereObserver(atmosphere, camera.CameraPosition, altitude, upWorld) == false ||
            math::TryNormalize(atmosphere.DirectionToSunWorld, sunWorld) != math::MathStatus::Success)
        {
            return mStatus = Status::InvalidDescription;
        }
        parameters.AtmosphereRadialUpWorld = {upWorld.X,
                                              upWorld.Y,
                                              upWorld.Z,
                                              static_cast<float32>(altitude / METRES_PER_KILOMETRE)};
        parameters.AtmosphereSunWorld = {sunWorld.X,
                                         sunWorld.Y,
                                         sunWorld.Z,
                                         static_cast<float32>(lights.AtmosphereSunIndex)};
        parameters.AtmosphereIlluminance = {atmosphere.SolarIlluminance.X,
                                            atmosphere.SolarIlluminance.Y,
                                            atmosphere.SolarIlluminance.Z,
                                            0};
        parameters.AtmosphereCameraWorld = {camera.CameraPosition.X,
                                            camera.CameraPosition.Y,
                                            camera.CameraPosition.Z,
                                            camera.OrthographicCamera == true ? 1.0F : 0.0F};
        if (TryPackCloudLayer(atmosphere, camera, parameters.AtmosphereCloudLayer) == false ||
            TryPackAuroraLayer(atmosphere,
                               camera.OrthographicCamera,
                               altitude > GetAtmosphereTopAltitude(),
                               parameters.AtmosphereAuroraLayer) == false)
        {
            return mStatus = Status::InvalidDescription;
        }
    }
    status = rhi::CreateBuffer(inoutTargets.mProgramOrNull->mDevice,
                               {rhi::BufferRole::Uniform, sizeof(parameters)},
                               {reinterpret_cast<const uint8*>(&parameters), sizeof(parameters)},
                               mParameters);
    // Lighting views draw single terms through the ordinary resolve, so only the final colour spreads skin.
    mShouldSpreadSkin = mMaterial.HasSkin == true && camera.LightingView == DeferredLightingView::Final;
    if (IsAccepted(status) == true && mShouldSpreadSkin == true)
    {
        const StaticArray<math::Vector4, DEFERRED_MAX_SKIN_PROFILES>& profiles = mMaterial.SkinProfiles;
        status = rhi::CreateBuffer(inoutTargets.mProgramOrNull->mDevice,
                                   {rhi::BufferRole::Uniform, sizeof(profiles)},
                                   {reinterpret_cast<const uint8*>(profiles.GetData()), sizeof(profiles)},
                                   mSkinProfiles);
    }
    mStatus = status;
    return IsAccepted(status) == true ? Poll() : status;
}
Status RenderGraphDeferredPass::Poll() noexcept
{
    if (mTargetsOrNull == nullptr || IsAccepted(mStatus) == false)
    {
        return mStatus;
    }
    RenderGraphDeferredProgram& program = *mTargetsOrNull->mProgramOrNull;
    const Status ready = rhi::GetStatus(program.mDevice, mParameters);
    if (ready != Status::Ready)
    {
        return mStatus = ready;
    }
    const Status atmosphere = mAtmosphere.Prepare(program.mAtmosphere,
                                                  program.mDevice,
                                                  mCamera,
                                                  mTargetsOrNull->mWidth,
                                                  mTargetsOrNull->mHeight,
                                                  mExposure,
                                                  mToneCurve);
    if (atmosphere != Status::Ready || mMaterial.HasMaterial == false)
    {
        return mStatus = atmosphere;
    }
    if (mHasBindings == false)
    {
        StaticArray<rhi::RasterBindingResource, DEFERRED_BINDING_COUNT> bindings{};
        for (usize slot = 0; slot < CLUSTERED_AUTHORED_BINDING_COUNT; ++slot)
        {
            bindings[slot] = mMaterial.Resources[slot];
        }
        bindings[DEFERRED_MESH_LIGHT_BINDING] = mMeshLights;
        if (mHasShadowAtlas == true)
        {
            bindings[DEFERRED_MESH_LIGHT_SHADOW_BINDING] =
            {
                .Binding = DEFERRED_MESH_LIGHT_SHADOW_BINDING,
                .Texture = mShadowAtlas,
            };
        }
        bindings[DEFERRED_LTC_BINDING] =
        {
            .Binding = DEFERRED_LTC_BINDING,
            .Buffer = program.mLtcTable,
            .Size = sizeof(LTC_TABLE),
        };
        for (usize target = 0; target < DEFERRED_TARGET_COUNT; ++target)
        {
            const uint32 slot = DEFERRED_NORMAL_BINDING + static_cast<uint32>(target);
            bindings[slot].Binding = slot;
            bindings[slot].Texture = mTargetsOrNull->mSampled[target];
        }
        bindings[DEFERRED_DEPTH_BINDING] = { .Binding = DEFERRED_DEPTH_BINDING, .Texture = mTargetsOrNull->mDepth };
        bindings[DEFERRED_VIEW_BINDING] =
        {
            .Binding = DEFERRED_VIEW_BINDING,
            .Buffer = mParameters,
            .Size = VIEW_BYTES,
        };
        AtmosphereLightingResources atmosphereResources;
        rhi::BindingLayoutHandle layout = program.mLayout;
        mIsAtmosphereLit = false;
        mLightingPipeline = program.mPipeline;
        usize bindingCount = bindings.GetSize();
        if (mAtmosphere.TryGetLightingResources(atmosphereResources) == true)
        {
            constexpr uint32 MULTIPLE_SLOT = LUDUS_ATMOSPHERE_DEFERRED_MULTIPLE_SLOT;
            constexpr uint32 SKY_SLOT = LUDUS_ATMOSPHERE_DEFERRED_SKY_SLOT;
            constexpr uint32 TRANSMITTANCE_SLOT = LUDUS_ATMOSPHERE_DEFERRED_TRANSMITTANCE_SLOT;
            bindings[MULTIPLE_SLOT] = { .Binding = MULTIPLE_SLOT, .Texture = atmosphereResources.Multiple };
            bindings[SKY_SLOT] = { .Binding = SKY_SLOT, .Texture = atmosphereResources.Sky };
            bindings[TRANSMITTANCE_SLOT] =
            {
                .Binding = TRANSMITTANCE_SLOT,
                .Texture = atmosphereResources.Transmittance,
            };
            bindings[DEFERRED_VIEW_BINDING].Size = sizeof(ViewParameters);
            mIsAtmosphereLit = true;
            StaticArray<rhi::RasterBindingResource, DEFERRED_BINDING_COUNT> selected{};
            bindingCount = 0;
            for (const rhi::RasterBinding& binding : atmosphereResources.Bindings)
            {
                if (bindingCount == selected.GetSize() || binding.Binding >= bindings.GetSize())
                {
                    return mStatus = Status::InvalidDescription;
                }
                selected[bindingCount++] = bindings[binding.Binding];
            }
            mResolveResources = bindings;
            bindings = selected;
            layout = atmosphereResources.Layout;
            mLightingPipeline = atmosphereResources.Pipeline;
        }
        if (mIsAtmosphereLit == false)
        {
            mResolveResources = bindings;
        }
        const Status status =
            rhi::CreateBindingSet(program.mDevice, layout, {bindings.GetData(), bindingCount}, mBindings);
        if (IsAccepted(status) == false)
        {
            return mStatus = status;
        }
        mHasBindings = true;
    }
    const Status bindings = rhi::GetStatus(program.mDevice, mBindings);
    if (bindings != Status::Ready || mShouldSpreadSkin == false)
    {
        return mStatus = bindings;
    }
    return mStatus = pollSkin();
}
Status RenderGraphDeferredPass::pollSkin() noexcept
{
    using Stage = RenderGraphDeferredProgram::SkinStage;
    RenderGraphDeferredProgram& program = *mTargetsOrNull->mProgramOrNull;
    RenderGraphDeferredTargets& targets = *mTargetsOrNull;
    const Status prepared = program.PrepareSkin();
    if (prepared == Status::Unsupported)
    {
        // Builds without the built-in shaders shade skin as its metallic/roughness surface.
        mHasSkin = false;
        return Status::Ready;
    }
    if (prepared != Status::Ready)
    {
        return prepared;
    }
    const Status allocated = targets.PrepareSkin();
    if (allocated != Status::Ready)
    {
        return allocated;
    }
    const Status profiles = rhi::GetStatus(program.mDevice, mSkinProfiles);
    if (profiles != Status::Ready)
    {
        return profiles;
    }
    if (mHasSkinBindings == false)
    {
        const RenderGraphDeferredProgram::SkinStageResources& lighting =
            program.getSkinStage(mIsAtmosphereLit == true ? Stage::AtmosphereLighting : Stage::Lighting);
        StaticArray<rhi::RasterBindingResource, DEFERRED_BINDING_COUNT> selected{};
        usize count = 0;
        Status status = SelectBindings(lighting.Bindings,
                                       {mResolveResources.GetData(), mResolveResources.GetSize()},
                                       selected,
                                       count);
        if (status == Status::Ready)
        {
            status = rhi::CreateBindingSet(program.mDevice,
                                           lighting.Layout,
                                           {selected.GetData(), count},
                                           mSkinLightingBindings);
        }
        mSkinLightingPipeline = lighting.Pipeline;
        constexpr usize RADIANCE = static_cast<usize>(RenderGraphDeferredTargets::SkinTarget::Radiance);
        constexpr usize DIFFUSE = static_cast<usize>(RenderGraphDeferredTargets::SkinTarget::Diffuse);
        constexpr usize BLURRED = static_cast<usize>(RenderGraphDeferredTargets::SkinTarget::Blurred);
        // G-buffer targets follow their resolve slots from DEFERRED_NORMAL_BINDING.
        constexpr usize NORMAL_TARGET = 0;
        constexpr usize BASE_TARGET = DEFERRED_BASE_BINDING - DEFERRED_NORMAL_BINDING;
        StaticArray<rhi::RasterBindingResource, SKIN_BINDING_COUNT> diffusion{};
        diffusion[skin_slots::SKIN_SOURCE_TEXTURE] =
        {
            .Binding = skin_slots::SKIN_SOURCE_TEXTURE,
            .Texture = targets.mSkinSampled[DIFFUSE],
        };
        diffusion[skin_slots::SKIN_RADIANCE_TEXTURE] =
        {
            .Binding = skin_slots::SKIN_RADIANCE_TEXTURE,
            .Texture = targets.mSkinSampled[RADIANCE],
        };
        diffusion[skin_slots::SKIN_NORMAL_TEXTURE] =
        {
            .Binding = skin_slots::SKIN_NORMAL_TEXTURE,
            .Texture = targets.mSampled[NORMAL_TARGET],
        };
        diffusion[skin_slots::SKIN_BASE_TEXTURE] =
        {
            .Binding = skin_slots::SKIN_BASE_TEXTURE,
            .Texture = targets.mSampled[BASE_TARGET],
        };
        diffusion[skin_slots::SKIN_PROFILES] =
        {
            .Binding = skin_slots::SKIN_PROFILES,
            .Buffer = mSkinProfiles,
            .Size = sizeof(DeferredMaterialBindings::SkinProfiles),
        };
        diffusion[skin_slots::SKIN_DEPTH_TEXTURE] =
        {
            .Binding = skin_slots::SKIN_DEPTH_TEXTURE,
            .Texture = targets.mDepth,
        };
        diffusion[skin_slots::SKIN_VIEW] =
        {
            .Binding = skin_slots::SKIN_VIEW,
            .Buffer = mParameters,
            .Size = VIEW_BYTES,
        };
        diffusion[skin_slots::SKIN_KERNEL] =
        {
            .Binding = skin_slots::SKIN_KERNEL,
            .Buffer = program.mSkinKernel,
            .Size = sizeof(SkinDiffusionKernel::Words),
        };
        const RenderGraphDeferredProgram::SkinStageResources& blur = program.getSkinStage(Stage::Blur);
        if (status == Status::Ready)
        {
            status = SelectBindings(blur.Bindings, {diffusion.GetData(), diffusion.GetSize()}, selected, count);
        }
        if (status == Status::Ready)
        {
            status =
                rhi::CreateBindingSet(program.mDevice, blur.Layout, {selected.GetData(), count}, mSkinBlurBindings);
        }
        // The composite spreads the horizontal blur vertically.
        diffusion[skin_slots::SKIN_SOURCE_TEXTURE].Texture = targets.mSkinSampled[BLURRED];
        const RenderGraphDeferredProgram::SkinStageResources& composite = program.getSkinStage(Stage::Composite);
        if (IsAccepted(status) == true)
        {
            status = SelectBindings(composite.Bindings, {diffusion.GetData(), diffusion.GetSize()}, selected, count);
        }
        if (status == Status::Ready)
        {
            status = rhi::CreateBindingSet(program.mDevice,
                                           composite.Layout,
                                           {selected.GetData(), count},
                                           mSkinCompositeBindings);
        }
        if (IsAccepted(status) == false)
        {
            return status;
        }
        mHasSkinBindings = true;
    }
    const StaticArray<rhi::BindingSetHandle, SKIN_STAGE_COUNT - 1> bindingSets{
        {mSkinLightingBindings, mSkinBlurBindings, mSkinCompositeBindings}};
    for (const rhi::BindingSetHandle bindings : bindingSets)
    {
        const Status status = rhi::GetStatus(program.mDevice, bindings);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    mHasSkin = true;
    return Status::Ready;
}
Status RenderGraphDeferredPass::drawSkin(const rhi::RasterPassDescription& lighting) noexcept
{
    using Stage = RenderGraphDeferredProgram::SkinStage;
    RenderGraphDeferredTargets& targets = *mTargetsOrNull;
    RenderGraphDeferredProgram& program = *targets.mProgramOrNull;
    constexpr usize RADIANCE = static_cast<usize>(RenderGraphDeferredTargets::SkinTarget::Radiance);
    constexpr usize DIFFUSE = static_cast<usize>(RenderGraphDeferredTargets::SkinTarget::Diffuse);
    constexpr usize BLURRED = static_cast<usize>(RenderGraphDeferredTargets::SkinTarget::Blurred);
    Status status = Status::Ready;
    {
        const rhi::GpuScope scope(program.mDevice, program.mLightingScope);
        StaticArray<rhi::RasterColorAttachment, SKIN_LIGHTING_TARGET_COUNT> colors{};
        colors[0].View = targets.mSkinAttachments[RADIANCE];
        colors[1].View = targets.mSkinAttachments[DIFFUSE];
        for (rhi::RasterColorAttachment& color : colors)
        {
            color.Clear[ALPHA_CHANNEL] = 0;
        }
        rhi::RasterPassDescription resolve;
        resolve.Colors = colors.GetData();
        resolve.ColorCount = colors.GetSize();
        status = rhi::BeginRasterPass(program.mDevice, resolve);
        if (status == Status::Ready)
        {
            status = program.draw(mSkinLightingPipeline, mSkinLightingBindings);
        }
    }
    if (status == Status::Ready)
    {
        const rhi::GpuScope scope(program.mDevice, program.mSkinBlurScope);
        rhi::RasterColorAttachment color;
        color.View = targets.mSkinAttachments[BLURRED];
        color.Clear[ALPHA_CHANNEL] = 0;
        rhi::RasterPassDescription blur;
        blur.Colors = &color;
        blur.ColorCount = 1;
        status = rhi::BeginRasterPass(program.mDevice, blur);
        if (status == Status::Ready)
        {
            status = program.draw(program.getSkinStage(Stage::Blur).Pipeline, mSkinBlurBindings);
        }
    }
    if (status == Status::Ready)
    {
        const rhi::GpuScope scope(program.mDevice, program.mSkinCompositeScope);
        const rhi::RasterVertexSlice fullscreenVertices{program.mVertices, 0, QUAD_VERTEX_BYTES};
        status = rhi::BeginRasterPass(program.mDevice, lighting);
        if (status == Status::Ready)
        {
            status = mAtmosphere.RecordSky(fullscreenVertices, program.mIndices);
        }
        if (status == Status::Ready)
        {
            status = program.draw(program.getSkinStage(Stage::Composite).Pipeline, mSkinCompositeBindings);
        }
    }
    return status;
}
Status RenderGraphDeferredPass::Draw(Renderer& inoutRenderer,
                                     PreparedView view,
                                     const rhi::RasterPassDescription& description) noexcept
{
    if (mStatus != Status::Ready || mTargetsOrNull == nullptr)
    {
        return Status::NotReady;
    }
    RenderGraphDeferredTargets& targets = *mTargetsOrNull;
    RenderGraphDeferredProgram& program = *targets.mProgramOrNull;
    const rhi::RasterVertexSlice fullscreenVertices{program.mVertices, 0, QUAD_VERTEX_BYTES};
    const Status tables = mAtmosphere.RecordTables(fullscreenVertices, program.mIndices);
    if (tables != Status::Ready)
    {
        return tables;
    }
    if (description.DepthLoad == rhi::RasterLoad::Load)
    {
        const rhi::GpuScope scope(program.mDevice, program.mDepthScope);
        rhi::RasterPassDescription copy;
        copy.Color = targets.mTextures[0];
        copy.ColorLoad = rhi::RasterLoad::Discard;
        copy.ColorStore = rhi::RasterStore::Discard;
        copy.DepthStore = rhi::RasterStore::Store;
        Status status = rhi::BeginRasterPass(program.mDevice, copy);
        if (status == Status::Ready)
        {
            status = program.draw(program.mDepthPipeline, targets.mDepthBindings);
        }
        if (status != Status::Ready)
        {
            return status;
        }
    }
    StaticArray<rhi::RasterColorAttachment, DEFERRED_TARGET_COUNT> colors{};
    for (usize target = 0; target < DEFERRED_TARGET_COUNT; ++target)
    {
        colors[target].View = targets.mAttachments[target];
        colors[target].Clear[ALPHA_CHANNEL] = 0;
    }
    rhi::RasterPassDescription geometry;
    geometry.Colors = colors.GetData();
    geometry.ColorCount = colors.GetSize();
    geometry.DepthLoad = description.DepthLoad;
    geometry.DepthStore = rhi::RasterStore::Store;
    Status status = Status::NotReady;
    {
        const rhi::GpuScope scope(program.mDevice, program.mGeometryScope);
        status = inoutRenderer.drawView(view, geometry, Renderer::DrawMode::GBuffer);
    }
    rhi::RasterPassDescription lighting = description;
    lighting.ClearDepth = mClearDepth;
    if (status == Status::Ready && mHasSkin == true)
    {
        status = drawSkin(lighting);
    }
    else
    {
        const rhi::GpuScope scope(program.mDevice, program.mLightingScope);
        if (status == Status::Ready)
        {
            status = rhi::BeginRasterPass(program.mDevice, lighting);
        }
        if (status == Status::Ready)
        {
            status = mAtmosphere.RecordSky(fullscreenVertices, program.mIndices);
        }
        if (status == Status::Ready && mMaterial.HasMaterial == true)
        {
            status = program.draw(mLightingPipeline, mBindings);
        }
    }
    if (status != Status::Ready)
    {
        return status;
    }
    lighting.ColorLoad = rhi::RasterLoad::Load;
    lighting.DepthLoad = rhi::RasterLoad::Load;
    const rhi::GpuScope scope(program.mDevice, program.mOverlayScope);
    return inoutRenderer.drawView(view, lighting, Renderer::DrawMode::Overlays);
}
} // namespace ludus::graphics::renderer::internal
