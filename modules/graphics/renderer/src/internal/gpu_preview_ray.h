#pragma once

#include "reference_data.h"

namespace ludus::graphics::renderer::internal
{
inline constexpr usize GPU_PREVIEW_OPACITY_PARTITION_COUNT = 2;
// Test-build-only control. Set on the RHI owner thread with no live previews;
// false reproduces the original single nonopaque BLAS and CPU-BVH primitive order.
void SetGpuPreviewOpacityPartitioningForTesting(bool shouldPartition) noexcept;
// Stable opaque-then-masked primitive mapping in X, independent of the CPU oracle's BVH.
// The first word's Y component stores the second partition's primitive base.
// False preserves the CPU BVH order in X. Z always holds the CPU BVH order for compute traversal.
void PackGpuPreviewRayOrder(const ReferenceSceneData& scene, bool shouldPartition, math::Vector4* outOrder) noexcept;
// One native BLAS contains an opaque prefix and a masked suffix. Native geometry
// range IDs plus local primitive IDs address the stable packed order.
[[nodiscard]] rhi::RasterStatus CreateGpuPreviewRayGeometry(rhi::DeviceHandle device,
                                                            const ReferenceSceneData& scene,
                                                            usize cpuByteBudget,
                                                            usize gpuByteBudget,
                                                            rhi::RayGeometryHandle& outGeometry,
                                                            rhi::RayAllocation& outAllocation) noexcept;
} // namespace ludus::graphics::renderer::internal
