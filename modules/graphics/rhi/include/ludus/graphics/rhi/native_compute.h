#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/native_interop.h>

namespace ludus::graphics::rhi
{
/// Native library dispatches per frame. Ordered graph execution rejects excess work
/// before encoding, preserving the graph and completion output.
inline constexpr foundation::usize MAX_NATIVE_COMPUTE_PASSES_PER_FRAME = 16;
/// Borrowed native storage descriptor supplied to Prepare. Metal uses id<MTLBuffer>.
/// The immutable binding snapshot retains the allocation through pipeline/GPU retirement.
struct NativeComputeBuffer final
{
    void* ObjectOrNull = nullptr; ///< Native buffer; never destroy or retain as a Ludus resource.
    foundation::usize Offset = 0; ///< First bound byte.
    foundation::usize Size = 0;   ///< Bound byte count.
};
/// Native compute adapter for libraries using a dedicated queue on the same device.
/// RHI owns the queue and orders shared-buffer producers/consumers with native events.
/// Owner-thread callbacks must not reenter RHI, wait for GPU completion or use an unprepared queue.
/// Only Metal storage-buffer bindings are currently admitted. Vulkan/browser reject explicitly.
struct NativeComputePipelineDescription final
{
    BindingSetHandle Bindings{};   ///< Ready, immutable, non-dynamic Compute storage-only snapshot.
    void* ContextOrNull = nullptr; ///< Caller context; retained on successful creation through GPU retirement.
    /// Prepare/cache native objects between frames. Buffers appear in layout order.
    /// The array view expires on return; copied descriptors remain valid through pipeline retirement.
    /// Pending retains context and bindings and polls Prepare again without blocking.
    /// Private setup may run on a worker; bound buffers must remain untouched until Submit.
    /// Drain must join pending private preparation during device shutdown.
    RasterStatus (*PrepareOrNull)(void*,
                                  const NativeDeviceIdentity&,
                                  foundation::BorrowedView<const NativeComputeBuffer>) noexcept = nullptr;
    /// Enqueue GPU work on the exact prepared private queue. RHI orders the prior
    /// producer before it, and subsequent consumers/completion after it with native events.
    RasterStatus (*SubmitOrNull)(void*) noexcept = nullptr;
    void (*RetainOrNull)(void*) noexcept = nullptr;  ///< Retain the context without failure or allocation.
    void (*ReleaseOrNull)(void*) noexcept = nullptr; ///< Release after the last retained GPU use, also on shutdown.
    /// Shutdown-only fence for pending private preparation, called before native buffers/queue retire.
    /// Must drain even if the caller still retains other context references; must not reenter RHI.
    void (*DrainOrNull)(void*) noexcept = nullptr;
};
/// Create a native adapter in the ordinary compute registry; no shader compilation.
/// Dispatch must use the exact supplied snapshot, unit group counts and no dynamic offsets.
/// Ready/Pending publishes ownership; Pending requires polling and destruction.
/// Failure preserves output and does not retain ContextOrNull. Call between frames.
/// Metal only; unsupported backends return Unsupported before calling Prepare.
[[nodiscard]] RasterStatus CreateNativeComputePipeline(DeviceHandle device,
                                                       const NativeComputePipelineDescription& description,
                                                       ComputePipelineHandle& outPipeline) noexcept;
} // namespace ludus::graphics::rhi
