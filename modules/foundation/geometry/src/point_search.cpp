#include <ludus/foundation/geometry/point_search.hpp>

#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::foundation::geometry
{
namespace
{
constexpr uint32 AXIS_COUNT = 3;
constexpr usize NODE_BYTES = LUDUS_POINT_SEARCH_NODE_BYTES;
static_assert(sizeof(PointSearchNode) == NODE_BYTES);
bool IsPositionValid(math::Vector3 positionWorld) noexcept
{
    return math::IsFinite(positionWorld) == true && math::Abs(positionWorld.X) <= MAX_POINT_SEARCH_COORDINATE &&
           math::Abs(positionWorld.Y) <= MAX_POINT_SEARCH_COORDINATE &&
           math::Abs(positionWorld.Z) <= MAX_POINT_SEARCH_COORDINATE;
}
float32 GetCoordinate(math::Vector3 positionWorld, uint32 axis) noexcept
{
    return axis == 0 ? positionWorld.X : axis == 1 ? positionWorld.Y : positionWorld.Z;
}
void BuildNodes(Array<PointSearchNode>& inoutNodes, usize first, usize end, uint32 axis) noexcept
{
    const usize count = end - first;
    Sort(inoutNodes.GetData() + first,
         count,
         [axis](const PointSearchNode& left, const PointSearchNode& right) noexcept {
             const float32 a = GetCoordinate(left.PositionWorld, axis);
             const float32 b = GetCoordinate(right.PositionWorld, axis);
             return a < b || (a == b && left.PointIndex < right.PointIndex);
         });
    const usize middle = first + count / 2;
    const uint32 nextAxis = (axis + 1) % AXIS_COUNT;
    PointSearchNode& node = inoutNodes[middle];
    node.Axis = axis;
    if (middle > first)
    {
        node.Lower = static_cast<uint32>(first + (middle - first) / 2);
        BuildNodes(inoutNodes, first, middle, nextAxis);
    }
    if (middle + 1 < end)
    {
        node.Upper = static_cast<uint32>(middle + 1 + (end - middle - 1) / 2);
        BuildNodes(inoutNodes, middle + 1, end, nextAxis);
    }
}
bool IsNearer(PointNeighbor left, PointNeighbor right) noexcept
{
    return left.DistanceSquared < right.DistanceSquared ||
           (left.DistanceSquared == right.DistanceSquared && left.PointIndex < right.PointIndex);
}
struct SearchVisit final
{
    uint32 Node = ABSENT_POINT_NODE;
    float32 MinimumDistanceSquared = 0;
};
} // namespace
GeometryStatus PointSearchTree::Build(BorrowedView<const math::Vector3> positionsWorld) noexcept
{
    if (positionsWorld.IsValid() == false || positionsWorld.IsEmpty() == true ||
        positionsWorld.GetSize() >= ABSENT_POINT_NODE)
    {
        return GeometryStatus::InvalidArgument;
    }
    for (usize i = 0; i < positionsWorld.GetSize(); ++i)
    {
        if (math::IsFinite(positionsWorld[i]) == false)
        {
            return GeometryStatus::NonFiniteInput;
        }
        if (IsPositionValid(positionsWorld[i]) == false)
        {
            return GeometryStatus::OutOfRange;
        }
    }
    Array<PointSearchNode> candidate;
    if (candidate.TryResize(positionsWorld.GetSize()) == false)
    {
        return GeometryStatus::OutOfMemory;
    }
    for (usize i = 0; i < positionsWorld.GetSize(); ++i)
    {
        candidate[i].PositionWorld = positionsWorld[i];
        candidate[i].PointIndex = static_cast<uint32>(i);
    }
    BuildNodes(candidate, 0, candidate.GetSize(), 0);
    mNodes = Move(candidate);
    return GeometryStatus::Success;
}
GeometryStatus PointSearchTree::FindNearest(math::Vector3 positionWorld,
                                            BorrowedView<PointNeighbor> outNeighbors) const noexcept
{
    if (outNeighbors.IsValid() == false || outNeighbors.IsEmpty() == true ||
        outNeighbors.GetSize() > MAX_POINT_NEIGHBORS || outNeighbors.GetSize() > mNodes.GetSize() ||
        IsPositionValid(positionWorld) == false)
    {
        return GeometryStatus::InvalidArgument;
    }
    StaticArray<PointNeighbor, MAX_POINT_NEIGHBORS> nearest{};
    StaticArray<SearchVisit, POINT_SEARCH_STACK_SIZE> stack{};
    usize stackSize = 1;
    usize found = 0;
    stack[0] = {GetRoot(), 0};
    while (stackSize > 0)
    {
        const SearchVisit visit = stack[--stackSize];
        if (found == outNeighbors.GetSize() && visit.MinimumDistanceSquared > nearest[found - 1].DistanceSquared)
        {
            continue;
        }
        const PointSearchNode& node = mNodes[visit.Node];
        const math::Vector3 offsetWorld = positionWorld - node.PositionWorld;
        const PointNeighbor neighbor{node.PointIndex, math::Dot(offsetWorld, offsetWorld)};
        usize insertion = found;
        if (insertion < outNeighbors.GetSize())
        {
            ++found;
        }
        else if (IsNearer(neighbor, nearest[insertion - 1]) == true)
        {
            --insertion;
        }
        else
        {
            insertion = outNeighbors.GetSize();
        }
        if (insertion < outNeighbors.GetSize())
        {
            while (insertion > 0 && IsNearer(neighbor, nearest[insertion - 1]) == true)
            {
                nearest[insertion] = nearest[insertion - 1];
                --insertion;
            }
            nearest[insertion] = neighbor;
        }
        const float32 delta = GetCoordinate(positionWorld, node.Axis) - GetCoordinate(node.PositionWorld, node.Axis);
        const uint32 nearNode = delta < 0 ? node.Lower : node.Upper;
        const uint32 farNode = delta < 0 ? node.Upper : node.Lower;
        if (farNode != ABSENT_POINT_NODE)
        {
            LUDUS_ASSERT(stackSize < stack.GetSize());
            stack[stackSize++] = {farNode, delta * delta};
        }
        if (nearNode != ABSENT_POINT_NODE)
        {
            LUDUS_ASSERT(stackSize < stack.GetSize());
            stack[stackSize++] = {nearNode, 0};
        }
    }
    for (usize i = 0; i < outNeighbors.GetSize(); ++i)
    {
        outNeighbors[i] = nearest[i];
    }
    return GeometryStatus::Success;
}
BorrowedView<const PointSearchNode> PointSearchTree::GetNodes() const noexcept
{
    return {mNodes.GetData(), mNodes.GetSize()};
}
uint32 PointSearchTree::GetRoot() const noexcept
{
    return mNodes.IsEmpty() == true ? ABSENT_POINT_NODE : static_cast<uint32>(mNodes.GetSize() / 2);
}
} // namespace ludus::foundation::geometry
