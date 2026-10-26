#include "internal/d3d12_raster.h"
#include "internal/d3d12_transfer.h"

#include <ludus/foundation/containers/static_array.hpp>

#include <cstring>

// Thanks to Microsoft, "Uploading texture data through buffers" and "Using
// Resource Barriers to Synchronize Resource States in Direct3D 12", for placed
// footprints, fixed upload/readback heap states and buffer promotion/decay:
// https://learn.microsoft.com/windows/win32/direct3d12/upload-and-readback-of-texture-data
// https://learn.microsoft.com/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12
// Each submission owns its native references independently of public cancellation.
namespace ludus::graphics::rhi::backend
{
namespace
{
using namespace foundation;
using namespace internal;
using namespace d3d12;
constexpr usize TRANSFER_DIRECTIONS = 2;
constexpr usize CBV_ALIGNMENT = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr D3D12_RESOURCE_STATES SAMPLED_STATE =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
struct Transfer final
{
    ComOwner<ID3D12Resource> Staging;
    ComOwner<ID3D12Resource> Source;
    ComOwner<ID3D12Resource> Destination;
    ComOwner<ID3D12CommandAllocator> Allocator;
    ComOwner<ID3D12GraphicsCommandList> Commands;
    ComOwner<ID3D12Fence> Completion;
    uint64 Fence = 0;
    usize Bytes = 0;
    uint32 Request = 0;
};
StaticArray<StaticArray<Transfer, LIFETIME_TRANSFERS>, TRANSFER_DIRECTIONS> gTransfers;
StorageTable<Transfer> gInitialBuffers;

RasterStatus GetTransferFailure(HRESULT result) noexcept
{
    return result == E_OUTOFMEMORY                               ? RasterStatus::OutOfMemory
           : GetDeviceOrNull()->GetDeviceRemovedReason() != S_OK ? RasterStatus::DeviceLost
                                                                 : RasterStatus::Failed;
}
RasterStatus PollTransfer(const Transfer& transfer) noexcept
{
    if (transfer.Fence == 0)
    {
        return RasterStatus::Failed;
    }
    const uint64 completed = transfer.Completion->GetCompletedValue();
    return completed == ~uint64{0}       ? RasterStatus::DeviceLost
           : completed >= transfer.Fence ? RasterStatus::Ready
                                         : RasterStatus::Pending;
}
void ClearTransfer(Transfer& inoutTransfer) noexcept
{
    ReleaseResourceStorage(inoutTransfer.Bytes);
    inoutTransfer = {};
}
void RetainResource(ID3D12Resource* resource, ComOwner<ID3D12Resource>& outOwner) noexcept
{
    LUDUS_ASSERT(resource != nullptr);
    resource->AddRef();
    outOwner = ComOwner<ID3D12Resource>(resource);
}
RasterStatus MakeBuffer(usize size, D3D12_HEAP_TYPE type, ComOwner<ID3D12Resource>& outResource) noexcept
{
    const D3D12_HEAP_PROPERTIES heap{ .Type = type };
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_RESOURCE_STATES state = type == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
                                        : type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                           : D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* resourceOrNull = nullptr;
    const HRESULT result = GetDeviceOrNull()->CreateCommittedResource(&heap,
                                                                      D3D12_HEAP_FLAG_NONE,
                                                                      &description,
                                                                      state,
                                                                      nullptr,
                                                                      GetInterfaceId(resourceOrNull),
                                                                      GetInterfaceOutput(resourceOrNull));
    if (FAILED(result) == true)
    {
        return GetTransferFailure(result);
    }
    outResource = ComOwner<ID3D12Resource>(resourceOrNull);
    return RasterStatus::Ready;
}
RasterStatus PrepareTransfer(Transfer& inoutTransfer,
                             bool isReadback,
                             const uint8* bytesOrNull,
                             usize size,
                             usize byteCount = 0) noexcept
{
    if (inoutTransfer.Fence != 0)
    {
        return RasterStatus::CapacityExceeded;
    }
    if (size != 0)
    {
        if (ReserveResourceStorage(size) == false)
        {
            return StorageFailure();
        }
        inoutTransfer.Bytes = size;
        const RasterStatus created = MakeBuffer(size,
                                                isReadback == true ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD,
                                                inoutTransfer.Staging);
        if (created != RasterStatus::Ready)
        {
            ClearTransfer(inoutTransfer);
            return created;
        }
        if (isReadback == false)
        {
            void* mappedOrNull = nullptr;
            const D3D12_RANGE noRead{};
            const HRESULT mapped = inoutTransfer.Staging->Map(0, &noRead, &mappedOrNull);
            if (FAILED(mapped) == true)
            {
                ClearTransfer(inoutTransfer);
                return GetTransferFailure(mapped);
            }
            std::memset(mappedOrNull, 0, size);
            std::memcpy(mappedOrNull, bytesOrNull, byteCount == 0 ? size : byteCount);
            inoutTransfer.Staging->Unmap(0, nullptr);
        }
    }
    ID3D12CommandAllocator* allocatorOrNull = nullptr;
    HRESULT result = GetDeviceOrNull()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                               GetInterfaceId(allocatorOrNull),
                                                               GetInterfaceOutput(allocatorOrNull));
    if (FAILED(result) == true)
    {
        ClearTransfer(inoutTransfer);
        return GetTransferFailure(result);
    }
    inoutTransfer.Allocator = ComOwner<ID3D12CommandAllocator>(allocatorOrNull);
    ID3D12GraphicsCommandList* commandsOrNull = nullptr;
    result = GetDeviceOrNull()->CreateCommandList(0,
                                                  D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  inoutTransfer.Allocator.Get(),
                                                  nullptr,
                                                  GetInterfaceId(commandsOrNull),
                                                  GetInterfaceOutput(commandsOrNull));
    if (FAILED(result) == true)
    {
        ClearTransfer(inoutTransfer);
        return GetTransferFailure(result);
    }
    inoutTransfer.Commands = ComOwner<ID3D12GraphicsCommandList>(commandsOrNull);
    return RasterStatus::Ready;
}
RasterStatus SubmitTransfer(Transfer& inoutTransfer) noexcept
{
    const HRESULT closed = inoutTransfer.Commands->Close();
    if (FAILED(closed) == true)
    {
        ClearTransfer(inoutTransfer);
        return GetTransferFailure(closed);
    }
    ID3D12Fence* fence = GetFenceOrNull();
    LUDUS_ASSERT(fence != nullptr);
    fence->AddRef();
    inoutTransfer.Completion = ComOwner<ID3D12Fence>(fence);
    // SubmitCreation marks an unsuccessful signal with an uncompletable fence.
    // Keep all references until a later successful drain or device removal.
    return SubmitCreation(inoutTransfer.Commands.Get(), inoutTransfer.Fence) == true ? RasterStatus::Ready
                                                                                     : RasterStatus::DeviceLost;
}
RasterStatus UploadBuffer(Transfer& inoutTransfer,
                          usize bufferSlot,
                          const BufferDescription& description,
                          const uint8* bytes,
                          usize size) noexcept
{
    ComOwner<ID3D12Resource> buffer;
    const usize allocation = (description.Size + CBV_ALIGNMENT - 1) / CBV_ALIGNMENT * CBV_ALIGNMENT;
    RasterStatus status = MakeBuffer(allocation, D3D12_HEAP_TYPE_DEFAULT, buffer);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    status = PrepareTransfer(inoutTransfer, false, bytes, allocation, size);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    RetainResource(buffer.Get(), inoutTransfer.Destination);
    inoutTransfer.Commands->CopyBufferRegion(buffer.Get(), 0, inoutTransfer.Staging.Get(), 0, allocation);
    status = SubmitTransfer(inoutTransfer);
    if (status == RasterStatus::Ready)
    {
        SetBuffer(bufferSlot, Move(buffer));
    }
    return status;
}
void TransitionTexture(ID3D12GraphicsCommandList* commands,
                       ID3D12Resource* resource,
                       D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after) noexcept
{
    if (before != after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        commands->ResourceBarrier(1, &barrier);
    }
}
D3D12_TEXTURE_COPY_LOCATION MakeTextureLocation(ID3D12Resource* resource, uint32 mip) noexcept
{
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = resource;
    location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    location.SubresourceIndex = mip;
    return location;
}
D3D12_TEXTURE_COPY_LOCATION
MakeStagingLocation(const Transfer& transfer, const TextureRegion& region, usize pitch) noexcept
{
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = transfer.Staging.Get();
    location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    location.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM,
                                          region.Width,
                                          region.Height,
                                          1,
                                          static_cast<UINT>(pitch)};
    return location;
}
} // namespace

namespace d3d12
{
void PollInitialBuffers() noexcept
{
    for (Transfer& transfer : gInitialBuffers)
    {
        if (transfer.Fence != 0)
        {
            const RasterStatus status = PollTransfer(transfer);
            if (status != RasterStatus::Pending)
            {
                const uint32 request = transfer.Request;
                ClearTransfer(transfer);
                RasterComplete(request, status);
            }
        }
    }
}
RasterStatus CreateInitialBuffer(const LifetimeUploadTarget& target,
                                 const BufferDescription& description,
                                 const uint8* bytes,
                                 usize size) noexcept
{
    const usize slot = target.Buffer;
    const uint32 request = target.Request;
    if (gInitialBuffers.EnsureSlots(slot + 1) == false)
    {
        return StorageFailure();
    }
    PollInitialBuffers();
    Transfer& transfer = gInitialBuffers[slot];
    if (transfer.Fence != 0)
    {
        return RasterStatus::CapacityExceeded;
    }
    // Ready is queue-usable: subsequent draws/copies use the same ordered queue.
    const RasterStatus status = UploadBuffer(transfer, slot, description, bytes, size);
    if (status == RasterStatus::Ready)
    {
        transfer.Request = request;
        // Ready permits ordered GPU use, while this callback retains the logical
        // resource budget if public ownership is destroyed before the copy finishes.
        RasterExpect(request, RasterCallbacks::One);
    }
    return status;
}
} // namespace d3d12

bool LifetimeTransferAvailable(bool isReadback, usize slot) noexcept
{
    Transfer& transfer = gTransfers[isReadback == true ? 1 : 0][slot];
    if (transfer.Fence != 0 && transfer.Completion.Get() != GetFenceOrNull() &&
        PollTransfer(transfer) != RasterStatus::Pending)
    {
        ClearTransfer(transfer);
    }
    return transfer.Fence == 0;
}
RasterStatus LifetimeUpload(usize slot,
                            const BufferDescription& description,
                            const LifetimeUploadTarget& target,
                            const uint8* bytes,
                            usize size) noexcept
{
    return UploadBuffer(gTransfers[0][slot], target.Buffer, description, bytes, size);
}
RasterStatus LifetimeReadback(usize slot, BufferRole, const LifetimeCopyRange& range) noexcept
{
    Transfer& transfer = gTransfers[1][slot];
    const RasterStatus status = PrepareTransfer(transfer, true, nullptr, range.Size);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    RetainResource(GetBuffer(range.Buffer), transfer.Source);
    transfer.Commands->CopyBufferRegion(transfer.Staging.Get(), 0, transfer.Source.Get(), range.Offset, range.Size);
    return SubmitTransfer(transfer);
}
RasterStatus LifetimeBufferUpdate(usize slot, BufferRole, const LifetimeCopyRange& range, const uint8* bytes) noexcept
{
    Transfer& transfer = gTransfers[0][slot];
    const RasterStatus status = PrepareTransfer(transfer, false, bytes, range.Size);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    RetainResource(GetBuffer(range.Buffer), transfer.Destination);
    transfer.Commands->CopyBufferRegion(transfer.Destination.Get(),
                                        range.Offset,
                                        transfer.Staging.Get(),
                                        0,
                                        range.Size);
    return SubmitTransfer(transfer);
}
RasterStatus LifetimeTextureTransfer(usize slot, const TextureTransfer& information, const uint8* bytesOrNull) noexcept
{
    const bool isReadback = information.Operation == TextureTransferOperation::Readback;
    const bool isCopy = information.Operation == TextureTransferOperation::Copy;
    Transfer& transfer = gTransfers[isReadback == true ? 1 : 0][slot];
    const RasterStatus ready = PrepareTransfer(transfer, isReadback, bytesOrNull, information.Size);
    if (ready != RasterStatus::Ready)
    {
        return ready;
    }
    ID3D12GraphicsCommandList* commands = transfer.Commands.Get();
    if (isReadback == true || isCopy == true)
    {
        RetainResource(GetTexture(information.Source), transfer.Source);
        TransitionTexture(commands,
                          transfer.Source.Get(),
                          GetTextureState(information.Source),
                          D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    if (isReadback == false)
    {
        RetainResource(GetTexture(information.Destination), transfer.Destination);
        TransitionTexture(commands,
                          transfer.Destination.Get(),
                          GetTextureState(information.Destination),
                          D3D12_RESOURCE_STATE_COPY_DEST);
    }
    const TextureRegion& sourceRegion = information.SourceRegion;
    const TextureRegion& destinationRegion = information.DestinationRegion;
    const D3D12_TEXTURE_COPY_LOCATION source =
        isReadback == true || isCopy == true ? MakeTextureLocation(transfer.Source.Get(), sourceRegion.Mip)
                                             : MakeStagingLocation(transfer, destinationRegion, information.RowPitch);
    const D3D12_TEXTURE_COPY_LOCATION destination =
        isReadback == true ? MakeStagingLocation(transfer, sourceRegion, information.RowPitch)
                           : MakeTextureLocation(transfer.Destination.Get(), destinationRegion.Mip);
    const D3D12_BOX box{sourceRegion.X,
                        sourceRegion.Y,
                        0,
                        sourceRegion.X + sourceRegion.Width,
                        sourceRegion.Y + sourceRegion.Height,
                        1};
    commands->CopyTextureRegion(&destination,
                                isReadback == true ? 0 : destinationRegion.X,
                                isReadback == true ? 0 : destinationRegion.Y,
                                0,
                                &source,
                                isReadback == true || isCopy == true ? &box : nullptr);
    if (isReadback == true || isCopy == true)
    {
        TransitionTexture(commands,
                          transfer.Source.Get(),
                          D3D12_RESOURCE_STATE_COPY_SOURCE,
                          GetTextureState(information.Source));
    }
    if (isReadback == false)
    {
        TransitionTexture(commands, transfer.Destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST, SAMPLED_STATE);
    }
    const RasterStatus submitted = SubmitTransfer(transfer);
    if (submitted == RasterStatus::Ready && isReadback == false)
    {
        SetTextureState(information.Destination, SAMPLED_STATE);
    }
    return submitted;
}
RasterStatus LifetimePollTransfer(bool isReadback, usize slot) noexcept
{
    return PollTransfer(gTransfers[isReadback == true ? 1 : 0][slot]);
}
RasterStatus LifetimeCopyReadback(usize slot, uint8* outBytes, usize size) noexcept
{
    Transfer& transfer = gTransfers[1][slot];
    const RasterStatus status = PollTransfer(transfer);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    if (size > transfer.Bytes)
    {
        return RasterStatus::InvalidDescription;
    }
    void* mappedOrNull = nullptr;
    const D3D12_RANGE readRange{0, size};
    const HRESULT mapped = transfer.Staging->Map(0, &readRange, &mappedOrNull);
    if (FAILED(mapped) == true)
    {
        return GetTransferFailure(mapped);
    }
    std::memcpy(outBytes, mappedOrNull, size);
    const D3D12_RANGE noWrite{};
    transfer.Staging->Unmap(0, &noWrite);
    return RasterStatus::Ready;
}
void LifetimeReleaseTransfer(bool isReadback, usize slot) noexcept
{
    Transfer& transfer = gTransfers[isReadback == true ? 1 : 0][slot];
    LUDUS_ASSERT(PollTransfer(transfer) != RasterStatus::Pending);
    ClearTransfer(transfer);
}
void LifetimeReset() noexcept
{
    const bool isDrained = WaitForQueue();
    const auto reset = [isDrained](Transfer& inoutTransfer) noexcept {
        if (inoutTransfer.Fence == 0 || PollTransfer(inoutTransfer) != RasterStatus::Pending ||
            (isDrained == true && inoutTransfer.Completion.Get() == GetFenceOrNull()))
        {
            const uint32 request = inoutTransfer.Request;
            const RasterStatus status = inoutTransfer.Fence == 0 ? RasterStatus::Ready : PollTransfer(inoutTransfer);
            ClearTransfer(inoutTransfer);
            RasterComplete(request, status == RasterStatus::Pending ? RasterStatus::Ready : status);
        }
        // A timeout does not establish completion. Preserve old native references
        // and their fence across restart; the slice cannot be reused prematurely.
    };
    for (StaticArray<Transfer, LIFETIME_TRANSFERS>& direction : gTransfers)
    {
        for (Transfer& transfer : direction)
        {
            reset(transfer);
        }
    }
    for (Transfer& transfer : gInitialBuffers)
    {
        reset(transfer);
    }
}
} // namespace ludus::graphics::rhi::backend
