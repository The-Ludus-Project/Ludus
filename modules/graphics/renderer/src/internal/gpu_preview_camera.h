#pragma once

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer
{
struct ViewDescription;
namespace internal
{
struct GpuPreviewUniform;
/// Create a zeroed failure summary (LUDUS_GRAPH_FAILURE_BYTES) for one camera history of a graph ray job.
[[nodiscard]] rhi::RasterStatus CreateGpuFailureSummary(rhi::DeviceHandle device,
                                                        rhi::BufferHandle& outBuffer) noexcept;
/// Staged camera parameters and bindings; scene and history buffers remain borrowed. A failure summary binding
/// (LUDUS_GRAPH_FAILURE_BINDING) receives a new zeroed buffer, because the new camera starts a new history.
/// The device and borrowed bindings outlive this transaction. Owner thread, between frames.
class GpuPreviewCameraUpdate final
{
public:
    GpuPreviewCameraUpdate() noexcept;
    ~GpuPreviewCameraUpdate() noexcept;
    GpuPreviewCameraUpdate(const GpuPreviewCameraUpdate&) = delete;
    GpuPreviewCameraUpdate& operator=(const GpuPreviewCameraUpdate&) = delete;
    [[nodiscard]] rhi::RasterStatus
    Prepare(rhi::DeviceHandle device,
            const GpuPreviewUniform& uniform,
            const ViewDescription& camera,
            foundation::uint64 revision,
            rhi::BindingLayoutHandle layout,
            foundation::BorrowedView<const rhi::RasterBindingResource> bindings) noexcept;
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    /// Requires Poll == Ready; transfers ownership without allocation or GPU submission. outFailures receives the
    /// new failure summary, or null when the bindings have none.
    void Commit(GpuPreviewUniform& outUniform,
                rhi::BufferHandle& outParameters,
                rhi::BindingSetHandle& outBindings,
                rhi::BufferHandle& outFailures) noexcept;
    void Reset() noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
};
} // namespace internal
} // namespace ludus::graphics::renderer
