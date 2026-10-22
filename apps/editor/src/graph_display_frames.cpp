#include "internal/graph_display_frames.h"

#include <ludus/foundation/base/core.h>
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
    description.Streams = streams.AsSpan();
    description.Attributes = attributes.AsSpan();
    description.Target = rhi::RasterTarget::Rgba8Unorm;
    status = rhi::CreateRasterPipeline(mDevice, description, mPipeline);
    mIsPrepared = IsAccepted(status);
    mHasPreparationFailed = mIsPrepared == false;
    return status;
}
rhi::RasterStatus
GraphDisplayFrames::Copy(rhi::SurfaceHandle surface, rhi::TextureHandle source, uint32 width, uint32 height) noexcept
{
    if (mHasDevice == false || width == 0 || height == 0)
    {
        return rhi::RasterStatus::InvalidState;
    }
    if (mCopying >= 0)
    {
        return PollCopy();
    }
    if (mCompleted >= 0 || mSerial == ~uint64{0})
    {
        return rhi::RasterStatus::InvalidState;
    }
    const rhi::RasterStatus prepared = prepareCopy();
    if (prepared != rhi::RasterStatus::Ready)
    {
        return prepared == rhi::RasterStatus::Pending ? rhi::RasterStatus::NotReady : prepared;
    }
    for (usize i = 0; i < DISPLAY_SLOT_COUNT; ++i)
    {
        DisplaySlot& slot = mSlots[i];
        if (slot.Readers != 0 || static_cast<int32>(i) == mLatest)
        {
            continue;
        }
        if (slot.Width != width || slot.Height != height)
        {
            static_cast<void>(rhi::Destroy(mDevice, slot.Texture));
            slot.Texture = {};
            rhi::TextureDescription description;
            description.Width = width;
            description.Height = height;
            description.Attachment = true;
            const rhi::RasterStatus created = rhi::CreateTexture(mDevice, description, {}, slot.Texture);
            if (IsAccepted(created) == false)
            {
                slot.Width = 0;
                slot.Height = 0;
                return created;
            }
            slot.Width = width;
            slot.Height = height;
        }
        rhi::RasterStatus status = rhi::CreateTextureView(mDevice, source, {}, mSourceView);
        if (status != rhi::RasterStatus::Ready)
        {
            releaseCopyBinding();
            return status == rhi::RasterStatus::Pending ? rhi::RasterStatus::NotReady : status;
        }
        const StaticArray<rhi::RasterBindingResource, COPY_BINDING_COUNT> resources{
            {{ .Texture = mSourceView }, { .Binding = 1, .Sampler = mSampler }}};
        status = rhi::CreateBindingSet(mDevice, mLayout, resources.AsSpan(), mBinding);
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
    return completed;
}
void GraphDisplayFrames::PublishCopy() noexcept
{
    LUDUS_REQUIRE(mCompleted >= 0);
    mLatest = mCompleted;
    mSlots[static_cast<usize>(mLatest)].Serial = ++mSerial;
    mCompleted = -1;
}
void GraphDisplayFrames::DiscardCopy() noexcept
{
    mCompleted = -1;
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
void GraphDisplayFrames::Resize(int32, int32) {}
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
} // namespace ludus::editor
