#pragma once
#include "gpu_preview_data.h"

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/graphics/rhi/graph.h>
namespace ludus::graphics::renderer::internal
{
[[nodiscard]] rhi::RasterStatus BuildGpuGraph(rhi::DeviceHandle device,
                                              const GpuPreviewState& state,
                                              const GpuPreviewRecord& job,
                                              foundation::uint32 image,
                                              rhi::OrderedGraph& outGraph,
                                              const rhi::TextureHandle* targetOrNull = nullptr,
                                              const VisibilityRasterPass* visibilityOrNull = nullptr) noexcept;
/// Record every job's ray dispatch, in order, and present each to its target. A job holding a prepared row band
/// (GpuPreviewTiles) dispatches that band only; other jobs dispatch their whole image.
[[nodiscard]] rhi::RasterStatus BuildGpuGraphSequence(rhi::DeviceHandle device,
                                                      const GpuPreviewState& state,
                                                      foundation::BorrowedView<const GpuPreviewRecord*> jobs,
                                                      foundation::BorrowedView<const rhi::TextureHandle> targets,
                                                      rhi::OrderedGraph& outGraph,
                                                      const VisibilityRasterPass* visibilityOrNull = nullptr) noexcept;
} // namespace ludus::graphics::renderer::internal
