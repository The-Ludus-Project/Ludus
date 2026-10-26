#include "internal/backend.h"
#include "internal/dxil_container.h"
#include "internal/lifecycle.h"
#include "internal/measurement.h"
#include "internal/native_interop.h"
#include "internal/profile.h"
#include "internal/raster.h"
#include "internal/resources.h"
#include "internal/storage.h"
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>

#include <initializer_list>
#include <span>
#include <string_view>
namespace ludus::graphics::rhi
{
namespace
{
using ludus::foundation::uint32;
StartupInfo gStartup;
uint32 gToken = 0;
uint32 gNextToken = 1;
ludus::foundation::uint64 gDeviceOwner = 0;
bool gLegacy = false;
bool gRendering = false;
bool gFrame = false;
bool gRayProfileRequest = false;
bool gDrawn = false;
bool gRasterDrawn = false;
bool gRequireRaster = false;
bool gRequireCompute = false;
DeviceProfileRequirements gRequiredProfile;
DeviceProfile gProfile;
internal::FallbackHandler gFallback = nullptr;
} // namespace
namespace internal
{
DeviceProfile CurrentDeviceProfile() noexcept
{
    return gStartup.State == StartupState::Ready ? gProfile : DeviceProfile{};
}
bool RequestsRayProfile() noexcept
{
    return gRayProfileRequest;
}
void SetRayProfileRequest(bool value) noexcept
{
    gRayProfileRequest = value;
}
bool FrameOpen() noexcept
{
    return gFrame;
}
bool ClaimRasterFrame() noexcept
{
    if (gFrame == false || gDrawn)
    {
        return false;
    }
    gRasterDrawn = true;
    return true;
}

ludus::foundation::uint64 DeviceOwner() noexcept
{
    return gDeviceOwner;
}
void SetDeviceOwner(ludus::foundation::uint64 owner) noexcept
{
    gDeviceOwner = owner;
}
bool SessionBusy() noexcept
{
    return gLegacy || gStartup.State != StartupState::Idle || NativeInteropBusy();
}
void SetFallback(FallbackHandler handler) noexcept
{
    gFallback = handler;
}
bool Current(uint32 token) noexcept
{
    return token != 0 && token == gToken &&
           (gStartup.State == StartupState::Pending || gStartup.State == StartupState::Ready);
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
    if (gFallback != nullptr && gStartup.State == StartupState::Pending && gFallback(token, error))
    {
        return;
    }
    RecordAttempt(token, gStartup.SelectedBackend, error);
    gToken = 0;
    gStartup.State = error == StartupError::DeviceLost ? StartupState::DeviceLost : StartupState::Failed;
    gStartup.Error = error;
    gStartup.Capabilities = {};
    gProfile = {};
    if (error != StartupError::RequirementsUnsatisfied)
    {
        gStartup.UnmetRequirement = RequirementFailure::None;
        gStartup.ProfileFailure = {};
    }
    gFrame = false;
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
    Fail(gToken, StartupError::RenderingUnavailable);
}
void Complete(uint32 token, StartupError error, const BackendLimits& limits) noexcept
{
    if (Current(token) == false || gStartup.State != StartupState::Pending)
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
    gStartup.UnmetRequirement =
        gStartup.Requirements.MinFrameDimension2D > maxTextureDimension ? RequirementFailure::FrameDimension2D
        : gStartup.Requirements.MinUniformBufferSize > uniformSize      ? RequirementFailure::UniformBufferSize
                                                                        : RequirementFailure::None;
    if (gStartup.UnmetRequirement != RequirementFailure::None)
    {
        Fail(token, StartupError::RequirementsUnsatisfied);
        return;
    }
    const auto profile = QueryDeviceProfile();
    gStartup.ProfileFailure = CheckProfile(gRequiredProfile, profile);
    if (gStartup.ProfileFailure.Requirement != ProfileRequirement::None)
    {
        gStartup.UnmetRequirement = RequirementFailure::Profile;
        Fail(token, StartupError::RequirementsUnsatisfied);
        return;
    }
    if (gRequireRaster == true &&
        (profile.Raster.MaxUniformRange < PORTABLE_UNIFORM_BYTES || profile.Raster.DrawsPerFrame == 0))
    {
        gStartup.UnmetRequirement = RequirementFailure::PortableRaster;
        Fail(token, StartupError::RequirementsUnsatisfied);
        return;
    }
    if (gRequireCompute && backend::ComputeLimits().MaxStorageRange == 0)
    {
        gStartup.UnmetRequirement = RequirementFailure::Compute;
        Fail(token, StartupError::RequirementsUnsatisfied);
        return;
    }
    gProfile = profile;
    RecordAttempt(token, gStartup.SelectedBackend, StartupError::None);
    gStartup.MaxTextureDimension2D = maxTextureDimension;
    gStartup.Capabilities =
    {
        .MaxFrameDimension2D = maxTextureDimension,
        .MaxUniformBufferSize = uniformSize,
        .UniformBufferSizeAlignment = 16,
        .MaxShaders = static_cast<uint32>(RESOURCE_CAPACITY),
        .MaxUniformBuffers = static_cast<uint32>(RESOURCE_CAPACITY),
        .MaxPipelines = static_cast<uint32>(RESOURCE_CAPACITY),
        .MaxDrawsPerFrame = 1,
    };
    gStartup.State = StartupState::Ready;
}
void SelectBackend(uint32 token, Backend backend) noexcept
{
    if (Current(token) == false)
    {
        return;
    }
    gStartup.SelectedBackend = backend;
    gStartup.ProfileFailure = {};
    gStartup.UnmetRequirement = RequirementFailure::None;
}
uint32 Reissue(uint32 token) noexcept
{
    if (Current(token) == false || gStartup.State != StartupState::Pending)
    {
        return 0;
    }
    if (gNextToken == 0)
    {
        return 0;
    }
    gToken = gNextToken++;
    return gToken;
}
void RecordAttempt(uint32 token, Backend backend, StartupError error) noexcept
{
    if (Current(token) == false)
    {
        return;
    }
    AttemptInfo* attempt = backend == Backend::WebGL2   ? &gStartup.WebGL2
                           : backend == Backend::WebGPU ? &gStartup.WebGpu
                                                        : nullptr;
    if (attempt != nullptr)
    {
        attempt->Attempted = true;
        attempt->Error = error;
        attempt->ProfileFailure = gStartup.ProfileFailure;
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
        gStartup = {};
        gStartup.State = StartupState::Failed;
        gStartup.Error = StartupError::DeviceUnavailable;
        return StartStatus::Failed;
    }
    gStartup.Requested = selection;
    gStartup.Requirements = requirements;
    gRequireRaster = requireRaster;
    gRequireCompute = requireCompute;
    gRequiredProfile = profile;
    gProfile = {};
    // A forced selection the build cannot provide fails explicitly; it never
    // silently switches to another backend.
    if (backend::Supports(selection) == false)
    {
        gStartup.State = StartupState::Failed;
        gStartup.Error = StartupError::BackendUnavailable;
        return StartStatus::Failed;
    }
    if (gNextToken == 0)
    {
        gStartup.State = StartupState::Failed;
        gStartup.Error = StartupError::GenerationExhausted;
        return StartStatus::Failed;
    }
    gStartup.Requested = selection;
    gStartup.SelectedBackend = backend::Kind();
    gToken = gNextToken++;
    gStartup.State = StartupState::Pending;
    const auto error = backend::Start(app, window, gToken, selection);
    if (error != StartupError::None)
    {
        internal::Fail(gToken, error);
    }
    if (gStartup.State == StartupState::Ready)
    {
        return StartStatus::Ready;
    }
    return gStartup.State == StartupState::Pending ? StartStatus::Pending : StartStatus::Failed;
}
} // namespace
namespace internal
{
bool RequiresComputeProfile() noexcept
{
    return gStartup.State == StartupState::Pending && gRequireCompute;
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
    return StartSelected(app, window, BackendSelection::Auto, {});
}
StartStatus Start(const ApplicationInfo& app, const WindowInfo& window, BackendSelection selection) noexcept
{
    return StartSelected(app, window, selection, {});
}
StartStatus Start(const ApplicationInfo& app,
                  const WindowInfo& window,
                  BackendSelection selection,
                  const DeviceRequirements& requirements) noexcept
{
    return StartSelected(app, window, selection, requirements);
}
StartupInfo GetStartup() noexcept
{
    return gStartup;
}
FrameStatus SetFrameTarget(const FrameTarget& target) noexcept
{
    if (gStartup.State != StartupState::Ready)
    {
        return FrameStatus::NotReady;
    }
    if (gFrame)
    {
        return FrameStatus::InvalidState;
    }
    if (target.Width > gStartup.Capabilities.MaxFrameDimension2D ||
        target.Height > gStartup.Capabilities.MaxFrameDimension2D)
    {
        return FrameStatus::Failed;
    }
    return backend::SetTarget(target);
}
FrameStatus BeginFrameStatus() noexcept
{
    if (gStartup.State != StartupState::Ready)
    {
        return FrameStatus::NotReady;
    }
    if (gFrame)
    {
        return FrameStatus::InvalidState;
    }
    if (internal::PrepareRasterFrame() == false)
    {
        return FrameStatus::Failed;
    }
    const auto result = backend::Begin();
    gFrame = result == FrameStatus::Ready;
    gDrawn = false;
    gRasterDrawn = false;
    internal::RasterBeginFrame();
    return result;
}
FrameStatus EndFrameStatus() noexcept
{
    if (gStartup.State != StartupState::Ready)
    {
        return FrameStatus::NotReady;
    }
    if (gFrame == false)
    {
        return FrameStatus::InvalidState;
    }
    if (internal::RasterFinishFrame() == false)
    {
        return FrameStatus::Failed;
    }
    gFrame = false;
    const auto result = backend::End();
    internal::RasterEndFrame(result == FrameStatus::Ready || result == FrameStatus::Skipped);
    return result;
}
bool Initialize(const ApplicationInfo& app) noexcept
{
    if (internal::NativeInteropBusy())
    {
        return false;
    }
    if (gStartup.State != StartupState::Idle)
    {
        return false;
    }
    gLegacy = backend::Initialize(app);
    return gLegacy;
}
bool ConnectWindow(const WindowInfo& window) noexcept
{
    return gLegacy && backend::ConnectWindow(window);
}
bool InitializeRendering() noexcept
{
    if (gLegacy == false)
    {
        return false;
    }
    if (gRendering == false)
    {
        gRendering = backend::InitializeRendering();
    }
    return gRendering;
}
void ShutdownRendering() noexcept
{
    if (gLegacy && gRendering)
    {
        backend::ShutdownRendering();
    }
    gRendering = false;
}
void Shutdown() noexcept
{
    gDeviceOwner = 0;
    internal::SetGpuTimingRequest(false);
    gRayProfileRequest = false;
    internal::SetBindlessRequest(false);
    internal::SetRuntimeTextureArrayRequest(false);
    gFallback = nullptr;
    gToken = 0;
    if (internal::DeferNativeInteropShutdown())
    {
        backend::NativeInteropDetachSurface();
    }
    else
    {
        internal::FinishNativeInteropShutdown();
    }
    gStartup = {};
    gLegacy = false;
    gRendering = false;
    gFrame = false;
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
bool BeginFrame() noexcept
{
    return gLegacy ? (gRendering && backend::BeginFrame()) : BeginFrameStatus() == FrameStatus::Ready;
}
bool EndFrame() noexcept
{
    return gLegacy ? (gRendering && backend::EndFrame()) : EndFrameStatus() == FrameStatus::Ready;
}

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
Resource gShaders[internal::RESOURCE_CAPACITY];
Resource gUniforms[internal::RESOURCE_CAPACITY];
Resource gPipelines[internal::RESOURCE_CAPACITY];
uint32 gNextResource = 1;
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
    if (gStartup.State != StartupState::Ready)
    {
        return ResourceStatus::NotReady;
    }
    if (gFrame)
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
    if (resource.Id == 0 || gStartup.State != StartupState::Ready)
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
    if (gStartup.State != StartupState::Ready)
    {
        return;
    }
    const auto shader = Find(gShaders, id);
    const auto uniform = Find(gUniforms, id);
    const auto pipeline = Find(gPipelines, id);
    Resource* resource = nullptr;
    if (shader != MISSING)
    {
        resource = &gShaders[shader];
    }
    if (uniform != MISSING)
    {
        resource = &gUniforms[uniform];
    }
    if (pipeline != MISSING)
    {
        resource = &gPipelines[pipeline];
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
    for (auto& resource : gPipelines)
    {
        resource.Id = 0;
    }
    for (auto& resource : gShaders)
    {
        resource.Id = 0;
    }
    for (auto& resource : gUniforms)
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
ResourceStatus GetStatus(ShaderHandle handle) noexcept
{
    return Status(gShaders, ResourceAccess::Id(handle));
}
ResourceStatus GetStatus(UniformHandle handle) noexcept
{
    return Status(gUniforms, ResourceAccess::Id(handle));
}
ResourceStatus GetStatus(PipelineHandle handle) noexcept
{
    return Status(gPipelines, ResourceAccess::Id(handle));
}
ResourceStatus CreateShader(const ShaderDescription& description, ShaderHandle& handle) noexcept
{
    if (ResourceAccess::Id(handle) != 0)
    {
        return ResourceStatus::InvalidState;
    }
    if (description.UniformSize > internal::UNIFORM_CAPACITY)
    {
        return ResourceStatus::InvalidDescription;
    }
    if (gStartup.State == StartupState::Ready && description.UniformSize > gStartup.Capabilities.MaxUniformBufferSize)
    {
        return ResourceStatus::InvalidDescription;
    }
    const auto kind = backend::Kind();
    // The backend later checks the DXIL program kind and entry inside the container.
    const bool validArtifact =
        kind == Backend::Vulkan   ? (description.Spirv.size() >= 5 && description.Spirv[0] == 0x07230203U &&
                                     ValidEntry(description.SpirvEntry))
        : kind == Backend::WebGL2 ? (description.GlslEs.empty() == false && description.GlslEsEntry == "main")
        : kind == Backend::Metal  ? (description.Msl.empty() == false && ValidEntry(description.MslEntry))
        : kind == Backend::Direct3D12
            ? (internal::IsDxilContainer(description.Dxil) == true && ValidEntry(description.DxilEntry))
            : (description.Wgsl.empty() == false && ValidEntry(description.WgslEntry));
    if ((description.Stage != ShaderStage::Vertex && description.Stage != ShaderStage::Fragment) ||
        validArtifact == false)
    {
        return ResourceStatus::InvalidDescription;
    }
    usize slot = 0;
    const auto admission = Allocate(gShaders, slot);
    if (admission != ResourceStatus::Ready)
    {
        return admission;
    }
    auto& resource = gShaders[slot];
    resource.Stage = description.Stage;
    resource.Size = description.UniformSize;
    ResourceAccess::Set(handle, resource.Id);
    return Finish(resource, backend::CreateShader(slot, description, resource.Id));
}
ResourceStatus CreateUniform(usize size, UniformHandle& handle) noexcept
{
    if (ResourceAccess::Id(handle) != 0)
    {
        return ResourceStatus::InvalidState;
    }
    if (size < 16 || size > internal::UNIFORM_CAPACITY || size % 16 != 0)
    {
        return ResourceStatus::InvalidDescription;
    }
    if (gStartup.State == StartupState::Ready && size > gStartup.Capabilities.MaxUniformBufferSize)
    {
        return ResourceStatus::InvalidDescription;
    }
    usize slot = 0;
    const auto admission = Allocate(gUniforms, slot);
    if (admission != ResourceStatus::Ready)
    {
        return admission;
    }
    auto& resource = gUniforms[slot];
    resource.Size = size;
    ResourceAccess::Set(handle, resource.Id);
    return Finish(resource, backend::CreateUniform(slot, { .Size = size }, resource.Id));
}
ResourceStatus CreatePipeline(const PipelineDescription& description, PipelineHandle& handle) noexcept
{
    if (ResourceAccess::Id(handle) != 0)
    {
        return ResourceStatus::InvalidState;
    }
    const auto vertex = Find(gShaders, ResourceAccess::Id(description.Vertex));
    const auto fragment = Find(gShaders, ResourceAccess::Id(description.Fragment));
    const auto uniform = Find(gUniforms, ResourceAccess::Id(description.Uniform));
    if (vertex == MISSING || fragment == MISSING || uniform == MISSING)
    {
        return ResourceStatus::InvalidHandle;
    }
    if (gShaders[vertex].Stage != ShaderStage::Vertex || gShaders[fragment].Stage != ShaderStage::Fragment)
    {
        return ResourceStatus::InvalidDescription;
    }
    for (auto status : {gShaders[vertex].Status, gShaders[fragment].Status, gUniforms[uniform].Status})
    {
        if (status == ResourceStatus::Pending)
        {
            // Pending is reserved for a creation that owns its output handle.
            return ResourceStatus::NotReady;
        }
        if (status != ResourceStatus::Ready)
        {
            return status;
        }
    }
    if (gUniforms[uniform].Size < gShaders[vertex].Size || gUniforms[uniform].Size < gShaders[fragment].Size)
    {
        return ResourceStatus::InvalidDescription;
    }
    usize slot = 0;
    const auto admission = Allocate(gPipelines, slot);
    if (admission != ResourceStatus::Ready)
    {
        return admission;
    }
    auto& resource = gPipelines[slot];
    resource.Vertex = vertex;
    resource.Fragment = fragment;
    resource.Uniform = uniform;
    ResourceAccess::Set(handle, resource.Id);
    return Finish(
        resource,
        backend::CreatePipeline(slot, { .Vertex = vertex, .Fragment = fragment, .Uniform = uniform }, resource.Id));
}
FrameInfo GetFrameInfo() noexcept
{
    return gFrame && gStartup.State == StartupState::Ready ? backend::GetFrameInfo() : FrameInfo{};
}
ResourceStatus UpdateUniform(UniformHandle handle, std::span<const foundation::uint8> bytes) noexcept
{
    const auto slot = Find(gUniforms, ResourceAccess::Id(handle));
    if (slot == MISSING)
    {
        return ResourceStatus::InvalidHandle;
    }
    if (gFrame && gDrawn)
    {
        return ResourceStatus::InvalidState;
    }
    if (gUniforms[slot].Status != ResourceStatus::Ready)
    {
        return gUniforms[slot].Status;
    }
    if (bytes.size() != gUniforms[slot].Size)
    {
        return ResourceStatus::InvalidDescription;
    }
    backend::UpdateUniform(slot, bytes);
    gUniforms[slot].Updated = true;
    return ResourceStatus::Ready;
}
ResourceStatus Destroy(PipelineHandle handle) noexcept
{
    const auto slot = Find(gPipelines, ResourceAccess::Id(handle));
    if (slot == MISSING)
    {
        return ResourceStatus::InvalidHandle;
    }
    if (gFrame)
    {
        return ResourceStatus::InvalidState;
    }
    gPipelines[slot].Id = 0;
    backend::DestroyPipeline(slot);
    return ResourceStatus::Ready;
}
ResourceStatus Destroy(ShaderHandle handle) noexcept
{
    const auto slot = Find(gShaders, ResourceAccess::Id(handle));
    if (slot == MISSING)
    {
        return ResourceStatus::InvalidHandle;
    }
    if (gFrame)
    {
        return ResourceStatus::InvalidState;
    }
    for (const auto& pipeline : gPipelines)
    {
        if (pipeline.Id != 0 && pipeline.Status != ResourceStatus::Failed &&
            (pipeline.Vertex == slot || pipeline.Fragment == slot))
        {
            return ResourceStatus::InUse;
        }
    }
    gShaders[slot].Id = 0;
    backend::DestroyShader(slot);
    return ResourceStatus::Ready;
}
ResourceStatus Destroy(UniformHandle handle) noexcept
{
    const auto slot = Find(gUniforms, ResourceAccess::Id(handle));
    if (slot == MISSING)
    {
        return ResourceStatus::InvalidHandle;
    }
    if (gFrame)
    {
        return ResourceStatus::InvalidState;
    }
    for (const auto& pipeline : gPipelines)
    {
        if (pipeline.Id != 0 && pipeline.Status != ResourceStatus::Failed && pipeline.Uniform == slot)
        {
            return ResourceStatus::InUse;
        }
    }
    gUniforms[slot].Id = 0;
    backend::DestroyUniform(slot);
    return ResourceStatus::Ready;
}
ResourceStatus DrawFullscreen(PipelineHandle handle) noexcept
{
    const auto slot = Find(gPipelines, ResourceAccess::Id(handle));
    if (slot == MISSING)
    {
        return ResourceStatus::InvalidHandle;
    }
    if (gPipelines[slot].Status != ResourceStatus::Ready)
    {
        return gPipelines[slot].Status;
    }
    if (gFrame == false || gDrawn || gRasterDrawn || gUniforms[gPipelines[slot].Uniform].Updated == false)
    {
        return ResourceStatus::InvalidState;
    }
    const auto result = backend::Draw(slot);
    gDrawn = result == ResourceStatus::Ready;
    return result;
}
} // namespace ludus::graphics::rhi
