#pragma once

/// @file
/// @brief Owned store of immutable collision shapes, referenced by leased shape handles.
///
/// See "Module and ownership model" in docs/architecture/physics-systems.md and "Geometry store and collision
/// world" in docs/architecture/physics-collision.md.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/identity/handle.hpp>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/status.h>
#include <ludus/physics/collision/triangle_mesh.h>

namespace ludus::physics::collision
{
/// Handle domain marker for shapes in a GeometryStore; never instantiated.
struct ShapeTag final
{
};

/// @brief Reference to one shape incarnation in one GeometryStore.
///
/// A handle names a shape; a *lease* keeps it alive. Every successful TryAdd* call returns a handle that carries
/// one lease for its caller. The store frees a shape when its last lease is released, and the handle then becomes
/// stale: its slot's generation advances before the slot is reused.
using ShapeHandle = foundation::identity::Handle<ShapeTag>;

/// Largest number of shape slots a store admits.
inline constexpr uint32 MAX_GEOMETRY_STORE_SHAPES = 1U << 20U;

/// Largest number of leases one shape admits.
inline constexpr uint32 MAX_SHAPE_LEASES = 0xFFFFFFFEU;

/// @brief Kind of a stored shape.
enum class ShapeKind : uint8
{
    TriangleMesh, ///< A cooked TriangleMesh, for static environment geometry.
    Sphere,       ///< A sphere primitive.
    Box,          ///< A box primitive.
    Capsule,      ///< A capsule primitive.
    Hull,         ///< A cooked ConvexHull.
};

/// @brief Fixed capacity of a GeometryStore, chosen at initialization.
struct GeometryStoreSettings final
{
    uint32 MaxShapes = 0; ///< Shape slots to reserve, in [1, MAX_GEOMETRY_STORE_SHAPES].
};

/// @brief Explicitly owned service holding immutable cooked shapes.
///
/// The store reserves every slot when it is initialized, so adding, leasing and releasing shapes never allocate.
/// Stored shapes never move: a shape's address is stable from its addition until its last lease is released. Every
/// handle carries the store's process-unique owner token, so a handle from one store is rejected by another, and slot
/// generations advance on release and never wrap (a slot released at the terminal generation is retired).
///
/// Thread affinity: initialization, additions, leases and releases happen on one owner thread. They never change a
/// shape another lease holds, so CollisionWorld queries that read leased shapes may run on other threads meanwhile.
/// The store is neither copyable nor movable, because each CollisionWorld that uses it keeps its address.
class GeometryStore final
{
public:
    /// Construct an uninitialized store. Does not allocate.
    GeometryStore() noexcept = default;
    /// Not copyable: shapes have one owner.
    GeometryStore(const GeometryStore&) = delete;
    /// Not copyable: shapes have one owner.
    GeometryStore& operator=(const GeometryStore&) = delete;
    /// Not movable: initialized worlds keep the store's address.
    GeometryStore(GeometryStore&&) = delete;
    /// Not movable: initialized worlds keep the store's address.
    GeometryStore& operator=(GeometryStore&&) = delete;
    /// Release every stored shape.
    /// @pre No CollisionWorld initialized with this store is still alive; checked by assertion.
    ~GeometryStore() noexcept;

    /// @brief Reserve every slot and obtain the store's owner token.
    /// @param settings Capacity in [1, MAX_GEOMETRY_STORE_SHAPES].
    /// @return Success; InvalidPhase when already initialized; InvalidArgument for zero capacity; CapacityExceeded
    /// above MAX_GEOMETRY_STORE_SHAPES; AllocationFailure; or IdentityExhausted. The store is unchanged on failure.
    [[nodiscard]] Status TryInitialize(const GeometryStoreSettings& settings) noexcept;

    /// @brief Take ownership of a cooked triangle mesh.
    /// @param mesh Moved from only on success.
    /// @param outShape Receives the new handle, carrying one lease, on success. Unchanged on failure.
    /// @return Success; InvalidPhase when uninitialized; NoGeometry for an uncooked mesh; or CapacityExceeded when
    /// no slot is free.
    [[nodiscard]] Status TryAddTriangleMesh(TriangleMesh&& mesh, ShapeHandle& outShape) noexcept;

    /// @brief Take ownership of a cooked convex hull.
    /// @param hull Moved from only on success.
    /// @param outShape Receives the new handle, carrying one lease, on success. Unchanged on failure.
    /// @return Success; InvalidPhase when uninitialized; NoGeometry for an uncooked hull; or CapacityExceeded.
    [[nodiscard]] Status TryAddConvexHull(ConvexHull&& hull, ShapeHandle& outShape) noexcept;

    /// @brief Store a sphere, box or capsule primitive.
    /// @param shape A sphere, box or capsule whose dimensions satisfy ConvexShape's admitted values.
    /// @param outShape Receives the new handle, carrying one lease, on success. Unchanged on failure.
    /// @return Success; InvalidPhase when uninitialized; NonFiniteInput, InvalidArgument (including a Hull kind,
    /// which TryAddConvexHull stores) or OutOfRange for the dimensions; or CapacityExceeded.
    [[nodiscard]] Status TryAddPrimitive(const ConvexShape& shape, ShapeHandle& outShape) noexcept;

    /// @brief Add one lease to a live shape.
    /// @return Success; InvalidHandle; or CapacityExceeded at MAX_SHAPE_LEASES.
    [[nodiscard]] Status TryRetain(ShapeHandle shape) noexcept;

    /// @brief Release one lease. Releasing the last lease frees the shape and makes every copy of the handle stale.
    /// @return Success, or InvalidHandle for a handle that is not live in this store.
    [[nodiscard]] Status TryRelease(ShapeHandle shape) noexcept;

    /// Whether `shape` names a live shape of this store.
    [[nodiscard]] bool IsAlive(ShapeHandle shape) const noexcept;

    /// @brief Report a live shape's kind.
    /// @param shape Handle to resolve.
    /// @param outKind Receives the kind on success; unchanged on failure.
    /// @return Success, or InvalidHandle.
    [[nodiscard]] Status TryGetKind(ShapeHandle shape, ShapeKind& outKind) const noexcept;

    /// @brief Report a live shape's lease count.
    /// @param shape Handle to resolve.
    /// @param outLeaseCount Receives the count on success; unchanged on failure.
    /// @return Success, or InvalidHandle.
    [[nodiscard]] Status TryGetLeaseCount(ShapeHandle shape, uint32& outLeaseCount) const noexcept;

    /// @brief Borrow a live triangle-mesh shape.
    /// @param shape Handle to resolve.
    /// @param outMeshOrNull Receives the mesh, valid while a lease is held, on success; unchanged on failure.
    /// @return Success; InvalidHandle; or Unsupported for a shape of another kind.
    [[nodiscard]] Status TryGetTriangleMesh(ShapeHandle shape, const TriangleMesh*& outMeshOrNull) const noexcept;

    /// Whether the store has been initialized.
    [[nodiscard]] LUDUS_INLINE bool IsInitialized() const noexcept
    {
        return mOwner != 0;
    }
    /// Number of live shapes.
    [[nodiscard]] LUDUS_INLINE usize GetShapeCount() const noexcept
    {
        return mLiveCount;
    }

private:
    /// One shape slot. Only the member that matches Kind holds data.
    struct Slot final
    {
        TriangleMesh Mesh;       ///< The shape of a ShapeKind::TriangleMesh slot.
        ConvexHull Hull;         ///< The hull of a ShapeKind::Hull slot.
        ConvexShape Primitive{}; ///< A primitive, or a Hull-kind shape that borrows Hull.
        uint32 Generation = foundation::identity::FIRST_GENERATION; ///< Current incarnation of the slot.
        uint32 LeaseCount = 0;                                      ///< Zero exactly when the slot is free or retired.
        ShapeKind Kind = ShapeKind::TriangleMesh;                   ///< Which member holds the shape.
    };

    friend class CollisionWorld;

    [[nodiscard]] Slot* findLive(ShapeHandle shape) noexcept;
    [[nodiscard]] const Slot* findLive(ShapeHandle shape) const noexcept;
    [[nodiscard]] Status reserveSlot(uint32& outSlot) const noexcept;
    [[nodiscard]] ShapeHandle publish(uint32 slot, ShapeKind kind) noexcept;

    foundation::Array<Slot> mSlots;
    foundation::Array<uint32> mFreeSlots; ///< LIFO list of reusable slots.
    uint64 mOwner = 0;
    usize mLiveCount = 0;
    uint32 mAttachedWorldCount = 0; ///< Initialized CollisionWorlds that use this store.
};
} // namespace ludus::physics::collision
