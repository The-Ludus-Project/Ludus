#include "internal/gpu_preview_ray.h"

#include <ludus/foundation/base/checked_integer.hpp>

namespace ludus::graphics::renderer::internal
{
rhi::RasterStatus CreateGpuPreviewRayGeometry(rhi::DeviceHandle device,
                                              const ReferenceSceneData& scene,
                                              usize cpuByteBudget,
                                              usize gpuByteBudget,
                                              rhi::RayGeometryHandle& outGeometry,
                                              rhi::RayAllocation& outAllocation) noexcept
{
    constexpr usize CORNER_COUNT = 3;
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
    Array<math::Vector3> positions;
    Array<uint32> indices;
    if (positions.TryResize(vertexCount) == false || indices.TryResize(vertexCount) == false)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    for (usize primitive = 0; primitive < scene.Order.GetSize(); ++primitive)
    {
        const ReferenceTriangle& triangle = scene.Triangles[scene.Order[primitive]];
        for (usize corner = 0; corner < CORNER_COUNT; ++corner)
        {
            const usize vertex = primitive * CORNER_COUNT + corner;
            positions[vertex] = triangle.Positions[corner];
            indices[vertex] = static_cast<uint32>(vertex);
        }
    }
    const rhi::RayGeometryDescription description
    {
        .Positions = positions.GetData(),
        .VertexCount = vertexCount,
        .Indices = indices.GetData(),
        .IndexCount = vertexCount,
        .ByteBudget = gpuByteBudget,
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
