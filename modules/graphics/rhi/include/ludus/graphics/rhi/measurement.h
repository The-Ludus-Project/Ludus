#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::rhi
{
/// State of an optional GPU measurement; sampling never waits for the GPU.
enum class GpuTimingState : foundation::uint8
{
    /// No requested sample in this session.
    Empty,
    /// Waiting for or recording the next acquired frame; skipped acquisition keeps it armed.
    Armed,
    /// Submitted query/command buffer has not completed.
    Pending,
    /// Duration is available, including a legitimate quantized zero.
    Available,
    /// Backend/device did not enable its optional timing facility.
    Unavailable,
    /// Clock discontinuity or unrepresentable/invalid timestamps; duration is unavailable.
    Disjoint,
    /// Query or command buffer failed; duration is unavailable.
    Failed,
};
/// Copied GPU duration; the device-local clock has no correlation with CPU/display time.
struct GpuTiming final
{
    /// Explicit lifecycle/availability; DurationNanoseconds is meaningful only when Available.
    GpuTimingState State = GpuTimingState::Empty;
    /// Device-clock elapsed interval: the acquired frame for ArmGpuTiming, or the backend
    /// stage interval for a GpuPassTiming operation. Frame probes include graph/compute
    /// commands, clear and GPU stalls. WebGL may include idle gaps while the CPU feeds commands.
    /// Excludes readback/display latency; this is not a CPU timer or a sum of GPU busy cycles.
    foundation::float64 DurationNanoseconds = 0;
    /// Clock tick in nanoseconds when specified by the API; zero means unknown precision.
    /// This does not claim an accuracy bound or browser privacy-quantization interval.
    foundation::float64 TickNanoseconds = 0;
    /// Number of valid timestamp bits, or zero when not exposed by the API.
    foundation::uint32 ValidBits = 0;
};
/// Optional frame sampling controls. One probe is retained per session, separate from resources.
/// Enable through DeviceDescription::EnableGpuTiming before startup. Owner-thread only.
/// Arm between frames; Ready replaces a terminal/empty sample, InvalidState preserves an
/// armed/pending sample or rejects an open frame. Unsupported means timing was not enabled.
/// Query allocation can return OutOfMemory/Failed; those and identity-capacity failures
/// preserve the previous terminal sample while the device stays ready. Skipped frames keep it armed.
/// No blocking or CPU-duration fallback. DestroyDevice cancels and releases its native storage.
[[nodiscard]] RasterStatus ArmGpuTiming(DeviceHandle) noexcept;
/// Nonblocking poll/copy. InvalidHandle preserves output; other unavailable device states zero it.
/// Ready reports the copied lifecycle, including Pending, Unavailable, Disjoint or Failed.
/// Polling a query does not claim presentation completion. Terminal results persist until re-armed.
[[nodiscard]] RasterStatus GetGpuTiming(DeviceHandle, GpuTiming&) noexcept;
/// Copied selected-adapter identity for qualification. Empty names/zero identifiers mean
/// unavailable, never a promise of a particular vendor/driver. Browser privacy may hide fields.
struct MeasurementAdapter final
{
    /// Bounded null-terminated UTF-8 name (at most 255 bytes, possibly truncated).
    char Name[256]{};
    /// Backend-reported vendor identifier, or zero when unavailable.
    foundation::uint32 VendorId = 0;
    /// Backend-reported device identifier, or zero when unavailable.
    foundation::uint32 DeviceId = 0;
    /// Raw Vulkan driver version; vendor-specific encoding. Zero means unavailable.
    foundation::uint32 DriverVersion = 0;
    /// Whether the backend distinguishes a CPU/software adapter from hardware.
    bool SoftwareKnown = false;
    /// True only for an explicitly identified CPU/software adapter; inspect SoftwareKnown.
    bool Software = false;
};
/// Copy backend adapter identity, without allocation/wait; owner-thread only.
/// InvalidHandle preserves output; unavailable device states zero it. Ready may still contain
/// hidden identity fields; qualification must keep those observations open.
[[nodiscard]] RasterStatus GetMeasurementAdapter(DeviceHandle, MeasurementAdapter&) noexcept;
/// Allocation telemetry scoped to general raster/compute buffers and textures (including depth
/// and retained texture-upload allocations); excludes facade/surface, transfer rings and ray objects.
/// R11 descriptor counts/reservations are a separate scope; descriptor driver bytes remain unavailable.
struct AllocationTelemetry final
{
    /// Current R6 logical resource bytes in this scope, including deferred retirement.
    foundation::uint64 LogicalBytes = 0;
    /// Sum of resource API allocation/binding sizes, including native padding. Vulkan reports
    /// VMA suballocation spans; this excludes allocator block slack (see AllocatorBlockBytes).
    /// Meaningful only when ApiBytesAvailable; WebGPU/WebGL texture backing is opaque.
    foundation::uint64 ApiBytes = 0;
    /// Maximum observed API allocation bytes in this session, sampled at each query.
    /// This is a sampled peak, not a guarantee that intervening allocations were observed.
    foundation::uint64 PeakObservedApiBytes = 0;
    /// API allocations still alive whose public resource identity has been destroyed.
    foundation::uint64 RetiringApiBytes = 0;
    /// True only if the entire stated scope exposes API allocation sizes.
    bool ApiBytesAvailable = false;
    /// Actual vkAllocateMemory block bytes owned by the Vulkan raster allocator, including
    /// unused pool slack, transfer rings and ray allocations in that allocator. Excludes facade/
    /// swapchain allocations. This broader scope is not the sum of ApiBytes above.
    /// Meaningful only when AllocatorBlockBytesAvailable; unavailable on other backends.
    foundation::uint64 AllocatorBlockBytes = 0;
    /// Maximum observed allocator block bytes in this session, sampled at each query.
    foundation::uint64 PeakObservedAllocatorBlockBytes = 0;
    /// Whether actual allocator block bytes can be observed in the stated broader scope.
    bool AllocatorBlockBytesAvailable = false;
    /// Physical/resident bytes; meaningful only when PhysicalBytesAvailable.
    foundation::uint64 PhysicalBytes = 0;
    /// Whether actual physical residency can be observed. Currently false on all backends.
    bool PhysicalBytesAvailable = false;
    /// Populated binding descriptors, including pending/detached/in-flight snapshots.
    foundation::uint64 BindingDescriptors = 0;
    /// Populated descriptors retained after their snapshot owner was destroyed.
    foundation::uint64 RetiringBindingDescriptors = 0;
    /// Native shared descriptor pools alive; meaningful only when DescriptorArenasAvailable.
    foundation::uint64 DescriptorArenaPools = 0;
    /// Descriptor slots reserved by those native pools, including unused capacity.
    /// Counts are not driver allocation bytes or physical memory measurements.
    foundation::uint64 ReservedArenaDescriptors = 0;
    /// Whether shared native descriptor arena reservations can be queried (Vulkan only).
    bool DescriptorArenasAvailable = false;
};
/// Copy scoped allocation/retirement telemetry without waiting/allocating; owner-thread only.
/// InvalidHandle preserves output; other unavailable device states zero it. ApiBytesAvailable
/// is false when any allocation size is opaque; partial sums are not published as complete totals.
/// CapacityExceeded zeros output when a complete byte count cannot be represented.
[[nodiscard]] RasterStatus GetAllocationTelemetry(DeviceHandle, AllocationTelemetry&) noexcept;
} // namespace ludus::graphics::rhi
