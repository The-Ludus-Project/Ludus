#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>

#include <ludus/graphics/rhi/adapter.h>
#include <ludus/graphics/rhi/profile.h>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>

namespace ludus::graphics::rhi
{
namespace internal
{
struct DeviceAccess;
}

/// Identity of one explicitly owned device session; never a backend pointer.
/// Copying does not transfer ownership. Main-thread use only; destroy exactly once.
/// The owner identity never wraps or becomes valid again after destruction/restart.
/// A backend that hosts several owners (R14c) gives each its own registries, completion
/// ordinals and loss state; handles of one owner are rejected by every other owner.
struct DeviceHandle final
{
    /// Compare session identities only, without querying readiness or retaining ownership.
    [[nodiscard]] LUDUS_INLINE bool operator==(const DeviceHandle&) const noexcept = default;

private:
    ludus::foundation::uint64 Owner = 0;
    /// Grants private dispatch access to assign and validate the owner identity.
    friend struct internal::DeviceAccess;
};

/// Borrowed frame target belonging to one device session, including a headless target.
/// The Platform window/canvas must outlive the surface. Destruction/loss invalidates
/// target operations; copying the handle does not extend its lifetime. Each surface of a
/// device has its own identity (R14b), never reused after DestroySurface or DestroyDevice.
struct SurfaceHandle final
{
    /// Compare surface identities only, without querying the surface or its device.
    [[nodiscard]] LUDUS_INLINE bool operator==(const SurfaceHandle&) const noexcept = default;

private:
    ludus::foundation::uint64 Owner = 0;
    ludus::foundation::uint64 mIdentity = 0;
    /// Grants private dispatch access to assign and validate the borrowed owner identity.
    friend struct internal::DeviceAccess;
};

/// Maximum surfaces one device hosts at once, including the primary surface CreateDevice
/// publishes (R14b). A backend may host fewer; see CreateSurface.
inline constexpr ludus::foundation::usize MAX_DEVICE_SURFACES = 4;

/// @brief Maximum device owners one process hosts at once (R14c).
///
/// A backend may host fewer; see CreateDevice. Owners share the owner thread but never
/// share registries, frames, completion ordinals, loss or retirement.
inline constexpr ludus::foundation::usize MAX_DEVICE_OWNERS = 4;

/// Presentation state of one surface; independent of the device's startup state.
enum class SurfaceState : ludus::foundation::uint8
{
    /// The surface acquires and presents frames.
    Ready,
    /// The platform target stopped accepting frames (for example its view left its window).
    /// Its presentation resources are retired; the device and other surfaces keep working.
    /// Destroy the surface and create a replacement to recover.
    Lost,
};

/// @brief Snapshot of one surface's target and completion counters (R14b).
///
/// Counters start at zero when the surface is created, count only this surface's frames
/// and never decrease. Render completion and presentation are reported separately: a
/// frame can be presented before or after its rendering is observed complete.
struct SurfaceInfo final
{
    /// Presentation state; Lost surfaces reject frames with DeviceStatus::SurfaceLost.
    SurfaceState State = SurfaceState::Ready;
    /// True for the surface CreateDevice published; it is retired only by DestroyDevice.
    bool IsPrimary = false;
    /// Window system of the borrowed target.
    ludus::platform::WindowSystem System = ludus::platform::WindowSystem::Headless;
    /// Last accepted frame target width in pixels; zero skips acquisition.
    ludus::foundation::uint32 Width = 0;
    /// Last accepted frame target height in pixels; zero skips acquisition.
    ludus::foundation::uint32 Height = 0;
    /// Frames this surface acquired and submitted with EndFrame.
    ludus::foundation::uint64 SubmittedFrames = 0;
    /// Submitted frames whose GPU work the owner thread has observed complete. Observation
    /// is polled without waiting, so it can lag the GPU.
    ludus::foundation::uint64 RenderedFrames = 0;
    /// Submitted frames handed to the platform's presentation queue or hosted consumer.
    /// Headless surfaces present nothing, so this stays zero. It is not a display time.
    ludus::foundation::uint64 PresentedFrames = 0;
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
    /// GPU-written indirect draws (R4) and the R12a profile reported by
    /// DeviceProfile::Indirect: non-indexed draws, offsets, command counts and indirect dispatch.
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
    /// Native Windows Direct3D 12 implementation (clear/present bring-up).
    bool HasDirect3D12 = false;
};

/// Copied device admission requirements. Unsupported required workloads fail before startup.
/// Preferences never cause failure or enable unimplemented operations. No pins are upgraded.
struct DeviceDescription final
{
    /// Native accepts Auto; browser supports Auto or a forced browser backend.
    BackendSelection Selection = BackendSelection::Auto;
    /// Native adapter to start on, from GetAdapterCatalog (R14a). All-zero bytes keep the
    /// automatic choice. A foreign backend returns InvalidDescription, browser builds
    /// Unsupported, and an identity absent from the catalog or below its backend baseline
    /// fails with StartupError::AdapterUnavailable before any device is published.
    AdapterIdentity Adapter{};
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
    /// The surface was lost; the device stays usable. Destroy and recreate the surface.
    SurfaceLost,
    /// The device already hosts as many surfaces as its backend allows, or the process
    /// already hosts as many device owners as its backend allows (R14c).
    CapacityExceeded,
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
    /// Raw catalog entry of the adapter this native session runs on, separate from the
    /// enabled Profile; populated only while Ready, and zero on browser backends.
    AdapterInfo Adapter{};
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

/// Start an explicit device and its frame target on the main thread.
/// @pre Both outputs are null; the borrowed Platform window/canvas outlives DestroyDevice.
/// @return Ready/Pending publishes both handles and requires DestroyDevice, including
/// subsequent failure/loss. Admission or synchronous startup failure preserves null outputs.
/// An occupied output, a facade session started with Start or Initialize, or native
/// interop teardown still pending returns InvalidState without changing them. A target
/// that a live surface of any device already borrows returns InvalidDescription.
/// Descriptions are copied; backend artifact inputs continue to use their existing contracts.
/// @note Metal, Vulkan, Direct3D 12, WebGPU and WebGL 2 host up to MAX_DEVICE_OWNERS
/// concurrent owners (R14c); each owner has its own device and queue (a WebGL 2 owner,
/// its own context), and a native owner may select its own catalog adapter. When they
/// are all live, CreateDevice returns CapacityExceeded.
/// Vulkan owners share one VkInstance, described by the first owner's application
/// metadata; a Vulkan device adopted by CreateNativeDevice is hosted only alone, so
/// CreateDevice returns InvalidState while it lives. In the browser each owner binds its
/// own sub-backend and canvas; a selector that names a canvas another owner presents to,
/// even through a different string, fails startup with StartupError::InvalidWindow.
/// Device-less facade calls such as Start, BeginFrameStatus and CreateShader
/// address the owner in the first slot, which is the device created while no other
/// device was live, so a single-device program behaves as before.
/// @param application Startup application metadata, consumed during the call.
/// @param window Borrowed Platform target; must outlive the owned device.
/// @param description Copied backend/workload/limit admission request.
/// @param outDevice Null output receiving the owned lifecycle record on Ready/Pending.
/// @param outSurface Null output receiving the target borrowed from that owned session.
/// @param failureOrNull Optional synchronous-startup diagnostic output, written only when
/// startup fails; never retained. Other return paths preserve this output.
[[nodiscard]] DeviceStatus CreateDevice(const ApplicationInfo& application,
                                        const WindowInfo& window,
                                        const DeviceDescription& description,
                                        DeviceHandle& outDevice,
                                        SurfaceHandle& outSurface,
                                        StartupInfo* failureOrNull = nullptr) noexcept;

/// Poll an owned startup/loss record without waiting. InvalidHandle preserves info.
/// Ready/Pending/Failed/DeviceLost writes the snapshot; Failed/Lost still require destruction.
[[nodiscard]] DeviceStatus GetDeviceInfo(DeviceHandle device, DeviceInfo& info) noexcept;

/// Cancel pending startup or destroy a ready/failed/lost session; invalidates callback tokens
/// before backend teardown. Clears device on success; borrowed surface copies become stale.
/// InvalidHandle preserves device and cannot destroy another owner's session. Other owners'
/// resources, frames and completion state are unaffected. Never waits for
/// browser callbacks. Native interop leases defer physical teardown until foreign
/// consumers complete; new device creation rejects InvalidState until all pins retire.
[[nodiscard]] DeviceStatus DestroyDevice(DeviceHandle& device) noexcept;

/// Set a borrowed target between frames; dimensions are pixels and zero skips acquisition.
/// Validates both owners before delegating to the facade's effective-limit checks. Each
/// surface keeps its own target (R14b); a Lost surface returns SurfaceLost.
[[nodiscard]] DeviceStatus
SetFrameTarget(DeviceHandle device, SurfaceHandle surface, const FrameTarget& target) noexcept;
/// Acquire a frame for this owner/target. Skipped opens no frame; nested acquisition is invalid,
/// including on another surface of the device. SurfaceLost reports a lost created surface
/// (R14b) without affecting the device; the primary surface keeps the facade loss contract.
[[nodiscard]] DeviceStatus BeginFrame(DeviceHandle device, SurfaceHandle surface) noexcept;
/// Submit/end this owner's open frame once, on the surface BeginFrame acquired; fails
/// explicitly after device loss.
[[nodiscard]] DeviceStatus EndFrame(DeviceHandle device) noexcept;
/// Query acquired dimensions/encoding; offscreen-only frames report their logical target extent.
/// Invalid owner, pending startup, closed frame or another surface's open frame
/// preserves info. This is not a presentation-completion query.
[[nodiscard]] DeviceStatus GetFrameInfo(DeviceHandle device, SurfaceHandle surface, FrameInfo& info) noexcept;

/// @brief Borrow another platform target as a surface of a ready device (R14b).
///
/// Each surface keeps its own frame target, swapchain or drawable layer, depth attachment
/// and completion counters, so surfaces acquire, resize and present independently. One
/// frame is open per device at a time: BeginFrame chooses its surface. Call only between
/// frames. Every surface of a device shares the primary surface's color format.
/// @param device Ready owner; Pending startup returns NotReady and failure/loss its status.
/// @param window Borrowed target that must outlive DestroySurface or DestroyDevice.
/// @param outSurface Null output; written only on Ready.
/// @return Ready on success; InvalidHandle for a stale/foreign device; InvalidState for a
/// non-null output or an open frame; InvalidDescription for an invalid window or one whose
/// target another surface, of this or another device, already borrows; Unsupported when the backend hosts only its
/// primary surface (WebGL 2 and an imported Vulkan device in this release), the
/// window system cannot share the primary color format (Metal, Direct3D 12), or differs
/// from the primary's window system (Vulkan); CapacityExceeded when the device hosts its
/// maximum; Failed when the platform target or its swap chain could not be created, or a
/// canvas already holds another context mode. WebGPU validates a canvas configuration
/// asynchronously: a failure after Ready reports the surface Lost instead.
[[nodiscard]] DeviceStatus
CreateSurface(DeviceHandle device, const WindowInfo& window, SurfaceHandle& outSurface) noexcept;
/// @brief Retire one surface created by CreateSurface and clear the handle.
///
/// Waits for this surface's submitted frames, then releases only its presentation
/// resources and restores its borrowed view; the device, its resources and its other
/// surfaces are unaffected. Lost surfaces and surfaces of a failed or lost device are
/// accepted. Copies of the handle become stale.
/// @return Ready on success; InvalidHandle preserves a stale/foreign handle; InvalidState
/// for the primary surface (retired by DestroyDevice) or the surface of the open frame.
[[nodiscard]] DeviceStatus DestroySurface(DeviceHandle device, SurfaceHandle& inoutSurface) noexcept;
/// @brief Snapshot one surface's state, target and completion counters without waiting.
/// @return Ready writes info, also for a Lost surface; InvalidHandle preserves info.
[[nodiscard]] DeviceStatus GetSurfaceInfo(DeviceHandle device, SurfaceHandle surface, SurfaceInfo& outInfo) noexcept;

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
/// Consumes caller storage before return. Invalid pointer/extent pairs return InvalidDescription.
[[nodiscard]] ResourceStatus
UpdateUniform(DeviceHandle device,
              UniformHandle uniform,
              ludus::foundation::BorrowedView<const ludus::foundation::uint8> bytes) noexcept;
/// Destroy a shader between frames. Existing pipeline dependencies still return InUse.
[[nodiscard]] ResourceStatus Destroy(DeviceHandle device, ShaderHandle shader) noexcept;
/// Destroy a uniform between frames. Existing pipeline dependencies still return InUse.
[[nodiscard]] ResourceStatus Destroy(DeviceHandle device, UniformHandle uniform) noexcept;
/// Destroy a pipeline between frames, using the shared facade registry and retirement behavior.
[[nodiscard]] ResourceStatus Destroy(DeviceHandle device, PipelineHandle pipeline) noexcept;
/// Draw through the existing fullscreen path; validates the owner before issuing any work.
[[nodiscard]] ResourceStatus DrawFullscreen(DeviceHandle device, PipelineHandle pipeline) noexcept;
} // namespace ludus::graphics::rhi
