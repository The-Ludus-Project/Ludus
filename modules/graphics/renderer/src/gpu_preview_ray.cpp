#include "internal/gpu_preview_ray.h"

#include <ludus/foundation/base/assert.hpp>
#include <ludus/foundation/base/checked_integer.hpp>

#include <cstring>

namespace ludus::graphics::renderer::internal
{
namespace
{
constexpr usize CORNER_COUNT = 3;
#if defined(LUDUS_RENDERER_TEST_READBACK)
bool shouldPartitionForTesting = true;
#endif
bool IsOpaqueTriangle(const ReferenceSceneData& scene, uint32 triangle) noexcept
{
#if defined(LUDUS_RENDERER_TEST_READBACK)
    if (shouldPartitionForTesting == false)
    {
        return false;
    }
#endif
    return scene.Instances[scene.Triangles[triangle].Instance].Surface->Alpha == MaterialAlpha::Opaque;
}
} // namespace
#if defined(LUDUS_RENDERER_TEST_READBACK)
void SetGpuPreviewOpacityPartitioningForTesting(bool shouldPartition) noexcept
{
    shouldPartitionForTesting = shouldPartition;
}
#endif
void PackGpuPreviewRayOrder(const ReferenceSceneData& scene, bool shouldPartition, math::Vector4* outOrder) noexcept
{
    LUDUS_ASSERT(outOrder != nullptr);
    if (shouldPartition == false)
    {
        for (usize i = 0; i < scene.Order.GetSize(); ++i)
        {
            std::memcpy(&outOrder[i].X, &scene.Order[i], sizeof(uint32));
        }
        if (scene.Order.IsEmpty() == false)
        {
            outOrder[0].Y = 0;
        }
        return;
    }
    usize outputIndex = 0;
    uint32 opaqueCount = 0;
    for (usize partition = 0; partition < GPU_PREVIEW_OPACITY_PARTITION_COUNT; ++partition)
    {
        const bool isOpaque = partition == 0;
        for (uint32 triangle : scene.Order)
        {
            if (IsOpaqueTriangle(scene, triangle) != isOpaque)
            {
                continue;
            }
            std::memcpy(&outOrder[outputIndex++].X, &triangle, sizeof(triangle));
            if (isOpaque == true)
            {
                ++opaqueCount;
            }
        }
    }
    if (outputIndex != 0)
    {
        std::memcpy(&outOrder[0].Y, &opaqueCount, sizeof(opaqueCount));
    }
}
rhi::RasterStatus CreateGpuPreviewRayGeometry(rhi::DeviceHandle device,
                                              const ReferenceSceneData& scene,
                                              usize cpuByteBudget,
                                              usize gpuByteBudget,
                                              rhi::RayGeometryHandle& outGeometry,
                                              rhi::RayAllocation& outAllocation) noexcept
{
    static_assert(sizeof(math::Vector3) == CORNER_COUNT * sizeof(float32));
    usize vertexCount = 0;
    usize inputBytes = 0;
    if (TryMultiply(scene.Order.GetSize(), CORNER_COUNT, vertexCount) == false || vertexCount > ~uint32{0} ||
        TryMultiply(vertexCount, sizeof(math::Vector3) + sizeof(uint32), inputBytes) == false ||
        inputBytes > cpuByteBudget)
    {
        return rhi::RasterStatus::CapacityExceeded;
    }
    if (vertexCount == 0)
    {
        outAllocation = {};
        return rhi::RasterStatus::Ready;
    }
    Array<math::Vector3> positionsWorld;
    Array<uint32> indices;
    if (positionsWorld.TryResize(vertexCount) == false || indices.TryResize(vertexCount) == false)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    // Thanks to Khronos, Vulkan Specification, "Ray Opacity Culling":
    // https://docs.vulkan.org/spec/latest/chapters/raytraversal.html#ray-opacity-culling
    // Separate native geometry ranges within one BLAS bypass opaque candidates
    // without an extra TLAS instance or changing the CPU oracle's traversal order.
    usize vertex = 0;
    uint32 opaqueTriangleCount = 0;
    for (usize partition = 0; partition < GPU_PREVIEW_OPACITY_PARTITION_COUNT; ++partition)
    {
        const bool isOpaque = partition == 0;
        for (uint32 triangleIndex : scene.Order)
        {
            if (IsOpaqueTriangle(scene, triangleIndex) != isOpaque)
            {
                continue;
            }
            const ReferenceTriangle& triangle = scene.Triangles[triangleIndex];
            for (usize corner = 0; corner < CORNER_COUNT; ++corner)
            {
                positionsWorld[vertex] = triangle.Positions[corner];
                indices[vertex] = static_cast<uint32>(vertex);
                ++vertex;
            }
            if (isOpaque == true)
            {
                ++opaqueTriangleCount;
            }
        }
    }
    const rhi::RayGeometryDescription description
    {
        .Positions = positionsWorld.GetData(),
        .VertexCount = vertexCount,
        .Indices = indices.GetData(),
        .IndexCount = vertexCount,
        .ByteBudget = gpuByteBudget,
        .OpaqueTriangleCount = opaqueTriangleCount,
    };
    const rhi::RasterStatus status = rhi::CreateRayGeometry(device, description, outGeometry);
    if (status != rhi::RasterStatus::Ready && status != rhi::RasterStatus::Pending)
    {
        return status;
    }
    return rhi::GetRayAllocation(device, outGeometry, outAllocation) == rhi::RasterStatus::Ready
               ? status
               : rhi::RasterStatus::Failed;
}
} // namespace ludus::graphics::renderer::internal
