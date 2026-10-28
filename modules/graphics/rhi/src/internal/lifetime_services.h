#pragma once
// Private implementation included only by raster.cpp, sharing its one registry.
// Thanks to the Ludus RHI/GDI design review, "Identity and physical lifetime"
// (docs/architecture/rhi-gdi.md), for the separate CPU lease and GPU completion
// obligations. Four fixed slices per transfer direction are reused only after
// backend completion/mapping, never merely on logical cancellation.
namespace ludus::graphics::rhi
{
namespace internal
{
using namespace foundation;
struct LifetimeAccess final
{
    template <typename T>
    static uint64 Owner(T handle) noexcept
    {
        return handle.Owner;
    }
    template <typename T>
    static usize Slot(T handle) noexcept
    {
        return handle.Slot == 0 ? RASTER_INVALID : handle.Slot - 1;
    }
    template <typename T>
    static uint64 Generation(T handle) noexcept
    {
        return handle.Generation;
    }
    template <typename T>
    static void Set(T& handle, const RasterIdentity& identity) noexcept
    {
        handle.Owner = DeviceOwner();
        handle.Slot = static_cast<uint32>(identity.Slot + 1);
        handle.Generation = identity.Generation;
    }
    static uint64 Ordinal(SubmissionToken token) noexcept
    {
        return token.Ordinal;
    }
    static void Set(SubmissionToken& token, uint64 ordinal) noexcept
    {
        token.Owner = DeviceOwner();
        token.Ordinal = ordinal;
    }
};
} // namespace internal
namespace
{
struct LifetimeRecord
{
    uint64 Owner = 0;
    uint64 Generation = 1;
    bool Published = false;
};
struct BatchRecord final : LifetimeRecord
{
    bool Finished = false;
    usize Count = 0;
    StorageArray<RasterPacket> Packets;
};
struct TransferRecord final : LifetimeRecord
{
    RasterStatus Status = RasterStatus::Pending;
    usize Source = 0;
    usize Size = 0;
    BufferHandle Upload{};
    bool Mutable = false;
    bool CompletedMutation = false;
    RasterKind Kind = RasterKind::Buffer;
    usize Other = RASTER_INVALID;
    TextureRegion Region;
    usize RowPitch = 0;
    usize NativePitch = 0;
    usize NativeSize = 0;
};
struct PipelineLease final : LifetimeRecord
{
    usize Pipeline = 0;
};
StorageTable<BatchRecord> gBatches;
TransferRecord gUploads[LIFETIME_TRANSFERS];
TransferRecord gReadbacks[LIFETIME_TRANSFERS];
StorageTable<PipelineLease> gPipelineLeases;
alignas(16) uint8 gReadbackScratch[LIFETIME_TRANSFER_BYTES]{};
alignas(16) uint8 gUploadBytes[LIFETIME_TRANSFERS][LIFETIME_TRANSFER_BYTES]{};

template <typename T, typename H, usize N>
T* ResolveLifetime(T (&records)[N], H handle) noexcept
{
    const auto slot = LifetimeAccess::Slot(handle);
    if (slot >= N || LifetimeAccess::Owner(handle) == 0)
    {
        return nullptr;
    }
    auto& record = records[slot];
    return record.Published && record.Owner == DeviceOwner() && record.Owner == LifetimeAccess::Owner(handle) &&
                   record.Generation == LifetimeAccess::Generation(handle)
               ? &record
               : nullptr;
}
template <typename T, typename H>
T* ResolveLifetime(StorageTable<T>& records, H handle) noexcept
{
    const auto slot = LifetimeAccess::Slot(handle);
    if (slot >= records.GetSize() || LifetimeAccess::Owner(handle) == 0)
    {
        return nullptr;
    }
    auto& record = records[slot];
    return record.Published && record.Owner == DeviceOwner() && record.Owner == LifetimeAccess::Owner(handle) &&
                   record.Generation == LifetimeAccess::Generation(handle)
               ? &record
               : nullptr;
}
template <typename T>
RasterStatus ReserveLifetime(StorageTable<T>& records, usize& slot, usize limit) noexcept
{
    bool exhausted = false;
    for (slot = 0; slot < limit; ++slot)
    {
        if (slot == records.GetSize() && records.EnsureSlots(slot + 1) == false)
        {
            return StorageFailure();
        }
        const auto& record = records[slot];
        if (record.Owner != 0)
        {
            continue;
        }
        if (record.Generation == 0)
        {
            exhausted = true;
            continue;
        }
        return RasterStatus::Ready;
    }
    return exhausted ? RasterStatus::IdentityExhausted : RasterStatus::CapacityExceeded;
}
template <typename T>
void ResetLifetime(T& record) noexcept
{
    const auto generation = RasterNextGeneration(record.Generation);
    record = {};
    record.Generation = generation;
}
template <typename T, usize N>
RasterStatus ReserveLifetime(T (&records)[N], usize& slot, bool (*available)(usize) noexcept = nullptr) noexcept
{
    bool exhausted = false;
    for (slot = 0; slot < N; ++slot)
    {
        if (records[slot].Owner != 0)
        {
            continue;
        }
        if (records[slot].Generation == 0)
        {
            exhausted = true;
            continue;
        }
        if (slot >= CurrentStorageLimits().TransferSlices || (available != nullptr && available(slot) == false))
        {
            continue;
        }
        return RasterStatus::Ready;
    }
    return exhausted ? RasterStatus::IdentityExhausted : RasterStatus::CapacityExceeded;
}
void PacketReferences(const RasterPacket& packet, bool retain) noexcept
{
    const auto reference = [retain](RasterKind kind, usize slot) noexcept {
        if (retain)
        {
            Retain(kind, slot);
        }
        else
        {
            Release(kind, slot);
        }
    };
    reference(RasterKind::Pipeline, packet.Pipeline);
    reference(RasterKind::Set, packet.Set);
    reference(RasterKind::Buffer, packet.Indices);
    if (packet.Indirect != RASTER_INVALID)
    {
        reference(RasterKind::Buffer, packet.Indirect);
    }
    for (usize i = 0; i < At(RasterKind::Pipeline, packet.Pipeline).Pipeline.StreamCount; ++i)
    {
        reference(RasterKind::Buffer, packet.Vertices[i]);
    }
}
void DropBatch(BatchRecord& record) noexcept
{
    for (usize i = 0; i < record.Count; ++i)
    {
        PacketReferences(record.Packets[i], false);
    }
    ResetLifetime(record);
}
RasterStatus PacketStatus(const RasterPacket& packet) noexcept
{
    const Record* records[]{&At(RasterKind::Pipeline, packet.Pipeline),
                            &At(RasterKind::Set, packet.Set),
                            &At(RasterKind::Buffer, packet.Indices)};
    for (const auto* record : records)
    {
        if (record->Status != RasterStatus::Ready)
        {
            return record->Status;
        }
    }
    if (packet.Indirect != RASTER_INVALID && At(RasterKind::Buffer, packet.Indirect).Status != RasterStatus::Ready)
    {
        return At(RasterKind::Buffer, packet.Indirect).Status;
    }
    for (usize i = 0; i < records[0]->Pipeline.StreamCount; ++i)
    {
        const auto status = At(RasterKind::Buffer, packet.Vertices[i]).Status;
        if (status != RasterStatus::Ready)
        {
            return status;
        }
    }
    return RasterStatus::Ready;
}
bool SamePipeline(const RasterPipelineInfo& a, const RasterPipelineInfo& b) noexcept
{
    if (a.Vertex != b.Vertex || a.Fragment != b.Fragment || a.Layout != b.Layout || a.Depth != b.Depth ||
        a.Blend != b.Blend || a.Target != b.Target || a.DepthWrite != b.DepthWrite ||
        a.ShouldWriteColor != b.ShouldWriteColor || a.DepthCompare != b.DepthCompare ||
        a.StreamCount != b.StreamCount || a.AttributeCount != b.AttributeCount)
    {
        return false;
    }
    for (usize i = 0; i < a.StreamCount; ++i)
    {
        if (a.Streams[i].Stride != b.Streams[i].Stride || a.Streams[i].PerInstance != b.Streams[i].PerInstance)
        {
            return false;
        }
    }
    for (usize i = 0; i < a.AttributeCount; ++i)
    {
        const auto& x = a.Attributes[i];
        const auto& y = b.Attributes[i];
        if (x.Location != y.Location || x.Stream != y.Stream || x.Offset != y.Offset || x.Format != y.Format)
        {
            return false;
        }
    }
    return true;
}
void CanonicalizePipeline(RasterPipelineInfo& info) noexcept
{
    // At most eight entries: bounded field-wise insertion, no owning algorithm/cache.
    for (usize i = 1; i < info.AttributeCount; ++i)
    {
        const auto entry = info.Attributes[i];
        usize j = i;
        while (j > 0 && entry.Location < info.Attributes[j - 1].Location)
        {
            info.Attributes[j] = info.Attributes[j - 1];
            --j;
        }
        info.Attributes[j] = entry;
    }
}
void RetireTransfer(TransferRecord& record, bool readback, usize slot) noexcept
{
    backend::LifetimeReleaseTransfer(readback, slot);
    if (readback)
    {
        Release(record.Kind, record.Source);
    }
    else if (record.Mutable)
    {
        Release(record.Kind, record.Source);
        if (record.Other != RASTER_INVALID)
        {
            Release(RasterKind::Texture, record.Other);
        }
    }
    else
    {
        auto* buffer = Resolve(record.Upload, RasterKind::Buffer, false);
        if (buffer != nullptr)
        {
            buffer->Published = false;
            --buffer->References;
        }
    }
    ResetLifetime(record);
}
void PollTransfers() noexcept
{
    for (usize i = 0; i < LIFETIME_TRANSFERS; ++i)
    {
        for (usize direction = 0; direction < 2; ++direction)
        {
            const bool readback = direction != 0;
            auto& record = readback ? gReadbacks[i] : gUploads[i];
            if (record.Owner == 0)
            {
                continue;
            }
            const auto status = backend::LifetimePollTransfer(readback, i);
            if (GetStartup().State != StartupState::Ready)
            {
                return; // completion-observed loss can reset all registries.
            }
            if (status == RasterStatus::Pending)
            {
                continue;
            }
            record.Status = status;
            if (readback == false && record.Mutable && record.CompletedMutation == false)
            {
                auto& destination = At(record.Kind, record.Source);
                destination.Status = status;
                if (status == RasterStatus::Ready)
                {
                    if (record.Kind == RasterKind::Buffer)
                    {
                        ++destination.BufferRevision;
                    }
                    else
                    {
                        destination.InitializedMips |= uint32{1} << record.Region.Mip;
                        destination.ColorDefined =
                            destination.InitializedMips == ((uint32{1} << destination.Texture.MipLevels) - 1);
                        if (destination.Texture.Attachment)
                        {
                            destination.AttachmentState[SubresourceIndex(destination.Texture, record.Region.Mip, 0)] |=
                                COLOR_DEFINED;
                            destination.ColorDefined = AllDefined(destination.AttachmentState, COLOR_DEFINED);
                        }
                        destination.TextureUse = RasterTextureUse::SampledBoth;
                        ++destination.TextureRevision;
                    }
                }
                record.CompletedMutation = true;
            }
            if (readback == false && record.Mutable == false && status == RasterStatus::Ready)
            {
                const auto* buffer = Resolve(record.Upload, RasterKind::Buffer, false);
                record.Status = buffer != nullptr ? buffer->Status : RasterStatus::Failed;
                if (record.Status == RasterStatus::Pending)
                {
                    continue;
                }
            }
            if (record.Published == false)
            {
                RetireTransfer(record, readback, i);
            }
        }
    }
}
} // namespace
namespace internal
{
void ResetLifetimeRecords() noexcept
{
    for (auto& batch : gBatches)
    {
        ResetLifetime(batch);
    }
    for (auto& record : gUploads)
    {
        ResetLifetime(record);
    }
    for (auto& record : gReadbacks)
    {
        ResetLifetime(record);
    }
    for (auto& record : gPipelineLeases)
    {
        ResetLifetime(record);
    }
}
} // namespace internal
RasterStatus GetLifetimeCapabilities(DeviceHandle device, LifetimeCapabilities& output) noexcept
{
    const auto admission = Admission(device, false);
    if (admission == RasterStatus::InvalidHandle)
    {
        return admission;
    }
    output = admission == RasterStatus::Ready ? LifetimeCapabilities{CurrentStorageLimits().Batches,
                                                                     CurrentStorageLimits().DrawsPerPlan,
                                                                     CurrentStorageLimits().TransferSlices,
                                                                     CurrentStorageLimits().TransferBytes,
                                                                     CurrentStorageLimits().ResourcesPerKind}
                                              : LifetimeCapabilities{};
    return admission;
}
RasterStatus BeginCommands(DeviceHandle device, CommandBatch& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (LifetimeAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    usize slot = 0;
    const auto reservation = ReserveLifetime(gBatches, slot, CurrentStorageLimits().Batches);
    if (reservation != RasterStatus::Ready)
    {
        return reservation;
    }
    auto& record = gBatches[slot];
    record.Owner = DeviceOwner();
    record.Published = true;
    LifetimeAccess::Set(output, {DeviceOwner(), slot, record.Generation});
    return RasterStatus::Ready;
}
RasterStatus RecordDraw(DeviceHandle device, CommandBatch batch, const RasterDraw& draw) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = ResolveLifetime(gBatches, batch);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (record->Finished)
    {
        return RasterStatus::InvalidState;
    }
    if (record->Count == CurrentStorageLimits().DrawsPerPlan)
    {
        return RasterStatus::CapacityExceeded;
    }
    RasterPacket packet;
    const auto valid = ValidateDraw(device, draw, packet);
    if (valid != RasterStatus::Ready)
    {
        return valid;
    }
    if (record->Packets.TryResize(record->Count + 1) == false)
    {
        return StorageFailure();
    }
    PacketReferences(packet, true);
    record->Packets[record->Count++] = packet;
    return RasterStatus::Ready;
}
RasterStatus FinishCommands(DeviceHandle device, CommandBatch batch) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = ResolveLifetime(gBatches, batch);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (record->Finished)
    {
        return RasterStatus::InvalidState;
    }
    record->Finished = true;
    return RasterStatus::Ready;
}
RasterStatus DiscardCommands(DeviceHandle device, CommandBatch& batch) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = ResolveLifetime(gBatches, batch);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    DropBatch(*record);
    batch = {};
    Collect();
    return RasterStatus::Ready;
}
RasterStatus GetStatus(DeviceHandle device, SubmissionToken completion) noexcept
{
    // Identity first, including after loss: stale tokens cannot query a new device.
    DeviceInfo info;
    if (GetDeviceInfo(device, info) == DeviceStatus::InvalidHandle || LifetimeAccess::Owner(completion) == 0 ||
        LifetimeAccess::Owner(completion) != DeviceOwner())
    {
        return RasterStatus::InvalidHandle;
    }
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto ordinal = LifetimeAccess::Ordinal(completion);
    if (ordinal == 0 || ordinal < gFirstAcceptedSubmission || ordinal > gLastAcceptedSubmission)
    {
        return RasterStatus::InvalidHandle;
    }
    const auto completed = backend::RasterCompleted();
    const auto current = Admission(device, false);
    if (current != RasterStatus::Ready)
    {
        return current;
    }
    Collect();
    const auto collected = Admission(device, false);
    if (collected != RasterStatus::Ready)
    {
        return collected;
    }
    return ordinal <= completed ? RasterStatus::Ready : RasterStatus::Pending;
}
RasterStatus EndFrame(DeviceHandle device, SubmissionToken& completion) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (FrameOpen() == false || LifetimeAccess::Owner(completion) != 0)
    {
        return RasterStatus::InvalidState;
    }
    const auto reservation = ReserveFrameSubmission();
    if (reservation != RasterStatus::Ready)
    {
        return reservation;
    }
    const auto ordinal = gFrameSubmission;
    const auto ended = EndFrame(device);
    if (ended == DeviceStatus::Ready || ended == DeviceStatus::Skipped)
    {
        // RasterSubmit may observe spontaneous queue loss and reset the owner state.
        const auto current = Admission(device, false);
        if (current != RasterStatus::Ready)
        {
            return current;
        }
        LifetimeAccess::Set(completion, ordinal);
        return RasterStatus::Ready;
    }
    return ended == DeviceStatus::DeviceLost ? RasterStatus::DeviceLost : RasterStatus::Failed;
}
RasterStatus
SubmitCommands(DeviceHandle device, SurfaceHandle surface, CommandBatch& batch, SubmissionToken& completion) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = ResolveLifetime(gBatches, batch);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (record->Finished == false || LifetimeAccess::Owner(completion) != 0)
    {
        return RasterStatus::InvalidState;
    }
    if (record->Count > CurrentDeviceProfile().Raster.DrawsPerFrame)
    {
        return RasterStatus::CapacityExceeded;
    }
    for (usize i = 0; i < record->Count; ++i)
    {
        const auto status = PacketStatus(record->Packets[i]);
        if (At(RasterKind::Pipeline, record->Packets[i].Pipeline).Pipeline.Target != RasterTarget::Surface)
        {
            return RasterStatus::InvalidDescription;
        }
        if (status != RasterStatus::Ready)
        {
            return status;
        }
        const auto compatible = ValidatePacketPass(record->Packets[i], {}, true);
        if (compatible != RasterStatus::Ready)
        {
            return compatible;
        }
    }
    if (gSubmissions.Next == 0)
    {
        return RasterStatus::IdentityExhausted;
    }
    const auto preparedStorage = backend::RasterPrepareStorage();
    if (preparedStorage != RasterStatus::Ready)
    {
        return preparedStorage;
    }
    const auto reserve = backend::RasterReserveSubmission();
    if (reserve != RasterStatus::Ready)
    {
        return reserve;
    }
    const auto begun = BeginFrame(device, surface);
    if (begun != DeviceStatus::Ready)
    {
        backend::RasterDiscardSubmission();
        return begun == DeviceStatus::Skipped         ? RasterStatus::NotReady
               : begun == DeviceStatus::InvalidHandle ? RasterStatus::InvalidHandle
               : begun == DeviceStatus::DeviceLost    ? RasterStatus::DeviceLost
                                                      : RasterStatus::Failed;
    }
    const auto current = Admission(device, false);
    if (current != RasterStatus::Ready)
    {
        return current;
    }
    RasterStatus encoded = RasterStatus::Ready;
    for (usize i = 0; i < record->Count; ++i)
    {
        encoded = EncodePacket(record->Packets[i]);
        if (encoded != RasterStatus::Ready)
        {
            break;
        }
    }
    const auto ended = EndFrame(device, completion);
    if (record->Owner != 0)
    {
        DropBatch(*record);
    }
    batch = {};
    Collect();
    return encoded != RasterStatus::Ready ? encoded : ended;
}
RasterStatus PollLifetime(DeviceHandle device) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    Collect();
    auto current = Admission(device);
    if (current != RasterStatus::Ready)
    {
        return current;
    }
    PollTransfers();
    current = Admission(device);
    if (current != RasterStatus::Ready)
    {
        return current;
    }
    for (usize slot = 0; slot < gRecords[static_cast<usize>(RasterKind::Pipeline)].GetSize(); ++slot)
    {
        auto& pipeline = At(RasterKind::Pipeline, slot);
        if (pipeline.QueuedPipeline == false)
        {
            continue;
        }
        pipeline.QueuedPipeline = false;
        const auto status = backend::RasterCreatePipeline(slot, pipeline.Pipeline, pipeline.Request);
        if (pipeline.Owner == 0)
        {
            break;
        }
        if (pipeline.Status == RasterStatus::Pending)
        {
            pipeline.Status = status;
        }
        break;
    }
    Collect();
    return Admission(device);
}
RasterStatus
RequestPipeline(DeviceHandle device, const RasterPipelineDescription& description, PipelineRequest& output) noexcept
{
    if (LifetimeAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    RasterPipelineInfo info;
    const auto valid = ValidatePipeline(device, description, info);
    if (valid != RasterStatus::Ready)
    {
        return valid;
    }
    CanonicalizePipeline(info);
    usize leaseSlot = 0;
    const auto reservation = ReserveLifetime(gPipelineLeases, leaseSlot, CurrentStorageLimits().ResourcesPerKind);
    if (reservation != RasterStatus::Ready)
    {
        return reservation;
    }
    usize pipelineSlot = RASTER_INVALID;
    for (usize i = 0; i < gRecords[static_cast<usize>(RasterKind::Pipeline)].GetSize(); ++i)
    {
        const auto& record = At(RasterKind::Pipeline, i);
        if (record.Owner == DeviceOwner() && record.Published && record.ServiceOwned &&
            SamePipeline(record.Pipeline, info))
        {
            pipelineSlot = i;
            Retain(RasterKind::Pipeline, i);
            break;
        }
    }
    if (pipelineSlot == RASTER_INVALID)
    {
        const auto reserved = Reserve(RasterKind::Pipeline, pipelineSlot);
        if (reserved != RasterStatus::Ready)
        {
            return reserved;
        }
        auto& record = At(RasterKind::Pipeline, pipelineSlot);
        record.Pipeline = info;
        record.Published = true;
        record.ServiceOwned = true;
        record.QueuedPipeline = true;
        Dependencies(record, RasterKind::Pipeline, true);
    }
    auto& lease = gPipelineLeases[leaseSlot];
    lease.Owner = DeviceOwner();
    lease.Published = true;
    lease.Pipeline = pipelineSlot;
    LifetimeAccess::Set(output, {DeviceOwner(), leaseSlot, lease.Generation});
    return At(RasterKind::Pipeline, pipelineSlot).Status == RasterStatus::Ready ? RasterStatus::Ready
                                                                                : RasterStatus::Pending;
}
RasterStatus GetStatus(DeviceHandle device, PipelineRequest request) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto* lease = ResolveLifetime(gPipelineLeases, request);
    if (lease == nullptr)
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
    return At(RasterKind::Pipeline, lease->Pipeline).Status;
}
RasterStatus GetRequestedPipeline(DeviceHandle device, PipelineRequest request, RasterPipelineHandle& output) noexcept
{
    if (RasterAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    const auto status = GetStatus(device, request);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    const auto* lease = ResolveLifetime(gPipelineLeases, request);
    if (lease == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    const auto& pipeline = At(RasterKind::Pipeline, lease->Pipeline);
    RasterAccess::Set(output, {pipeline.Owner, lease->Pipeline, pipeline.Generation});
    return RasterStatus::Ready;
}
RasterStatus Release(DeviceHandle device, PipelineRequest& request) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* lease = ResolveLifetime(gPipelineLeases, request);
    if (lease == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    const auto slot = lease->Pipeline;
    ResetLifetime(*lease);
    request = {};
    bool owned = false;
    for (const auto& other : gPipelineLeases)
    {
        owned = owned || (other.Owner == DeviceOwner() && other.Published && other.Pipeline == slot);
    }
    auto& pipeline = At(RasterKind::Pipeline, slot);
    --pipeline.References;
    if (owned == false)
    {
        pipeline.Published = false;
        if (pipeline.QueuedPipeline)
        {
            pipeline.QueuedPipeline = false;
            pipeline.Status = RasterStatus::Failed;
        }
    }
    Collect();
    return RasterStatus::Ready;
}
RasterStatus RequestBufferUpload(DeviceHandle device,
                                 const BufferDescription& description,
                                 const uint8* bytes,
                                 usize byteCount,
                                 UploadTicket& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (LifetimeAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    const auto alignment = description.Role == BufferRole::Index16   ? 2U
                           : description.Role == BufferRole::Uniform ? 16U
                                                                     : 4U;
    if (bytes == nullptr || byteCount == 0 || byteCount != description.Size || byteCount % alignment != 0 ||
        static_cast<uint8>(description.Role) > static_cast<uint8>(BufferRole::Uniform))
    {
        return RasterStatus::InvalidDescription;
    }
    if (byteCount > backend::RasterLimits().MaxBufferSize)
    {
        return RasterStatus::InvalidDescription;
    }
    if (byteCount > CurrentStorageLimits().TransferBytes)
    {
        return RasterStatus::CapacityExceeded;
    }
    const auto polled = PollLifetime(device);
    if (polled != RasterStatus::Ready)
    {
        return polled;
    }
    usize slot = 0;
    const auto reserved = ReserveLifetime(gUploads, slot, [](usize candidate) noexcept {
        return backend::LifetimeTransferAvailable(false, candidate);
    });
    if (reserved != RasterStatus::Ready)
    {
        return reserved;
    }
    std::memcpy(gUploadBytes[slot], bytes, byteCount);
    const usize padded = (byteCount + 3) & ~usize{3};
    std::memset(gUploadBytes[slot] + byteCount, 0, padded - byteCount);
    BufferHandle buffer;
    const auto created = Publish(
        RasterKind::Buffer,
        buffer,
        [&](usize bufferSlot, Record& record) noexcept {
            record.Buffer = description;
            if (description.Role == BufferRole::Index16 || description.Role == BufferRole::Index32)
            {
                if (record.Indices.TryResize(byteCount) == false)
                {
                    return StorageFailure();
                }
                std::memcpy(record.Indices.GetData(), gUploadBytes[slot], byteCount);
            }
            return backend::LifetimeUpload(slot, description, {bufferSlot, record.Request}, gUploadBytes[slot], padded);
        },
        description.Size);
    if (created != RasterStatus::Ready && created != RasterStatus::Pending)
    {
        return created;
    }
    auto& request = gUploads[slot];
    request.Owner = DeviceOwner();
    request.Published = true;
    request.Source = RasterAccess::Slot(buffer);
    request.Size = byteCount;
    request.Upload = buffer;
    LifetimeAccess::Set(output, {DeviceOwner(), slot, request.Generation});
    return RasterStatus::Pending;
}
RasterStatus RequestBufferReadback(DeviceHandle device,
                                   BufferHandle source,
                                   usize offset,
                                   usize size,
                                   ReadbackTicket& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (LifetimeAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    const auto* buffer = Resolve(source, RasterKind::Buffer);
    if (buffer == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (buffer->Status != RasterStatus::Ready)
    {
        return buffer->Status;
    }
    if (size == 0 || size % internal::BUFFER_ALIGNMENT_BYTES != 0 || offset % internal::BUFFER_ALIGNMENT_BYTES != 0 ||
        Fits(offset, size, buffer->Buffer.Size) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    if (size > CurrentStorageLimits().TransferBytes)
    {
        return RasterStatus::CapacityExceeded;
    }
    const auto polled = PollLifetime(device);
    if (polled != RasterStatus::Ready)
    {
        return polled;
    }
    usize slot = 0;
    const auto reserved = ReserveLifetime(gReadbacks, slot, [](usize candidate) noexcept {
        return backend::LifetimeTransferAvailable(true, candidate);
    });
    if (reserved != RasterStatus::Ready)
    {
        return reserved;
    }
    const auto started =
        backend::LifetimeReadback(slot, buffer->Buffer.Role, {RasterAccess::Slot(source), offset, size});
    if (started != RasterStatus::Ready && started != RasterStatus::Pending)
    {
        return started;
    }
    if (GetStartup().State != StartupState::Ready)
    {
        return Admission(device);
    }
    auto& request = gReadbacks[slot];
    request.Owner = DeviceOwner();
    request.Published = true;
    request.Source = RasterAccess::Slot(source);
    request.Size = size;
    request.NativeSize = size;
    Retain(RasterKind::Buffer, request.Source);
    LifetimeAccess::Set(output, {DeviceOwner(), slot, request.Generation});
    return RasterStatus::Pending;
}
RasterStatus GetStatus(DeviceHandle device, UploadTicket ticket) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = ResolveLifetime(gUploads, ticket);
    if (record == nullptr)
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
RasterStatus GetStatus(DeviceHandle device, ReadbackTicket ticket) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = ResolveLifetime(gReadbacks, ticket);
    if (record == nullptr)
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
RasterStatus TakeUploadedBuffer(DeviceHandle device, UploadTicket& ticket, BufferHandle& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (RasterAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    const auto status = GetStatus(device, ticket);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    auto* record = ResolveLifetime(gUploads, ticket);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    output = record->Upload;
    backend::LifetimeReleaseTransfer(false, LifetimeAccess::Slot(ticket));
    ResetLifetime(*record);
    ticket = {};
    return RasterStatus::Ready;
}
RasterStatus CopyReadback(DeviceHandle device, ReadbackTicket ticket, uint8* output, usize capacity) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto* record = ResolveLifetime(gReadbacks, ticket);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (output == nullptr || capacity < record->Size)
    {
        return RasterStatus::InvalidDescription;
    }
    const auto status = GetStatus(device, ticket);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    const auto copied =
        backend::LifetimeCopyReadback(LifetimeAccess::Slot(ticket), gReadbackScratch, record->NativeSize);
    if (copied == RasterStatus::Ready)
    {
        const auto current = Admission(device, false);
        if (current != RasterStatus::Ready)
        {
            return current;
        }
        if (record->Kind == RasterKind::Texture)
        {
            std::memset(output, 0, record->Size);
            for (usize row = 0; row < record->Region.Height; ++row)
            {
                std::memcpy(output + row * record->RowPitch,
                            gReadbackScratch + row * record->NativePitch,
                            static_cast<usize>(record->Region.Width) * 4);
            }
        }
        else
        {
            std::memcpy(output, gReadbackScratch, record->Size);
        }
    }
    return copied;
}
RasterStatus Release(DeviceHandle device, UploadTicket& ticket) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = ResolveLifetime(gUploads, ticket);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    record->Published = false;
    auto* buffer = Resolve(record->Upload, RasterKind::Buffer, false);
    if (buffer != nullptr)
    {
        buffer->Published = false;
    }
    ticket = {};
    PollTransfers();
    Collect();
    return Admission(device);
}
RasterStatus Release(DeviceHandle device, ReadbackTicket& ticket) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = ResolveLifetime(gReadbacks, ticket);
    if (record == nullptr)
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
