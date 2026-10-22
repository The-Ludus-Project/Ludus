#include "internal/render_graph_storage.h"

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/graphics/rhi/compute.h>

namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace format = content::render_graph_format;
RenderGraphStorage::~RenderGraphStorage() noexcept
{
    Reset();
}

rhi::RasterStatus RenderGraphStorage::Initialize(rhi::DeviceHandle device,
                                                 BorrowedView<const format::StorageBuffer> descriptions,
                                                 uint32 width,
                                                 uint32 height,
                                                 RenderGraphStorageBudget budget) noexcept
{
    if (mCount != 0 || descriptions.IsValid() == false || descriptions.GetSize() > format::MAX_STORAGE_BUFFERS)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    rhi::ComputeCapabilities capabilities;
    rhi::RasterStatus status = rhi::GetComputeCapabilities(device, capabilities);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    usize bytes = 0;
    usize largest = 0;
    for (usize i = 0; i < descriptions.GetSize(); ++i)
    {
        const format::StorageBuffer& description = descriptions[i];
        constexpr uint32 WORD_BYTES = sizeof(uint32);
        if (description.Slot < format::FIRST_STORAGE_SLOT ||
            description.Slot >= format::FIRST_STORAGE_SLOT + format::MAX_STORAGE_BUFFERS || description.Stride == 0 ||
            description.Stride > format::MAX_STORAGE_STRIDE || description.Stride % WORD_BYTES != 0 ||
            description.Count > format::MAX_STORAGE_ELEMENTS || description.IsReadOnly > 1)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        for (usize earlier = 0; earlier < i; ++earlier)
        {
            if (descriptions[earlier].Slot == description.Slot)
            {
                return rhi::RasterStatus::InvalidDescription;
            }
        }
        usize count = description.Count;
        if (count == 0 && TryMultiply(usize{width}, usize{height}, count) == false)
        {
            return rhi::RasterStatus::CapacityExceeded;
        }
        if (count == 0 || count > format::MAX_STORAGE_ELEMENTS ||
            TryMultiply(count, usize{description.Stride}, mSizes[i]) == false ||
            TryAdd(bytes, mSizes[i], bytes) == false)
        {
            return rhi::RasterStatus::CapacityExceeded;
        }
        if (mSizes[i] > capabilities.MaxStorageRange)
        {
            return rhi::RasterStatus::CapacityExceeded;
        }
        if (mSizes[i] > largest)
        {
            largest = mSizes[i];
        }
    }
    if (largest > budget.ScratchBytes || bytes > budget.GpuBytes)
    {
        return rhi::RasterStatus::CapacityExceeded;
    }
    Array<uint8> zeros;
    if (zeros.TryResize(largest) == false)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    for (usize i = 0; i < largest; ++i)
    {
        zeros[i] = 0;
    }
    mDevice = device;
    mCount = descriptions.GetSize();
    mBytes = bytes;
    for (usize i = 0; i < mCount; ++i)
    {
        mDescriptions[i] = descriptions[i];
        status =
            rhi::CreateBuffer(device, {rhi::BufferRole::Storage, mSizes[i]}, {zeros.GetData(), mSizes[i]}, mBuffers[i]);
        if (status != rhi::RasterStatus::Ready && status != rhi::RasterStatus::Pending)
        {
            Reset();
            return status;
        }
    }
    return Poll();
}
rhi::RasterStatus RenderGraphStorage::Poll() const noexcept
{
    for (usize i = 0; i < mCount; ++i)
    {
        const rhi::RasterStatus status = rhi::GetStatus(mDevice, mBuffers[i]);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
    }
    return rhi::RasterStatus::Ready;
}
rhi::RasterBindingResource RenderGraphStorage::GetBinding(usize index) const noexcept
{
    LUDUS_ASSERT(index < mCount);
    return {mDescriptions[index].Slot, mBuffers[index], 0, mSizes[index]};
}
rhi::RasterStatus RenderGraphStorage::AppendUses(rhi::OrderedGraph graph,
                                                 BorrowedView<rhi::GraphUse> outUses) const noexcept
{
    if (outUses.IsValid() == false || outUses.GetSize() < mCount)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    for (usize i = 0; i < mCount; ++i)
    {
        rhi::GraphVersion input;
        rhi::RasterStatus status = rhi::ImportGraphBuffer(mDevice, graph, mBuffers[i], input);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        rhi::GraphVersion output;
        const bool isReadOnly = mDescriptions[i].IsReadOnly == 1;
        if (isReadOnly == false)
        {
            status = rhi::NextGraphVersion(mDevice, graph, input, output);
        }
        else
        {
            output = input;
        }
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        outUses[i] = {output,
                      isReadOnly == true ? rhi::GraphAccessMode::StorageRead : rhi::GraphAccessMode::StorageReadWrite,
                      0,
                      mSizes[i],
                      rhi::RasterVisibility::Compute};
        status = rhi::AddGraphRoot(mDevice, graph, output, rhi::GraphRoot::History);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
    }
    return rhi::RasterStatus::Ready;
}
void RenderGraphStorage::Reset() noexcept
{
    for (usize i = 0; i < mCount; ++i)
    {
        static_cast<void>(rhi::Destroy(mDevice, mBuffers[i]));
    }
    mCount = 0;
    mBytes = 0;
    mDevice = {};
}
} // namespace ludus::graphics::renderer::internal
