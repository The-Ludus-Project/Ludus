#pragma once

#include <ludus/foundation/geometry/point_search.hpp>
#include <ludus/graphics/rhi/compute_workload.hpp>

namespace ludus::graphics::renderer
{
/// Result allocation index in a workload prepared by PreparePointNeighborWorkload.
inline constexpr foundation::usize POINT_NEIGHBOR_RESULT_BUFFER = 3;
/// Upload a point tree and query positions, preparing a portable buffer-only compute
/// workload using the installed renderer_point_neighbors recipe. No ray hardware,
/// camera or render attachment is needed. Source borrows end before return.
/// @param device Ready compute-capable Metal/Vulkan/WebGPU device; owner thread, between frames.
/// @param tree Nonempty balanced tree built by PointSearchTree; copied into immutable GPU storage.
/// @param positionsWorld Finite queries within MAX_POINT_SEARCH_COORDINATE; copied as float4 records.
/// @param neighborCount 1..MAX_POINT_NEIGHBORS, no more than the tree's point count.
/// @param shader Cooked renderer_point_neighbors Compute artifact; retained until workload.Poll completes.
/// @param maximumBufferBytes Logical bytes for parameters, tree, queries and results; device limits also apply.
/// @param outWorkload Empty owner; acquired on Ready/Pending, preserved on failure.
/// @return Ready/Pending or explicit preparation failure. Execute BuildGraph through
/// normal RHI APIs, then GetBuffer(POINT_NEIGHBOR_RESULT_BUFFER) to consume query-major
/// PointNeighbor records on the GPU. Invalid-tree/query records use ABSENT_POINT_NODE;
/// they must not be used as embedding indices. No CPU result transfer is performed.
[[nodiscard]] rhi::RasterStatus
PreparePointNeighborWorkload(rhi::DeviceHandle device,
                             const foundation::geometry::PointSearchTree& tree,
                             foundation::BorrowedView<const foundation::math::Vector3> positionsWorld,
                             foundation::uint32 neighborCount,
                             const rhi::RasterShaderDescription& shader,
                             foundation::usize maximumBufferBytes,
                             rhi::ComputeWorkload& outWorkload) noexcept;
} // namespace ludus::graphics::renderer
