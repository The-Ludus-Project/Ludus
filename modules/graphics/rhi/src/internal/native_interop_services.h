#pragma once

// Thanks to The Qt Company, "QSGMetalTexture", "QSGVulkanTexture" and
// "QSGRendererInterface" (Qt Quick native interfaces): native wrappers borrow
// images on an exact device, and recording completion is not GPU completion.
// https://doc.qt.io/qt-6/qsgrendererinterface.html
// This Qt-independent bridge adds retained identities and shared-queue retirement.
#include <type_traits>

namespace ludus::graphics::rhi::internal
{
constexpr uint64 NATIVE_INVALID_OWNER = 0;
constexpr uint64 NATIVE_FIRST_GENERATION = 1;
constexpr uint64 NATIVE_EXHAUSTED_GENERATION = 0;
constexpr usize NATIVE_SLOT_OFFSET = 1;
constexpr uint32 NATIVE_NULL_SLOT = 0;
template <typename Handle>
concept NativeInteropHandle = std::is_same_v<Handle, NativeDeviceLease> || std::is_same_v<Handle, NativeTextureRead>;
struct NativeInteropAccess final
{
    template <NativeInteropHandle T>
    static uint64 Owner(const T& value) noexcept
    {
        return value.mOwner;
    }
    template <NativeInteropHandle T>
    static uint64 Generation(const T& value) noexcept
    {
        return value.mGeneration;
    }
    template <NativeInteropHandle T>
    static usize Slot(const T& value) noexcept
    {
        return value.mSlot == NATIVE_NULL_SLOT ? RASTER_INVALID : value.mSlot - NATIVE_SLOT_OFFSET;
    }
    template <NativeInteropHandle T>
    static void Set(T& value, const RasterIdentity& identity) noexcept
    {
        value.mOwner = identity.Owner;
        value.mSlot = static_cast<uint32>(identity.Slot + NATIVE_SLOT_OFFSET);
        value.mGeneration = identity.Generation;
    }
};
} // namespace ludus::graphics::rhi::internal
namespace ludus::graphics::rhi
{
namespace
{
struct NativeLeaseRecord
{
    uint64 Owner = NATIVE_INVALID_OWNER;
    uint64 Generation = NATIVE_FIRST_GENERATION;
};
struct NativeDeviceRecord final
{
    NativeLeaseRecord Lease{};
    DeviceHandle Session{};
    NativeDeviceInfo Info{};
    usize Reads = 0;
};
struct NativeReadRecord final
{
    NativeLeaseRecord Lease{};
    usize Device = RASTER_INVALID;
    usize Texture = RASTER_INVALID;
    NativeTextureInfo Info{};
    bool Releasing = false;
};
template <typename RecordType>
concept NativeInteropRecord =
    std::is_same_v<RecordType, NativeDeviceRecord> || std::is_same_v<RecordType, NativeReadRecord>;
template <typename RecordType, typename Handle>
concept NativeInteropRecordFor =
    (std::is_same_v<RecordType, NativeDeviceRecord> && std::is_same_v<Handle, NativeDeviceLease>) ||
    (std::is_same_v<RecordType, NativeReadRecord> && std::is_same_v<Handle, NativeTextureRead>);
/// Native interop leases, imported source and teardown state of one owner slot (R14c).
struct NativeInteropState final
{
    StorageTable<NativeDeviceRecord> Devices;
    StorageTable<NativeReadRecord> Reads;
    NativeDeviceDescription Source;
    usize Pins = 0;
    bool IsSourcePinned = false;
    bool IsInCallback = false;
    bool IsRetiring = false;
    bool HasTerminalLoss = false;
};
StaticArray<NativeInteropState, MAX_DEVICE_OWNERS> gNativeInteropStates{};
/// The native interop state of the bound owner slot.
NativeInteropState& GetNativeInteropState() noexcept
{
    return gNativeInteropStates[GetBoundOwnerSlot()];
}

bool SameNativeContext(const NativeDeviceIdentity& producer, const NativeDeviceIdentity& consumer) noexcept
{
    return producer.Api == consumer.Api && producer.Instance == consumer.Instance &&
           producer.PhysicalDevice == consumer.PhysicalDevice && producer.Device == consumer.Device &&
           producer.CommandQueue == consumer.CommandQueue && producer.QueueFamily == consumer.QueueFamily &&
           producer.QueueIndex == consumer.QueueIndex;
}
template <NativeInteropRecord T, NativeInteropHandle H>
    requires NativeInteropRecordFor<T, H>
T* NativeResolve(StorageTable<T>& records, const H& handle) noexcept
{
    const auto slot = NativeInteropAccess::Slot(handle);
    if (NativeInteropAccess::Owner(handle) == NATIVE_INVALID_OWNER || slot >= records.GetSize())
    {
        return nullptr;
    }
    auto& record = records[slot];
    return record.Lease.Owner == NativeInteropAccess::Owner(handle) &&
                   record.Lease.Generation == NativeInteropAccess::Generation(handle)
               ? &record
               : nullptr;
}
/// @brief Bind the owner slot whose interop table holds `handle` (R14c).
///
/// Leases and reads can outlive DestroyDevice while foreign consumers finish, so the slot is
/// found by its records rather than by a live device owner. An unknown handle binds nothing.
template <NativeInteropRecord T, NativeInteropHandle H>
    requires NativeInteropRecordFor<T, H>
void BindNativeOwner(StorageTable<T> NativeInteropState::* table, const H& handle) noexcept
{
    for (usize slot = 0; slot < MAX_DEVICE_OWNERS; ++slot)
    {
        if (NativeResolve(gNativeInteropStates[slot].*table, handle) != nullptr)
        {
            BindOwnerSlot(slot);
            return;
        }
    }
}
template <NativeInteropRecord T>
RasterStatus NativeReserve(StorageTable<T>& records, usize& slot) noexcept
{
    const usize limit = CurrentStorageLimits().ResourcesPerKind;
    bool exhausted = false;
    for (slot = 0; slot < limit; ++slot)
    {
        if (slot == records.GetSize() && records.EnsureSlots(slot + NATIVE_SLOT_OFFSET) == false)
        {
            return StorageFailure();
        }
        auto& record = records[slot];
        exhausted |= record.Lease.Generation == NATIVE_EXHAUSTED_GENERATION;
        if (record.Lease.Owner == NATIVE_INVALID_OWNER && record.Lease.Generation != NATIVE_EXHAUSTED_GENERATION)
        {
            return RasterStatus::Ready;
        }
    }
    return exhausted ? RasterStatus::IdentityExhausted : RasterStatus::CapacityExceeded;
}
template <NativeInteropRecord T>
void NativeReset(T& record) noexcept
{
    const auto generation = RasterNextGeneration(record.Lease.Generation);
    record = { .Lease = { .Generation = generation } };
}
NativeDeviceInfo NativeSnapshot(const NativeDeviceRecord& record) noexcept
{
    auto result = record.Info;
    result.Retiring = GetNativeInteropState().IsRetiring || internal::GetStartup().State != StartupState::Ready;
    return result;
}
RasterStatus CloseNativeRead(NativeTextureRead& handle, bool wait) noexcept
{
    NativeReadRecord* read = NativeResolve(GetNativeInteropState().Reads, handle);
    if (read == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (FrameOpen())
    {
        return RasterStatus::InvalidState;
    }
    const auto slot = NativeInteropAccess::Slot(handle);
    if (read->Releasing == false)
    {
        const auto released = backend::NativeTextureRelease(slot);
        if (released != RasterStatus::Ready && released != RasterStatus::Pending &&
            released != RasterStatus::DeviceLost)
        {
            return released;
        }
        read->Releasing = true;
    }
    const auto completed = backend::NativeTexturePoll(slot, wait);
    if (completed != RasterStatus::Ready && completed != RasterStatus::DeviceLost)
    {
        return completed;
    }
    backend::NativeTextureDestroy(slot);
    auto& texture = At(RasterKind::Texture, read->Texture);
    --texture.ExternalReads;
    Release(RasterKind::Texture, read->Texture);
    --GetNativeInteropState().Devices[read->Device].Reads;
    NativeReset(*read);
    handle = {};
    if (GetNativeInteropState().IsRetiring == false)
    {
        Collect();
    }
    return RasterStatus::Ready;
}
} // namespace
namespace internal
{
bool NativeInteropPinned() noexcept
{
    return GetNativeInteropState().Pins != 0;
}
bool NativeInteropBusy() noexcept
{
    return GetNativeInteropState().IsInCallback || GetNativeInteropState().IsRetiring;
}
bool DeferNativeInteropShutdown() noexcept
{
    if (GetNativeInteropState().Pins == 0)
    {
        return false;
    }
    GetNativeInteropState().IsRetiring = true;
    GetNativeInteropState().HasTerminalLoss |= internal::GetStartup().State == StartupState::DeviceLost;
    return true;
}
const NativeDeviceDescription* NativeDeviceRequest() noexcept
{
    return GetNativeInteropState().IsSourcePinned ? &GetNativeInteropState().Source : nullptr;
}
void ReleaseNativeDeviceSource() noexcept
{
    if (GetNativeInteropState().IsSourcePinned == false)
    {
        return;
    }
    const NativeDeviceLifetime lifetime = GetNativeInteropState().Source.Lifetime;
    GetNativeInteropState().Source = {};
    GetNativeInteropState().IsSourcePinned = false;
    GetNativeInteropState().IsInCallback = true;
    lifetime.Release(lifetime.Context);
    GetNativeInteropState().IsInCallback = false;
}
DeviceStatus PrepareNativeDeviceSource(const NativeDeviceDescription& description) noexcept
{
    const auto& identity = description.Identity;
    if (backend::NativeInteropSupported() == false || identity.Api != backend::Kind())
    {
        return DeviceStatus::Unsupported;
    }
    if (description.SerializedOwnerQueue == false)
    {
        return DeviceStatus::Unsupported;
    }
    if (identity.Device.IsNull() || identity.CommandQueue.IsNull() || description.Lifetime.Context == nullptr ||
        description.Lifetime.Retain == nullptr || description.Lifetime.Release == nullptr)
    {
        return DeviceStatus::InvalidDescription;
    }
    if (identity.Api == Backend::Vulkan)
    {
        constexpr uint32 VULKAN_1_1 = (1U << 22) | (1U << 12);
        if (identity.Instance.IsNull() || identity.PhysicalDevice.IsNull())
        {
            return DeviceStatus::InvalidDescription;
        }
        if (description.InstanceApiVersion < VULKAN_1_1)
        {
            return DeviceStatus::Unsupported;
        }
        if (description.CalibratedTimestamps != NativeCalibratedTimestamps::None &&
            description.CalibratedTimestamps != NativeCalibratedTimestamps::Khr &&
            description.CalibratedTimestamps != NativeCalibratedTimestamps::Ext)
        {
            return DeviceStatus::InvalidDescription;
        }
    }
    else if (identity.Instance.IsNull() == false || identity.PhysicalDevice.IsNull() == false ||
             identity.QueueFamily != 0 || identity.QueueIndex != 0 || description.InstanceApiVersion != 0 ||
             description.CalibratedTimestamps != NativeCalibratedTimestamps::None)
    {
        return DeviceStatus::InvalidDescription;
    }
    GetNativeInteropState().IsInCallback = true;
    const bool retained = description.Lifetime.Retain(description.Lifetime.Context);
    GetNativeInteropState().IsInCallback = false;
    if (retained == false)
    {
        return DeviceStatus::Failed;
    }
    GetNativeInteropState().Source = description;
    GetNativeInteropState().IsSourcePinned = true;
    return DeviceStatus::Ready;
}
void NativeInteropShutdownFinished() noexcept
{
    GetNativeInteropState().IsRetiring = false;
    GetNativeInteropState().HasTerminalLoss = false;
    ReleaseNativeDeviceSource();
}
} // namespace internal
RasterStatus GetNativeInteropCapabilities(DeviceHandle device, NativeInteropCapabilities& output) noexcept
{
    const auto admission = Admission(device, false);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (backend::NativeInteropSupported() == false)
    {
        return RasterStatus::Unsupported;
    }
    output = {true, true, CurrentStorageLimits().ResourcesPerKind};
    return RasterStatus::Ready;
}
RasterStatus AcquireNativeDevice(DeviceHandle device, NativeDeviceLease& output) noexcept
{
    const auto admission = Admission(device);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (NativeInteropAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    if (backend::NativeInteropSupported() == false)
    {
        return RasterStatus::Unsupported;
    }
    NativeDeviceInfo info;
    const auto snapshot = backend::NativeDeviceSnapshot(info);
    if (snapshot != RasterStatus::Ready)
    {
        return snapshot;
    }
    usize slot = 0;
    const RasterStatus reserved = NativeReserve(GetNativeInteropState().Devices, slot);
    if (reserved != RasterStatus::Ready)
    {
        return reserved;
    }
    NativeDeviceRecord& record = GetNativeInteropState().Devices[slot];
    info.Incarnation = DeviceOwner();
    record =
    {
        .Lease = { .Owner = info.Incarnation, .Generation = record.Lease.Generation },
        .Session = device,
        .Info = info,
    };
    ++GetNativeInteropState().Pins;
    NativeInteropAccess::Set(output, {record.Lease.Owner, slot, record.Lease.Generation});
    return RasterStatus::Ready;
}
RasterStatus GetNativeDeviceInfo(NativeDeviceLease handle, NativeDeviceInfo& output) noexcept
{
    BindNativeOwner(&NativeInteropState::Devices, handle);
    const NativeDeviceRecord* record = NativeResolve(GetNativeInteropState().Devices, handle);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    output = NativeSnapshot(*record);
    return RasterStatus::Ready;
}
RasterStatus ReleaseNativeDevice(NativeDeviceLease& handle) noexcept
{
    BindNativeOwner(&NativeInteropState::Devices, handle);
    NativeDeviceRecord* record = NativeResolve(GetNativeInteropState().Devices, handle);
    if (record == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (FrameOpen() || record->Reads != 0)
    {
        return RasterStatus::InvalidState;
    }
    NativeReset(*record);
    --GetNativeInteropState().Pins;
    handle = {};
    if (GetNativeInteropState().Pins == 0 && GetNativeInteropState().IsRetiring)
    {
        FinishNativeInteropShutdown();
    }
    return RasterStatus::Ready;
}
RasterStatus AcquireNativeTextureRead(NativeDeviceLease device,
                                      const NativeDeviceIdentity& consumer,
                                      TextureHandle handle,
                                      SubmissionToken producer,
                                      NativeTextureRead& output,
                                      NativeTextureInfo& info) noexcept
{
    BindNativeOwner(&NativeInteropState::Devices, device);
    NativeDeviceRecord* native = NativeResolve(GetNativeInteropState().Devices, device);
    if (native == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (GetNativeInteropState().IsRetiring || native->Lease.Owner != DeviceOwner())
    {
        return RasterStatus::InvalidState;
    }
    const auto admission = Admission(native->Session);
    if (admission != RasterStatus::Ready)
    {
        return admission;
    }
    if (NativeInteropAccess::Owner(output) != 0)
    {
        return RasterStatus::InvalidState;
    }
    if (SameNativeContext(native->Info.Identity, consumer) == false)
    {
        return RasterStatus::Unsupported;
    }
    auto* texture = Resolve(handle, RasterKind::Texture);
    if (texture == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (texture->Status != RasterStatus::Ready)
    {
        return texture->Status == RasterStatus::Pending ? RasterStatus::NotReady : texture->Status;
    }
    const auto& description = texture->Texture;
    if (description.Attachment == false || description.Format != RasterFormat::Rgba8Unorm ||
        description.Dimension != TextureDimension::D2 || description.MipLevels != 1 || description.DepthOrLayers != 1 ||
        description.SampleCount != 1)
    {
        return RasterStatus::Unsupported;
    }
    if (texture->ColorDefined == false)
    {
        return RasterStatus::InvalidDescription;
    }
    if (texture->TextureRevision == ~uint64{0})
    {
        return RasterStatus::IdentityExhausted;
    }
    const auto ordinal = LifetimeAccess::Ordinal(producer);
    if (LifetimeAccess::Owner(producer) != native->Lease.Owner || ordinal == 0 || ordinal != texture->LastUse ||
        ordinal < GetRasterState().FirstAcceptedSubmission || ordinal > GetRasterState().LastAcceptedSubmission)
    {
        return RasterStatus::InvalidHandle;
    }
    // An accepted producer can still be in flight; the shared native queue barrier
    // orders the following foreign sample without a CPU completion wait.
    usize slot = 0;
    const RasterStatus reserved = NativeReserve(GetNativeInteropState().Reads, slot);
    if (reserved != RasterStatus::Ready)
    {
        return reserved;
    }
    NativeTextureInfo result
    {
        .Device = NativeSnapshot(*native),
        .Revision = texture->TextureRevision + 1,
        .ProducerOrdinal = ordinal,
        .Format = description.Format,
        .Width = description.Width,
        .Height = description.Height,
    };
    const auto textureSlot = RasterAccess::Slot(handle);
    Retain(RasterKind::Texture, textureSlot);
    const auto published = backend::NativeTexturePublish({slot, textureSlot}, result);
    if (published != RasterStatus::Ready)
    {
        Release(RasterKind::Texture, textureSlot);
        backend::NativeTextureDestroy(slot);
        return published;
    }
    texture->TextureUse = RasterTextureUse::SampledFragment;
    ++texture->TextureRevision;
    ++texture->ExternalReads;
    NativeReadRecord& read = GetNativeInteropState().Reads[slot];
    read =
    {
        .Lease = { .Owner = native->Lease.Owner, .Generation = read.Lease.Generation },
        .Device = NativeInteropAccess::Slot(device),
        .Texture = textureSlot,
        .Info = result,
    };
    ++native->Reads;
    NativeInteropAccess::Set(output, {read.Lease.Owner, slot, read.Lease.Generation});
    info = result;
    return RasterStatus::Ready;
}
RasterStatus GetNativeTextureInfo(NativeTextureRead handle, NativeTextureInfo& output) noexcept
{
    BindNativeOwner(&NativeInteropState::Reads, handle);
    const NativeReadRecord* read = NativeResolve(GetNativeInteropState().Reads, handle);
    if (read == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (read->Releasing)
    {
        return RasterStatus::InvalidState;
    }
    if (GetNativeInteropState().HasTerminalLoss ||
        (read->Lease.Owner == DeviceOwner() && internal::GetStartup().State == StartupState::DeviceLost))
    {
        return RasterStatus::DeviceLost;
    }
    const auto status = backend::NativeTexturePoll(NativeInteropAccess::Slot(handle), false);
    if (status == RasterStatus::DeviceLost || status == RasterStatus::Failed)
    {
        return status;
    }
    auto result = read->Info;
    result.Device = NativeSnapshot(GetNativeInteropState().Devices[read->Device]);
    output = result;
    return RasterStatus::Ready;
}
RasterStatus ReleaseNativeTextureRead(NativeTextureRead& read) noexcept
{
    BindNativeOwner(&NativeInteropState::Reads, read);
    return CloseNativeRead(read, false);
}
RasterStatus DrainNativeTextureRead(NativeTextureRead& read) noexcept
{
    BindNativeOwner(&NativeInteropState::Reads, read);
    return CloseNativeRead(read, true);
}
RasterStatus GetStatus(NativeTextureRead handle) noexcept
{
    BindNativeOwner(&NativeInteropState::Reads, handle);
    const NativeReadRecord* read = NativeResolve(GetNativeInteropState().Reads, handle);
    if (read == nullptr)
    {
        return RasterStatus::InvalidHandle;
    }
    if (GetNativeInteropState().HasTerminalLoss ||
        (read->Lease.Owner == DeviceOwner() && internal::GetStartup().State == StartupState::DeviceLost))
    {
        return RasterStatus::DeviceLost;
    }
    const auto status = backend::NativeTexturePoll(NativeInteropAccess::Slot(handle), false);
    return read->Releasing == false && status == RasterStatus::Pending ? RasterStatus::Ready : status;
}
} // namespace ludus::graphics::rhi
