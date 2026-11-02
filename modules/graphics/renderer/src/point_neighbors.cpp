#include <ludus/graphics/renderer/point_neighbors.hpp>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::graphics::renderer
{
using namespace foundation;
namespace
{
constexpr usize BUFFER_COUNT = POINT_NEIGHBOR_RESULT_BUFFER + 1;
constexpr usize PARAMETER_COUNT = 4;
constexpr uint32 QUERY_GROUP_SIZE = LUDUS_POINT_SEARCH_GROUP_SIZE;
template <typename Element>
BorrowedView<const uint8> GetBytes(BorrowedView<const Element> values) noexcept
{
    return {reinterpret_cast<const uint8*>(values.GetDataOrNull()), values.GetSize() * sizeof(Element)};
}
} // namespace
rhi::RasterStatus PreparePointNeighborWorkload(rhi::DeviceHandle device,
                                               const geometry::PointSearchTree& tree,
                                               BorrowedView<const math::Vector3> positionsWorld,
                                               uint32 neighborCount,
                                               const rhi::RasterShaderDescription& shader,
                                               usize maximumBufferBytes,
                                               rhi::ComputeWorkload& outWorkload) noexcept
{
    const BorrowedView<const geometry::PointSearchNode> nodes = tree.GetNodes();
    if (positionsWorld.IsValid() == false || positionsWorld.IsEmpty() == true ||
        positionsWorld.GetSize() > ~uint32{0} || nodes.IsEmpty() == true ||
        nodes.GetSize() > (~uint32{0}) / LUDUS_POINT_SEARCH_NODE_WORDS || neighborCount == 0 ||
        neighborCount > geometry::MAX_POINT_NEIGHBORS || neighborCount > nodes.GetSize())
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    usize resultCount = 0;
    usize resultBytes = 0;
    usize queryBytes = 0;
    usize nodeBytes = 0;
    usize totalBytes = PARAMETER_COUNT * sizeof(uint32);
    if (TryMultiply(positionsWorld.GetSize(), usize{neighborCount}, resultCount) == false || resultCount > ~uint32{0} ||
        TryMultiply(resultCount, sizeof(geometry::PointNeighbor), resultBytes) == false ||
        TryMultiply(positionsWorld.GetSize(), sizeof(math::Vector4), queryBytes) == false ||
        TryMultiply(nodes.GetSize(), sizeof(geometry::PointSearchNode), nodeBytes) == false ||
        TryAdd(totalBytes, resultBytes, totalBytes) == false || TryAdd(totalBytes, queryBytes, totalBytes) == false ||
        TryAdd(totalBytes, nodeBytes, totalBytes) == false || totalBytes > maximumBufferBytes)
    {
        return rhi::RasterStatus::CapacityExceeded;
    }
    Array<math::Vector4> queries;
    if (queries.TryResize(positionsWorld.GetSize()) == false)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    for (usize i = 0; i < positionsWorld.GetSize(); ++i)
    {
        const math::Vector3 positionWorld = positionsWorld[i];
        if (math::IsFinite(positionWorld) == false ||
            math::Abs(positionWorld.X) > geometry::MAX_POINT_SEARCH_COORDINATE ||
            math::Abs(positionWorld.Y) > geometry::MAX_POINT_SEARCH_COORDINATE ||
            math::Abs(positionWorld.Z) > geometry::MAX_POINT_SEARCH_COORDINATE)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        queries[i] = {positionWorld.X, positionWorld.Y, positionWorld.Z, 0};
    }
    const StaticArray<uint32, PARAMETER_COUNT> parameters{{static_cast<uint32>(nodes.GetSize()),
                                                           static_cast<uint32>(positionsWorld.GetSize()),
                                                           neighborCount,
                                                           tree.GetRoot()}};
    const StaticArray<rhi::ComputeWorkloadBuffer, BUFFER_COUNT> buffers{{
        {sizeof(parameters), GetBytes(BorrowedView<const uint32>{parameters.GetData(), parameters.GetSize()}), true},
        {nodeBytes, GetBytes(nodes), true},
        {queryBytes, GetBytes(BorrowedView<const math::Vector4>{queries.GetData(), queries.GetSize()}), true},
        {resultBytes, {}, false},
    }};
    StaticArray<rhi::ComputeWorkloadBinding, BUFFER_COUNT> bindings{};
    for (usize i = 0; i < BUFFER_COUNT; ++i)
    {
        bindings[i] = {static_cast<uint32>(i),
                       static_cast<uint32>(i),
                       0,
                       buffers[i].Size,
                       i == POINT_NEIGHBOR_RESULT_BUFFER};
    }
    rhi::ComputeWorkloadKernel kernel;
    kernel.NameOrNull = "Point nearest neighbors";
    kernel.Shader = shader;
    kernel.Bindings = {bindings.GetData(), bindings.GetSize()};
    kernel.GroupsX = static_cast<uint32>((positionsWorld.GetSize() - 1) / QUERY_GROUP_SIZE + 1);
    return outWorkload.Initialize(device, {{buffers.GetData(), buffers.GetSize()}, {&kernel, 1}, maximumBufferBytes});
}
} // namespace ludus::graphics::renderer
