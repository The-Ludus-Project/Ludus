#include "internal/render_graph_pool.h"

#include <ludus/foundation/base/assert.hpp>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <new>

// Thanks to Yuriy O'Donnell, "FrameGraph: Extensible Rendering Architecture in
// Frostbite", GDC 2017, transient resource system, and to Epic Games, "Render
// Dependency Graph" (Unreal Engine documentation, resource pooling and lifetimes):
// transient allocations are recycled by complete description once their GPU work
// retires. This original pool adopts only that cross-instance reuse; it performs no
// memory aliasing, keeps exports out of the pool and proves retirement through the
// Ludus RHI's ordered submission tokens instead of frame counting.
// Contract: docs/architecture/render-graph-authoring.md, "Delivery, optimization gates".
namespace ludus::graphics::renderer
{
using namespace foundation;
using Status = rhi::RasterStatus;
namespace
{
/// One idle allocation and the order in which it was released, for least-recent eviction.
struct IdleAttachment final
{
    rhi::TextureDescription Description{};
    rhi::TextureHandle Texture{};
    uint64 Release = 0;
};
// AreDescriptionsEqual names every field; extend it whenever TextureDescription changes.
static_assert(sizeof(rhi::TextureDescription) == 32, "compare every TextureDescription field in the pool");
/// Every field of the RHI description: equal descriptions allocate interchangeable textures.
bool AreDescriptionsEqual(const rhi::TextureDescription& left, const rhi::TextureDescription& right) noexcept
{
    return left.Width == right.Width && left.Height == right.Height && left.Format == right.Format &&
           left.Attachment == right.Attachment && left.MipLevels == right.MipLevels &&
           left.AsyncUpload == right.AsyncUpload && left.RetainCpuContent == right.RetainCpuContent &&
           left.DeferredInitialization == right.DeferredInitialization && left.Dimension == right.Dimension &&
           left.DepthOrLayers == right.DepthOrLayers && left.SampleCount == right.SampleCount &&
           left.Stencil == right.Stencil;
}
/// Ready when no submitted use exists or the submission of the last frame use has completed;
/// Pending while creation or that submission is unfinished; any other status disqualifies reuse.
Status ProveCompletion(rhi::DeviceHandle device, rhi::TextureHandle texture) noexcept
{
    rhi::RasterTextureState state;
    const Status queried = rhi::GetTextureState(device, texture, state);
    if (queried == Status::NotReady)
    {
        return Status::Pending;
    }
    if (queried != Status::Ready || state.Dependency.IsNull() == true)
    {
        return queried;
    }
    // A completed higher ordinal covers every earlier accepted use on the same queue.
    return rhi::GetStatus(device, state.Dependency);
}
} // namespace
struct RenderGraphAttachmentPool::State final
{
    rhi::DeviceHandle Device{};
    StaticArray<IdleAttachment, RENDER_GRAPH_POOL_CAPACITY> Idle{};
    usize IdleCount = 0;
    usize IdleCapacity = 0;
    uint64 NextRelease = 0;
    RenderGraphPoolReport Report{};
    /// Destroy the idle entry at index and close the gap; order is kept by Release, not position.
    void DestroyIdle(usize index) noexcept
    {
        static_cast<void>(rhi::Destroy(Device, Idle[index].Texture));
        Idle[index] = Idle[IdleCount - 1];
        Idle[IdleCount - 1] = {};
        --IdleCount;
        ++Report.DestroyedAllocations;
    }
    void DestroyAllIdle() noexcept
    {
        while (IdleCount != 0)
        {
            DestroyIdle(IdleCount - 1);
        }
    }
};
RenderGraphAttachmentPool::RenderGraphAttachmentPool() noexcept = default;
RenderGraphAttachmentPool::~RenderGraphAttachmentPool() noexcept
{
    Reset();
}
Status RenderGraphAttachmentPool::Initialize(rhi::DeviceHandle device, uint32 idleCapacity) noexcept
{
    if (mState.Get() != nullptr)
    {
        return Status::InvalidState;
    }
    if (idleCapacity == 0 || idleCapacity > RENDER_GRAPH_POOL_CAPACITY)
    {
        return Status::InvalidDescription;
    }
    rhi::DeviceInfo information;
    const rhi::DeviceStatus admitted = rhi::GetDeviceInfo(device, information);
    if (admitted != rhi::DeviceStatus::Ready)
    {
        return admitted == rhi::DeviceStatus::InvalidHandle ? Status::InvalidHandle : Status::NotReady;
    }
    State* allocationOrNull = new (std::nothrow) State;
    core::UniquePtr<State> candidate{allocationOrNull};
    if (candidate.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    candidate->Device = device;
    candidate->IdleCapacity = idleCapacity;
    mState.Swap(candidate);
    return Status::Ready;
}
void RenderGraphAttachmentPool::Trim() noexcept
{
    if (mState.Get() != nullptr)
    {
        mState->DestroyAllIdle();
    }
}
RenderGraphPoolReport RenderGraphAttachmentPool::GetReport() const noexcept
{
    if (mState.Get() == nullptr)
    {
        return {};
    }
    RenderGraphPoolReport report = mState->Report;
    report.IdleAllocations = static_cast<uint32>(mState->IdleCount);
    return report;
}
void RenderGraphAttachmentPool::Reset() noexcept
{
    if (mState.Get() == nullptr)
    {
        return;
    }
    LUDUS_ASSERT(mState->Report.LeasedAllocations == 0, "a RenderGraph still holds attachments from this pool");
    mState->DestroyAllIdle();
    mState.Reset();
}
namespace internal
{
bool RenderGraphPoolAccess::IsUsable(const RenderGraphAttachmentPool& pool) noexcept
{
    // The RHI owns one device record at a time, so a ready pool device is the graph's device;
    // a pool left over from a destroyed device fails admission here.
    rhi::DeviceInfo information;
    return pool.mState.Get() != nullptr &&
           rhi::GetDeviceInfo(pool.mState->Device, information) == rhi::DeviceStatus::Ready;
}
Status RenderGraphPoolAccess::Acquire(RenderGraphAttachmentPool& inoutPool,
                                      const rhi::TextureDescription& description,
                                      rhi::TextureHandle& outTexture,
                                      bool& outIsReused) noexcept
{
    RenderGraphAttachmentPool::State& state = *inoutPool.mState;
    bool isDeferred = false;
    usize index = 0;
    while (index < state.IdleCount)
    {
        if (AreDescriptionsEqual(state.Idle[index].Description, description) == false)
        {
            ++index;
            continue;
        }
        const Status proven = ProveCompletion(state.Device, state.Idle[index].Texture);
        if (proven == Status::Ready)
        {
            outTexture = state.Idle[index].Texture;
            outIsReused = true;
            state.Idle[index] = state.Idle[state.IdleCount - 1];
            state.Idle[state.IdleCount - 1] = {};
            --state.IdleCount;
            ++state.Report.LeasedAllocations;
            ++state.Report.ReusedAllocations;
            return Status::Ready;
        }
        if (proven == Status::Pending)
        {
            isDeferred = true;
            ++index;
            continue;
        }
        // A failed submission or lost device never proves completion; retire the entry.
        state.DestroyIdle(index);
    }
    rhi::TextureHandle created;
    const Status status = rhi::CreateTexture(state.Device, description, {}, created);
    if (status == Status::Ready || status == Status::Pending)
    {
        outTexture = created;
        outIsReused = false;
        ++state.Report.LeasedAllocations;
        ++state.Report.CreatedAllocations;
        if (isDeferred == true)
        {
            ++state.Report.DeferredAcquisitions;
        }
    }
    return status;
}
void RenderGraphPoolAccess::Release(RenderGraphAttachmentPool& inoutPool,
                                    const rhi::TextureDescription& description,
                                    rhi::TextureHandle& inoutTexture) noexcept
{
    RenderGraphAttachmentPool::State& state = *inoutPool.mState;
    LUDUS_ASSERT(state.Report.LeasedAllocations != 0, "released an attachment this pool did not lease");
    --state.Report.LeasedAllocations;
    const Status status = rhi::GetStatus(state.Device, inoutTexture);
    if (status != Status::Ready && status != Status::Pending)
    {
        static_cast<void>(rhi::Destroy(state.Device, inoutTexture));
        ++state.Report.DestroyedAllocations;
        return;
    }
    if (state.IdleCount == state.IdleCapacity)
    {
        usize oldest = 0;
        for (usize i = 1; i < state.IdleCount; ++i)
        {
            oldest = state.Idle[i].Release < state.Idle[oldest].Release ? i : oldest;
        }
        state.DestroyIdle(oldest);
    }
    state.Idle[state.IdleCount++] = {description, inoutTexture, ++state.NextRelease};
    inoutTexture = {};
}
} // namespace internal
} // namespace ludus::graphics::renderer
