#pragma once

#include <ludus/foundation/base/compiler.h>

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/lifetime.h>

#include <span>

namespace ludus::graphics::renderer::internal
{
/// Plan pass of each cooked callback-set index; ABSENT where no ray pass uses that set.
using RayPathPassTable = foundation::StaticArray<foundation::uint32, content::render_graph_format::MAX_RAY_PROGRAMS>;
/// Bytes of one capture buffer for a validated description: header, summaries and records.
[[nodiscard]] foundation::usize GetRayPathCaptureBytes(const RayPathCaptureDescription& description) noexcept;
/// Write the CPU-owned header of a zeroed capture buffer.
/// @param description Validated selection.
/// @param outBytes Exactly GetRayPathCaptureBytes(description) bytes; only header words are written.
/// @return False when outBytes has another size; outBytes is then unchanged.
[[nodiscard]] bool WriteRayPathCaptureHeader(const RayPathCaptureDescription& description,
                                             std::span<foundation::uint8> outBytes) noexcept;
/// Decode a read-back capture buffer field by field, independent of host struct layout.
/// Events are sorted by pixel, sample, pass and sequence; summaries by pixel, sample and pass.
/// A summary is truncated unless its recorded sequences are exactly 0..EventCount-1.
/// @param bytes Complete little-endian capture buffer written by a capture-variant kernel.
/// @param passes Plan pass of each recorded callback-set index.
/// @param inoutEvents Replaced by the decoded events; preserved on failure.
/// @param inoutSummaries Replaced by the written summaries; preserved on failure.
/// @param outReport Decoded counters; unchanged on failure.
/// @return Ready; InvalidDescription for a malformed header, size or pass index; OutOfMemory.
[[nodiscard]] rhi::RasterStatus DecodeRayPathCapture(std::span<const foundation::uint8> bytes,
                                                     const RayPathPassTable& passes,
                                                     foundation::Array<RayPathEvent>& inoutEvents,
                                                     foundation::Array<RayPathSummary>& inoutSummaries,
                                                     RayPathCaptureReport& outReport) noexcept;
/// One graph's diagnostic ray path capture: its buffer, bounded readback and decoded result.
/// RenderGraph owns it; all calls use the RHI owner thread between frames. The device outlives it.
/// See docs/architecture/render-graph-authoring.md#ray-path-inspection-proposed.
class RenderGraphRayPathCapture final
{
public:
    RenderGraphRayPathCapture() noexcept = default;
    ~RenderGraphRayPathCapture() noexcept;
    RenderGraphRayPathCapture(const RenderGraphRayPathCapture&) = delete;
    RenderGraphRayPathCapture& operator=(const RenderGraphRayPathCapture&) = delete;
    /// Create the zeroed buffer with its header. Ready/Pending publish the request; failure keeps none.
    [[nodiscard]] rhi::RasterStatus Request(rhi::DeviceHandle device,
                                            const RayPathCaptureDescription& description,
                                            const RayPathPassTable& passes) noexcept;
    [[nodiscard]] LUDUS_INLINE bool IsRequested() const noexcept
    {
        return mBuffer != rhi::BufferHandle{};
    }
    /// Buffer bound into every ray pass; empty before a request.
    [[nodiscard]] LUDUS_INLINE GpuPreviewGraphBuffer GetBuffer() const noexcept
    {
        return {mBuffer, mBytes.GetSize()};
    }
    /// Start or finish one readback without waiting. The caller proves producer completion first.
    [[nodiscard]] rhi::RasterStatus Poll(RayPathCaptureReport& outReport) noexcept;
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const RayPathEvent> GetEvents() const noexcept
    {
        return {mEvents.GetData(), mEvents.GetSize()};
    }
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const RayPathSummary> GetSummaries() const noexcept
    {
        return {mSummaries.GetData(), mSummaries.GetSize()};
    }
    /// Release the readback and destroy the buffer; RHI retires their GPU uses.
    void Reset() noexcept;

private:
    rhi::DeviceHandle mDevice{};
    rhi::BufferHandle mBuffer{};
    rhi::ReadbackTicket mReadback{};
    bool mIsReading = false;
    RayPathPassTable mPasses{};
    foundation::Array<foundation::uint8> mBytes;
    foundation::Array<RayPathEvent> mEvents;
    foundation::Array<RayPathSummary> mSummaries;
};
} // namespace ludus::graphics::renderer::internal
