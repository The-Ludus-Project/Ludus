#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/measurement.h>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::rhi
{
/// Maximum UTF-8 scope storage, including its terminating zero. Longer names are rejected.
inline constexpr foundation::usize GPU_SCOPE_NAME_BYTES = 128;
/// Maximum measured physical passes in one acquired frame. Surplus passes still render.
inline constexpr foundation::usize MAX_GPU_TIMED_PASSES = 64;
/// Maximum simultaneously open scopes on the owner thread, for example view, graph and
/// physical pass. A deeper BeginGpuScope returns CapacityExceeded and rendering continues.
inline constexpr foundation::uint32 MAX_GPU_SCOPE_DEPTH = 8;
/// Owned annotation identity, also copied into each timing result. Identity is producer-owned
/// and must be nonzero; it is interpreted together with Name, frame and device generation.
struct GpuScopeDescription final
{
    /// Stable producer-local scope identifier; not a GPU timestamp or a native object handle.
    foundation::uint64 Identity = 0;
    /// Valid, nonempty, terminated UTF-8 name shared by native debug annotations and Perfetto.
    foundation::StaticArray<char, GPU_SCOPE_NAME_BYTES> Name{};
    /// Number of logical operations represented by this physical scope, at least one.
    foundation::uint32 LogicalPassCount = 1;
};
/// Construct an owned scope without allocation. Rejects zero identity, invalid/empty UTF-8,
/// embedded zero, oversized names and zero logical count. Failure preserves outScope.
[[nodiscard]] bool MakeGpuScope(foundation::uint64 identity,
                                foundation::BorrowedView<const char> name,
                                GpuScopeDescription& outScope,
                                foundation::uint32 logicalPassCount = 1) noexcept;
/// Measured native operation. Raster uses the backend's physical-pass stage interval;
/// Compute uses its encoder/dispatch interval. Values are elapsed time, not shader busy time.
enum class GpuPassKind : foundation::uint8
{
    /// Vertex/fragment stage envelope on Metal, TOP/BOTTOM render-pass interval on Vulkan,
    /// or the interval between two bottom-of-pipe samples around a Direct3D 12 raster pass.
    Raster,
    /// Physical compute operation, including GPU stalls.
    Compute,
    /// Native ray-stage/function-table pipeline; Metal measures its compute encoder interval.
    RayTracing,
};
/// One physical operation inside a named scope; native objects are never exposed or borrowed.
struct GpuPassTiming final
{
    /// Copied identity and name of the outermost scope open when the operation was encoded.
    /// Nested scopes label native captures only, so frame-level breakdowns stay stable.
    GpuScopeDescription Scope{};
    /// Native operation class.
    GpuPassKind Kind = GpuPassKind::Raster;
    /// Zero-based physical operation within the outermost scope; distinguishes multi-encoder scopes.
    foundation::uint32 ScopePassIndex = 0;
    /// Available, Disjoint or Failed after the enclosing batch becomes terminal.
    GpuTiming Timing{};
    /// True when StartTicks/EndTicks place this operation on the foundation::time::NowTicks
    /// clock through the batch's validated device-clock calibration. False whenever the
    /// backend has no calibration, the calibration was rejected, or Timing is not Available.
    bool HasCpuInterval = false;
    /// Calibrated operation start in NowTicks nanoseconds; meaningful only with HasCpuInterval.
    foundation::uint64 StartTicks = 0;
    /// Calibrated operation end in NowTicks nanoseconds, not before StartTicks; meaningful
    /// only with HasCpuInterval. EndTicks - StartTicks may differ from Timing's raw duration
    /// by the calibrated clock rate.
    foundation::uint64 EndTicks = 0;
};
/// Copied bounded batch. Terminal results persist until rearmed; polling never consumes them.
struct GpuPassTimings final
{
    /// Empty/Armed/Pending/Available/Unavailable/Failed lifecycle of this batch.
    GpuTimingState State = GpuTimingState::Empty;
    /// Initialized entries in Passes; at most MAX_GPU_TIMED_PASSES.
    foundation::usize Count = 0;
    /// Physical operations omitted due to query capacity; never fabricated as zero durations.
    foundation::uint64 DroppedPasses = 0;
    /// Bound, in nanoseconds, on the CPU-clock error of every pass with HasCpuInterval: for the
    /// two device/CPU calibration samples bracketing the batch, half the larger CPU read gap on
    /// Metal, or the larger driver-reported maxDeviation on Vulkan. Never above 50 µs. Zero when
    /// no pass has a CPU interval. Metal, and Vulkan devices with calibrated timestamps, only;
    /// other backends stay duration-only.
    foundation::uint64 ClockUncertaintyNanoseconds = 0;
    /// Initialized prefix contains identities and explicit per-operation validity.
    foundation::StaticArray<GpuPassTiming, MAX_GPU_TIMED_PASSES> Passes{};
};
/// Reserve/arm a native per-pass batch for the next acquired frame. Owner-thread only,
/// between frames, with DeviceDescription::EnableGpuTiming enabled. No GPU waits.
/// Ready replaces terminal data; InvalidState preserves armed/pending data. Unsupported
/// leaves frame timing and rendering available. Allocation failure preserves terminal data.
[[nodiscard]] RasterStatus ArmGpuPassTimings(DeviceHandle device) noexcept;
/// Nonblocking owner-thread copy of the current batch. InvalidHandle preserves outTimings; unavailable
/// device states clear it. Duration-only data has no CPU/GPU clock alignment.
[[nodiscard]] RasterStatus GetGpuPassTimings(DeviceHandle device, GpuPassTimings& outTimings) noexcept;
/// Begin a named scope on the owner thread in an acquired frame, before encoding its operations.
/// Names are copied for timing and native annotations. Scopes nest up to MAX_GPU_SCOPE_DEPTH
/// (deeper scopes return CapacityExceeded), so a view, graph and physical pass each keep their
/// own native region; timing belongs to the outermost scope. A merged physical pass uses one
/// scope with its logical member count. Timing need not be armed. Submitting the frame closes
/// every scope still open; a later EndGpuScope for one returns InvalidState. Annotation
/// statuses (InvalidState, CapacityExceeded, Unsupported) must not be treated as rendering failures.
[[nodiscard]] RasterStatus BeginGpuScope(DeviceHandle device, const GpuScopeDescription& scope) noexcept;
/// End the innermost owner-thread scope after its encoders/passes end. Rejects an unbalanced end.
[[nodiscard]] RasterStatus EndGpuScope(DeviceHandle device) noexcept;
/// Owner-thread lexical scope; failed annotation admission never prevents rendering.
/// Destruction closes only the scope successfully begun by this instance, and only while it is
/// still the innermost scope of the same frame: a scope already closed by submission, or one
/// left open beneath it, is not closed in its place.
class GpuScope final
{
public:
    /// Begin scope on device, borrowing neither the descriptor nor its name after return.
    GpuScope(DeviceHandle device, const GpuScopeDescription& scope) noexcept;
    /// Close an admitted scope; no GPU wait and no ownership of the device.
    ~GpuScope() noexcept;
    /// Copying would duplicate native scope ownership and is forbidden.
    GpuScope(const GpuScope&) = delete;
    /// Assignment cannot transfer an active lexical scope.
    GpuScope& operator=(const GpuScope&) = delete;

private:
    DeviceHandle mDevice;
    foundation::uint64 mEpoch = 0;
    foundation::uint32 mDepth = 0;
    bool mIsActive = false;
};
/// Owned debug name of a GPU resource, shown by native graphics debuggers. Uses the scope text
/// owner: valid UTF-8 without embedded zeros, at most GPU_SCOPE_NAME_BYTES - 1 bytes, always
/// terminated. An empty name (first byte zero) means unnamed. Names are diagnostic only: they
/// never identify, retain or alter the resource.
struct GpuDebugName final
{
    /// Terminated UTF-8; bytes after the terminator are zero.
    foundation::StaticArray<char, GPU_SCOPE_NAME_BYTES> Text{};
};
/// Copy name into outName without allocation. An empty view clears the name. Rejects a view
/// without storage, invalid UTF-8, embedded zeros and names of GPU_SCOPE_NAME_BYTES bytes or
/// more. Failure preserves outName.
[[nodiscard]] bool MakeGpuDebugName(foundation::BorrowedView<const char> name, GpuDebugName& outName) noexcept;
/// Name a live buffer on the owner thread between frames, replacing any previous name; an empty
/// name clears it. The text is copied before return. Applies a native object label where the
/// backend supports one (Metal label, Vulkan VK_EXT_debug_utils object name, Direct3D 12
/// SetName, WebGPU label). Missing native naming still records the name and returns Ready.
/// Destroying the buffer discards its name, so a recycled slot starts unnamed.
/// @return Ready; InvalidDescription for invalid text; InvalidHandle for a stale or foreign
/// handle; InvalidState inside an open frame; device admission statuses otherwise.
[[nodiscard]] RasterStatus
SetDebugName(DeviceHandle device, BufferHandle buffer, foundation::BorrowedView<const char> name) noexcept;
/// Name a live texture, including its private depth backing; otherwise as for buffers.
[[nodiscard]] RasterStatus
SetDebugName(DeviceHandle device, TextureHandle texture, foundation::BorrowedView<const char> name) noexcept;
/// Copy the recorded name of a live buffer, empty when unnamed. Nonblocking and admitted inside
/// frames. InvalidHandle and device failures preserve outName.
[[nodiscard]] RasterStatus GetDebugName(DeviceHandle device, BufferHandle buffer, GpuDebugName& outName) noexcept;
/// Copy the recorded name of a live texture; otherwise as for buffers.
[[nodiscard]] RasterStatus GetDebugName(DeviceHandle device, TextureHandle texture, GpuDebugName& outName) noexcept;
} // namespace ludus::graphics::rhi
