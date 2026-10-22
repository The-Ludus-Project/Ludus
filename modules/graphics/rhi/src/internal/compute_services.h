#pragma once
// Included only by raster.cpp. Compute packets share its physical registry,
// non-wrapping identities, immutable binding snapshots and R2 completion leases.
namespace ludus::graphics::rhi
{
namespace
{
RasterStatus ComputeAdmission(DeviceHandle device, bool mutation) noexcept
{
    const auto admission = Admission(device, mutation);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    DeviceInfo info;
    const auto state = GetDeviceInfo(device, info);
    if (state != DeviceStatus::Ready)
    {
        return state == DeviceStatus::DeviceLost ? RasterStatus::DeviceLost : RasterStatus::NotReady;
    }
    return info.Enabled.Compute ? RasterStatus::Ready : RasterStatus::Unsupported;
}
RasterStatus ValidateCompute(DeviceHandle device, const ComputeDispatch& dispatch, ComputePacket& packet) noexcept
{
    const auto admission = ComputeAdmission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto* pipeline = Resolve(dispatch.Pipeline, RasterKind::ComputePipeline);
    const auto* set = Resolve(dispatch.Bindings, RasterKind::Set);
    if (pipeline == nullptr || set == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    const Record* records[]{pipeline, set};
    for (const auto* record : records)
    {
        if (record->Status != RasterStatus::Ready)
        {
            return record->Status == RasterStatus::Pending ? RasterStatus::NotReady : record->Status;
        }
    }
    if (pipeline->Compute.Layout != set->Set.Layout)
    {
        return RasterStatus::InvalidDescription;
    }
    const RasterStatus offsetsStatus = ValidateBindingOffsets(set->Set, dispatch.BindingOffsets);
    if (offsetsStatus != RasterStatus::Ready)
    {
        return offsetsStatus;
    }
    packet.BindingOffsets = dispatch.BindingOffsets;
    packet.DynamicOffsetCount = CollectDynamicOffsets(At(RasterKind::Layout, set->Set.Layout).Layout,
                                                      dispatch.BindingOffsets,
                                                      packet.DynamicOffsets);
    const auto limits = backend::ComputeLimits();
    for (usize i = 0; i < 3; ++i)
    {
        if (dispatch.Groups[i] == 0 || dispatch.Groups[i] > limits.MaxDispatch[i])
        {
            return RasterStatus::InvalidDescription;
        }
        packet.Groups[i] = dispatch.Groups[i];
    }
    packet.Pipeline = RasterAccess::Slot(dispatch.Pipeline);
    packet.Set = RasterAccess::Slot(dispatch.Bindings);
    return RasterStatus::Ready;
}
void ComputeReferences(const ComputePacket& packet, bool retain) noexcept
{
    if (retain)
    {
        Retain(RasterKind::ComputePipeline, packet.Pipeline);
        Retain(RasterKind::Set, packet.Set);
    }
    else
    {
        Release(RasterKind::ComputePipeline, packet.Pipeline);
        Release(RasterKind::Set, packet.Set);
    }
}
RasterStatus ComputePacketStatus(const ComputePacket& packet) noexcept
{
    const Record* records[]{&At(RasterKind::ComputePipeline, packet.Pipeline), &At(RasterKind::Set, packet.Set)};
    for (const auto* record : records)
    {
        if (record->Status != RasterStatus::Ready)
        {
            return record->Status;
        }
    }
    return RasterStatus::Ready;
}
RasterStatus EncodeCompute(const ComputePacket& packet) noexcept
{
    FrameRetain(RasterKind::ComputePipeline, packet.Pipeline);
    FrameRetain(RasterKind::Set, packet.Set);
    const auto& set = At(RasterKind::Set, packet.Set).Set;
    const RasterLayout& layout = set.Description;
    for (usize i = 0; i < layout.DescriptorCount; ++i)
    {
        const RasterBindingKind bindingKind = layout.GetDescriptorBinding(i).Kind;
        const RasterKind kind = bindingKind == RasterBindingKind::RayScene ? RasterKind::RayScene
                                : TextureBinding(bindingKind) == true      ? RasterKind::View
                                                                           : RasterKind::Buffer;
        FrameRetain(kind, set.Slots[i]);
        if (kind == RasterKind::View)
        {
            FrameRetain(RasterKind::Texture, At(RasterKind::View, set.Slots[i]).TextureSlot);
        }
        if (layout.GetDescriptorBinding(i).Kind == RasterBindingKind::StorageReadWrite)
        {
            At(RasterKind::Buffer, set.Slots[i]).FrameBufferWritten = true;
        }
    }
    return backend::ComputeEncode(packet);
}
} // namespace
RasterStatus GetComputeCapabilities(DeviceHandle device, ComputeCapabilities& output) noexcept
{
    const auto admission = ComputeAdmission(device, false);
    if (admission != RasterStatus::InvalidHandle)
    {
        output = admission == RasterStatus::Ready ? backend::ComputeLimits() : ComputeCapabilities{};
    }
    return admission;
}
RasterStatus CreateComputePipeline(DeviceHandle device,
                                   const ComputePipelineDescription& description,
                                   ComputePipelineHandle& output) noexcept
{
    const auto admission = ComputeAdmission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    const auto* shader = Resolve(description.Shader, RasterKind::Shader);
    const auto* layout = Resolve(description.Layout, RasterKind::Layout);
    if (shader == nullptr || layout == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    const Record* dependencies[]{shader, layout};
    for (const auto* dependency : dependencies)
    {
        if (dependency->Status != RasterStatus::Ready)
        {
            return dependency->Status == RasterStatus::Pending ? RasterStatus::NotReady : dependency->Status;
        }
    }
    if (shader->Shader.Stage != ShaderStage::Compute || ShaderFits(shader->Shader, layout->Layout) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    for (usize i = 0; i < layout->Layout.Count; ++i)
    {
        if (layout->Layout.Entries[i].Visibility != RasterVisibility::Compute ||
            (BufferBinding(layout->Layout.Entries[i].Kind) == false &&
             layout->Layout.Entries[i].Kind != RasterBindingKind::RayScene &&
             layout->Layout.Entries[i].Kind != RasterBindingKind::Texture2D &&
             layout->Layout.Entries[i].Kind != RasterBindingKind::TextureDepth2D))
        {
            return RasterStatus::InvalidDescription;
        }
    }
    return Publish(RasterKind::ComputePipeline, output, [&](usize slot, Record& record) noexcept {
        record.Compute.ShouldPrepareAsynchronously =
            description.ShouldPrepareAsynchronously == true || shader->Shader.ShouldPrepareAsynchronously == true;
        record.Compute.Shader = RasterAccess::Slot(description.Shader);
        record.Compute.Layout = RasterAccess::Slot(description.Layout);
        for (usize i = 0; i < 3; ++i)
        {
            record.Compute.WorkgroupSize[i] = shader->Shader.WorkgroupSize[i];
        }
        return backend::ComputeCreatePipeline(slot, record.Compute, record.Request);
    });
}
RasterStatus GetStatus(DeviceHandle device, ComputePipelineHandle handle) noexcept
{
    const auto admission = ComputeAdmission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    Collect();
    const auto current = ComputeAdmission(device, false);
    if (current != RasterStatus::Ready)
    {
        return current;
    }
    const auto* record = Resolve(handle, RasterKind::ComputePipeline);
    return record == nullptr ? RasterStatus::InvalidHandle : record->Status;
}
RasterStatus Destroy(DeviceHandle device, ComputePipelineHandle& handle) noexcept
{
    const auto admission = ComputeAdmission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    auto* record = Resolve(handle, RasterKind::ComputePipeline);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    record->Published = false;
    --record->References;
    handle = {};
    Collect();
    return RasterStatus::Ready;
}
} // namespace ludus::graphics::rhi
