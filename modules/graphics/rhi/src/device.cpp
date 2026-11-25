#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/device.h>

#include "internal/adapter.h"
#include "internal/backend.h"
#include "internal/device.h"
#include "internal/lifecycle.h"
#include "internal/measurement.h"
#include "internal/native_interop.h"
#include "internal/owner.h"
#include "internal/profile.h"
#include "internal/raster.h"
#include "internal/ray_backend.h"
#include "internal/resources.h"
#include "internal/storage.h"
#include "internal/surface.h"

namespace ludus::graphics::rhi
{
namespace internal
{
struct DeviceAccess final
{
    template <typename T>
    static ludus::foundation::uint64 Owner(T handle) noexcept
    {
        return handle.Owner;
    }

    template <typename T>
    static void Set(T& handle, ludus::foundation::uint64 owner) noexcept
    {
        handle.Owner = owner;
    }

    static ludus::foundation::uint64 Identity(SurfaceHandle surface) noexcept
    {
        return surface.mIdentity;
    }

    static void SetIdentity(SurfaceHandle& outSurface, ludus::foundation::uint64 identity) noexcept
    {
        outSurface.mIdentity = identity;
    }
};
} // namespace internal
namespace
{
using internal::DeviceAccess;
internal::DeviceOwnerSequence gOwners;

// R14b surface registry. Slot 0 is the primary surface; a record is live only while its
// owner is the current device owner, so DestroyDevice and restart retire every record
// without a separate reset. Identities come from a process sequence and are never reused.
struct SurfaceRecord final
{
    foundation::uint64 Owner = 0;
    foundation::uint64 Identity = 0;
    platform::WindowSystem System = platform::WindowSystem::Headless;
    /// Borrowed native target, compared only for identity and never dereferenced.
    const void* TargetOrNull = nullptr;
    foundation::uint32 Width = 0;
    foundation::uint32 Height = 0;
    foundation::uint64 SubmittedFrames = 0;
    bool IsLost = false;
};
/// Copied description and surface registry of one owner slot (R14c).
struct OwnerRecord final
{
    DeviceDescription Description;
    foundation::StaticArray<SurfaceRecord, MAX_DEVICE_SURFACES> Surfaces{};
    bool IsGpuTimingRequested = false;
};
foundation::StaticArray<OwnerRecord, MAX_DEVICE_OWNERS> gOwnerRecords{};
/// Surface identities are process-wide, so no owner resolves another owner's surface.
internal::DeviceOwnerSequence gSurfaceIdentities;
/// The description and surfaces of the bound owner slot.
OwnerRecord& GetOwnerRecord() noexcept
{
    return gOwnerRecords[internal::GetBoundOwnerSlot()];
}

// R0 shares the existing registry/loss protocol rather than introducing a second
// resource registry. See docs/architecture/rhi-gdi.md, ownership and identity.
/// Bind the slot of a live device so the rest of the operation addresses only its state.
bool Valid(DeviceHandle device) noexcept
{
    const foundation::usize slot = internal::FindOwnerSlot(DeviceAccess::Owner(device));
    if (slot == MAX_DEVICE_OWNERS)
    {
        return false;
    }
    internal::BindOwnerSlot(slot);
    return true;
}
/// @brief Bind the lowest free owner slot for a new device (R14c).
///
/// A device-less facade session or native interop teardown pending in any slot rejects
/// creation, as does a live owner when the backend hosts this kind of owner only alone.
DeviceStatus BindFreeOwnerSlot(bool isNativeDevice) noexcept
{
    const foundation::usize limit = backend::GetDeviceOwnerLimit(isNativeDevice);
    foundation::usize freeSlot = MAX_DEVICE_OWNERS;
    foundation::usize liveOwners = 0;
    bool hasUnownedSession = false;
    for (foundation::usize slot = 0; slot < MAX_DEVICE_OWNERS; ++slot)
    {
        internal::BindOwnerSlot(slot);
        if (internal::SessionBusy() == true)
        {
            ++liveOwners;
            hasUnownedSession = hasUnownedSession == true || internal::DeviceOwner() == 0;
        }
        else if (freeSlot == MAX_DEVICE_OWNERS)
        {
            freeSlot = slot;
        }
    }
    if (hasUnownedSession == true || (liveOwners >= limit && limit <= 1))
    {
        return DeviceStatus::InvalidState;
    }
    if (liveOwners >= limit || freeSlot == MAX_DEVICE_OWNERS)
    {
        return DeviceStatus::CapacityExceeded;
    }
    internal::BindOwnerSlot(freeSlot);
    return DeviceStatus::Ready;
}
bool IsLive(const SurfaceRecord& record) noexcept
{
    return record.Owner != 0 && record.Owner == internal::DeviceOwner();
}
/// Slot of a live surface of this device, or MAX_DEVICE_SURFACES for a null, stale or foreign handle.
foundation::usize FindSurface(DeviceHandle device, SurfaceHandle surface) noexcept
{
    if (Valid(device) == false || DeviceAccess::Owner(surface) != DeviceAccess::Owner(device) ||
        DeviceAccess::Identity(surface) == 0)
    {
        return MAX_DEVICE_SURFACES;
    }
    for (foundation::usize slot = 0; slot < MAX_DEVICE_SURFACES; ++slot)
    {
        if (IsLive(GetOwnerRecord().Surfaces[slot]) == true &&
            GetOwnerRecord().Surfaces[slot].Identity == DeviceAccess::Identity(surface))
        {
            return slot;
        }
    }
    return MAX_DEVICE_SURFACES;
}
/// The native object identifying a borrowed target; null for headless targets, which borrow nothing.
const void* GetTargetIdentityOrNull(const WindowInfo& window) noexcept
{
    switch (window.System)
    {
        case platform::WindowSystem::Headless:
            return nullptr;
        case platform::WindowSystem::Wayland:
            return window.Surface;
        case platform::WindowSystem::WebCanvas:
            return window.CanvasSelector;
        case platform::WindowSystem::Cocoa:
            return window.CocoaView;
        case platform::WindowSystem::Hosted:
            return window.HostedSurfaceOrNull;
        case platform::WindowSystem::Win32:
            return window.Win32WindowOrNull;
    }
    return nullptr;
}
/// Two live surfaces of one device must not borrow the same native target.
/// No two live surfaces borrow the same native target, also across device owners (R14c): a
/// view, window or canvas hosts one presentation layer.
bool IsTargetBorrowed(const WindowInfo& window) noexcept
{
    const void* targetOrNull = GetTargetIdentityOrNull(window);
    if (targetOrNull == nullptr)
    {
        return false;
    }
    for (const OwnerRecord& owner : gOwnerRecords)
    {
        for (const SurfaceRecord& record : owner.Surfaces)
        {
            if (record.Owner != 0 && internal::FindOwnerSlot(record.Owner) != MAX_DEVICE_OWNERS &&
                record.System == window.System && record.TargetOrNull == targetOrNull)
            {
                return true;
            }
        }
    }
    return false;
}
/// Publish a registry record for a connected surface and its handle.
void PublishSurface(foundation::usize slot,
                    foundation::uint64 owner,
                    const WindowInfo& window,
                    SurfaceHandle& outSurface) noexcept
{
    const foundation::uint64 identity = gSurfaceIdentities.Take();
    GetOwnerRecord().Surfaces[slot] =
    {
        .Owner = owner,
        .Identity = identity,
        .System = window.System,
        .TargetOrNull = GetTargetIdentityOrNull(window),
        .Width = window.Width,
        .Height = window.Height,
    };
    DeviceAccess::Set(outSurface, owner);
    DeviceAccess::SetIdentity(outSurface, identity);
}
DeviceStatus State() noexcept
{
    switch (internal::GetStartup().State)
    {
        case StartupState::Ready:
            return DeviceStatus::Ready;
        case StartupState::Pending:
            return DeviceStatus::Pending;
        case StartupState::DeviceLost:
            return DeviceStatus::DeviceLost;
        case StartupState::Idle:
        case StartupState::Failed:
            return DeviceStatus::Failed;
    }
    return DeviceStatus::Failed;
}
DeviceStatus FrameResult(FrameStatus status) noexcept
{
    // Preserve terminal startup failure and synchronous acquisition/submission loss.
    const auto state = State();
    if (state == DeviceStatus::Failed || state == DeviceStatus::DeviceLost)
    {
        return state;
    }
    switch (status)
    {
        case FrameStatus::Ready:
            return DeviceStatus::Ready;
        case FrameStatus::Skipped:
            return DeviceStatus::Skipped;
        case FrameStatus::NotReady:
            return DeviceStatus::NotReady;
        case FrameStatus::InvalidState:
            return DeviceStatus::InvalidState;
        case FrameStatus::Unsupported:
            return DeviceStatus::Unsupported;
        case FrameStatus::Failed:
            return DeviceStatus::Failed;
    }
    return DeviceStatus::Failed;
}
ResourceStatus ResourceAdmission(DeviceHandle device) noexcept
{
    if (Valid(device) == false)
    {
        return ResourceStatus::InvalidHandle;
    }
    const auto state = State();
    return state == DeviceStatus::DeviceLost ? ResourceStatus::DeviceLost
           : state == DeviceStatus::Ready    ? ResourceStatus::Ready
                                             : ResourceStatus::NotReady;
}
ResourceStatus ResourceResult(ResourceStatus result) noexcept
{
    return State() == DeviceStatus::DeviceLost ? ResourceStatus::DeviceLost : result;
}
// R14a: an explicit adapter must be a catalog entry meeting its backend baseline,
// checked before any backend request so failure publishes no device.
bool IsRequestedAdapterAvailable(const AdapterIdentity& adapter) noexcept
{
    AdapterCatalog catalog;
    if (GetAdapterCatalog(catalog) != AdapterStatus::Ready)
    {
        return false;
    }
    for (foundation::usize i = 0; i < catalog.Count; ++i)
    {
        if (catalog.Adapters[i].Identity == adapter)
        {
            return catalog.Adapters[i].HasBackendBaseline;
        }
    }
    return false;
}
// The backend honours the request during selection; an imported native device or a
// backend that ignored it is caught here after Ready.
bool IsSelectedAdapterRequested(const AdapterIdentity& adapter) noexcept
{
    AdapterInfo selected;
    return adapter.IsAutomatic() == true ||
           (backend::GetSelectedAdapter(selected) == true && selected.Identity == adapter);
}
void ReportAdapterUnavailable(StartupInfo* failureOrNull) noexcept
{
    if (failureOrNull != nullptr)
    {
        *failureOrNull = {};
        failureOrNull->SelectedBackend = backend::Kind();
        failureOrNull->State = StartupState::Failed;
        failureOrNull->Error = StartupError::AdapterUnavailable;
    }
}
} // namespace
DeviceStatus ValidateDeviceDescription(const DeviceDescription& description) noexcept
{
    if (description.Selection != BackendSelection::Auto && description.Selection != BackendSelection::WebGPU &&
        description.Selection != BackendSelection::WebGL2)
    {
        return DeviceStatus::InvalidDescription;
    }
    if (internal::ValidStorageLimits(description.Storage) == false ||
        internal::ValidProfileRequirements(description.RequiredProfile) == false ||
        internal::ValidProfileRequirements(description.PreferredProfile) == false)
    {
        return DeviceStatus::InvalidDescription;
    }
    const auto compiled = GetCompiledBackends();
    if (description.Adapter.IsAutomatic() == false)
    {
        // R14a: only native backends publish a catalog; WebGPU exposes one requested adapter.
        const bool isNative = compiled.Vulkan == true || compiled.Metal == true || compiled.HasDirect3D12 == true;
        if (isNative == false)
        {
            return DeviceStatus::Unsupported;
        }
        if (description.Adapter.Api != backend::Kind())
        {
            return DeviceStatus::InvalidDescription;
        }
    }
    if (description.Required.BindlessTextureIndexing == true &&
        ((compiled.Metal == false && compiled.Vulkan == false) || description.Selection != BackendSelection::Auto))
    {
        return DeviceStatus::Unsupported;
    }
    if (description.Required.HasRuntimeTextureArrays == true && compiled.Vulkan == false)
    {
        return DeviceStatus::Unsupported;
    }
    if (description.Required.RayQueries &&
        ((compiled.Metal == false && compiled.Vulkan == false) || description.Selection == BackendSelection::WebGPU ||
         description.Selection == BackendSelection::WebGL2))
    {
        return DeviceStatus::Unsupported;
    }
    return description.Selection == BackendSelection::WebGL2 &&
                   (description.Required.Compute || description.Required.IndirectRendering ||
                    internal::NeedsCompute(description.RequiredProfile))
               ? DeviceStatus::Unsupported
               : DeviceStatus::Ready;
}
BackendAvailability GetCompiledBackends() noexcept
{
    const auto kind = backend::Kind();
    return
    {
        .Vulkan = kind == Backend::Vulkan,
        .Metal = kind == Backend::Metal,
        .WebGPU = backend::Supports(BackendSelection::WebGPU),
        .WebGL2 = backend::Supports(BackendSelection::WebGL2),
        .HasDirect3D12 = kind == Backend::Direct3D12,
    };
}
namespace
{
/// Start a device in the bound free owner slot; see CreateDevice.
DeviceStatus CreateBoundDevice(const ApplicationInfo& application,
                               const WindowInfo& window,
                               const DeviceDescription& description,
                               DeviceHandle& outDevice,
                               SurfaceHandle& outSurface,
                               StartupInfo* failureOrNull) noexcept
{
    const DeviceStatus admission = ValidateDeviceDescription(description);
    if (admission != DeviceStatus::Ready)
    {
        return admission;
    }
    if (IsTargetBorrowed(window) == true)
    {
        return DeviceStatus::InvalidDescription;
    }
    if (gOwners.Next == 0 || gSurfaceIdentities.Next == 0)
    {
        return DeviceStatus::IdentityExhausted;
    }
    if (description.Adapter.IsAutomatic() == false && IsRequestedAdapterAvailable(description.Adapter) == false)
    {
        ReportAdapterUnavailable(failureOrNull);
        return DeviceStatus::Failed;
    }
    internal::SetGpuTimingRequest(description.EnableGpuTiming);
    internal::SetAdapterRequest(description.Adapter);
    internal::SetBindlessRequest(description.Required.BindlessTextureIndexing == true ||
                                     description.Preferred.BindlessTextureIndexing == true,
                                 description.Required.BindlessTextureIndexing);
    internal::SetRuntimeTextureArrayRequest(description.Required.HasRuntimeTextureArrays == true ||
                                                description.Preferred.HasRuntimeTextureArrays == true,
                                            description.Required.HasRuntimeTextureArrays);
    internal::SetRayProfileRequest(description.Required.RayQueries || description.Preferred.RayQueries);
    const auto result = internal::StartOwned(
        application,
        window,
        description.Selection,
        description.Limits,
        description.Required.PortableRaster || description.Required.OrderedRasterGraph ||
            description.Required.Compute || description.Required.IndirectRendering || description.Required.RayQueries ||
            description.Required.BindlessTextureIndexing == true ||
            description.Required.HasRuntimeTextureArrays == true ||
            internal::NeedsRaster(description.RequiredProfile) || internal::NeedsCompute(description.RequiredProfile),
        description.Required.Compute || description.Required.IndirectRendering || description.Required.RayQueries ||
            internal::NeedsCompute(description.RequiredProfile),
        description.RequiredProfile,
        description.Storage);
    if (result == StartStatus::Busy)
    {
        return DeviceStatus::InvalidState;
    }
    if (result == StartStatus::Failed)
    {
        if (failureOrNull != nullptr)
        {
            *failureOrNull = internal::GetStartup();
        }
        internal::Shutdown();
        return DeviceStatus::Failed;
    }
    if (result == StartStatus::Ready && IsSelectedAdapterRequested(description.Adapter) == false)
    {
        internal::Shutdown();
        ReportAdapterUnavailable(failureOrNull);
        return DeviceStatus::Failed;
    }
    if (result == StartStatus::Ready && description.Required.RayQueries && backend::RayLimits().InlineQueries == false)
    {
        internal::Shutdown();
        return DeviceStatus::Unsupported;
    }
    if (result == StartStatus::Ready && description.Required.BindlessTextureIndexing == true &&
        backend::BindingLimits().HasBindlessTextureIndexing == false)
    {
        internal::Shutdown();
        return DeviceStatus::Unsupported;
    }
    if (result == StartStatus::Ready && description.Required.HasRuntimeTextureArrays == true &&
        backend::BindingLimits().HasRuntimeTextureArrays == false)
    {
        internal::Shutdown();
        return DeviceStatus::Unsupported;
    }
    const auto owner = gOwners.Take();
    internal::SetDeviceOwner(owner);
    GetOwnerRecord().Description = description;
    DeviceAccess::Set(outDevice, owner);
    PublishSurface(0, owner, window, outSurface);
    return result == StartStatus::Ready ? DeviceStatus::Ready : DeviceStatus::Pending;
}
} // namespace
DeviceStatus CreateDevice(const ApplicationInfo& application,
                          const WindowInfo& window,
                          const DeviceDescription& description,
                          DeviceHandle& outDevice,
                          SurfaceHandle& outSurface,
                          StartupInfo* failureOrNull) noexcept
{
    if (DeviceAccess::Owner(outDevice) != 0 || DeviceAccess::Owner(outSurface) != 0)
    {
        return DeviceStatus::InvalidState;
    }
    const DeviceStatus reserved = BindFreeOwnerSlot(false);
    if (reserved != DeviceStatus::Ready)
    {
        return reserved;
    }
    return CreateBoundDevice(application, window, description, outDevice, outSurface, failureOrNull);
}
DeviceStatus CreateNativeDevice(const ApplicationInfo& application,
                                const NativeDeviceDescription& native,
                                const DeviceDescription& description,
                                foundation::uint32 width,
                                foundation::uint32 height,
                                DeviceHandle& device,
                                SurfaceHandle& surface) noexcept
{
    if (DeviceAccess::Owner(device) != 0 || DeviceAccess::Owner(surface) != 0)
    {
        return DeviceStatus::InvalidState;
    }
    // The imported source is pinned in the slot the device will occupy.
    const DeviceStatus reserved = BindFreeOwnerSlot(true);
    if (reserved != DeviceStatus::Ready)
    {
        return reserved;
    }
    if (width == 0 || height == 0)
    {
        return DeviceStatus::InvalidDescription;
    }
    const auto valid = ValidateDeviceDescription(description);
    if (valid != DeviceStatus::Ready)
    {
        return valid;
    }
    if (native.Identity.Api == Backend::Vulkan && description.Required.RayQueries)
    {
        return DeviceStatus::Unsupported;
    }
    const auto prepared = internal::PrepareNativeDeviceSource(native);
    if (prepared != DeviceStatus::Ready)
    {
        return prepared;
    }
    const WindowInfo window{ .Width = width, .Height = height };
    const DeviceStatus result = CreateBoundDevice(application, window, description, device, surface, nullptr);
    if (result != DeviceStatus::Ready && result != DeviceStatus::Pending)
    {
        internal::ReleaseNativeDeviceSource();
    }
    return result;
}
DeviceStatus GetDeviceInfo(DeviceHandle device, DeviceInfo& info) noexcept
{
    if (Valid(device) == false)
    {
        return DeviceStatus::InvalidHandle;
    }
    info = {};
    info.Compiled = GetCompiledBackends();
    info.Startup = internal::GetStartup();
    info.Description = GetOwnerRecord().Description;
    const auto state = State();
    if (state == DeviceStatus::Ready)
    {
        info.FrameOpen = internal::FrameOpen();
        info.Supported.RayQueries = backend::RayLimits().InlineQueries;
        info.Enabled.RayQueries = info.Supported.RayQueries;
        info.Supported.FullscreenRaster = true;
        info.Enabled.FullscreenRaster = true;
        info.Profile = internal::CurrentDeviceProfile();
        static_cast<void>(backend::GetSelectedAdapter(info.Adapter));
        info.Supported.BindlessTextureIndexing = info.Profile.Binding.HasBindlessTextureIndexing;
        info.Enabled.BindlessTextureIndexing = info.Supported.BindlessTextureIndexing;
        info.Supported.HasRuntimeTextureArrays = info.Profile.Binding.HasRuntimeTextureArrays;
        info.Enabled.HasRuntimeTextureArrays = info.Supported.HasRuntimeTextureArrays;
        info.Supported.PortableRaster = info.Profile.Raster.MaxUniformRange >= internal::PORTABLE_UNIFORM_BYTES &&
                                        info.Profile.Raster.DrawsPerFrame != 0;
        info.Enabled.PortableRaster = info.Supported.PortableRaster;
        info.Supported.OrderedRasterGraph = info.Supported.PortableRaster;
        info.Enabled.OrderedRasterGraph = info.Supported.OrderedRasterGraph;
        info.Supported.Compute = info.Supported.PortableRaster && info.Profile.Compute.MaxStorageRange != 0;
        info.Enabled.Compute = info.Supported.Compute;
        info.Supported.IndirectRendering = info.Supported.Compute;
        info.Enabled.IndirectRendering = info.Supported.IndirectRendering;
        info.UnmetPreferences =
        {
            .FullscreenRaster =
                GetOwnerRecord().Description.Preferred.FullscreenRaster && info.Enabled.FullscreenRaster == false,
            .PortableRaster =
                GetOwnerRecord().Description.Preferred.PortableRaster && info.Enabled.PortableRaster == false,
            .Compute = GetOwnerRecord().Description.Preferred.Compute && info.Enabled.Compute == false,
            .IndirectRendering =
                GetOwnerRecord().Description.Preferred.IndirectRendering && info.Enabled.IndirectRendering == false,
            .RayQueries = GetOwnerRecord().Description.Preferred.RayQueries && info.Enabled.RayQueries == false,
            .OrderedRasterGraph =
                GetOwnerRecord().Description.Preferred.OrderedRasterGraph && info.Enabled.OrderedRasterGraph == false,
            .BindlessTextureIndexing = GetOwnerRecord().Description.Preferred.BindlessTextureIndexing == true &&
                                       info.Enabled.BindlessTextureIndexing == false,
            .HasRuntimeTextureArrays = GetOwnerRecord().Description.Preferred.HasRuntimeTextureArrays == true &&
                                       info.Enabled.HasRuntimeTextureArrays == false,
        };
        info.UnmetProfilePreference =
            internal::CheckProfile(GetOwnerRecord().Description.PreferredProfile, info.Profile);
    }
    return state;
}
RasterStatus GetMeasurementAdapter(DeviceHandle device, MeasurementAdapter& output) noexcept
{
    if (Valid(device) == false)
    {
        return RasterStatus::InvalidHandle;
    }
    output = {};
    if (State() == DeviceStatus::DeviceLost)
    {
        return RasterStatus::DeviceLost;
    }
    if (State() != DeviceStatus::Ready)
    {
        return State() == DeviceStatus::Failed ? RasterStatus::Failed : RasterStatus::NotReady;
    }
    output = backend::TimingAdapter();
    return RasterStatus::Ready;
}
RasterStatus ArmGpuTiming(DeviceHandle device) noexcept
{
    if (Valid(device) == false)
    {
        return RasterStatus::InvalidHandle;
    }
    if (State() == DeviceStatus::DeviceLost)
    {
        return RasterStatus::DeviceLost;
    }
    if (State() != DeviceStatus::Ready)
    {
        return State() == DeviceStatus::Failed ? RasterStatus::Failed : RasterStatus::NotReady;
    }
    if (internal::FrameOpen())
    {
        return RasterStatus::InvalidState;
    }
    if (internal::GpuTimingRequested() == false)
    {
        return RasterStatus::Unsupported;
    }
    const auto result = backend::TimingArm();
    return State() == DeviceStatus::DeviceLost ? RasterStatus::DeviceLost
           : State() == DeviceStatus::Failed   ? RasterStatus::Failed
                                               : result;
}
RasterStatus GetGpuTiming(DeviceHandle device, GpuTiming& output) noexcept
{
    if (Valid(device) == false)
    {
        return RasterStatus::InvalidHandle;
    }
    output = {};
    if (State() == DeviceStatus::DeviceLost)
    {
        return RasterStatus::DeviceLost;
    }
    if (State() != DeviceStatus::Ready)
    {
        return State() == DeviceStatus::Failed ? RasterStatus::Failed : RasterStatus::NotReady;
    }
    output = internal::GpuTimingRequested() ? backend::TimingPoll() : GpuTiming{ .State = GpuTimingState::Unavailable };
    if (State() != DeviceStatus::Ready)
    {
        output = {};
        return State() == DeviceStatus::DeviceLost ? RasterStatus::DeviceLost : RasterStatus::Failed;
    }
    return RasterStatus::Ready;
}
namespace internal
{
RasterStatus ValidateDeviceReadiness(DeviceHandle device) noexcept
{
    if (Valid(device) == false)
    {
        return RasterStatus::InvalidHandle;
    }
    switch (State())
    {
        case DeviceStatus::Ready:
            return RasterStatus::Ready;
        case DeviceStatus::DeviceLost:
            return RasterStatus::DeviceLost;
        case DeviceStatus::Failed:
            return RasterStatus::Failed;
        default:
            return RasterStatus::NotReady;
    }
}
void SetGpuTimingRequest(bool requested) noexcept
{
    GetOwnerRecord().IsGpuTimingRequested = requested;
}
bool GpuTimingRequested() noexcept
{
    return GetOwnerRecord().IsGpuTimingRequested;
}
} // namespace internal
DeviceStatus DestroyDevice(DeviceHandle& device) noexcept
{
    if (Valid(device) == false)
    {
        return DeviceStatus::InvalidHandle;
    }
    internal::Shutdown();
    device = {};
    return DeviceStatus::Ready;
}
namespace internal
{
void LoseSurface(foundation::usize slot) noexcept
{
    if (slot != 0 && slot < MAX_DEVICE_SURFACES && IsLive(GetOwnerRecord().Surfaces[slot]) == true)
    {
        GetOwnerRecord().Surfaces[slot].IsLost = true;
    }
}
bool IsSurfaceLost(foundation::usize slot) noexcept
{
    return slot < MAX_DEVICE_SURFACES && IsLive(GetOwnerRecord().Surfaces[slot]) == true &&
           GetOwnerRecord().Surfaces[slot].IsLost == true;
}
} // namespace internal
DeviceStatus SetFrameTarget(DeviceHandle device, SurfaceHandle surface, const FrameTarget& target) noexcept
{
    const foundation::usize slot = FindSurface(device, surface);
    if (slot == MAX_DEVICE_SURFACES)
    {
        return DeviceStatus::InvalidHandle;
    }
    if (internal::IsSurfaceLost(slot) == true && State() == DeviceStatus::Ready)
    {
        return DeviceStatus::SurfaceLost;
    }
    const FrameStatus result = internal::SetSurfaceFrameTarget(slot, target);
    if (result == FrameStatus::Ready)
    {
        GetOwnerRecord().Surfaces[slot].Width = target.Width;
        GetOwnerRecord().Surfaces[slot].Height = target.Height;
    }
    return FrameResult(result);
}
DeviceStatus BeginFrame(DeviceHandle device, SurfaceHandle surface) noexcept
{
    const foundation::usize slot = FindSurface(device, surface);
    if (slot == MAX_DEVICE_SURFACES)
    {
        return DeviceStatus::InvalidHandle;
    }
    if (internal::IsSurfaceLost(slot) == true && State() == DeviceStatus::Ready)
    {
        return DeviceStatus::SurfaceLost;
    }
    const FrameStatus result = internal::BeginSurfaceFrame(slot);
    // A backend reports a created surface's loss during acquisition without failing the device.
    if (internal::IsSurfaceLost(slot) == true && State() == DeviceStatus::Ready)
    {
        return DeviceStatus::SurfaceLost;
    }
    return FrameResult(result);
}
DeviceStatus EndFrame(DeviceHandle device) noexcept
{
    return Valid(device) ? FrameResult(internal::EndFrameStatus()) : DeviceStatus::InvalidHandle;
}
DeviceStatus GetFrameInfo(DeviceHandle device, SurfaceHandle surface, FrameInfo& info) noexcept
{
    const foundation::usize slot = FindSurface(device, surface);
    if (slot == MAX_DEVICE_SURFACES)
    {
        return DeviceStatus::InvalidHandle;
    }
    const auto state = State();
    if (state != DeviceStatus::Ready)
    {
        return state == DeviceStatus::Pending ? DeviceStatus::NotReady : state;
    }
    if (internal::FrameOpen() == true && internal::GetFrameSurface() != slot)
    {
        return DeviceStatus::InvalidState;
    }
    const FrameInfo acquired = internal::GetFrameInfo();
    if (acquired.Width == 0 || acquired.Height == 0)
    {
        return DeviceStatus::InvalidState;
    }
    info = acquired;
    return DeviceStatus::Ready;
}
DeviceStatus CreateSurface(DeviceHandle device, const WindowInfo& window, SurfaceHandle& outSurface) noexcept
{
    if (Valid(device) == false)
    {
        return DeviceStatus::InvalidHandle;
    }
    if (DeviceAccess::Owner(outSurface) != 0 || DeviceAccess::Identity(outSurface) != 0)
    {
        return DeviceStatus::InvalidState;
    }
    const DeviceStatus state = State();
    if (state != DeviceStatus::Ready)
    {
        return state == DeviceStatus::Pending ? DeviceStatus::NotReady : state;
    }
    if (internal::FrameOpen() == true)
    {
        return DeviceStatus::InvalidState;
    }
    const foundation::usize limit = backend::AdditionalSurfaceLimit();
    if (limit == 0)
    {
        return DeviceStatus::Unsupported;
    }
    const foundation::uint32 maxDimension = internal::GetStartup().Capabilities.MaxFrameDimension2D;
    if (window.Width > maxDimension || window.Height > maxDimension || IsTargetBorrowed(window) == true)
    {
        return DeviceStatus::InvalidDescription;
    }
    if (gSurfaceIdentities.Next == 0)
    {
        return DeviceStatus::IdentityExhausted;
    }
    foundation::usize slot = MAX_DEVICE_SURFACES;
    for (foundation::usize candidate = 1; candidate <= limit && candidate < MAX_DEVICE_SURFACES; ++candidate)
    {
        if (IsLive(GetOwnerRecord().Surfaces[candidate]) == false)
        {
            slot = candidate;
            break;
        }
    }
    if (slot == MAX_DEVICE_SURFACES)
    {
        return DeviceStatus::CapacityExceeded;
    }
    const StartupError error = backend::CreateSurface(slot, window);
    switch (error)
    {
        case StartupError::None:
            PublishSurface(slot, DeviceAccess::Owner(device), window, outSurface);
            internal::ResetSurfaceFrames(slot);
            return DeviceStatus::Ready;
        case StartupError::InvalidWindow:
            return DeviceStatus::InvalidDescription;
        case StartupError::BackendUnavailable:
            return DeviceStatus::Unsupported;
        default:
            return State() == DeviceStatus::Ready ? DeviceStatus::Failed : State();
    }
}
DeviceStatus DestroySurface(DeviceHandle device, SurfaceHandle& inoutSurface) noexcept
{
    const foundation::usize slot = FindSurface(device, inoutSurface);
    if (slot == MAX_DEVICE_SURFACES)
    {
        return DeviceStatus::InvalidHandle;
    }
    if (slot == 0 || (internal::FrameOpen() == true && internal::GetFrameSurface() == slot))
    {
        return DeviceStatus::InvalidState;
    }
    backend::DestroySurface(slot);
    GetOwnerRecord().Surfaces[slot] = {};
    inoutSurface = {};
    return DeviceStatus::Ready;
}
DeviceStatus GetSurfaceInfo(DeviceHandle device, SurfaceHandle surface, SurfaceInfo& outInfo) noexcept
{
    const foundation::usize slot = FindSurface(device, surface);
    if (slot == MAX_DEVICE_SURFACES)
    {
        return DeviceStatus::InvalidHandle;
    }
    const SurfaceRecord& record = GetOwnerRecord().Surfaces[slot];
    const backend::SurfaceCompletion completion = backend::GetSurfaceCompletion(slot);
    outInfo =
    {
        .State = record.IsLost == true ? SurfaceState::Lost : SurfaceState::Ready,
        .IsPrimary = slot == 0,
        .System = record.System,
        .Width = record.Width,
        .Height = record.Height,
        .SubmittedFrames = internal::GetSurfaceSubmittedFrames(slot),
        .RenderedFrames = completion.RenderedFrames,
        .PresentedFrames = completion.PresentedFrames,
    };
    return DeviceStatus::Ready;
}
ResourceStatus CreateShader(DeviceHandle device, const ShaderDescription& description, ShaderHandle& shader) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(internal::CreateShader(description, shader)) : admission;
}
ResourceStatus CreateUniform(DeviceHandle device, foundation::usize size, UniformHandle& uniform) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(internal::CreateUniform(size, uniform)) : admission;
}
ResourceStatus
CreatePipeline(DeviceHandle device, const PipelineDescription& description, PipelineHandle& pipeline) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(internal::CreatePipeline(description, pipeline))
                                              : admission;
}
ResourceStatus GetStatus(DeviceHandle device, ShaderHandle shader) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(internal::GetStatus(shader)) : admission;
}
ResourceStatus GetStatus(DeviceHandle device, UniformHandle uniform) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(internal::GetStatus(uniform)) : admission;
}
ResourceStatus GetStatus(DeviceHandle device, PipelineHandle pipeline) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(internal::GetStatus(pipeline)) : admission;
}
ResourceStatus UpdateUniform(DeviceHandle device,
                             UniformHandle uniform,
                             ludus::foundation::BorrowedView<const foundation::uint8> bytes) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(internal::UpdateUniform(uniform, bytes)) : admission;
}
ResourceStatus Destroy(DeviceHandle device, ShaderHandle shader) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(internal::Destroy(shader)) : admission;
}
ResourceStatus Destroy(DeviceHandle device, UniformHandle uniform) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(internal::Destroy(uniform)) : admission;
}
ResourceStatus Destroy(DeviceHandle device, PipelineHandle pipeline) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(internal::Destroy(pipeline)) : admission;
}
ResourceStatus DrawFullscreen(DeviceHandle device, PipelineHandle pipeline) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(internal::DrawFullscreen(pipeline)) : admission;
}
} // namespace ludus::graphics::rhi
