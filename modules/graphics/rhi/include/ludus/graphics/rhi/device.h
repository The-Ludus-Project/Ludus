#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/profile.h>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>

#include <span>

namespace ludus::graphics::rhi
{
namespace internal
{
struct DeviceAccess;
}

/// Identity of the single explicitly owned device session; never a backend pointer.
/// Copying does not transfer ownership. Main-thread use only; destroy exactly once.
/// The owner identity never wraps or becomes valid again after destruction/restart.
struct DeviceHandle final
{
private:
    ludus::foundation::uint64 Owner = 0;
    /// Grants private dispatch access to assign and validate the owner identity.
    friend struct internal::DeviceAccess;
};

/// Borrowed frame target belonging to one device session, including a headless target.
/// The Platform window/canvas must outlive the device. Destruction/loss invalidates
/// target operations; copying the handle does not extend its lifetime.
struct SurfaceHandle final
{
private:
    ludus::foundation::uint64 Owner = 0;
    /// Grants private dispatch access to assign and validate the borrowed owner identity.
    friend struct internal::DeviceAccess;
};

/// Implemented workload capabilities, independently queryable from a backend name.
/// These describe engine operations, not raw adapter support for future APIs.
struct DeviceFeatures final
{
    /// The existing one-triangle fullscreen shader/uniform/pipeline path.
    bool FullscreenRaster = false;
    /// The bounded buffer/texture/binding and indexed/instanced profile in raster.h.
    bool PortableRaster = false;
    /// Bounded single-queue buffer compute operations (R4); unavailable on WebGL2.
    bool Compute = false;
    /// One indexed indirect draw per packet (R4); indirect dispatch is deferred.
    bool IndirectRendering = false;
    /// Optional static native triangle structures and inline queries (RT0).
    /// Vulkan negotiates this only when Required/Preferred explicitly requests it.
    bool RayQueries = false;
    /// Ordered portable raster graph, version/hazard validation and offscreen passes (R3).
    bool OrderedRasterGraph = false;
    /// Optional non-uniform fixed texture descriptor indexing (R11), separately
    /// requested through Required/Preferred; never implied by binding arrays.
    bool BindlessTextureIndexing = false;
    /// Optional native unsized sampled texture descriptor arrays (R11c), requested
    /// independently of non-uniform selection. Vulkan 1.2 only in this profile.
    bool HasRuntimeTextureArrays = false;
};

/// Backend code compiled into this artifact; does not promise adapter/runtime availability.
struct BackendAvailability final
{
    /// Native Linux Vulkan implementation.
    bool Vulkan = false;
    /// Native macOS Metal implementation.
    bool Metal = false;
    /// Browser WebGPU implementation.
    bool WebGPU = false;
    /// Browser WebGL 2 fallback implementation.
    bool WebGL2 = false;
};

/// Copied device admission requirements. Unsupported required workloads fail before startup.
/// Preferences never cause failure or enable unimplemented operations. No pins are upgraded.
struct DeviceDescription final
{
    /// Native accepts Auto; browser supports Auto or a forced browser backend.
    BackendSelection Selection = BackendSelection::Auto;
    /// Minimum effective fullscreen limits; checked on each backend attempt.
    DeviceRequirements Limits;
    /// Required workloads; unimplemented operations reject before startup, while the
    /// portable raster profile is verified on each negotiated backend before Ready.
    DeviceFeatures Required{ .FullscreenRaster = true };
    /// Optional workloads; inspect Enabled after Ready rather than assuming availability.
    DeviceFeatures Preferred;
    /// R6 general resource/plan maxima and memory budgets, copied before startup.
    /// Growth is owner-thread only between frames; enabled maxima stay immutable until shutdown.
    StorageLimits Storage;
    /// Request optional GPU frame timing (R7); absent backend support never fails admission.
    bool EnableGpuTiming = false;
    /// Copied profile constraints, checked on every enabled backend attempt before Ready.
    /// Nonzero raster/compute constraints imply admission of that workload.
    DeviceProfileRequirements RequiredProfile;
    /// Optional constraints; unmet values do not prevent Ready or elevate backend limits.
    DeviceProfileRequirements PreferredProfile;
};

/// Result of an explicit device operation; each operation documents output preservation.
enum class DeviceStatus : ludus::foundation::uint8
{
    /// Operation accepted, or device ready for work.
    Ready,
    /// Owned startup is in flight; poll GetDeviceInfo without blocking.
    Pending,
    /// Zero-size target skipped acquisition and did not open a frame.
    Skipped,
    /// A frame operation was attempted before startup became ready.
    NotReady,
    /// A null, destroyed or foreign owner/target was supplied.
    InvalidHandle,
    /// Busy session, occupied creation output or invalid frame ordering.
    InvalidState,
    /// Malformed backend selection enum.
    InvalidDescription,
    /// Unimplemented required workload or backend operation.
    Unsupported,
    /// Backend startup/frame operation failed; inspect Startup in DeviceInfo.
    Failed,
    /// Device loss stopped work; destroy and create a fresh session to recover.
    DeviceLost,
    /// Process device-owner identities exhausted; no backend request was launched.
    IdentityExhausted,
};

/// Snapshot of one owned session, including its copied admission request.
struct DeviceInfo final
{
    /// Build availability, independent of the chosen backend's enabled operations.
    BackendAvailability Compiled;
    /// Existing startup state, effective limits, backend selection and attempt diagnostics.
    StartupInfo Startup;
    /// Copied request; no caller description storage is retained.
    DeviceDescription Description;
    /// Implemented support for the selected ready backend; zero before Ready or after loss.
    DeviceFeatures Supported;
    /// Operations available to this session; zero before Ready or after loss.
    DeviceFeatures Enabled;
    /// Immutable enabled limit/format snapshot; zero before Ready and after failure/loss.
    DeviceProfile Profile;
    /// Preferred workloads absent from Enabled; populated only while Ready.
    DeviceFeatures UnmetPreferences;
    /// First unmet preferred profile constraint; populated only while Ready.
    ProfileDiagnostic UnmetProfilePreference;
    /// True only during this ready owner's acquired frame. Diagnostic/read-only;
    /// mutation APIs remain authoritative about their between-frame restrictions.
    bool FrameOpen = false;
};

/// Reference admission validator shared by every backend; performs no allocation or GPU work.
/// @return Ready for implemented required workloads, Unsupported for missing workloads,
/// InvalidDescription for malformed selection, format count/identity, duplicate format, sample count,
/// zero/reserved storage counts or zero byte budgets.
/// Actual limits and format uses are checked on each backend attempt before Ready;
/// no feature or limit is inferred from a backend name.
[[nodiscard]] DeviceStatus ValidateDeviceDescription(const DeviceDescription& description) noexcept;
/// Query backend build availability without starting a device or requesting an adapter.
[[nodiscard]] BackendAvailability GetCompiledBackends() noexcept;

/// Start the single explicit device and its frame target on the main thread.
/// @pre Both outputs are null; the borrowed Platform window/canvas outlives DestroyDevice.
/// @return Ready/Pending publishes both handles and requires DestroyDevice, including
/// subsequent failure/loss. Admission or synchronous startup failure preserves null outputs.
/// Duplicate creation or an existing facade session returns InvalidState without changing it.
/// Descriptions are copied; backend artifact inputs continue to use their existing contracts.
/// @param application Startup application metadata, consumed during the call.
/// @param window Borrowed Platform target; must outlive the owned device.
/// @param description Copied backend/workload/limit admission request.
/// @param device Null output receiving the single owned lifecycle record on Ready/Pending.
/// @param surface Null output receiving the target borrowed from that owned session.
/// @param failure Optional synchronous-startup diagnostic output, written only when startup
/// fails; never retained. Other return paths preserve this output.
[[nodiscard]] DeviceStatus CreateDevice(const ApplicationInfo& application,
                                        const WindowInfo& window,
                                        const DeviceDescription& description,
                                        DeviceHandle& device,
                                        SurfaceHandle& surface,
                                        StartupInfo* failure = nullptr) noexcept;

/// Poll an owned startup/loss record without waiting. InvalidHandle preserves info.
/// Ready/Pending/Failed/DeviceLost writes the snapshot; Failed/Lost still require destruction.
[[nodiscard]] DeviceStatus GetDeviceInfo(DeviceHandle device, DeviceInfo& info) noexcept;

/// Cancel pending startup or destroy a ready/failed/lost session; invalidates callback tokens
/// before backend teardown. Clears device on success; borrowed surface copies become stale.
/// InvalidHandle preserves device and cannot destroy another owner's session. Never waits for
/// browser callbacks. Native interop leases defer physical teardown until foreign
/// consumers complete; new device creation rejects InvalidState until all pins retire.
[[nodiscard]] DeviceStatus DestroyDevice(DeviceHandle& device) noexcept;

/// Set a borrowed target between frames; dimensions are pixels and zero skips acquisition.
/// Validates both owners before delegating to the facade's effective-limit checks.
[[nodiscard]] DeviceStatus
SetFrameTarget(DeviceHandle device, SurfaceHandle surface, const FrameTarget& target) noexcept;
/// Acquire a frame for this owner/target. Skipped opens no frame; nested acquisition is invalid.
[[nodiscard]] DeviceStatus BeginFrame(DeviceHandle device, SurfaceHandle surface) noexcept;
/// Submit/end this owner's open frame once; fails explicitly after device loss.
[[nodiscard]] DeviceStatus EndFrame(DeviceHandle device) noexcept;
/// Query acquired dimensions/encoding; offscreen-only frames report their logical target extent.
/// Invalid owner, pending startup or closed frame
/// preserves info. This is not a presentation-completion query.
[[nodiscard]] DeviceStatus GetFrameInfo(DeviceHandle device, SurfaceHandle surface, FrameInfo& info) noexcept;

/// Create a facade shader after validating device ownership; output/async artifact contracts
/// match CreateShader without a device parameter. Invalid owner returns InvalidHandle.
[[nodiscard]] ResourceStatus
CreateShader(DeviceHandle device, const ShaderDescription& description, ShaderHandle& shader) noexcept;
/// Create a facade uniform; size is bytes. Validates owner before applying existing admission rules.
[[nodiscard]] ResourceStatus
CreateUniform(DeviceHandle device, ludus::foundation::usize size, UniformHandle& uniform) noexcept;
/// Create a facade pipeline; all dependencies must resolve in this owner's shared registry.
[[nodiscard]] ResourceStatus
CreatePipeline(DeviceHandle device, const PipelineDescription& description, PipelineHandle& pipeline) noexcept;
/// Poll a shader in this owner; stale/foreign resource identities return InvalidHandle.
[[nodiscard]] ResourceStatus GetStatus(DeviceHandle device, ShaderHandle shader) noexcept;
/// Poll a uniform in this owner; stale/foreign resource identities return InvalidHandle.
[[nodiscard]] ResourceStatus GetStatus(DeviceHandle device, UniformHandle uniform) noexcept;
/// Poll a pipeline in this owner; stale/foreign resource identities return InvalidHandle.
[[nodiscard]] ResourceStatus GetStatus(DeviceHandle device, PipelineHandle pipeline) noexcept;
/// Replace uniform bytes under the existing frame/order/size rules; no ownership transfer.
[[nodiscard]] ResourceStatus
UpdateUniform(DeviceHandle device, UniformHandle uniform, std::span<const ludus::foundation::uint8> bytes) noexcept;
/// Destroy a shader between frames. Existing pipeline dependencies still return InUse.
[[nodiscard]] ResourceStatus Destroy(DeviceHandle device, ShaderHandle shader) noexcept;
/// Destroy a uniform between frames. Existing pipeline dependencies still return InUse.
[[nodiscard]] ResourceStatus Destroy(DeviceHandle device, UniformHandle uniform) noexcept;
/// Destroy a pipeline between frames, using the shared facade registry and retirement behavior.
[[nodiscard]] ResourceStatus Destroy(DeviceHandle device, PipelineHandle pipeline) noexcept;
/// Draw through the existing fullscreen path; validates the owner before issuing any work.
[[nodiscard]] ResourceStatus DrawFullscreen(DeviceHandle device, PipelineHandle pipeline) noexcept;
} // namespace ludus::graphics::rhi
