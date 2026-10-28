#include "internal/render_graph_deferred.h"

#include <ludus/content/render_graph_format/plan.h>

#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>

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
constexpr float32 ORTHOGRAPHIC_EYE_FLAGS =
    3; // Valid eye plus direction-vector encoding, matching Renderer::PrepareView.
struct ViewParameters final
{
    StaticArray<math::Vector4, DEFERRED_VIEW_ROWS> ClipToWorldRows;
    math::Vector4 EyeWorld;
    math::Vector4 TargetSizePixel;
    math::Vector4 Options;
};
static_assert(sizeof(ViewParameters) == VIEW_BYTES);
bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
} // namespace

RenderGraphDeferredProgram::~RenderGraphDeferredProgram() noexcept
{
    static_cast<void>(rhi::Destroy(mDevice, mPipeline));
    static_cast<void>(rhi::Destroy(mDevice, mDepthPipeline));
    static_cast<void>(rhi::Destroy(mDevice, mLayout));
    static_cast<void>(rhi::Destroy(mDevice, mDepthLayout));
    static_cast<void>(rhi::Destroy(mDevice, mVertices));
    static_cast<void>(rhi::Destroy(mDevice, mIndices));
    static_cast<void>(rhi::Destroy(mDevice, mVertex));
    static_cast<void>(rhi::Destroy(mDevice, mLighting));
    static_cast<void>(rhi::Destroy(mDevice, mDepth));
}
Status RenderGraphDeferredProgram::Initialize(rhi::DeviceHandle device,
                                              const rhi::RasterShaderDescription& vertex,
                                              const rhi::RasterShaderDescription& lighting,
                                              const rhi::RasterShaderDescription& depth) noexcept
{
    if (mIsInitialized == true || lighting.Bindings.size() != DEFERRED_BINDING_COUNT || depth.Bindings.size() != 1)
    {
        return Status::InvalidDescription;
    }
    constexpr uint64 GEOMETRY_SCOPE = content::render_graph_format::MAX_PASSES + 1;
    constexpr uint64 LIGHTING_SCOPE = GEOMETRY_SCOPE + 1;
    constexpr uint64 DEPTH_SCOPE = LIGHTING_SCOPE + 1;
    constexpr uint64 OVERLAY_SCOPE = DEPTH_SCOPE + 1;
    constexpr char GEOMETRY_NAME[] = "Deferred/Material";
    constexpr char LIGHTING_NAME[] = "Deferred/Lighting";
    constexpr char DEPTH_NAME[] = "Deferred/PrepassDepth";
    constexpr char OVERLAY_NAME[] = "Deferred/Overlays";
    if (rhi::MakeGpuScope(GEOMETRY_SCOPE, {GEOMETRY_NAME, sizeof(GEOMETRY_NAME) - 1}, mGeometryScope) == false ||
        rhi::MakeGpuScope(LIGHTING_SCOPE, {LIGHTING_NAME, sizeof(LIGHTING_NAME) - 1}, mLightingScope) == false ||
        rhi::MakeGpuScope(DEPTH_SCOPE, {DEPTH_NAME, sizeof(DEPTH_NAME) - 1}, mDepthScope) == false ||
        rhi::MakeGpuScope(OVERLAY_SCOPE, {OVERLAY_NAME, sizeof(OVERLAY_NAME) - 1}, mOverlayScope) == false)
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
    mStatus = status;
    return IsAccepted(status) == true ? Poll() : status;
}
Status RenderGraphDeferredProgram::Poll() noexcept
{
    if (mIsInitialized == false || IsAccepted(mStatus) == false)
    {
        return mStatus;
    }
    constexpr usize DEPENDENCY_COUNT = 7;
    const StaticArray<Status, DEPENDENCY_COUNT> dependencies{{rhi::GetStatus(mDevice, mVertex),
                                                              rhi::GetStatus(mDevice, mLighting),
                                                              rhi::GetStatus(mDevice, mDepth),
                                                              rhi::GetStatus(mDevice, mLayout),
                                                              rhi::GetStatus(mDevice, mDepthLayout),
                                                              rhi::GetStatus(mDevice, mVertices),
                                                              rhi::GetStatus(mDevice, mIndices)}};
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
        rhi::RasterPipelineDescription description{mVertex, mLighting, mLayout, streams.AsSpan(), attributes.AsSpan()};
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
Status RenderGraphDeferredProgram::draw(rhi::RasterPipelineHandle pipeline, rhi::BindingSetHandle bindings) noexcept
{
    const StaticArray<rhi::RasterVertexSlice, QUAD_STREAM_COUNT> vertices{{{mVertices, 0, QUAD_VERTEX_BYTES}}};
    return rhi::DrawIndexed(
        mDevice,
        {pipeline, bindings, vertices.AsSpan(), mIndices, 0, QUAD_INDEX_COUNT, QUAD_VERTEX_COUNT, 1});
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

RenderGraphDeferredPass::~RenderGraphDeferredPass() noexcept
{
    if (mTargetsOrNull != nullptr)
    {
        const rhi::DeviceHandle device = mTargetsOrNull->mProgramOrNull->mDevice;
        static_cast<void>(rhi::Destroy(device, mBindings));
        static_cast<void>(rhi::Destroy(device, mParameters));
    }
}
Status RenderGraphDeferredPass::Initialize(RenderGraphDeferredTargets& inoutTargets,
                                           Renderer& inoutRenderer,
                                           PreparedView view,
                                           const ViewDescription& camera) noexcept
{
    if (mTargetsOrNull != nullptr || inoutTargets.mStatus != Status::Ready)
    {
        return Status::InvalidState;
    }
    math::Matrix4 clipToWorld;
    if (math::TryInverse(camera.WorldToClip, clipToWorld) != math::MathStatus::Success)
    {
        return Status::InvalidDescription;
    }
    Status status = inoutRenderer.getDeferredMaterial(view, mMaterial);
    if (status != Status::Ready)
    {
        return status;
    }
    mTargetsOrNull = &inoutTargets;
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
                                  0,
                                  0};
    parameters.Options = {camera.Reference == true || camera.DirectLighting == true ? 1.0F : 0.0F, mClearDepth, 0, 0};
    status = rhi::CreateBuffer(inoutTargets.mProgramOrNull->mDevice,
                               {rhi::BufferRole::Uniform, VIEW_BYTES},
                               {reinterpret_cast<const uint8*>(&parameters), VIEW_BYTES},
                               mParameters);
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
    if (mMaterial.HasMaterial == false)
    {
        return mStatus = Status::Ready;
    }
    if (mHasBindings == false)
    {
        StaticArray<rhi::RasterBindingResource, DEFERRED_BINDING_COUNT> bindings{};
        for (usize slot = 0; slot < CLUSTERED_AUTHORED_BINDING_COUNT; ++slot)
        {
            bindings[slot] = mMaterial.Resources[slot];
        }
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
        const Status status = rhi::CreateBindingSet(program.mDevice, program.mLayout, bindings.AsSpan(), mBindings);
        if (IsAccepted(status) == false)
        {
            return mStatus = status;
        }
        mHasBindings = true;
    }
    return mStatus = rhi::GetStatus(program.mDevice, mBindings);
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
    {
        const rhi::GpuScope scope(program.mDevice, program.mLightingScope);
        if (status == Status::Ready)
        {
            status = rhi::BeginRasterPass(program.mDevice, lighting);
        }
        if (status == Status::Ready && mMaterial.HasMaterial == true)
        {
            status = program.draw(program.mPipeline, mBindings);
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
