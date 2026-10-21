#pragma once

#include "reference_data.h"

namespace ludus::graphics::renderer::internal
{
// Upload world triangles in the immutable packed order. Native primitive IDs
// index that same order; degenerate triangles excluded by preparation stay absent.
[[nodiscard]] rhi::RasterStatus CreateGpuPreviewRayGeometry(rhi::DeviceHandle device,
                                                            const ReferenceSceneData& scene,
                                                            usize cpuByteBudget,
                                                            usize gpuByteBudget,
                                                            rhi::RayGeometryHandle& outGeometry,
                                                            rhi::RayAllocation& outAllocation) noexcept;
} // namespace ludus::graphics::renderer::internal
