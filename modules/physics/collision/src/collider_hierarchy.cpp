#include "internal/collider_hierarchy.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::physics::collision
{
namespace
{
namespace math = foundation::math;

/// Bounds of two boxes.
struct Bounds final
{
    math::Vector3d Minimum{};
    math::Vector3d Maximum{};
};

/// The node type is deduced: it is CollisionWorld's private HierarchyNode.
template <typename NodeType>
Bounds GetBounds(const NodeType& node) noexcept
{
    return { .Minimum = node.BoundsMinWorld, .Maximum = node.BoundsMaxWorld };
}

Bounds Combine(const Bounds& first, const Bounds& second) noexcept
{
    return
    {
        .Minimum = {math::Min(first.Minimum.X, second.Minimum.X),
                    math::Min(first.Minimum.Y, second.Minimum.Y),
                    math::Min(first.Minimum.Z, second.Minimum.Z)},
        .Maximum = {math::Max(first.Maximum.X, second.Maximum.X),
                    math::Max(first.Maximum.Y, second.Maximum.Y),
                    math::Max(first.Maximum.Z, second.Maximum.Z)},
    };
}

/// Surface area, the insertion cost: the chance a random ray meets a box is proportional to it.
float64 GetArea(const Bounds& bounds) noexcept
{
    const math::Vector3d extent = bounds.Maximum - bounds.Minimum;
    return 2.0 * (extent.X * extent.Y + extent.Y * extent.Z + extent.Z * extent.X);
}

template <typename NodeType>
void SetBounds(NodeType& outNode, const Bounds& bounds) noexcept
{
    outNode.BoundsMinWorld = bounds.Minimum;
    outNode.BoundsMaxWorld = bounds.Maximum;
}
} // namespace

uint32 ColliderHierarchy::allocateNode(CollisionWorld& inoutWorld) noexcept
{
    // The pool holds two nodes per slot, and a tree of n leaves uses 2n - 1, so it never runs out.
    LUDUS_ASSERT(inoutWorld.mFreeNodes.IsEmpty() == false);
    const uint32 node = inoutWorld.mFreeNodes[inoutWorld.mFreeNodes.GetSize() - 1];
    inoutWorld.mFreeNodes.RemoveLast();
    inoutWorld.mNodes[node] = Node
    {
        .Parent = NULL_HIERARCHY_NODE,
        .Left = NULL_HIERARCHY_NODE,
        .Right = NULL_HIERARCHY_NODE,
        .Slot = 0,
        .Height = 0,
    };
    return node;
}

void ColliderHierarchy::freeNode(CollisionWorld& inoutWorld, uint32 node) noexcept
{
    // The free list's capacity holds every node, so this cannot allocate.
    const bool isListed = inoutWorld.mFreeNodes.TryAdd(node);
    LUDUS_ASSERT(isListed == true);
    (void)isListed;
}

uint32 ColliderHierarchy::findSibling(const CollisionWorld& world, uint32 leaf) noexcept
{
    const Bounds leafBounds = GetBounds(world.mNodes[leaf]);
    uint32 index = world.mRoot;
    while (world.mNodes[index].Height > 0)
    {
        const Node& node = world.mNodes[index];
        const float64 area = GetArea(GetBounds(node));
        const float64 combinedArea = GetArea(Combine(GetBounds(node), leafBounds));
        // Making a new parent here costs its whole area; descending pushes the enlargement onto every ancestor.
        const float64 cost = 2.0 * combinedArea;
        const float64 inheritance = 2.0 * (combinedArea - area);
        const auto getDescentCost = [&world, &leafBounds, inheritance](uint32 child) {
            const Node& childNode = world.mNodes[child];
            const float64 enlarged = GetArea(Combine(GetBounds(childNode), leafBounds));
            return (childNode.Height == 0 ? enlarged : enlarged - GetArea(GetBounds(childNode))) + inheritance;
        };
        const float64 leftCost = getDescentCost(node.Left);
        const float64 rightCost = getDescentCost(node.Right);
        if (cost < leftCost && cost < rightCost)
        {
            break;
        }
        index = leftCost < rightCost ? node.Left : node.Right;
    }
    return index;
}

void ColliderHierarchy::refit(CollisionWorld& inoutWorld, uint32 node) noexcept
{
    Node& parent = inoutWorld.mNodes[node];
    const Node& left = inoutWorld.mNodes[parent.Left];
    const Node& right = inoutWorld.mNodes[parent.Right];
    parent.Height = 1 + math::Max(left.Height, right.Height);
    SetBounds(parent, Combine(GetBounds(left), GetBounds(right)));
}

uint32 ColliderHierarchy::balance(CollisionWorld& inoutWorld, uint32 node) noexcept
{
    Node& top = inoutWorld.mNodes[node];
    if (top.Left == NULL_HIERARCHY_NODE)
    {
        return node;
    }
    const uint32 left = top.Left;
    const uint32 right = top.Right;
    const foundation::int64 imbalance = static_cast<foundation::int64>(inoutWorld.mNodes[right].Height) -
                                        static_cast<foundation::int64>(inoutWorld.mNodes[left].Height);
    if (imbalance >= -1 && imbalance <= 1)
    {
        return node;
    }
    // Rotate the taller child up into this node's place; the taller of its children stays with it and the shorter
    // moves under this node, replacing the risen child.
    const bool isRightTaller = imbalance > 1;
    const uint32 risen = isRightTaller == true ? right : left;
    Node& up = inoutWorld.mNodes[risen];
    const uint32 first = up.Left;
    const uint32 second = up.Right;
    const bool isFirstTaller = inoutWorld.mNodes[first].Height > inoutWorld.mNodes[second].Height;
    const uint32 kept = isFirstTaller == true ? first : second;
    const uint32 moved = isFirstTaller == true ? second : first;

    up.Left = node;
    up.Parent = top.Parent;
    top.Parent = risen;
    if (up.Parent == NULL_HIERARCHY_NODE)
    {
        inoutWorld.mRoot = risen;
    }
    else if (inoutWorld.mNodes[up.Parent].Left == node)
    {
        inoutWorld.mNodes[up.Parent].Left = risen;
    }
    else
    {
        inoutWorld.mNodes[up.Parent].Right = risen;
    }
    up.Right = kept;
    if (isRightTaller == true)
    {
        top.Right = moved;
    }
    else
    {
        top.Left = moved;
    }
    inoutWorld.mNodes[moved].Parent = node;
    refit(inoutWorld, node);
    refit(inoutWorld, risen);
    return risen;
}

void ColliderHierarchy::refitAncestors(CollisionWorld& inoutWorld, uint32 node) noexcept
{
    uint32 index = node;
    while (index != NULL_HIERARCHY_NODE)
    {
        index = balance(inoutWorld, index);
        refit(inoutWorld, index);
        index = inoutWorld.mNodes[index].Parent;
    }
}

void ColliderHierarchy::Insert(CollisionWorld& inoutWorld, uint32 slot) noexcept
{
    CollisionWorld::Slot& record = inoutWorld.mSlots[slot];
    const uint32 leaf = allocateNode(inoutWorld);
    Node& leafNode = inoutWorld.mNodes[leaf];
    leafNode.Slot = slot;
    SetBounds(leafNode, { .Minimum = record.BoundsMinWorld, .Maximum = record.BoundsMaxWorld });
    record.Leaf = leaf;
    if (inoutWorld.mRoot == NULL_HIERARCHY_NODE)
    {
        inoutWorld.mRoot = leaf;
        return;
    }
    const uint32 sibling = findSibling(inoutWorld, leaf);
    const uint32 oldParent = inoutWorld.mNodes[sibling].Parent;
    const uint32 parent = allocateNode(inoutWorld);
    Node& parentNode = inoutWorld.mNodes[parent];
    parentNode.Parent = oldParent;
    parentNode.Left = sibling;
    parentNode.Right = leaf;
    parentNode.Height = inoutWorld.mNodes[sibling].Height + 1;
    SetBounds(parentNode, Combine(GetBounds(inoutWorld.mNodes[sibling]), GetBounds(inoutWorld.mNodes[leaf])));
    inoutWorld.mNodes[sibling].Parent = parent;
    inoutWorld.mNodes[leaf].Parent = parent;
    if (oldParent == NULL_HIERARCHY_NODE)
    {
        inoutWorld.mRoot = parent;
    }
    else if (inoutWorld.mNodes[oldParent].Left == sibling)
    {
        inoutWorld.mNodes[oldParent].Left = parent;
    }
    else
    {
        inoutWorld.mNodes[oldParent].Right = parent;
    }
    refitAncestors(inoutWorld, parent);
}

void ColliderHierarchy::Remove(CollisionWorld& inoutWorld, uint32 slot) noexcept
{
    const uint32 leaf = inoutWorld.mSlots[slot].Leaf;
    if (leaf == inoutWorld.mRoot)
    {
        inoutWorld.mRoot = NULL_HIERARCHY_NODE;
        freeNode(inoutWorld, leaf);
        return;
    }
    const uint32 parent = inoutWorld.mNodes[leaf].Parent;
    const uint32 grandparent = inoutWorld.mNodes[parent].Parent;
    const uint32 sibling =
        inoutWorld.mNodes[parent].Left == leaf ? inoutWorld.mNodes[parent].Right : inoutWorld.mNodes[parent].Left;
    inoutWorld.mNodes[sibling].Parent = grandparent;
    if (grandparent == NULL_HIERARCHY_NODE)
    {
        inoutWorld.mRoot = sibling;
    }
    else
    {
        if (inoutWorld.mNodes[grandparent].Left == parent)
        {
            inoutWorld.mNodes[grandparent].Left = sibling;
        }
        else
        {
            inoutWorld.mNodes[grandparent].Right = sibling;
        }
        refitAncestors(inoutWorld, grandparent);
    }
    freeNode(inoutWorld, parent);
    freeNode(inoutWorld, leaf);
}

uint32 ColliderHierarchy::GetHeight(const CollisionWorld& world) noexcept
{
    return world.mRoot == NULL_HIERARCHY_NODE ? 0 : world.mNodes[world.mRoot].Height + 1;
}
} // namespace ludus::physics::collision
