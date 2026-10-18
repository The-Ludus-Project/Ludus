#pragma once
// Thanks to W3C, "WebGPU", section "Copying texture data", for explicit texel
// bounds and staging-row alignment (https://www.w3.org/TR/webgpu/#copying-texture-data).
// This portable profile copies RGBA8 rows into 256-byte staging rows for all APIs.
// Whole-resource CPU leases and completed GPU ordinals conservatively reject mutation
// rather than changing data under retained R2/R3 plans. See docs/architecture/rhi-gdi.md.
namespace ludus::graphics::rhi
{
namespace
{
bool InitializedMip(const Record& texture, uint32 mip) noexcept
{
    return texture.Texture.Attachment
               ? (texture.AttachmentState[SubresourceIndex(texture.Texture, mip, 0)] & COLOR_DEFINED) != 0
               : (texture.InitializedMips & (uint32{1} << mip)) != 0;
}
RasterStatus ValidateRegion(const Record& texture, const TextureRegion& region) noexcept
{
    if (texture.Texture.Format != RasterFormat::Rgba8Unorm || texture.Texture.Dimension != TextureDimension::D2 ||
        texture.Texture.SampleCount != 1)
    {
        return RasterStatus::Unsupported;
    }
    if (region.Mip >= texture.Texture.MipLevels || region.Width == 0 || region.Height == 0)
    {
        return RasterStatus::InvalidDescription;
    }
    const auto width = texture.Texture.Width >> region.Mip;
    const auto height = texture.Texture.Height >> region.Mip;
    return Fits(region.X, region.Width, width == 0 ? 1 : width) &&
                   Fits(region.Y, region.Height, height == 0 ? 1 : height)
               ? RasterStatus::Ready
               : RasterStatus::InvalidDescription;
}
bool FullMip(const Record& texture, const TextureRegion& region) noexcept
{
    const auto width = texture.Texture.Width >> region.Mip;
    const auto height = texture.Texture.Height >> region.Mip;
    return region.X == 0 && region.Y == 0 && region.Width == (width == 0 ? 1 : width) &&
           region.Height == (height == 0 ? 1 : height);
}
RasterStatus MutableAdmission(DeviceHandle device) noexcept
{
    const auto admission = Admission(device);
    return admission == RasterStatus::Ready ? PollLifetime(device) : admission;
}
RasterStatus MutableDestination(Record& record, RasterKind kind) noexcept
{
    if (record.Status != RasterStatus::Ready)
    {
        return record.Status == RasterStatus::Pending ? RasterStatus::NotReady : record.Status;
    }
    const auto completed = backend::RasterCompleted();
    if (GetStartup().State != StartupState::Ready)
    {
        return RasterStatus::DeviceLost;
    }
    if (record.References != 1 || record.LastUse > completed)
    {
        return RasterStatus::InvalidState;
    }
    if ((kind == RasterKind::Buffer ? record.BufferRevision : record.TextureRevision) == ~uint64{0})
    {
        return RasterStatus::IdentityExhausted;
    }
    return RasterStatus::Ready;
}
RasterStatus TransferRows(const TextureRegion& region, usize pitch, usize& nativePitch, usize& nativeSize) noexcept
{
    usize rowBytes = 0, size = 0, rounded = 0;
    if (TryMultiply(usize{region.Width}, usize{4}, rowBytes) == false || pitch < rowBytes || pitch % 4 != 0 ||
        TryMultiply(pitch, usize{region.Height}, size) == false || TryAdd(rowBytes, usize{255}, rounded) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    nativePitch = rounded & ~usize{255};
    if (TryMultiply(nativePitch, usize{region.Height}, nativeSize) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    return size <= CurrentStorageLimits().TransferBytes && nativeSize <= CurrentStorageLimits().TransferBytes
               ? RasterStatus::Ready
               : RasterStatus::CapacityExceeded;
}
RasterStatus ReserveMutation(usize& slot) noexcept
{
    return ReserveLifetime(gUploads, slot, [](usize candidate) noexcept {
        return backend::LifetimeTransferAvailable(false, candidate);
    });
}
void PublishMutation(usize slot,
                     RasterKind kind,
                     usize destination,
                     usize source,
                     const TextureRegion& region,
                     TransferTicket& output) noexcept
{
    auto& request = gUploads[slot];
    request.Owner = DeviceOwner();
    request.Published = true;
    request.Mutable = true;
    request.Kind = kind;
    request.Source = destination;
    request.Other = source;
    request.Region = region;
    auto& resource = At(kind, destination);
    resource.Status = RasterStatus::Pending;
    Retain(kind, destination);
    if (source != RASTER_INVALID)
    {
        Retain(RasterKind::Texture, source);
    }
    LifetimeAccess::Set(output, {DeviceOwner(), slot, request.Generation});
}
RasterStatus TextureDestination(Record& texture, const TextureRegion& region) noexcept
{
    const auto valid = ValidateRegion(texture, region);
    if (valid != RasterStatus::Ready)
    {
        return valid;
    }
    if (texture.Texture.RetainCpuContent)
    {
        return RasterStatus::Unsupported;
    }
    if (InitializedMip(texture, region.Mip) == false && FullMip(texture, region) == false)
    {
        return RasterStatus::InvalidState;
    }
    return MutableDestination(texture, RasterKind::Texture);
}
} // namespace
RasterStatus RequestBufferUpdate(DeviceHandle device,
                                 BufferHandle handle,
                                 usize offset,
                                 const uint8* bytes,
                                 usize size,
                                 TransferTicket& output) noexcept
{
    const auto admission = MutableAdmission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (LifetimeAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    auto* buffer = Resolve(handle, RasterKind::Buffer);
    if (buffer == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (bytes == nullptr || size == 0 || offset % 4 != 0 || size % 4 != 0 ||
        Fits(offset, size, buffer->Buffer.Size) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    if (size > CurrentStorageLimits().TransferBytes)
    {
        return RasterStatus::CapacityExceeded;
    }
    const auto valid = MutableDestination(*buffer, RasterKind::Buffer);
    if (valid != RasterStatus::Ready)
    {
        return valid;
    }
    usize slot = 0;
    const auto reserved = ReserveMutation(slot);
    if (reserved != RasterStatus::Ready)
    {
        return reserved;
    }
    std::memcpy(gUploadBytes[slot], bytes, size);
    const auto started = backend::LifetimeBufferUpdate(slot,
                                                       buffer->Buffer.Role,
                                                       {RasterAccess::Slot(handle), offset, size},
                                                       gUploadBytes[slot]);
    if (GetStartup().State != StartupState::Ready)
    {
        return Admission(device);
    }
    if (started != RasterStatus::Ready && started != RasterStatus::Pending)
    {
        return started;
    }
    if (buffer->Indices.GetSize() != 0)
    {
        std::memcpy(buffer->Indices.GetData() + offset, gUploadBytes[slot], size);
    }
    PublishMutation(slot, RasterKind::Buffer, RasterAccess::Slot(handle), RASTER_INVALID, {}, output);
    return RasterStatus::Pending;
}
RasterStatus RequestTextureUpdate(DeviceHandle device,
                                  TextureHandle handle,
                                  const TextureRegion& region,
                                  const uint8* bytes,
                                  usize byteCount,
                                  usize rowPitch,
                                  TransferTicket& output) noexcept
{
    const auto admission = MutableAdmission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (LifetimeAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    auto* texture = Resolve(handle, RasterKind::Texture);
    if (texture == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    const auto valid = TextureDestination(*texture, region);
    if (valid != RasterStatus::Ready)
    {
        return valid;
    }
    usize pitch = 0, size = 0, callerSize = 0;
    const auto rows = TransferRows(region, rowPitch, pitch, size);
    if (rows != RasterStatus::Ready)
    {
        return rows;
    }
    if (bytes == nullptr || TryMultiply(rowPitch, usize{region.Height}, callerSize) == false || byteCount != callerSize)
    {
        return RasterStatus::InvalidDescription;
    }
    usize slot = 0;
    const auto reserved = ReserveMutation(slot);
    if (reserved != RasterStatus::Ready)
    {
        return reserved;
    }
    std::memset(gUploadBytes[slot], 0, size);
    for (usize row = 0; row < region.Height; ++row)
    {
        std::memcpy(gUploadBytes[slot] + row * pitch, bytes + row * rowPitch, usize{region.Width} * 4);
    }
    internal::TextureTransfer info;
    info.Destination = RasterAccess::Slot(handle);
    info.DestinationRegion = region;
    info.RowPitch = pitch;
    info.Size = size;
    const auto started = backend::LifetimeTextureTransfer(slot, info, gUploadBytes[slot]);
    if (GetStartup().State != StartupState::Ready)
    {
        return Admission(device);
    }
    if (started != RasterStatus::Ready && started != RasterStatus::Pending)
    {
        return started;
    }
    PublishMutation(slot, RasterKind::Texture, info.Destination, RASTER_INVALID, region, output);
    return RasterStatus::Pending;
}
RasterStatus RequestTextureCopy(DeviceHandle device,
                                TextureHandle sourceHandle,
                                const TextureRegion& sourceRegion,
                                TextureHandle destinationHandle,
                                const TextureRegion& destinationRegion,
                                TransferTicket& output) noexcept
{
    const auto admission = MutableAdmission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (LifetimeAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    auto* source = Resolve(sourceHandle, RasterKind::Texture);
    auto* destination = Resolve(destinationHandle, RasterKind::Texture);
    if (source == nullptr || destination == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (source == destination || sourceRegion.Width != destinationRegion.Width ||
        sourceRegion.Height != destinationRegion.Height)
    {
        return RasterStatus::InvalidDescription;
    }
    const auto sourceValid = ValidateRegion(*source, sourceRegion);
    if (sourceValid != RasterStatus::Ready)
    {
        return sourceValid;
    }
    if (source->Status != RasterStatus::Ready)
    {
        return source->Status == RasterStatus::Pending ? RasterStatus::NotReady : source->Status;
    }
    if (InitializedMip(*source, sourceRegion.Mip) == false)
    {
        return RasterStatus::InvalidState;
    }
    const auto valid = TextureDestination(*destination, destinationRegion);
    if (valid != RasterStatus::Ready)
    {
        return valid;
    }
    usize slot = 0;
    const auto reserved = ReserveMutation(slot);
    if (reserved != RasterStatus::Ready)
    {
        return reserved;
    }
    internal::TextureTransfer info;
    info.Operation = internal::TextureTransferOperation::Copy;
    info.Source = RasterAccess::Slot(sourceHandle);
    info.SourceRegion = sourceRegion;
    info.Destination = RasterAccess::Slot(destinationHandle);
    info.DestinationRegion = destinationRegion;
    const auto started = backend::LifetimeTextureTransfer(slot, info, nullptr);
    if (GetStartup().State != StartupState::Ready)
    {
        return Admission(device);
    }
    if (started != RasterStatus::Ready && started != RasterStatus::Pending)
    {
        return started;
    }
    PublishMutation(slot, RasterKind::Texture, info.Destination, info.Source, destinationRegion, output);
    return RasterStatus::Pending;
}
RasterStatus RequestTextureReadback(DeviceHandle device,
                                    TextureHandle handle,
                                    const TextureRegion& region,
                                    usize rowPitch,
                                    ReadbackTicket& output) noexcept
{
    const auto admission = MutableAdmission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (LifetimeAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    const auto* texture = Resolve(handle, RasterKind::Texture);
    if (texture == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    const auto valid = ValidateRegion(*texture, region);
    if (valid != RasterStatus::Ready)
    {
        return valid;
    }
    if (texture->Status != RasterStatus::Ready)
    {
        return texture->Status == RasterStatus::Pending ? RasterStatus::NotReady : texture->Status;
    }
    if (InitializedMip(*texture, region.Mip) == false)
    {
        return RasterStatus::InvalidState;
    }
    usize pitch = 0, size = 0;
    const auto rows = TransferRows(region, rowPitch, pitch, size);
    if (rows != RasterStatus::Ready)
    {
        return rows;
    }
    usize slot = 0;
    const auto reserved = ReserveLifetime(gReadbacks, slot, [](usize candidate) noexcept {
        return backend::LifetimeTransferAvailable(true, candidate);
    });
    if (reserved != RasterStatus::Ready)
    {
        return reserved;
    }
    internal::TextureTransfer info;
    info.Operation = internal::TextureTransferOperation::Readback;
    info.Source = RasterAccess::Slot(handle);
    info.SourceRegion = region;
    info.RowPitch = pitch;
    info.Size = size;
    const auto started = backend::LifetimeTextureTransfer(slot, info, nullptr);
    if (GetStartup().State != StartupState::Ready)
    {
        return Admission(device);
    }
    if (started != RasterStatus::Ready && started != RasterStatus::Pending)
    {
        return started;
    }
    auto& request = gReadbacks[slot];
    request.Owner = DeviceOwner();
    request.Published = true;
    request.Kind = RasterKind::Texture;
    request.Source = info.Source;
    request.Region = region;
    request.RowPitch = rowPitch;
    request.Size = rowPitch * region.Height;
    request.NativePitch = pitch;
    request.NativeSize = size;
    Retain(RasterKind::Texture, info.Source);
    LifetimeAccess::Set(output, {DeviceOwner(), slot, request.Generation});
    return RasterStatus::Pending;
}
RasterStatus GetTextureResidency(DeviceHandle device, TextureHandle handle, TextureResidency& output) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto* texture = Resolve(handle, RasterKind::Texture);
    if (texture == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    TextureResidency result;
    result.MipLevels = texture->Texture.MipLevels;
    for (uint32 mip = 0; mip < result.MipLevels; ++mip)
    {
        if (InitializedMip(*texture, mip) == false)
        {
            continue;
        }
        result.InitializedMips |= uint32{1} << mip;
        const auto width = texture->Texture.Width >> mip, height = texture->Texture.Height >> mip;
        result.InitializedBytes += uint64{TextureBlockRows(texture->Texture.Format, width == 0 ? 1 : width)} *
                                   TextureBlockRows(texture->Texture.Format, height == 0 ? 1 : height) *
                                   TextureBlockBytes(texture->Texture.Format) * TextureSlices(texture->Texture, mip);
    }
    output = result;
    return RasterStatus::Ready;
}
RasterStatus GetStatus(DeviceHandle device, TransferTicket ticket) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = ResolveLifetime(gUploads, ticket);
    if (record == nullptr || record->Mutable == false)
    {
        return RasterStatus::InvalidHandle;
    }
    if (FrameOpen() == false)
    {
        const auto polled = PollLifetime(device);
        if (polled != RasterStatus::Ready)
        {
            return polled;
        }
    }
    return record->Status;
}
RasterStatus Release(DeviceHandle device, TransferTicket& ticket) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = ResolveLifetime(gUploads, ticket);
    if (record == nullptr || record->Mutable == false)
    {
        return RasterStatus::InvalidHandle;
    }
    record->Published = false;
    ticket = {};
    PollTransfers();
    Collect();
    return Admission(device);
}
} // namespace ludus::graphics::rhi
