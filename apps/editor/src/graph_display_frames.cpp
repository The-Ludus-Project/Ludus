#include "internal/graph_display_frames.h"

#include "renderer_selection_mask.h"
#include "renderer_selection_outline.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::editor
{
using namespace foundation;
namespace rhi = graphics::rhi;
namespace
{
constexpr usize QUAD_VERTEX_COUNT = 4;
constexpr usize QUAD_INDEX_COUNT = 6;
constexpr usize VERTEX_STREAM_COUNT = 1;
constexpr usize VERTEX_ATTRIBUTE_COUNT = 1;
constexpr usize COPY_BINDING_COUNT = 2;
constexpr usize COPY_RESOURCE_COUNT = 4;
constexpr foundation::float32 MINIMUM_STROKE_PIXELS = 1;
constexpr usize BYTES_PER_PIXEL = 4;
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
} // namespace
GraphDisplayFrames::GraphDisplayFrames(QObject* parentOrNull) : ViewportFrameSource(parentOrNull) {}
GraphDisplayFrames::~GraphDisplayFrames()
{
    Reset();
}
void GraphDisplayFrames::Initialize(rhi::DeviceHandle device,
                                    const rhi::RasterShaderDescription& vertex,
                                    const rhi::RasterShaderDescription& fragment) noexcept
{
    LUDUS_REQUIRE(mHasDevice == false);
    mDevice = device;
    mVertexDescription = vertex;
    mFragmentDescription = fragment;
    mHasDevice = true;
    mIsOutlineDisabled = false;
    const rhi::RasterStatus outline = mOutline.Initialize(device,
                                                          shaders::renderer_selection_mask::Vertex(),
                                                          shaders::renderer_selection_mask::Fragment(),
                                                          shaders::renderer_selection_outline::Vertex(),
                                                          shaders::renderer_selection_outline::Fragment());
    mIsOutlineInitialized = IsAccepted(outline);
}
void GraphDisplayFrames::releaseCopyBinding() noexcept
{
    static_cast<void>(rhi::DiscardOrderedGraph(mDevice, mCopyGraph));
    mCopyGraph = {};
    static_cast<void>(rhi::Destroy(mDevice, mBinding));
    static_cast<void>(rhi::Destroy(mDevice, mSourceView));
    mBinding = {};
    mSourceView = {};
}
void GraphDisplayFrames::Reset() noexcept
{
    if (mHasDevice == false)
    {
        return;
    }
    Q_EMIT ResettingFrames();
    releaseCopyBinding();
    mOutline.Reset();
    mIsOutlineInitialized = false;
    mIsOutlineWanted = false;
    mOutlineSceneIdentity.clear();
    mOutlineMesh = -1;
    mClean = -1;
    mComposing = -1;
    mIsComposingCopy = false;
    mCompletedDisplay = -1;
    for (DisplaySlot& slot : mSlots)
    {
        LUDUS_REQUIRE(slot.Readers == 0);
        static_cast<void>(rhi::Destroy(mDevice, slot.Texture));
        slot = {};
    }
    static_cast<void>(rhi::Destroy(mDevice, mSampler));
    static_cast<void>(rhi::Destroy(mDevice, mPipeline));
    static_cast<void>(rhi::Destroy(mDevice, mLayout));
    static_cast<void>(rhi::Destroy(mDevice, mVertex));
    static_cast<void>(rhi::Destroy(mDevice, mFragment));
    static_cast<void>(rhi::Destroy(mDevice, mVertices));
    static_cast<void>(rhi::Destroy(mDevice, mIndices));
    mSampler = {};
    mPipeline = {};
    mLayout = {};
    mVertex = {};
    mFragment = {};
    mVertices = {};
    mIndices = {};
    mCopying = -1;
    mCompleted = -1;
    mLatest = -1;
    mIsPrepared = false;
    mHasPreparationFailed = false;
    mHasDevice = false;
    mIsSubmitted = false;
}
rhi::RasterStatus GraphDisplayFrames::prepareCopy() noexcept
{
    if (mHasPreparationFailed == true)
    {
        return rhi::RasterStatus::Failed;
    }
    if (mIsPrepared == true)
    {
        return rhi::GetStatus(mDevice, mPipeline);
    }
    // The renderer's composite recipe preserves encoded display values without tone mapping.
    const StaticArray<math::Vector4, QUAD_VERTEX_COUNT> vertices{
        {{-1, 1, 0, 0}, {1, 1, 1, 0}, {1, -1, 1, 1}, {-1, -1, 0, 1}}};
    const StaticArray<uint32, QUAD_INDEX_COUNT> indices{{0, 1, 2, 0, 2, 3}};
    const StaticArray<rhi::RasterVertexStream, VERTEX_STREAM_COUNT> streams{{{sizeof(math::Vector4), false}}};
    const StaticArray<rhi::RasterVertexAttribute, VERTEX_ATTRIBUTE_COUNT> attributes{
        {{0, 0, 0, rhi::RasterVertexFormat::Float4}}};
    rhi::RasterStatus status = rhi::CreateRasterShader(mDevice, mVertexDescription, mVertex);
    if (IsAccepted(status) == false)
    {
        mHasPreparationFailed = true;
        return status;
    }
    status = rhi::CreateRasterShader(mDevice, mFragmentDescription, mFragment);
    if (IsAccepted(status) == false)
    {
        mHasPreparationFailed = true;
        return status;
    }
    status = rhi::CreateBindingLayout(mDevice, mFragmentDescription.Bindings, mLayout);
    if (IsAccepted(status) == false)
    {
        mHasPreparationFailed = true;
        return status;
    }
    status = rhi::CreateBuffer(mDevice,
                               {rhi::BufferRole::Vertex, sizeof(vertices)},
                               {reinterpret_cast<const uint8*>(vertices.GetData()), sizeof(vertices)},
                               mVertices);
    if (IsAccepted(status) == false)
    {
        mHasPreparationFailed = true;
        return status;
    }
    status = rhi::CreateBuffer(mDevice,
                               {rhi::BufferRole::Index32, sizeof(indices)},
                               {reinterpret_cast<const uint8*>(indices.GetData()), sizeof(indices)},
                               mIndices);
    if (IsAccepted(status) == false)
    {
        mHasPreparationFailed = true;
        return status;
    }
    status = rhi::CreateSampler(mDevice, {}, mSampler);
    if (IsAccepted(status) == false)
    {
        mHasPreparationFailed = true;
        return status;
    }
    rhi::RasterPipelineDescription description;
    description.Vertex = mVertex;
    description.Fragment = mFragment;
    description.Layout = mLayout;
    description.Streams = {streams.GetData(), streams.GetSize()};
    description.Attributes = {attributes.GetData(), attributes.GetSize()};
    description.Target = rhi::RasterTarget::Rgba8Unorm;
    status = rhi::CreateRasterPipeline(mDevice, description, mPipeline);
    mIsPrepared = IsAccepted(status);
    mHasPreparationFailed = mIsPrepared == false;
    return status;
}
rhi::RasterStatus GraphDisplayFrames::prepareSlotTexture(DisplaySlot& inoutSlot, uint32 width, uint32 height) noexcept
{
    if (inoutSlot.Width == width && inoutSlot.Height == height)
    {
        return rhi::RasterStatus::Ready;
    }
    static_cast<void>(rhi::Destroy(mDevice, inoutSlot.Texture));
    inoutSlot.Texture = {};
    rhi::TextureDescription description;
    description.Width = width;
    description.Height = height;
    description.Attachment = true;
    const rhi::RasterStatus created = rhi::CreateTexture(mDevice, description, {}, inoutSlot.Texture);
    if (IsAccepted(created) == false)
    {
        inoutSlot.Width = 0;
        inoutSlot.Height = 0;
        return created;
    }
    inoutSlot.Width = width;
    inoutSlot.Height = height;
    return rhi::RasterStatus::Ready;
}
usize GraphDisplayFrames::getSlotLimit() const noexcept
{
    return mIsOutlineWanted == true || mClean != mLatest ? DISPLAY_SLOT_COUNT : PLAIN_SLOT_COUNT;
}
int32 GraphDisplayFrames::findFreeSlot() const noexcept
{
    for (usize i = 0; i < getSlotLimit(); ++i)
    {
        const int32 index = static_cast<int32>(i);
        if (mSlots[i].Readers == 0 && index != mLatest && index != mClean && index != mCopying && index != mCompleted &&
            index != mComposing && index != mCompletedDisplay)
        {
            return index;
        }
    }
    return -1;
}
rhi::RasterStatus GraphDisplayFrames::Copy(rhi::SurfaceHandle surface,
                                           rhi::TextureHandle source,
                                           uint32 width,
                                           uint32 height,
                                           const math::Matrix4* worldToClipOrNull) noexcept
{
    if (mHasDevice == false || width == 0 || height == 0)
    {
        return rhi::RasterStatus::InvalidState;
    }
    if (mCopying >= 0 || (mComposing >= 0 && mIsComposingCopy == true))
    {
        return PollCopy();
    }
    if (mCompleted >= 0 || mSerial == ~uint64{0})
    {
        return rhi::RasterStatus::InvalidState;
    }
    if (mComposing >= 0)
    {
        // A recomposition of the shown image finishes first; PollSelectionOutline completes it.
        return rhi::RasterStatus::NotReady;
    }
    const rhi::RasterStatus prepared = prepareCopy();
    if (prepared != rhi::RasterStatus::Ready)
    {
        return prepared == rhi::RasterStatus::Pending ? rhi::RasterStatus::NotReady : prepared;
    }
    for (usize i = 0; i < getSlotLimit(); ++i)
    {
        DisplaySlot& slot = mSlots[i];
        if (slot.Readers != 0 || static_cast<int32>(i) == mLatest || static_cast<int32>(i) == mClean)
        {
            continue;
        }
        const rhi::RasterStatus sized = prepareSlotTexture(slot, width, height);
        if (sized != rhi::RasterStatus::Ready)
        {
            return sized;
        }
        rhi::RasterStatus status = rhi::CreateTextureView(mDevice, source, {}, mSourceView);
        if (status != rhi::RasterStatus::Ready)
        {
            releaseCopyBinding();
            return status == rhi::RasterStatus::Pending ? rhi::RasterStatus::NotReady : status;
        }
        const StaticArray<rhi::RasterBindingResource, COPY_BINDING_COUNT> resources{
            {{ .Texture = mSourceView }, { .Binding = 1, .Sampler = mSampler }}};
        status = rhi::CreateBindingSet(mDevice, mLayout, {resources.GetData(), resources.GetSize()}, mBinding);
        if (status != rhi::RasterStatus::Ready)
        {
            releaseCopyBinding();
            return status == rhi::RasterStatus::Pending ? rhi::RasterStatus::NotReady : status;
        }
        const rhi::RasterVertexSlice vertices{mVertices, 0, sizeof(math::Vector4) * QUAD_VERTEX_COUNT};
        rhi::RasterDraw draw;
        draw.Pipeline = mPipeline;
        draw.Bindings = mBinding;
        draw.Vertices = {&vertices, 1};
        draw.Indices = mIndices;
        draw.VertexCount = QUAD_VERTEX_COUNT;
        draw.IndexCount = QUAD_INDEX_COUNT;
        status = buildCopyGraph(source, slot, draw);
        if (status != rhi::RasterStatus::Ready)
        {
            releaseCopyBinding();
            return status;
        }
        slot.Producer = {};
        slot.HasView = worldToClipOrNull != nullptr;
        slot.WorldToClip = worldToClipOrNull != nullptr ? *worldToClipOrNull : math::Matrix4::Identity();
        mSurface = surface;
        mCopying = static_cast<int32>(i);
        mIsSubmitted = false;
        return PollCopy();
    }
    return rhi::RasterStatus::NotReady;
}
rhi::RasterStatus GraphDisplayFrames::buildCopyGraph(rhi::TextureHandle source,
                                                     const DisplaySlot& destination,
                                                     const rhi::RasterDraw& draw) noexcept
{
    rhi::GraphVersion vertices;
    rhi::GraphVersion indices;
    rhi::GraphVersion input;
    rhi::GraphVersion target;
    rhi::GraphVersion output;
    rhi::RasterTextureState sourceState;
    rhi::RasterTextureState destinationState;
    rhi::RasterStatus status = rhi::CreateOrderedGraph(mDevice, mCopyGraph);
    if (status == rhi::RasterStatus::Ready)
    {
        status = rhi::ImportGraphBuffer(mDevice, mCopyGraph, mVertices, vertices);
    }
    if (status == rhi::RasterStatus::Ready)
    {
        status = rhi::ImportGraphBuffer(mDevice, mCopyGraph, mIndices, indices);
    }
    if (status == rhi::RasterStatus::Ready)
    {
        status = rhi::GetTextureState(mDevice, source, sourceState);
    }
    if (status == rhi::RasterStatus::Ready)
    {
        status = rhi::ImportGraphTexture(mDevice,
                                         mCopyGraph,
                                         source,
                                         sourceState,
                                         rhi::RasterTextureUse::SampledFragment,
                                         input);
    }
    if (status == rhi::RasterStatus::Ready)
    {
        status = rhi::GetTextureState(mDevice, destination.Texture, destinationState);
    }
    if (status == rhi::RasterStatus::Ready)
    {
        status = rhi::ImportGraphTexture(mDevice,
                                         mCopyGraph,
                                         destination.Texture,
                                         destinationState,
                                         rhi::RasterTextureUse::SampledFragment,
                                         target);
    }
    if (status == rhi::RasterStatus::Ready)
    {
        status = rhi::NextGraphVersion(mDevice, mCopyGraph, target, output);
    }
    const StaticArray<rhi::GraphUse, COPY_RESOURCE_COUNT> uses{{
        {
            .Resource = vertices,
            .Access = rhi::GraphAccessMode::Vertex,
            .Size = sizeof(math::Vector4) * QUAD_VERTEX_COUNT,
        },
        { .Resource = indices, .Access = rhi::GraphAccessMode::Index, .Size = sizeof(uint32) * QUAD_INDEX_COUNT },
        { .Resource = input, .Access = rhi::GraphAccessMode::Sampled, .Visibility = rhi::RasterVisibility::Fragment },
        { .Resource = output, .Access = rhi::GraphAccessMode::ColorWrite },
    }};
    if (status == rhi::RasterStatus::Ready)
    {
        const rhi::GraphPassDescription pass
        {
            .Name = "Editor GPU display snapshot",
            .Attachment = { .Color = destination.Texture },
            .Uses = uses.GetData(),
            .UseCount = uses.GetSize(),
            .Draws = &draw,
            .DrawCount = 1,
        };
        status = rhi::AddGraphPass(mDevice, mCopyGraph, pass);
    }
    if (status == rhi::RasterStatus::Ready)
    {
        status = rhi::AddGraphRoot(mDevice, mCopyGraph, output, rhi::GraphRoot::Export);
    }
    if (status == rhi::RasterStatus::Ready)
    {
        rhi::GraphReport report;
        status = rhi::CompileOrderedGraph(mDevice, mCopyGraph, report);
    }
    return status;
}
rhi::RasterStatus GraphDisplayFrames::PollCopy() noexcept
{
    if (mComposing >= 0 && mIsComposingCopy == true)
    {
        const rhi::RasterStatus composed = pollCompose();
        if (composed == rhi::RasterStatus::Pending)
        {
            return composed;
        }
        // A failed composition publishes the clean snapshot; PollSelectionOutline retries or the caller falls back.
        mCompletedDisplay = composed == rhi::RasterStatus::Ready ? mComposing : -1;
        mIsComposingCopy = false;
        mComposing = -1;
        return rhi::RasterStatus::Ready;
    }
    if (mCopying < 0)
    {
        return rhi::RasterStatus::NotReady;
    }
    DisplaySlot& slot = mSlots[static_cast<usize>(mCopying)];
    if (mIsSubmitted == false)
    {
        // Ordered execution reserves submission capacity before encoding. A retry or Reset
        // therefore never inherits an open frame from a failed EndFrame reservation.
        const rhi::RasterStatus submitted = rhi::ExecuteOrderedGraph(mDevice, mSurface, mCopyGraph, slot.Producer);
        if (submitted == rhi::RasterStatus::NotReady || submitted == rhi::RasterStatus::CapacityExceeded ||
            submitted == rhi::RasterStatus::Pending)
        {
            return rhi::RasterStatus::Pending;
        }
        if (submitted != rhi::RasterStatus::Ready)
        {
            releaseCopyBinding();
            mCopying = -1;
            return submitted;
        }
        mIsSubmitted = true;
    }
    const rhi::RasterStatus completed = rhi::GetStatus(mDevice, slot.Producer);
    if (completed == rhi::RasterStatus::Pending)
    {
        return completed;
    }
    releaseCopyBinding();
    if (completed == rhi::RasterStatus::Ready)
    {
        mCompleted = mCopying;
    }
    mCopying = -1;
    mIsSubmitted = false;
    return completed == rhi::RasterStatus::Ready ? startCopyCompose() : completed;
}
void GraphDisplayFrames::PublishCopy() noexcept
{
    LUDUS_REQUIRE(mCompleted >= 0);
    mClean = mCompleted;
    if (mCompletedDisplay >= 0)
    {
        mLatest = mCompletedDisplay;
        mComposedRevision = mComposingRevision;
    }
    else
    {
        mLatest = mCompleted;
    }
    mSlots[static_cast<usize>(mLatest)].Serial = ++mSerial;
    mCompleted = -1;
    mCompletedDisplay = -1;
}
void GraphDisplayFrames::DiscardCopy() noexcept
{
    mCompleted = -1;
    mCompletedDisplay = -1;
}
bool GraphDisplayFrames::IsCopying() const noexcept
{
    return mCopying >= 0;
}
bool GraphDisplayFrames::HasPendingFrame() const noexcept
{
    return mLatest >= 0;
}
uint64 GraphDisplayFrames::GetLatestSerial() const noexcept
{
    return mLatest < 0 ? 0 : mSlots[static_cast<usize>(mLatest)].Serial;
}
bool GraphDisplayFrames::TakeFrame(graphics::renderer::ViewportFrame&, graphics::renderer::ViewportImage&) noexcept
{
    return false;
}
void GraphDisplayFrames::ReleaseFrame(graphics::renderer::ViewportFrame&) noexcept {}
void GraphDisplayFrames::Resize(int32, int32, float32) {}
rhi::RasterStatus GraphDisplayFrames::TakeFrameRead(rhi::NativeDeviceLease device,
                                                    const rhi::NativeDeviceIdentity& consumer,
                                                    ViewportFrameRead& outRead) noexcept
{
    if (mLatest < 0)
    {
        return rhi::RasterStatus::NotReady;
    }
    DisplaySlot& slot = mSlots[static_cast<usize>(mLatest)];
    ViewportFrameRead candidate;
    const rhi::RasterStatus acquired =
        rhi::AcquireNativeTextureRead(device, consumer, slot.Texture, slot.Producer, candidate.Read, candidate.Image);
    if (acquired != rhi::RasterStatus::Ready)
    {
        return acquired;
    }
    ++slot.Readers;
    candidate.Token = slot.Serial;
    candidate.Slot = static_cast<uint32>(mLatest);
    outRead = candidate;
    return rhi::RasterStatus::Ready;
}
void GraphDisplayFrames::releaseRead(ViewportFrameRead& inoutRead) noexcept
{
    LUDUS_REQUIRE(inoutRead.Slot < DISPLAY_SLOT_COUNT);
    DisplaySlot& slot = mSlots[inoutRead.Slot];
    LUDUS_REQUIRE(slot.Serial == inoutRead.Token && slot.Readers > 0);
    --slot.Readers;
    inoutRead = {};
    Q_EMIT CapacityAvailable();
}
rhi::RasterStatus GraphDisplayFrames::EndFrameRead(ViewportFrameRead& inoutRead) noexcept
{
    const rhi::RasterStatus status = rhi::ReleaseNativeTextureRead(inoutRead.Read);
    if (status == rhi::RasterStatus::Ready)
    {
        releaseRead(inoutRead);
    }
    return status;
}
void GraphDisplayFrames::DrainFrameRead(ViewportFrameRead& inoutRead) noexcept
{
    LUDUS_REQUIRE(rhi::DrainNativeTextureRead(inoutRead.Read) == rhi::RasterStatus::Ready);
    releaseRead(inoutRead);
}
bool GraphDisplayFrames::CanDrawSelectionOutline() const noexcept
{
    return mHasDevice == true && mIsOutlineInitialized == true && mIsOutlineDisabled == false;
}
void GraphDisplayFrames::disableSelectionOutline() noexcept
{
    const bool wasAvailable = CanDrawSelectionOutline();
    mIsOutlineDisabled = true;
    mIsOutlineWanted = false;
    mOutline.ClearMesh();
    mOutlineSceneIdentity.clear();
    mOutlineMesh = -1;
    ++mOutlineRevision;
    if (wasAvailable == true)
    {
        Q_EMIT SelectionOutlineAvailabilityChanged();
        Q_EMIT SelectionOutlineWorkRequested();
    }
}
bool GraphDisplayFrames::SetSelectionOutline(const SelectionOutlineRequest& request) noexcept
{
    if (CanDrawSelectionOutline() == false || request.Mesh < 0)
    {
        return false;
    }
    if (request.Mesh == mRejectedMesh && request.SceneIdentity == mRejectedSceneIdentity)
    {
        return false;
    }
    bool isChanged = mIsOutlineWanted == false;
    if (request.Mesh != mOutlineMesh || request.SceneIdentity != mOutlineSceneIdentity)
    {
        const rhi::RasterStatus status = mOutline.SetMesh(request.PositionsMesh, request.Indices);
        if (status == rhi::RasterStatus::InvalidDescription)
        {
            // A mesh without a usable triangle is not an outline failure; the caller draws it another way.
            mRejectedMesh = request.Mesh;
            mRejectedSceneIdentity = request.SceneIdentity;
            return false;
        }
        if (IsAccepted(status) == false)
        {
            disableSelectionOutline();
            return false;
        }
        mOutlineMesh = request.Mesh;
        mOutlineSceneIdentity = request.SceneIdentity;
        isChanged = true;
    }
    if (request.MeshToWorld != mOutlineMeshToWorld || request.StrokeScale != mOutlineStrokeScale ||
        request.OutlineColor != mOutlineStyle.OutlineColor || request.CasingColor != mOutlineStyle.CasingColor ||
        request.OutlineWidth != mOutlineLogicalOutlineWidth || request.CasingWidth != mOutlineLogicalCasingWidth)
    {
        mOutlineMeshToWorld = request.MeshToWorld;
        mOutlineStrokeScale = request.StrokeScale;
        mOutlineStyle.OutlineColor = request.OutlineColor;
        mOutlineStyle.CasingColor = request.CasingColor;
        mOutlineLogicalOutlineWidth = request.OutlineWidth;
        mOutlineLogicalCasingWidth = request.CasingWidth;
        isChanged = true;
    }
    mIsOutlineWanted = true;
    if (isChanged == true)
    {
        ++mOutlineRevision;
        Q_EMIT SelectionOutlineWorkRequested();
    }
    return true;
}
void GraphDisplayFrames::ClearSelectionOutline() noexcept
{
    mRejectedMesh = -1;
    mRejectedSceneIdentity.clear();
    if (mIsOutlineWanted == false)
    {
        return;
    }
    mIsOutlineWanted = false;
    mOutline.ClearMesh();
    mOutlineSceneIdentity.clear();
    mOutlineMesh = -1;
    ++mOutlineRevision;
    Q_EMIT SelectionOutlineWorkRequested();
}
rhi::RasterStatus GraphDisplayFrames::RequestShownReadback(rhi::ReadbackTicket& outTicket) noexcept
{
    if (mHasDevice == false || mLatest < 0)
    {
        return rhi::RasterStatus::NotReady;
    }
    const DisplaySlot& slot = mSlots[static_cast<usize>(mLatest)];
    return rhi::RequestTextureReadback(mDevice,
                                       slot.Texture,
                                       {0, 0, 0, slot.Width, slot.Height},
                                       static_cast<usize>(slot.Width) * BYTES_PER_PIXEL,
                                       outTicket);
}
QSize GraphDisplayFrames::GetShownSize() const noexcept
{
    if (mHasDevice == false || mLatest < 0)
    {
        return {};
    }
    const DisplaySlot& slot = mSlots[static_cast<usize>(mLatest)];
    return {static_cast<int>(slot.Width), static_cast<int>(slot.Height)};
}
bool GraphDisplayFrames::HasPendingSelectionOutline() const noexcept
{
    if (mHasDevice == false)
    {
        return false;
    }
    if (mComposing >= 0)
    {
        return true;
    }
    return mIsOutlineInitialized == true && mLatest >= 0 && mClean >= 0 && mCopying < 0 && mCompleted < 0 &&
           mComposedRevision != mOutlineRevision;
}
rhi::RasterStatus GraphDisplayFrames::beginCompose(int32 cleanIndex, int32 targetIndex) noexcept
{
    DisplaySlot& clean = mSlots[static_cast<usize>(cleanIndex)];
    DisplaySlot& target = mSlots[static_cast<usize>(targetIndex)];
    if (mOutline.IsReady() == false)
    {
        const rhi::RasterStatus setup = mOutline.Poll();
        return IsAccepted(setup) == true ? rhi::RasterStatus::NotReady : setup;
    }
    const rhi::RasterStatus meshStatus = mOutline.GetMeshStatus();
    if (meshStatus != rhi::RasterStatus::Ready)
    {
        return meshStatus == rhi::RasterStatus::Pending ? rhi::RasterStatus::NotReady : meshStatus;
    }
    if (clean.HasView == false || prepareCopy() != rhi::RasterStatus::Ready)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    rhi::RasterStatus status = prepareSlotTexture(target, clean.Width, clean.Height);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    graphics::renderer::SelectionOutlineView view;
    view.MeshToClip = clean.WorldToClip * math::ToMatrix4(mOutlineMeshToWorld);
    view.Width = clean.Width;
    view.Height = clean.Height;
    view.Style = mOutlineStyle;
    const foundation::float32 maximum = graphics::renderer::SelectionOutlineRenderer::MAX_STROKE_WIDTH_PIXELS;
    view.Style.OutlineWidthPixels =
        math::Clamp(mOutlineLogicalOutlineWidth * mOutlineStrokeScale, MINIMUM_STROKE_PIXELS, maximum);
    view.Style.CasingWidthPixels =
        math::Clamp(mOutlineLogicalCasingWidth * mOutlineStrokeScale, view.Style.OutlineWidthPixels, maximum);
    graphics::renderer::PreparedSelectionOutline prepared;
    status = mOutline.Prepare(view, prepared);
    if (IsAccepted(status) == false)
    {
        return status == rhi::RasterStatus::NotReady ? status : rhi::RasterStatus::InvalidDescription;
    }
    if (mOutline.GetStatus(prepared) != rhi::RasterStatus::Ready)
    {
        static_cast<void>(mOutline.Release(prepared));
        return rhi::RasterStatus::NotReady;
    }
    rhi::TextureViewHandle sourceView;
    rhi::BindingSetHandle binding;
    const auto release = [&]() {
        static_cast<void>(mOutline.Release(prepared));
        static_cast<void>(rhi::Destroy(mDevice, binding));
        static_cast<void>(rhi::Destroy(mDevice, sourceView));
    };
    status = rhi::CreateTextureView(mDevice, clean.Texture, {}, sourceView);
    if (status != rhi::RasterStatus::Ready)
    {
        release();
        return status == rhi::RasterStatus::Pending ? rhi::RasterStatus::NotReady : status;
    }
    const StaticArray<rhi::RasterBindingResource, COPY_BINDING_COUNT> resources{
        {{ .Texture = sourceView }, { .Binding = 1, .Sampler = mSampler }}};
    status = rhi::CreateBindingSet(mDevice, mLayout, {resources.GetData(), resources.GetSize()}, binding);
    if (status != rhi::RasterStatus::Ready)
    {
        release();
        return status == rhi::RasterStatus::Pending ? rhi::RasterStatus::NotReady : status;
    }
    if (rhi::BeginFrame(mDevice, mSurface) != rhi::DeviceStatus::Ready)
    {
        release();
        return rhi::RasterStatus::NotReady;
    }
    rhi::RasterPassDescription pass;
    pass.Color = target.Texture;
    pass.DepthLoad = rhi::RasterLoad::Discard;
    pass.DepthStore = rhi::RasterStore::Discard;
    status = rhi::BeginRasterPass(mDevice, pass);
    if (status == rhi::RasterStatus::Ready)
    {
        const rhi::RasterVertexSlice vertices{mVertices, 0, sizeof(math::Vector4) * QUAD_VERTEX_COUNT};
        rhi::RasterDraw draw;
        draw.Pipeline = mPipeline;
        draw.Bindings = binding;
        draw.Vertices = {&vertices, 1};
        draw.Indices = mIndices;
        draw.VertexCount = QUAD_VERTEX_COUNT;
        draw.IndexCount = QUAD_INDEX_COUNT;
        status = rhi::DrawIndexed(mDevice, draw);
    }
    if (status == rhi::RasterStatus::Ready)
    {
        status = mOutline.Draw(prepared, pass);
    }
    rhi::SubmissionToken token;
    const rhi::RasterStatus submitted = rhi::EndFrame(mDevice, token);
    release();
    if (status != rhi::RasterStatus::Ready || submitted != rhi::RasterStatus::Ready)
    {
        return status != rhi::RasterStatus::Ready ? status : submitted;
    }
    target.Producer = token;
    target.HasView = clean.HasView;
    target.WorldToClip = clean.WorldToClip;
    mComposing = targetIndex;
    mComposingRevision = mOutlineRevision;
    return rhi::RasterStatus::Pending;
}
rhi::RasterStatus GraphDisplayFrames::pollCompose() noexcept
{
    LUDUS_REQUIRE(mComposing >= 0);
    const rhi::RasterStatus status = rhi::GetStatus(mDevice, mSlots[static_cast<usize>(mComposing)].Producer);
    if (status == rhi::RasterStatus::Pending || status == rhi::RasterStatus::Ready)
    {
        return status;
    }
    disableSelectionOutline();
    return rhi::RasterStatus::Failed;
}
rhi::RasterStatus GraphDisplayFrames::startCopyCompose() noexcept
{
    if (mIsOutlineWanted == false || CanDrawSelectionOutline() == false)
    {
        return rhi::RasterStatus::Ready;
    }
    const int32 target = findFreeSlot();
    if (target < 0)
    {
        // Publish the clean image now; PollSelectionOutline composes it as soon as a slot frees.
        return rhi::RasterStatus::Ready;
    }
    const rhi::RasterStatus status = beginCompose(mCompleted, target);
    if (status == rhi::RasterStatus::Pending)
    {
        mIsComposingCopy = true;
        return status;
    }
    if (status != rhi::RasterStatus::NotReady && status != rhi::RasterStatus::InvalidDescription)
    {
        disableSelectionOutline();
    }
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus GraphDisplayFrames::PollSelectionOutline() noexcept
{
    if (CanDrawSelectionOutline() == false)
    {
        return rhi::RasterStatus::NotReady;
    }
    if (mComposing >= 0)
    {
        if (mIsComposingCopy == true)
        {
            return rhi::RasterStatus::Pending;
        }
        const rhi::RasterStatus composed = pollCompose();
        if (composed != rhi::RasterStatus::Ready)
        {
            mComposing = composed == rhi::RasterStatus::Pending ? mComposing : -1;
            return composed;
        }
        mLatest = mComposing;
        mSlots[static_cast<usize>(mLatest)].Serial = ++mSerial;
        mComposedRevision = mComposingRevision;
        mComposing = -1;
        Q_EMIT FrameReady();
        return rhi::RasterStatus::Ready;
    }
    if (mCopying >= 0 || mCompleted >= 0)
    {
        return rhi::RasterStatus::NotReady;
    }
    if (mLatest < 0 || mClean < 0 || mComposedRevision == mOutlineRevision)
    {
        return rhi::RasterStatus::Ready;
    }
    if (mIsOutlineWanted == true)
    {
        const int32 target = findFreeSlot();
        if (target < 0)
        {
            return rhi::RasterStatus::NotReady;
        }
        const rhi::RasterStatus status = beginCompose(mClean, target);
        if (status == rhi::RasterStatus::Pending)
        {
            mIsComposingCopy = false;
            return status;
        }
        if (status == rhi::RasterStatus::NotReady)
        {
            return status;
        }
        if (status != rhi::RasterStatus::InvalidDescription)
        {
            disableSelectionOutline();
        }
    }
    // Nothing to draw, or the outline was withdrawn: show the clean snapshot again.
    mComposedRevision = mOutlineRevision;
    if (mLatest != mClean)
    {
        mLatest = mClean;
        mSlots[static_cast<usize>(mLatest)].Serial = ++mSerial;
        Q_EMIT FrameReady();
    }
    return rhi::RasterStatus::Ready;
}
} // namespace ludus::editor
