#include "internal/adapter.h"
#include "internal/backend.h"
#include "internal/dxil_container.h"
#include "internal/graphics_capture.h"
#include "internal/lifecycle.h"
#include "internal/measurement.h"
#include "internal/native_interop.h"
#include "internal/owner.h"
#include "internal/profile.h"
#include "internal/raster.h"
#include "internal/resources.h"
#include "internal/storage.h"
#include "internal/surface.h"
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>

#include <initializer_list>
#include <string_view>
namespace ludus::graphics::rhi
{
namespace
{
using ludus::foundation::uint32;
// Frame state of each surface slot (R14b). The last accepted target keeps a capture from
// starting for a frame that cannot open.
struct SurfaceFrames final
{
    bool IsTargetEmpty = false;
    bool IsTargetOffscreen = false;
    ludus::foundation::uint64 SubmittedFrames = 0;
};
/// Startup, frame and admission state of one owner slot (R14c).
struct SessionState final
{
    StartupInfo Startup;
    /// Callback token of the current startup attempt; zero when none is live.
    uint32 Token = 0;
    ludus::foundation::uint64 DeviceOwner = 0;
    bool IsFrameOpen = false;
    bool IsRayProfileRequested = false;
    bool IsDrawn = false;
    bool IsRasterDrawn = false;
    bool IsRasterRequired = false;
    bool IsComputeRequired = false;
    ludus::foundation::StaticArray<SurfaceFrames, MAX_DEVICE_SURFACES> SurfaceFrames{};
    /// Slot of the open frame's surface; meaningful only while IsFrameOpen.
    ludus::foundation::usize FrameSurface = 0;
    DeviceProfileRequirements RequiredProfile;
    DeviceProfile Profile;
    internal::FallbackHandler Fallback = nullptr;
};
ludus::foundation::StaticArray<SessionState, MAX_DEVICE_OWNERS> gSessions{};
/// Callback tokens are process-wide, so a token names exactly one owner's attempt.
uint32 gNextToken = 1;
/// The session state of the bound owner slot.
SessionState& GetSession() noexcept
{
    return gSessions[internal::GetBoundOwnerSlot()];
}
} // namespace
namespace internal
{
DeviceProfile CurrentDeviceProfile() noexcept
{
    return GetSession().Startup.State == StartupState::Ready ? GetSession().Profile : DeviceProfile{};
}
bool RequestsRayProfile() noexcept
{
    return GetSession().IsRayProfileRequested;
}
void SetRayProfileRequest(bool value) noexcept
{
    GetSession().IsRayProfileRequested = value;
}
bool FrameOpen() noexcept
{
    return GetSession().IsFrameOpen;
}
bool ClaimRasterFrame() noexcept
{
    if (GetSession().IsFrameOpen == false || GetSession().IsDrawn)
    {
        return false;
    }
    GetSession().IsRasterDrawn = true;
    return true;
}

ludus::foundation::uint64 DeviceOwner() noexcept
{
    return GetSession().DeviceOwner;
}
void SetDeviceOwner(ludus::foundation::uint64 owner) noexcept
{
    GetSession().DeviceOwner = owner;
}
bool SessionBusy() noexcept
{
    return GetSession().Startup.State != StartupState::Idle || NativeInteropBusy();
}
void SetFallback(FallbackHandler handler) noexcept
{
    GetSession().Fallback = handler;
}
ludus::foundation::usize FindOwnerSlot(ludus::foundation::uint64 owner) noexcept
{
    for (ludus::foundation::usize slot = 0; owner != 0 && slot < MAX_DEVICE_OWNERS; ++slot)
    {
        if (gSessions[slot].DeviceOwner == owner)
        {
            return slot;
        }
    }
    return MAX_DEVICE_OWNERS;
}
ludus::foundation::usize FindTokenSlot(uint32 token) noexcept
{
    for (ludus::foundation::usize slot = 0; token != 0 && slot < MAX_DEVICE_OWNERS; ++slot)
    {
        const SessionState& session = gSessions[slot];
        if (session.Token == token &&
            (session.Startup.State == StartupState::Pending || session.Startup.State == StartupState::Ready))
        {
            return slot;
        }
    }
    return MAX_DEVICE_OWNERS;
}
bool IsAnySessionBusy() noexcept
{
    const ludus::foundation::usize bound = GetBoundOwnerSlot();
    bool isBusy = false;
    for (ludus::foundation::usize slot = 0; slot < MAX_DEVICE_OWNERS; ++slot)
    {
        BindOwnerSlot(slot);
        isBusy = isBusy == true || SessionBusy() == true;
    }
    BindOwnerSlot(bound);
    return isBusy;
}
bool Current(uint32 token) noexcept
{
    // A callback token names one owner's attempt; bind it so the caller updates that owner.
    const ludus::foundation::usize slot = FindTokenSlot(token);
    if (slot == MAX_DEVICE_OWNERS)
    {
        return false;
    }
    BindOwnerSlot(slot);
    return true;
}
void Fail(uint32 token, StartupError error) noexcept
{
    if (Current(token) == false)
    {
        return;
    }
    // Browser Auto may convert a Pending-attempt failure into another backend
    // attempt. The handler tears down the failed sub-backend and restarts under
    // the same session token; the session stays Pending and is not finalized.
    if (GetSession().Fallback != nullptr && GetSession().Startup.State == StartupState::Pending &&
        GetSession().Fallback(token, error))
    {
        return;
    }
    RecordAttempt(token, GetSession().Startup.SelectedBackend, error);
    // Abandon a capture with the adapter's cleanup while its borrowed native objects live.
    EndGraphicsCaptureSession(error == StartupError::DeviceLost ? GraphicsCaptureError::DeviceLost
                                                                : GraphicsCaptureError::SessionEnded);
    GetSession().Token = 0;
    GetSession().Startup.State = error == StartupError::DeviceLost ? StartupState::DeviceLost : StartupState::Failed;
    GetSession().Startup.Error = error;
    GetSession().Startup.Capabilities = {};
    GetSession().Profile = {};
    if (error != StartupError::RequirementsUnsatisfied)
    {
        GetSession().Startup.UnmetRequirement = RequirementFailure::None;
        GetSession().Startup.ProfileFailure = {};
    }
    GetSession().IsFrameOpen = false;
    if (DeferNativeInteropShutdown())
    {
        backend::NativeInteropDetachSurface();
    }
    else
    {
        FinishNativeInteropShutdown();
    }
}
void FaultRasterSession() noexcept
{
    Fail(GetSession().Token, StartupError::RenderingUnavailable);
}
void Complete(uint32 token, StartupError error, const BackendLimits& limits) noexcept
{
    if (Current(token) == false || GetSession().Startup.State != StartupState::Pending)
    {
        return;
    }
    if (error != StartupError::None)
    {
        Fail(token, error);
        return;
    }
    // Clamp the negotiated device limit to the bounded engine API and round
    // down to an accepted uniform size. Adapter support alone is insufficient.
    const auto maxTextureDimension = limits.MaxFrameDimension2D;
    const auto maxUniformBufferSize = limits.MaxUniformBufferSize;
    const auto uniformLimit =
        static_cast<uint32>(maxUniformBufferSize < UNIFORM_CAPACITY ? maxUniformBufferSize : UNIFORM_CAPACITY);
    const auto uniformSize = uniformLimit - uniformLimit % 16;
    if (maxTextureDimension == 0 || uniformSize < 16)
    {
        Fail(token, StartupError::DeviceUnavailable);
        return;
    }
    GetSession().Startup.UnmetRequirement = GetSession().Startup.Requirements.MinFrameDimension2D > maxTextureDimension
                                                ? RequirementFailure::FrameDimension2D
                                            : GetSession().Startup.Requirements.MinUniformBufferSize > uniformSize
                                                ? RequirementFailure::UniformBufferSize
                                                : RequirementFailure::None;
    if (GetSession().Startup.UnmetRequirement != RequirementFailure::None)
    {
        Fail(token, StartupError::RequirementsUnsatisfied);
        return;
    }
    const auto profile = QueryDeviceProfile();
    GetSession().Startup.ProfileFailure = CheckProfile(GetSession().RequiredProfile, profile);
    if (GetSession().Startup.ProfileFailure.Requirement != ProfileRequirement::None)
    {
        GetSession().Startup.UnmetRequirement = RequirementFailure::Profile;
        Fail(token, StartupError::RequirementsUnsatisfied);
        return;
    }
    if (GetSession().IsRasterRequired == true &&
        (profile.Raster.MaxUniformRange < PORTABLE_UNIFORM_BYTES || profile.Raster.DrawsPerFrame == 0))
    {
        GetSession().Startup.UnmetRequirement = RequirementFailure::PortableRaster;
        Fail(token, StartupError::RequirementsUnsatisfied);
        return;
    }
    if (GetSession().IsComputeRequired && backend::ComputeLimits().MaxStorageRange == 0)
    {
        GetSession().Startup.UnmetRequirement = RequirementFailure::Compute;
        Fail(token, StartupError::RequirementsUnsatisfied);
        return;
    }
    GetSession().Profile = profile;
    RecordAttempt(token, GetSession().Startup.SelectedBackend, StartupError::None);
    GetSession().Startup.MaxTextureDimension2D = maxTextureDimension;
    GetSession().Startup.Capabilities =
    {
        .MaxFrameDimension2D = maxTextureDimension,
        .MaxUniformBufferSize = uniformSize,
        .UniformBufferSizeAlignment = 16,
        .MaxShaders = static_cast<uint32>(RESOURCE_CAPACITY),
        .MaxUniformBuffers = static_cast<uint32>(RESOURCE_CAPACITY),
        .MaxPipelines = static_cast<uint32>(RESOURCE_CAPACITY),
        .MaxDrawsPerFrame = 1,
    };
    GetSession().Startup.State = StartupState::Ready;
}
void SelectBackend(uint32 token, Backend backend) noexcept
{
    if (Current(token) == false)
    {
        return;
    }
    GetSession().Startup.SelectedBackend = backend;
    GetSession().Startup.ProfileFailure = {};
    GetSession().Startup.UnmetRequirement = RequirementFailure::None;
}
uint32 Reissue(uint32 token) noexcept
{
    if (Current(token) == false || GetSession().Startup.State != StartupState::Pending)
    {
        return 0;
    }
    if (gNextToken == 0)
    {
        return 0;
    }
    GetSession().Token = gNextToken++;
    return GetSession().Token;
}
void RecordAttempt(uint32 token, Backend backend, StartupError error) noexcept
{
    if (Current(token) == false)
    {
        return;
    }
    AttemptInfo* attempt = backend == Backend::WebGL2   ? &GetSession().Startup.WebGL2
                           : backend == Backend::WebGPU ? &GetSession().Startup.WebGpu
                                                        : nullptr;
    if (attempt != nullptr)
    {
        attempt->Attempted = true;
        attempt->Error = error;
        attempt->ProfileFailure = GetSession().Startup.ProfileFailure;
    }
}
} // namespace internal
namespace
{
StartStatus StartSelected(const ApplicationInfo& app,
                          const WindowInfo& window,
                          BackendSelection selection,
                          const DeviceRequirements& requirements,
                          bool requireRaster = false,
                          bool requireCompute = false,
                          const DeviceProfileRequirements& profile = {},
                          const StorageLimits& storage = {}) noexcept
{
    if (internal::SessionBusy())
    {
        return StartStatus::Busy;
    }
    if (internal::ConfigureStorage(storage) == false)
    {
        GetSession().Startup = {};
        GetSession().Startup.State = StartupState::Failed;
        GetSession().Startup.Error = StartupError::DeviceUnavailable;
        return StartStatus::Failed;
    }
    GetSession().Startup.Requested = selection;
    GetSession().Startup.Requirements = requirements;
    GetSession().IsRasterRequired = requireRaster;
    GetSession().IsComputeRequired = requireCompute;
    GetSession().RequiredProfile = profile;
    GetSession().Profile = {};
    // A forced selection the build cannot provide fails explicitly; it never
    // silently switches to another backend.
    if (backend::Supports(selection) == false)
    {
        GetSession().Startup.State = StartupState::Failed;
        GetSession().Startup.Error = StartupError::BackendUnavailable;
        return StartStatus::Failed;
    }
    if (gNextToken == 0)
    {
        GetSession().Startup.State = StartupState::Failed;
        GetSession().Startup.Error = StartupError::GenerationExhausted;
        return StartStatus::Failed;
    }
    GetSession().Startup.Requested = selection;
    GetSession().Startup.SelectedBackend = backend::Kind();
    GetSession().Token = gNextToken++;
    GetSession().Startup.State = StartupState::Pending;
    const StartupError error = backend::Start(app, window, GetSession().Token, selection);
    if (error != StartupError::None)
    {
        internal::Fail(GetSession().Token, error);
    }
    if (GetSession().Startup.State == StartupState::Ready)
    {
        return StartStatus::Ready;
    }
    return GetSession().Startup.State == StartupState::Pending ? StartStatus::Pending : StartStatus::Failed;
}
} // namespace
namespace internal
{
bool RequiresComputeProfile() noexcept
{
    return GetSession().Startup.State == StartupState::Pending && GetSession().IsComputeRequired;
}
StartStatus StartOwned(const ApplicationInfo& app,
                       const WindowInfo& window,
                       BackendSelection selection,
                       const DeviceRequirements& limits,
                       bool requireRaster,
                       bool requireCompute,
                       const DeviceProfileRequirements& profile,
                       const StorageLimits& storage) noexcept
{
    return StartSelected(app, window, selection, limits, requireRaster, requireCompute, profile, storage);
}
} // namespace internal
StartStatus Start(const ApplicationInfo& app, const WindowInfo& window) noexcept
{
    return Start(app, window, BackendSelection::Auto, {});
}
StartStatus Start(const ApplicationInfo& app, const WindowInfo& window, BackendSelection selection) noexcept
{
    return Start(app, window, selection, {});
}
StartStatus Start(const ApplicationInfo& app,
                  const WindowInfo& window,
                  BackendSelection selection,
                  const DeviceRequirements& requirements) noexcept
{
    internal::BindOwnerSlot(0);
    // The device-less facade owns the first slot and runs only while no other owner is live.
    if (internal::IsAnySessionBusy() == true)
    {
        return StartStatus::Busy;
    }
    return StartSelected(app, window, selection, requirements);
}
StartupInfo internal::GetStartup() noexcept
{
    return GetSession().Startup;
}
namespace internal
{
FrameStatus SetSurfaceFrameTarget(ludus::foundation::usize slot, const FrameTarget& target) noexcept
{
    if (GetSession().Startup.State != StartupState::Ready)
    {
        return FrameStatus::NotReady;
    }
    if (GetSession().IsFrameOpen)
    {
        return FrameStatus::InvalidState;
    }
    if (target.Width > GetSession().Startup.Capabilities.MaxFrameDimension2D ||
        target.Height > GetSession().Startup.Capabilities.MaxFrameDimension2D)
    {
        return FrameStatus::Failed;
    }
    backend::SelectSurface(slot);
    const FrameStatus result = backend::SetTarget(target);
    if (result == FrameStatus::Ready)
    {
        GetSession().SurfaceFrames[slot].IsTargetEmpty = target.Width == 0 || target.Height == 0;
        GetSession().SurfaceFrames[slot].IsTargetOffscreen = target.ShouldPresentToSurface == false;
    }
    return result;
}
FrameStatus BeginSurfaceFrame(ludus::foundation::usize slot) noexcept
{
    if (GetSession().Startup.State != StartupState::Ready)
    {
        return FrameStatus::NotReady;
    }
    if (GetSession().IsFrameOpen)
    {
        return FrameStatus::InvalidState;
    }
    if (internal::PrepareRasterFrame() == false)
    {
        return FrameStatus::Failed;
    }
    // An armed capture starts before the frame's command buffers exist (Metal requires it).
    internal::BeginGraphicsCaptureFrame(GetSession().SurfaceFrames[slot].IsTargetEmpty,
                                        GetSession().SurfaceFrames[slot].IsTargetOffscreen);
    backend::SelectSurface(slot);
    const auto result = backend::Begin();
    GetSession().IsFrameOpen = result == FrameStatus::Ready;
    GetSession().FrameSurface = slot;
    GetSession().IsDrawn = false;
    GetSession().IsRasterDrawn = false;
    internal::RasterBeginFrame();
    internal::FinishGraphicsCaptureBegin(GetSession().IsFrameOpen);
    return result;
}
ludus::foundation::usize GetFrameSurface() noexcept
{
    return GetSession().FrameSurface;
}
void ResetSurfaceFrames(ludus::foundation::usize slot) noexcept
{
    GetSession().SurfaceFrames[slot] = {};
}
ludus::foundation::uint64 GetSurfaceSubmittedFrames(ludus::foundation::usize slot) noexcept
{
    return GetSession().SurfaceFrames[slot].SubmittedFrames;
}
} // namespace internal
FrameStatus SetFrameTarget(const FrameTarget& target) noexcept
{
    internal::BindOwnerSlot(0);
    return internal::SetSurfaceFrameTarget(0, target);
}
FrameStatus BeginFrameStatus() noexcept
{
    internal::BindOwnerSlot(0);
    return internal::BeginSurfaceFrame(0);
}
FrameStatus internal::EndFrameStatus() noexcept
{
    if (GetSession().Startup.State != StartupState::Ready)
    {
        return FrameStatus::NotReady;
    }
    if (GetSession().IsFrameOpen == false)
    {
        return FrameStatus::InvalidState;
    }
    if (internal::RasterFinishFrame() == false)
    {
        return FrameStatus::Failed;
    }
    GetSession().IsFrameOpen = false;
    const auto result = backend::End();
    // Skipped means the frame was submitted but its presentation was out of date.
    if (result == FrameStatus::Ready || result == FrameStatus::Skipped)
    {
        ++GetSession().SurfaceFrames[GetSession().FrameSurface].SubmittedFrames;
    }
    internal::RasterEndFrame(result == FrameStatus::Ready || result == FrameStatus::Skipped);
    internal::EndGraphicsCaptureFrame((result == FrameStatus::Ready || result == FrameStatus::Skipped) &&
                                      GetSession().Startup.State == StartupState::Ready);
    return result;
}
void internal::Shutdown() noexcept
{
    // Stop or abandon a capture before any native object it borrows is released.
    internal::EndGraphicsCaptureSession(GraphicsCaptureError::SessionEnded);
    GetSession().SurfaceFrames.Fill({});
    GetSession().FrameSurface = 0;
    GetSession().DeviceOwner = 0;
    internal::SetGpuTimingRequest(false);
    GetSession().IsRayProfileRequested = false;
    internal::SetBindlessRequest(false);
    internal::SetRuntimeTextureArrayRequest(false);
    internal::SetAdapterRequest({});
    GetSession().Fallback = nullptr;
    GetSession().Token = 0;
    if (internal::DeferNativeInteropShutdown())
    {
        backend::NativeInteropDetachSurface();
    }
    else
    {
        internal::FinishNativeInteropShutdown();
    }
    GetSession().Startup = {};
    GetSession().IsFrameOpen = false;
}
namespace internal
{
void FinishNativeInteropShutdown() noexcept
{
    ReleaseRasterResources();
    ReleaseResources();
    backend::NativeInteropReset();
    backend::Shutdown();
    NativeInteropShutdownFinished();
}
} // namespace internal

namespace internal
{
struct ResourceAccess final
{
    template <typename T>
    static uint32 Id(T handle) noexcept
    {
        return handle.Id;
    }
    template <typename T>
    static void Set(T& handle, uint32 id) noexcept
    {
        handle.Id = id;
    }
};
} // namespace internal
namespace
{
using foundation::usize;
using internal::ResourceAccess;
struct Resource final
{
    uint32 Id = 0;
    ResourceStatus Status = ResourceStatus::Pending;
    ShaderStage Stage = ShaderStage::Vertex;
    usize Size = 0;
    bool Updated = false;
    usize Vertex = 0;
    usize Fragment = 0;
    usize Uniform = 0;
};
/// Fullscreen shader, uniform and pipeline records of one owner slot (R14c).
struct ResourceState final
{
    Resource Shaders[internal::RESOURCE_CAPACITY];
    Resource Uniforms[internal::RESOURCE_CAPACITY];
    Resource Pipelines[internal::RESOURCE_CAPACITY];
};
foundation::StaticArray<ResourceState, MAX_DEVICE_OWNERS> gResources{};
/// Fullscreen resource identities are process-wide, so one owner never resolves another's.
uint32 gNextResource = 1;
/// The fullscreen resource records of the bound owner slot.
ResourceState& GetResources() noexcept
{
    return gResources[internal::GetBoundOwnerSlot()];
}
constexpr usize MISSING = internal::RESOURCE_CAPACITY;

usize Find(const Resource* resources, uint32 id) noexcept
{
    if (id != 0)
    {
        for (usize i = 0; i < internal::RESOURCE_CAPACITY; ++i)
        {
            if (resources[i].Id == id)
            {
                return i;
            }
        }
    }
    return MISSING;
}
ResourceStatus Status(const Resource* resources, uint32 id) noexcept
{
    const auto slot = Find(resources, id);
    return slot == MISSING ? ResourceStatus::InvalidHandle : resources[slot].Status;
}
ResourceStatus Allocate(Resource* resources, usize& slot) noexcept
{
    if (GetSession().Startup.State != StartupState::Ready)
    {
        return ResourceStatus::NotReady;
    }
    if (GetSession().IsFrameOpen)
    {
        return ResourceStatus::InvalidState;
    }
    if (gNextResource == 0)
    {
        return ResourceStatus::CapacityExceeded;
    }
    for (slot = 0; slot < internal::RESOURCE_CAPACITY; ++slot)
    {
        if (resources[slot].Id == 0)
        {
            resources[slot] = {};
            resources[slot].Id = gNextResource++;
            return ResourceStatus::Ready;
        }
    }
    return ResourceStatus::CapacityExceeded;
}
/// Shader entry point names fit 64 bytes including the terminator.
constexpr usize ENTRY_POINT_CAPACITY_BYTES = 64;
bool ValidEntry(std::string_view entry) noexcept
{
    return entry.empty() == false && entry.size() < ENTRY_POINT_CAPACITY_BYTES &&
           entry.find('\0') == std::string_view::npos;
}
ResourceStatus Finish(Resource& resource, ResourceStatus result) noexcept
{
    if (resource.Id == 0 || GetSession().Startup.State != StartupState::Ready)
    {
        // A synchronous loss callback can invalidate the creation in progress.
        return ResourceStatus::Failed;
    }
    // A provider may complete synchronously before returning Pending.
    if (resource.Status == ResourceStatus::Pending && result != ResourceStatus::Pending)
    {
        internal::ResourceComplete(resource.Id, result);
    }
    return resource.Status;
}
} // namespace
namespace internal
{
void ResourceComplete(uint32 id, ResourceStatus status) noexcept
{
    // Identities are process-wide; a completion updates the owner that created the resource.
    for (foundation::usize owner = 0; owner < MAX_DEVICE_OWNERS; ++owner)
    {
        const ResourceState& resources = gResources[owner];
        if (Find(resources.Shaders, id) != MISSING || Find(resources.Uniforms, id) != MISSING ||
            Find(resources.Pipelines, id) != MISSING)
        {
            BindOwnerSlot(owner);
            break;
        }
    }
    if (GetSession().Startup.State != StartupState::Ready)
    {
        return;
    }
    const usize shader = Find(GetResources().Shaders, id);
    const usize uniform = Find(GetResources().Uniforms, id);
    const usize pipeline = Find(GetResources().Pipelines, id);
    Resource* resource = nullptr;
    if (shader != MISSING)
    {
        resource = &GetResources().Shaders[shader];
    }
    if (uniform != MISSING)
    {
        resource = &GetResources().Uniforms[uniform];
    }
    if (pipeline != MISSING)
    {
        resource = &GetResources().Pipelines[pipeline];
    }
    if (resource == nullptr || resource->Status != ResourceStatus::Pending)
    {
        return;
    }
    resource->Status = status == ResourceStatus::Ready ? status : ResourceStatus::Failed;
    if (resource->Status == ResourceStatus::Failed)
    {
        if (shader != MISSING)
        {
            backend::DestroyShader(shader);
        }
        if (uniform != MISSING)
        {
            backend::DestroyUniform(uniform);
        }
        if (pipeline != MISSING)
        {
            backend::DestroyPipeline(pipeline);
        }
    }
}
void ReleaseResources() noexcept
{
    // Invalidate all IDs before releasing backend state; callbacks may be delayed.
    for (Resource& resource : GetResources().Pipelines)
    {
        resource.Id = 0;
    }
    for (Resource& resource : GetResources().Shaders)
    {
        resource.Id = 0;
    }
    for (Resource& resource : GetResources().Uniforms)
    {
        resource.Id = 0;
    }
    for (usize i = 0; i < RESOURCE_CAPACITY; ++i)
    {
        backend::DestroyPipeline(i);
        backend::DestroyShader(i);
        backend::DestroyUniform(i);
    }
}
} // namespace internal
ResourceStatus internal::GetStatus(ShaderHandle handle) noexcept
{
    return Status(GetResources().Shaders, ResourceAccess::Id(handle));
}
ResourceStatus internal::GetStatus(UniformHandle handle) noexcept
{
    return Status(GetResources().Uniforms, ResourceAccess::Id(handle));
}
ResourceStatus internal::GetStatus(PipelineHandle handle) noexcept
{
    return Status(GetResources().Pipelines, ResourceAccess::Id(handle));
}
ResourceStatus internal::CreateShader(const ShaderDescription& description, ShaderHandle& outHandle) noexcept
{
    if (ResourceAccess::Id(outHandle) != 0)
    {
        return ResourceStatus::InvalidState;
    }
    if (description.Spirv.IsValid() == false || description.Dxil.IsValid() == false ||
        description.UniformSize > internal::UNIFORM_CAPACITY)
    {
        return ResourceStatus::InvalidDescription;
    }
    if (GetSession().Startup.State == StartupState::Ready &&
        description.UniformSize > GetSession().Startup.Capabilities.MaxUniformBufferSize)
    {
        return ResourceStatus::InvalidDescription;
    }
    const auto kind = backend::Kind();
    // The backend later checks the DXIL program kind and entry inside the container.
    const bool validArtifact =
        kind == Backend::Vulkan ? (description.Spirv.GetSize() >= internal::SPIRV_HEADER_WORDS &&
                                   description.Spirv[0] == internal::SPIRV_MAGIC && ValidEntry(description.SpirvEntry))
        : kind == Backend::WebGL2 ? (description.GlslEs.empty() == false && description.GlslEsEntry == "main")
        : kind == Backend::Metal  ? (description.Msl.empty() == false && ValidEntry(description.MslEntry))
        : kind == Backend::Direct3D12
            ? (internal::IsDxilContainer({description.Dxil.GetDataOrNull(), description.Dxil.GetSize()}) == true &&
               ValidEntry(description.DxilEntry))
            : (description.Wgsl.empty() == false && ValidEntry(description.WgslEntry));
    if ((description.Stage != ShaderStage::Vertex && description.Stage != ShaderStage::Fragment) ||
        validArtifact == false)
    {
        return ResourceStatus::InvalidDescription;
    }
    usize slot = 0;
    const ResourceStatus admission = Allocate(GetResources().Shaders, slot);
    if (admission != ResourceStatus::Ready)
    {
        return admission;
    }
    Resource& resource = GetResources().Shaders[slot];
    resource.Stage = description.Stage;
    resource.Size = description.UniformSize;
    ResourceAccess::Set(outHandle, resource.Id);
    return Finish(resource, backend::CreateShader(slot, description, resource.Id));
}
ResourceStatus internal::CreateUniform(usize size, UniformHandle& outHandle) noexcept
{
    if (ResourceAccess::Id(outHandle) != 0)
    {
        return ResourceStatus::InvalidState;
    }
    if (size < 16 || size > internal::UNIFORM_CAPACITY || size % 16 != 0)
    {
        return ResourceStatus::InvalidDescription;
    }
    if (GetSession().Startup.State == StartupState::Ready &&
        size > GetSession().Startup.Capabilities.MaxUniformBufferSize)
    {
        return ResourceStatus::InvalidDescription;
    }
    usize slot = 0;
    const ResourceStatus admission = Allocate(GetResources().Uniforms, slot);
    if (admission != ResourceStatus::Ready)
    {
        return admission;
    }
    Resource& resource = GetResources().Uniforms[slot];
    resource.Size = size;
    ResourceAccess::Set(outHandle, resource.Id);
    return Finish(resource, backend::CreateUniform(slot, { .Size = size }, resource.Id));
}
ResourceStatus internal::CreatePipeline(const PipelineDescription& description, PipelineHandle& outHandle) noexcept
{
    if (ResourceAccess::Id(outHandle) != 0)
    {
        return ResourceStatus::InvalidState;
    }
    const usize vertex = Find(GetResources().Shaders, ResourceAccess::Id(description.Vertex));
    const usize fragment = Find(GetResources().Shaders, ResourceAccess::Id(description.Fragment));
    const usize uniform = Find(GetResources().Uniforms, ResourceAccess::Id(description.Uniform));
    if (vertex == MISSING || fragment == MISSING || uniform == MISSING)
    {
        return ResourceStatus::InvalidHandle;
    }
    if (GetResources().Shaders[vertex].Stage != ShaderStage::Vertex ||
        GetResources().Shaders[fragment].Stage != ShaderStage::Fragment)
    {
        return ResourceStatus::InvalidDescription;
    }
    for (ResourceStatus status : {GetResources().Shaders[vertex].Status,
                                  GetResources().Shaders[fragment].Status,
                                  GetResources().Uniforms[uniform].Status})
    {
        if (status == ResourceStatus::Pending)
        {
            // Pending is reserved for a creation that owns its output outHandle.
            return ResourceStatus::NotReady;
        }
        if (status != ResourceStatus::Ready)
        {
            return status;
        }
    }
    if (GetResources().Uniforms[uniform].Size < GetResources().Shaders[vertex].Size ||
        GetResources().Uniforms[uniform].Size < GetResources().Shaders[fragment].Size)
    {
        return ResourceStatus::InvalidDescription;
    }
    usize slot = 0;
    const ResourceStatus admission = Allocate(GetResources().Pipelines, slot);
    if (admission != ResourceStatus::Ready)
    {
        return admission;
    }
    Resource& resource = GetResources().Pipelines[slot];
    resource.Vertex = vertex;
    resource.Fragment = fragment;
    resource.Uniform = uniform;
    ResourceAccess::Set(outHandle, resource.Id);
    return Finish(
        resource,
        backend::CreatePipeline(slot, { .Vertex = vertex, .Fragment = fragment, .Uniform = uniform }, resource.Id));
}
FrameInfo internal::GetFrameInfo() noexcept
{
    return GetSession().IsFrameOpen && GetSession().Startup.State == StartupState::Ready ? backend::GetFrameInfo()
                                                                                         : FrameInfo{};
}
ResourceStatus internal::UpdateUniform(UniformHandle handle,
                                       ludus::foundation::BorrowedView<const foundation::uint8> bytes) noexcept
{
    const usize slot = Find(GetResources().Uniforms, ResourceAccess::Id(handle));
    if (slot == MISSING)
    {
        return ResourceStatus::InvalidHandle;
    }
    if (GetSession().IsFrameOpen && GetSession().IsDrawn)
    {
        return ResourceStatus::InvalidState;
    }
    if (GetResources().Uniforms[slot].Status != ResourceStatus::Ready)
    {
        return GetResources().Uniforms[slot].Status;
    }
    if (bytes.IsValid() == false || bytes.GetSize() != GetResources().Uniforms[slot].Size)
    {
        return ResourceStatus::InvalidDescription;
    }
    backend::UpdateUniform(slot, bytes);
    GetResources().Uniforms[slot].Updated = true;
    return ResourceStatus::Ready;
}
ResourceStatus internal::Destroy(PipelineHandle handle) noexcept
{
    const usize slot = Find(GetResources().Pipelines, ResourceAccess::Id(handle));
    if (slot == MISSING)
    {
        return ResourceStatus::InvalidHandle;
    }
    if (GetSession().IsFrameOpen)
    {
        return ResourceStatus::InvalidState;
    }
    GetResources().Pipelines[slot].Id = 0;
    backend::DestroyPipeline(slot);
    return ResourceStatus::Ready;
}
ResourceStatus internal::Destroy(ShaderHandle handle) noexcept
{
    const usize slot = Find(GetResources().Shaders, ResourceAccess::Id(handle));
    if (slot == MISSING)
    {
        return ResourceStatus::InvalidHandle;
    }
    if (GetSession().IsFrameOpen)
    {
        return ResourceStatus::InvalidState;
    }
    for (const Resource& pipeline : GetResources().Pipelines)
    {
        if (pipeline.Id != 0 && pipeline.Status != ResourceStatus::Failed &&
            (pipeline.Vertex == slot || pipeline.Fragment == slot))
        {
            return ResourceStatus::InUse;
        }
    }
    GetResources().Shaders[slot].Id = 0;
    backend::DestroyShader(slot);
    return ResourceStatus::Ready;
}
ResourceStatus internal::Destroy(UniformHandle handle) noexcept
{
    const usize slot = Find(GetResources().Uniforms, ResourceAccess::Id(handle));
    if (slot == MISSING)
    {
        return ResourceStatus::InvalidHandle;
    }
    if (GetSession().IsFrameOpen)
    {
        return ResourceStatus::InvalidState;
    }
    for (const Resource& pipeline : GetResources().Pipelines)
    {
        if (pipeline.Id != 0 && pipeline.Status != ResourceStatus::Failed && pipeline.Uniform == slot)
        {
            return ResourceStatus::InUse;
        }
    }
    GetResources().Uniforms[slot].Id = 0;
    backend::DestroyUniform(slot);
    return ResourceStatus::Ready;
}
ResourceStatus internal::DrawFullscreen(PipelineHandle handle) noexcept
{
    const usize slot = Find(GetResources().Pipelines, ResourceAccess::Id(handle));
    if (slot == MISSING)
    {
        return ResourceStatus::InvalidHandle;
    }
    if (GetResources().Pipelines[slot].Status != ResourceStatus::Ready)
    {
        return GetResources().Pipelines[slot].Status;
    }
    if (GetSession().IsFrameOpen == false || GetSession().IsDrawn || GetSession().IsRasterDrawn ||
        GetResources().Uniforms[GetResources().Pipelines[slot].Uniform].Updated == false)
    {
        return ResourceStatus::InvalidState;
    }
    const auto result = backend::Draw(slot);
    GetSession().IsDrawn = result == ResourceStatus::Ready;
    return result;
}
} // namespace ludus::graphics::rhi
namespace ludus::graphics::rhi
{
// Device-less facade calls address the first owner slot (R14c); the RHI's own callers use the
// internal forms, which address the bound owner.
StartupInfo GetStartup() noexcept
{
    internal::BindOwnerSlot(0);
    return internal::GetStartup();
}
FrameStatus EndFrameStatus() noexcept
{
    internal::BindOwnerSlot(0);
    return internal::EndFrameStatus();
}
void Shutdown() noexcept
{
    internal::BindOwnerSlot(0);
    internal::Shutdown();
}
ResourceStatus GetStatus(ShaderHandle handle) noexcept
{
    internal::BindOwnerSlot(0);
    return internal::GetStatus(handle);
}
ResourceStatus GetStatus(UniformHandle handle) noexcept
{
    internal::BindOwnerSlot(0);
    return internal::GetStatus(handle);
}
ResourceStatus GetStatus(PipelineHandle handle) noexcept
{
    internal::BindOwnerSlot(0);
    return internal::GetStatus(handle);
}
ResourceStatus CreateShader(const ShaderDescription& description, ShaderHandle& outHandle) noexcept
{
    internal::BindOwnerSlot(0);
    return internal::CreateShader(description, outHandle);
}
ResourceStatus CreateUniform(foundation::usize size, UniformHandle& outHandle) noexcept
{
    internal::BindOwnerSlot(0);
    return internal::CreateUniform(size, outHandle);
}
ResourceStatus CreatePipeline(const PipelineDescription& description, PipelineHandle& outHandle) noexcept
{
    internal::BindOwnerSlot(0);
    return internal::CreatePipeline(description, outHandle);
}
FrameInfo GetFrameInfo() noexcept
{
    internal::BindOwnerSlot(0);
    return internal::GetFrameInfo();
}
ResourceStatus UpdateUniform(UniformHandle handle, foundation::BorrowedView<const foundation::uint8> bytes) noexcept
{
    internal::BindOwnerSlot(0);
    return internal::UpdateUniform(handle, bytes);
}
ResourceStatus Destroy(PipelineHandle handle) noexcept
{
    internal::BindOwnerSlot(0);
    return internal::Destroy(handle);
}
ResourceStatus Destroy(ShaderHandle handle) noexcept
{
    internal::BindOwnerSlot(0);
    return internal::Destroy(handle);
}
ResourceStatus Destroy(UniformHandle handle) noexcept
{
    internal::BindOwnerSlot(0);
    return internal::Destroy(handle);
}
ResourceStatus DrawFullscreen(PipelineHandle handle) noexcept
{
    internal::BindOwnerSlot(0);
    return internal::DrawFullscreen(handle);
}
} // namespace ludus::graphics::rhi
