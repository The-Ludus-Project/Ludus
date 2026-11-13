#pragma once

// Dynamic bounding volume hierarchy over a CollisionWorld's collider bounds.
//
// Thanks to Erin Catto, "Dynamic Bounding Volume Hierarchies", Game
// Developers Conference 2019, and the b2DynamicTree of Box2D
// (https://box2d.org, MIT licence), for incremental insertion that descends
// toward the sibling with the smallest surface-area cost and for restoring
// balance with single rotations on the way back to the root. Ludus
// implements its own tree over float64 world bounds, with no fattened
// bounds: moving a collider removes and reinserts its leaf. No Box2D code is
// copied.

#include "mesh_arithmetic.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/status.h>

namespace ludus::physics::collision
{
/// Index of no node: the parent of the root and the root of an empty hierarchy.
inline constexpr uint32 NULL_HIERARCHY_NODE = 0xFFFFFFFFU;

/// Pending-node capacity of a traversal. Balancing keeps the height within about 1.44 log2 of the leaf count, under
/// 30 for MAX_COLLISION_WORLD_COLLIDERS, and a depth-first traversal holds at most height + 1 pending nodes.
inline constexpr usize MAX_HIERARCHY_TRAVERSAL_NODES = 128;

/// What a leaf visit asks the traversal to do next.
enum class HierarchyVisit : uint8
{
    Continue,
    Stop,
};

/// Operations on CollisionWorld's hierarchy storage. All of them run on the owner thread except Traverse, which only
/// reads and may run on any thread that holds a read view.
class ColliderHierarchy final
{
public:
    /// Insert the live slot's world bounds as a new leaf; records the leaf in the slot. Never allocates: the node
    /// pool holds two nodes per slot.
    static void Insert(CollisionWorld& inoutWorld, uint32 slot) noexcept;

    /// Remove the live slot's leaf.
    static void Remove(CollisionWorld& inoutWorld, uint32 slot) noexcept;

    /// Height of the tree: zero when empty, one for a single leaf.
    [[nodiscard]] static uint32 GetHeight(const CollisionWorld& world) noexcept;

    /// @brief Visit the leaves whose ancestors all pass `canReach`, depth first, left before right.
    /// @param canReach `bool(const Vector3d& minimum, const Vector3d& maximum)`: false prunes a node and its subtree.
    /// @param visitLeaf `Status(uint32 slot, HierarchyVisit& outVisit)`: a failure stops the traversal and is returned.
    template <typename NodeTest, typename LeafVisit>
    [[nodiscard]] static Status
    Traverse(const CollisionWorld& world, const NodeTest& canReach, const LeafVisit& visitLeaf) noexcept
    {
        if (world.mRoot == NULL_HIERARCHY_NODE)
        {
            return Status::Success;
        }
        foundation::StaticArray<uint32, MAX_HIERARCHY_TRAVERSAL_NODES> pending{};
        usize pendingCount = 0;
        pending[pendingCount++] = world.mRoot;
        while (pendingCount > 0)
        {
            const CollisionWorld::HierarchyNode& node = world.mNodes[pending[--pendingCount]];
            if (canReach(node.BoundsMinWorld, node.BoundsMaxWorld) == false)
            {
                continue;
            }
            if (node.Height == 0)
            {
                HierarchyVisit visit = HierarchyVisit::Continue;
                const Status status = visitLeaf(node.Slot, visit);
                if (status != Status::Success || visit == HierarchyVisit::Stop)
                {
                    return status;
                }
                continue;
            }
            LUDUS_ASSERT(pendingCount + 2 <= pending.GetSize());
            pending[pendingCount++] = node.Right;
            pending[pendingCount++] = node.Left;
        }
        return Status::Success;
    }

private:
    using Node = CollisionWorld::HierarchyNode;

    [[nodiscard]] static uint32 allocateNode(CollisionWorld& inoutWorld) noexcept;
    static void freeNode(CollisionWorld& inoutWorld, uint32 node) noexcept;
    [[nodiscard]] static uint32 findSibling(const CollisionWorld& world, uint32 leaf) noexcept;
    [[nodiscard]] static uint32 balance(CollisionWorld& inoutWorld, uint32 node) noexcept;
    static void refitAncestors(CollisionWorld& inoutWorld, uint32 node) noexcept;
    static void refit(CollisionWorld& inoutWorld, uint32 node) noexcept;
};
} // namespace ludus::physics::collision
