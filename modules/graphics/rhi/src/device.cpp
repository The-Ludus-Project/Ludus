#include <ludus/graphics/rhi/device.h>

#include "internal/backend.h"
#include "internal/device.h"
#include "internal/gpu_scope_backend.h"
#include "internal/lifecycle.h"
#include "internal/measurement.h"
#include "internal/native_interop.h"
#include "internal/profile.h"
#include "internal/raster.h"
#include "internal/ray_backend.h"
#include "internal/storage.h"

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
};
} // namespace internal
namespace
{
using internal::DeviceAccess;
internal::DeviceOwnerSequence gOwners;
DeviceDescription gDescription;

// R0 shares the existing registry/loss protocol rather than introducing a second
// resource registry. See docs/architecture/rhi-gdi.md, ownership and identity.
bool Valid(DeviceHandle device) noexcept
{
    return DeviceAccess::Owner(device) != 0 && DeviceAccess::Owner(device) == internal::DeviceOwner();
}
bool Valid(DeviceHandle device, SurfaceHandle surface) noexcept
{
    return Valid(device) && DeviceAccess::Owner(surface) == DeviceAccess::Owner(device);
}
DeviceStatus State() noexcept
{
    switch (GetStartup().State)
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
    };
}
DeviceStatus CreateDevice(const ApplicationInfo& application,
                          const WindowInfo& window,
                          const DeviceDescription& description,
                          DeviceHandle& device,
                          SurfaceHandle& surface,
                          StartupInfo* failure) noexcept
{
    if (DeviceAccess::Owner(device) != 0 || DeviceAccess::Owner(surface) != 0 || internal::SessionBusy())
    {
        return DeviceStatus::InvalidState;
    }
    const auto admission = ValidateDeviceDescription(description);
    if (admission != DeviceStatus::Ready)
    {
        return admission;
    }
    if (gOwners.Next == 0)
    {
        return DeviceStatus::IdentityExhausted;
    }
    internal::SetGpuTimingRequest(description.EnableGpuTiming);
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
        if (failure != nullptr)
        {
            *failure = GetStartup();
        }
        Shutdown();
        return DeviceStatus::Failed;
    }
    if (result == StartStatus::Ready && description.Required.RayQueries && backend::RayLimits().InlineQueries == false)
    {
        Shutdown();
        return DeviceStatus::Unsupported;
    }
    if (result == StartStatus::Ready && description.Required.BindlessTextureIndexing == true &&
        backend::BindingLimits().HasBindlessTextureIndexing == false)
    {
        Shutdown();
        return DeviceStatus::Unsupported;
    }
    if (result == StartStatus::Ready && description.Required.HasRuntimeTextureArrays == true &&
        backend::BindingLimits().HasRuntimeTextureArrays == false)
    {
        Shutdown();
        return DeviceStatus::Unsupported;
    }
    const auto owner = gOwners.Take();
    internal::SetDeviceOwner(owner);
    gDescription = description;
    DeviceAccess::Set(device, owner);
    DeviceAccess::Set(surface, owner);
    return result == StartStatus::Ready ? DeviceStatus::Ready : DeviceStatus::Pending;
}
DeviceStatus CreateNativeDevice(const ApplicationInfo& application,
                                const NativeDeviceDescription& native,
                                const DeviceDescription& description,
                                foundation::uint32 width,
                                foundation::uint32 height,
                                DeviceHandle& device,
                                SurfaceHandle& surface) noexcept
{
    if (DeviceAccess::Owner(device) != 0 || DeviceAccess::Owner(surface) != 0 || internal::SessionBusy())
    {
        return DeviceStatus::InvalidState;
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
    const auto result = CreateDevice(application, window, description, device, surface);
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
    info.Startup = GetStartup();
    info.Description = gDescription;
    const auto state = State();
    if (state == DeviceStatus::Ready)
    {
        info.FrameOpen = internal::FrameOpen();
        info.Supported.RayQueries = backend::RayLimits().InlineQueries;
        info.Enabled.RayQueries = info.Supported.RayQueries;
        info.Supported.FullscreenRaster = true;
        info.Enabled.FullscreenRaster = true;
        info.Profile = internal::CurrentDeviceProfile();
        info.Supported.BindlessTextureIndexing = info.Profile.Binding.HasBindlessTextureIndexing;
        info.Enabled.BindlessTextureIndexing = info.Supported.BindlessTextureIndexing;
        info.Supported.HasRuntimeTextureArrays = info.Profile.Binding.HasRuntimeTextureArrays;
        info.Enabled.HasRuntimeTextureArrays = info.Supported.HasRuntimeTextureArrays;
        info.Supported.PortableRaster = info.Profile.Raster.MaxUniformRange >= 16384;
        info.Enabled.PortableRaster = info.Supported.PortableRaster;
        info.Supported.OrderedRasterGraph = info.Supported.PortableRaster;
        info.Enabled.OrderedRasterGraph = info.Supported.OrderedRasterGraph;
        info.Supported.Compute = info.Supported.PortableRaster && info.Profile.Compute.MaxStorageRange != 0;
        info.Enabled.Compute = info.Supported.Compute;
        info.Supported.IndirectRendering = info.Supported.Compute;
        info.Enabled.IndirectRendering = info.Supported.IndirectRendering;
        info.UnmetPreferences =
        {
            .FullscreenRaster = gDescription.Preferred.FullscreenRaster && info.Enabled.FullscreenRaster == false,
            .PortableRaster = gDescription.Preferred.PortableRaster && info.Enabled.PortableRaster == false,
            .Compute = gDescription.Preferred.Compute && info.Enabled.Compute == false,
            .IndirectRendering = gDescription.Preferred.IndirectRendering && info.Enabled.IndirectRendering == false,
            .RayQueries = gDescription.Preferred.RayQueries && info.Enabled.RayQueries == false,
            .OrderedRasterGraph = gDescription.Preferred.OrderedRasterGraph && info.Enabled.OrderedRasterGraph == false,
            .BindlessTextureIndexing =
                gDescription.Preferred.BindlessTextureIndexing == true && info.Enabled.BindlessTextureIndexing == false,
            .HasRuntimeTextureArrays =
                gDescription.Preferred.HasRuntimeTextureArrays == true && info.Enabled.HasRuntimeTextureArrays == false,
        };
        info.UnmetProfilePreference = internal::CheckProfile(gDescription.PreferredProfile, info.Profile);
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
namespace
{
bool gGpuTimingRequested = false;
}
RasterStatus ValidateMeasurementDevice(DeviceHandle device) noexcept
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
    gGpuTimingRequested = requested;
}
bool GpuTimingRequested() noexcept
{
    return gGpuTimingRequested;
}
} // namespace internal
DeviceStatus DestroyDevice(DeviceHandle& device) noexcept
{
    if (Valid(device) == false)
    {
        return DeviceStatus::InvalidHandle;
    }
    Shutdown();
    device = {};
    return DeviceStatus::Ready;
}
DeviceStatus SetFrameTarget(DeviceHandle device, SurfaceHandle surface, const FrameTarget& target) noexcept
{
    return Valid(device, surface) ? FrameResult(SetFrameTarget(target)) : DeviceStatus::InvalidHandle;
}
DeviceStatus BeginFrame(DeviceHandle device, SurfaceHandle surface) noexcept
{
    return Valid(device, surface) ? FrameResult(BeginFrameStatus()) : DeviceStatus::InvalidHandle;
}
DeviceStatus EndFrame(DeviceHandle device) noexcept
{
    return Valid(device) ? FrameResult(EndFrameStatus()) : DeviceStatus::InvalidHandle;
}
DeviceStatus GetFrameInfo(DeviceHandle device, SurfaceHandle surface, FrameInfo& info) noexcept
{
    if (Valid(device, surface) == false)
    {
        return DeviceStatus::InvalidHandle;
    }
    const auto state = State();
    if (state != DeviceStatus::Ready)
    {
        return state == DeviceStatus::Pending ? DeviceStatus::NotReady : state;
    }
    const auto acquired = GetFrameInfo();
    if (acquired.Width == 0 || acquired.Height == 0)
    {
        return DeviceStatus::InvalidState;
    }
    info = acquired;
    return DeviceStatus::Ready;
}
ResourceStatus CreateShader(DeviceHandle device, const ShaderDescription& description, ShaderHandle& shader) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(CreateShader(description, shader)) : admission;
}
ResourceStatus CreateUniform(DeviceHandle device, foundation::usize size, UniformHandle& uniform) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(CreateUniform(size, uniform)) : admission;
}
ResourceStatus
CreatePipeline(DeviceHandle device, const PipelineDescription& description, PipelineHandle& pipeline) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(CreatePipeline(description, pipeline)) : admission;
}
ResourceStatus GetStatus(DeviceHandle device, ShaderHandle shader) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(GetStatus(shader)) : admission;
}
ResourceStatus GetStatus(DeviceHandle device, UniformHandle uniform) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(GetStatus(uniform)) : admission;
}
ResourceStatus GetStatus(DeviceHandle device, PipelineHandle pipeline) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(GetStatus(pipeline)) : admission;
}
ResourceStatus
UpdateUniform(DeviceHandle device, UniformHandle uniform, std::span<const foundation::uint8> bytes) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(UpdateUniform(uniform, bytes)) : admission;
}
ResourceStatus Destroy(DeviceHandle device, ShaderHandle shader) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(Destroy(shader)) : admission;
}
ResourceStatus Destroy(DeviceHandle device, UniformHandle uniform) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(Destroy(uniform)) : admission;
}
ResourceStatus Destroy(DeviceHandle device, PipelineHandle pipeline) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(Destroy(pipeline)) : admission;
}
ResourceStatus DrawFullscreen(DeviceHandle device, PipelineHandle pipeline) noexcept
{
    const auto admission = ResourceAdmission(device);
    return admission == ResourceStatus::Ready ? ResourceResult(DrawFullscreen(pipeline)) : admission;
}
} // namespace ludus::graphics::rhi
