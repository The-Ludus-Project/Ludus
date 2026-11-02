#pragma once

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/geometry/detail/point_search_layout.h>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::geometry
{
/// Maximum neighbors returned by the bounded CPU/GPU point-search interface.
inline constexpr usize MAX_POINT_NEIGHBORS = LUDUS_MAX_POINT_NEIGHBORS;
/// Maximum balanced tree traversal depth, sufficient for every uint32 node count.
inline constexpr usize POINT_SEARCH_STACK_SIZE = LUDUS_POINT_SEARCH_STACK_SIZE;
/// Sentinel for a missing child; never a valid node index.
inline constexpr uint32 ABSENT_POINT_NODE = LUDUS_ABSENT_POINT_NODE;
/// Coordinate magnitude bound in world units, keeping float32 squared distances finite.
inline constexpr float32 MAX_POINT_SEARCH_COORDINATE = LUDUS_MAX_POINT_SEARCH_COORDINATE;
/// Packed CPU/GPU node, two sixteen-byte words. Fields are explicit; upload only
/// to the matching shader layout. This is not a host-padding serialization format.
struct PointSearchNode final
{
    math::Vector3 PositionWorld{};    ///< Original input position, unchanged.
    uint32 PointIndex = 0;            ///< Original source index, preserved through tree partitioning.
    uint32 Lower = ABSENT_POINT_NODE; ///< Smaller-coordinate child index, or ABSENT_POINT_NODE.
    uint32 Upper = ABSENT_POINT_NODE; ///< Larger-coordinate child index, or ABSENT_POINT_NODE.
    uint32 Axis = 0;                  ///< Partition axis: 0=X, 1=Y, 2=Z.
    uint32 Reserved = 0;              ///< Zero; reserved for a future version.
};
/// One nearest result, sorted by squared distance then original source index.
struct PointNeighbor final
{
    uint32 PointIndex = ABSENT_POINT_NODE; ///< Original input index; never a tree node index.
    float32 DistanceSquared = 0;           ///< Squared world units; finite and nonnegative.
};
/// Deterministic balanced point tree, independent of mesh topology and ray-tracing BVHs.
/// Build is fallible and O(n log² n); queries allocate nothing. Concurrent queries
/// are safe while the tree remains immutable. Mutation requires exclusive access.
class PointSearchTree final
{
public:
    /// Copy finite positions and build a balanced tree; duplicate positions are valid.
    /// Reject invalid/empty views, excessive coordinate magnitudes or uint32 count
    /// overflow. Failure preserves the entire previous tree. No input borrow survives.
    [[nodiscard]] GeometryStatus Build(BorrowedView<const math::Vector3> positionsWorld) noexcept;
    /// Find exactly outNeighbors.GetSize() nearest points, 1..MAX_POINT_NEIGHBORS
    /// and no more than the tree's point count. Query obeys the build coordinate
    /// bound. Invalid input preserves output. Ties use original source index;
    /// geometrically near ties can differ across CPU/GPU floating implementations.
    [[nodiscard]] GeometryStatus FindNearest(math::Vector3 positionWorld,
                                             BorrowedView<PointNeighbor> outNeighbors) const noexcept;
    /// Borrow immutable packed nodes until the next successful Build or destruction.
    [[nodiscard]] BorrowedView<const PointSearchNode> GetNodes() const noexcept;
    /// Root node index, or ABSENT_POINT_NODE for an unbuilt tree.
    [[nodiscard]] uint32 GetRoot() const noexcept;

private:
    Array<PointSearchNode> mNodes;
};
} // namespace ludus::foundation::geometry
