#pragma once

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/graphics/rhi/graph.h>

namespace ludus::graphics::rhi
{
/// One persistent storage allocation. Nonempty InitialBytes must cover Size exactly;
/// empty input creates zeroed storage. Preparation consumes input before returning.
struct ComputeWorkloadBuffer final
{
    foundation::usize Size = 0; ///< Positive byte capacity, independent of viewport size.
    foundation::BorrowedView<const foundation::uint8> InitialBytes; ///< Borrowed upload, or empty for zero fill.
    bool IsReadOnly = false; ///< Reject every writable binding to this allocation when true.
    /// Optional existing storage buffer instead of a new allocation. InitialBytes
    /// must be empty; Size bounds usable bytes. Caller keeps its public owner alive
    /// until this workload is reset. Workload never destroys this borrowed owner.
    /// Same-device readiness, range and role are independently checked by the RHI.
    BufferHandle External{};
};
/// One storage range in a kernel's reflected binding layout.
struct ComputeWorkloadBinding final
{
    foundation::uint32 Slot = 0;   ///< Reflected shader binding number.
    foundation::uint32 Buffer = 0; ///< Index into ComputeWorkloadDescription::Buffers.
    foundation::usize Offset = 0;  ///< Byte offset; device storage alignment applies.
    foundation::usize Size = 0;    ///< Positive byte length, within the selected allocation.
    bool IsWritable = false;       ///< StorageReadWrite when true, StorageRead otherwise.
};
/// One independent dispatch; dependent kernels occupy distinct ordered passes.
struct ComputeWorkloadKernel final
{
    const char* NameOrNull = nullptr; ///< Required nonempty diagnostic name, at most 63 UTF-8 bytes; copied.
    RasterShaderDescription Shader{}; ///< Cooked Compute artifact with storage-only reflected bindings.
    foundation::BorrowedView<const ComputeWorkloadBinding>
        Bindings;                   ///< At most sixteen; borrowed until Initialize returns.
    foundation::uint32 GroupsX = 1; ///< Positive workgroup count along X, bounded by device capability.
    foundation::uint32 GroupsY = 1; ///< Positive workgroup count along Y.
    foundation::uint32 GroupsZ = 1; ///< Positive workgroup count along Z.
    /// Reuse one earlier kernel's shader/layout/pipeline instead of Shader. The
    /// referenced kernel must declare its own program (no chains). Bindings are
    /// still validated against that program and each dispatch owns its snapshot.
    /// The default selects Shader. This avoids compiling identical layer kernels.
    foundation::uint32 UseProgramFrom = ~foundation::uint32{0};
};
/// Prepared buffer-only GPU work, including model weights, embeddings and scratch.
/// No scene, camera, render target, native ray queries or runtime compiler is required.
struct ComputeWorkloadDescription final
{
    foundation::BorrowedView<const ComputeWorkloadBuffer> Buffers; ///< Consumed synchronously during Initialize.
    foundation::BorrowedView<const ComputeWorkloadKernel> Kernels; ///< Serial dispatch order; metadata is copied.
    foundation::usize MaximumBufferBytes = 0; ///< Explicit total logical storage budget, including immutable uploads.
};
namespace internal
{
struct ComputeWorkloadState;
}
/// Reusable prepared dispatches and persistent buffers, owned by one device/owner thread.
/// All operations, moves and destruction run between frames, before device destruction.
/// Ordered graphs retain their own resource leases, including after Reset or replacement.
/// Metal/Vulkan/WebGPU use ordinary compute; unsupported devices reject explicitly.
class ComputeWorkload final
{
public:
    /// Construct an empty owner without allocation.
    ComputeWorkload() noexcept;
    /// Release public owners; already encoded work retires through the RHI ledger.
    ~ComputeWorkload() noexcept;
    /// Transfer the complete device/resource ownership, leaving other empty.
    ComputeWorkload(ComputeWorkload&& other) noexcept;
    /// Release this workload and transfer other, leaving other empty.
    ComputeWorkload& operator=(ComputeWorkload&& other) noexcept;
    /// Unique resource ownership forbids copying.
    ComputeWorkload(const ComputeWorkload&) = delete;
    /// Unique resource ownership forbids copy assignment.
    ComputeWorkload& operator=(const ComputeWorkload&) = delete;
    /// Validate, upload and prepare an empty owner. Initial data and shader reflection
    /// are consumed/copied before return; artifact storage must survive through Poll.
    /// Ready/Pending acquires workload ownership; any other result leaves this object empty.
    /// Device limits and MaximumBufferBytes bound admission; no GPU wait occurs.
    [[nodiscard]] RasterStatus Initialize(DeviceHandle device, const ComputeWorkloadDescription& description) noexcept;
    /// Advance asynchronous shader/pipeline preparation. NotReady for an empty owner.
    /// Failure is retained for inspection; Reset releases a failed workload.
    [[nodiscard]] RasterStatus Poll() noexcept;
    /// Build a fresh, uncompiled ordered graph importing each allocation exactly once.
    /// By default every mutable final version is a History root. Uses match reflected ranges;
    /// read/write hazards are tracked across all kernels. No upload or GPU execution.
    /// outGraph must be empty. Failure preserves it and this workload; discard a
    /// successful graph with DiscardOrderedGraph if it will not be executed.
    /// Optional outVersions must be empty or have one entry per described buffer;
    /// success copies final versions for appending consumers without duplicate imports.
    /// Failure preserves the supplied version entries.
    /// @param outGraph Empty output, receives the uncompiled graph on Ready and is preserved on failure.
    /// @param outVersions Empty or one entry per buffer; receives final versions on Ready only.
    /// @param shouldRootBuffers False lets an appending consumer root the final versions itself;
    /// supply outVersions and add roots after all writers when extending the graph.
    [[nodiscard]] RasterStatus BuildGraph(OrderedGraph& outGraph,
                                          foundation::BorrowedView<GraphVersion> outVersions = {},
                                          bool shouldRootBuffers = true) const noexcept;
    /// Borrow a ready allocation for subsequent compute/raster consumers. The handle
    /// expires on Reset/replacement; consumers must acquire their own RHI leases.
    /// Invalid index, empty or pending owner preserves outBuffer. Never transfer ownership.
    [[nodiscard]] RasterStatus GetBuffer(foundation::usize index, BufferHandle& outBuffer) const noexcept;
    /// Number of persistent allocations described by this owner; zero when empty.
    [[nodiscard]] foundation::usize GetBufferCount() const noexcept;
    /// Total logical allocated buffer bytes; zero for an empty owner. Does not include driver scratch.
    [[nodiscard]] foundation::usize GetBufferBytes() const noexcept;
    /// Release all public owners without waiting; requires no open device frame.
    void Reset() noexcept;

private:
    foundation::core::UniquePtr<internal::ComputeWorkloadState> mState;
};
} // namespace ludus::graphics::rhi
